#include<linux/version.h>
#include "logger.hpp"
#include "systemCallList.hpp"
#include "systemCall.hpp"
#include "dettraceSystemCall.hpp"
#include "util.hpp"
#include "state.hpp"
#include "ptracer.hpp"
#include "execution.hpp"

#include <stack>
// =======================================================================================
execution::execution(int debugLevel, pid_t startingPid):
  log {stderr, debugLevel},
  nextPid {startingPid},
  // Waits for first process to be ready! Probably not good to have this kind of
  // dependency of a initialization list?
  tracer{startingPid},
  debugLevel(debugLevel){
    // Set state for first process.
    states.emplace(startingPid, state {log, startingPid});

    // First process is special and we must set the options ourselves.
    // This is done everytime a new process is spawned.
    ptracer::setOptions(startingPid);
  }
// =======================================================================================
void execution::handleExit(){
  if(processHier.empty()){
    // All processes have finished!
    exitLoop = true;
    return;
  }
  // Pop entry from map.
  states.erase(traceesPid);
  // Set next pid to our parent.
  nextPid = processHier.top();
  processHier.pop();

  log.unsetPadding();
  return;
}
// =======================================================================================
bool execution::handlePreSystemCall(state& currState){
  int syscallNum = tracer.getSystemCallNumber();
  currState.systemcall = getSystemCall(syscallNum, systemCallMappings[syscallNum]);

  // No idea what this system call is! error out.
  if(syscallNum < 0 || syscallNum > SYSTEM_CALL_COUNT){
    throw runtime_error("Unkown system call number: " + to_string(syscallNum));
  }

  // Print!
  string systemCall = currState.systemcall->syscallName;
  string redColoredSyscall = logger::makeTextColored(Color::red, systemCall);
  log.writeToLog(Importance::inter,"[Time %d][Pid %d] Intercepted %s (#%d)\n",
		 currState.getLogicalTime(), traceesPid, redColoredSyscall.c_str(),
		 syscallNum);
  log.setPadding();

  bool callPostHook = currState.systemcall->handleDetPre(currState, tracer);
#if LINUX_VERSION_CODE < KERNEL_VERSION(4,8,0)
  // Next event will be a sytem call pre-exit event.
  currState.isPreExit = true;
#endif

  // This is the easiest time to tell a fork even happened. It's not trivial
  // to check the event as we might get a signal first from the child process.
  // See:
  // https://stackoverflow.com/questions/29997244/
  // occasionally-missing-ptrace-event-vfork-when-running-ptrace
  if(systemCall == "fork" || systemCall == "vfork" || systemCall == "clone"){
    int status;
    ptraceEvent e;

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,8,0)
    // fork/vfork/clone pre system call.
    e = getNextEvent(traceesPid, traceesPid, status, true);
    // That was the pre-exit event, make sure we set isPreExit to false.
    currState.isPreExit = false;
#endif
    // This event is known to be either a fork/vfork event or a signal.
    e = getNextEvent(traceesPid, traceesPid, status, false);
    handleFork(e);

    // This was a fork, vfork, or clone. No need to go into the post-interception hook.
    return false;
  }

#if LINUX_VERSION_CODE < KERNEL_VERSION(4,8,0)
  // This is the seccomp event where we do the work for the pre-system call hook.
  // In older versions of seccomp, we must also do the pre-exit ptrace event, as we
  // have to. This is dictated by this variable.
  return true;
#else
  // If debugging we let system call go to post hook so we can see return values.
  // Notice we must still return false in the fork case. So we should not move this
  // expression "higher up" in the call chain.
  return debugLevel >= 4 ? true : callPostHook;
#endif
}
// =======================================================================================
void execution::handlePostSystemCall(state& currState){
  log.writeToLog(Importance::info,"%s value before post-hook: %d\n",
		 currState.systemcall->syscallName.c_str(),
		 tracer.getReturnValue());

  currState.systemcall->handleDetPost(currState, tracer);

  // System call was done in the last iteration.
  log.writeToLog(Importance::info,"%s returned with value: %d\n",
		 currState.systemcall->syscallName.c_str(),
		 tracer.getReturnValue());

  log.unsetPadding();
  return;
}
// =======================================================================================
void execution::runProgram(){
  // When using seccomp, we usually run with PTRACE_CONT. The issue is that seccomp only
  // reports pre hook events. To get post hook events we must call ptrace with
  // PTRACE_SYSCALL intead. This happens in @getNextEvent.
  bool callPostHook = false;

  // Iterate over entire process' and all subprocess' execution.
  while(! exitLoop){
    int status;
    ptraceEvent ret = getNextEvent(nextPid, traceesPid, status, callPostHook);
    nextPid = traceesPid;

    // Most common event. Basically, only system calls that must be determinized
    // come here, we run the pre-systemCall hook.
    if(ret == ptraceEvent::seccomp){
      callPostHook = handleSeccomp();
      continue;
    }

    // We still need this case even though we use seccomp + bpf. Since we do post-hook
    // interception of system calls through PTRACE_SYSCALL. Only post system call
    // events come here.
    if(ret == ptraceEvent::syscall){
      #if LINUX_VERSION_CODE < KERNEL_VERSION(4,8,0)
      state& currentState = states.at(traceesPid);
      // Skip pre exit calls nothing for us to do. We did the work during handleSeccomp()
      // on the seccomp event.
      if(currentState.isPreExit){
	callPostHook = true;
	currentState.isPreExit = false;
	continue;
      }
      #endif
      tracer.updateState(traceesPid);
      handlePostSystemCall( states.at(traceesPid) );
      // Nope, we're done with the current system call. Wait for next seccomp event.
      callPostHook = false;
      continue;
    }

    // Current process was ended by signal.
    if(ret == ptraceEvent::terminatedBySignal){
      auto msg =
	logger::makeTextColored(Color::blue, "Process [%d] ended by signal %d.\n");
      log.writeToLog(Importance::inter, msg, traceesPid, WTERMSIG(status));
      handleExit();
      continue;
    }

    // Current process is done.
    if(ret == ptraceEvent::exit){
      log.writeToLog(Importance::inter,
		     logger::makeTextColored(Color::blue, "Process [%d] has finished.\n"),
		     traceesPid);
      handleExit();
      continue;
    }

    // We have encountered a call to fork, vfork, clone.
    if(ret == ptraceEvent::fork || ret == ptraceEvent::vfork){
      // Nothing to do, instead we handle it when we see the system call pre exit.
      // Since this is the easiest time to tell a fork even happened. It's not trivial
      // to check the event as we might get a signal first from the child process.
      // See:
      // https://stackoverflow.com/questions/29997244/
      // occasionally-missing-ptrace-event-vfork-when-running-ptrace
      continue;
    }

    if(ret == ptraceEvent::clone){
      handleClone();
      continue;
    }

    if(ret == ptraceEvent::exec){
      handleExecve();
      continue;
    }

    if(ret == ptraceEvent::signal){
      int signalNum = WSTOPSIG(status);
      handleSignal(signalNum);
      continue;
    }

    throw runtime_error(to_string(traceesPid) +
			" Uknown return value for ptracer::getNextEvent()\n");
  }
}
// =======================================================================================
void execution::handleFork(ptraceEvent event){
  pid_t newChildPid;

  // Notice in both cases, we catch one of the two events and ignore the other.
  if(event == ptraceEvent::fork || event == ptraceEvent::vfork){
    // Fork event came first.
    newChildPid = handleForkEvent();

    // Wait for child to be ready.
    log.writeToLog(Importance::info,
		   logger::makeTextColored(Color::blue,
		     "Waiting for child to be ready for tracing...\n"));
    int status;

    int retPid = waitpid(newChildPid, &status, 0);
    if(retPid == -1){
      throw runtime_error("waitpid failed:" + string { strerror(errno) });
    }

    // This should never happen.
    if(retPid != newChildPid){
      throw runtime_error("wait call return pid does not match new child's pid.");
    }
    log.writeToLog(Importance::info,
		   logger::makeTextColored(Color::blue, "Child ready: %d\n"),
		   retPid);
  }else{
    if(event != ptraceEvent::signal){
      throw runtime_error("Expected signal after fork/vfork event!");
    }
    // Signal event came first.
    handleForkSignal();
    newChildPid = handleForkEvent();
  }

  // Set child to run as next event.
  nextPid = newChildPid;
}
// =======================================================================================
pid_t execution::handleForkEvent(){
  log.writeToLog(Importance::inter,
		 logger::makeTextColored(Color::blue,
		   "[%d] Fork event came before signal!\n"),
		 traceesPid);
  // Current scheduling policy: Let child run to completion.
  pid_t newChildPid = tracer.getEventMessage();
  pid_t parentsPid = traceesPid;
  // Push parent id to process stack.
  processHier.push(parentsPid);

  // Add this new process to our states.
  log.writeToLog(Importance::info,
 		 logger::makeTextColored(Color::blue,"Added process [%d] to states map.\n"),
		 newChildPid);
  states.emplace(newChildPid, state {log, newChildPid} );

  return newChildPid;
}
// =======================================================================================
void execution::handleForkSignal(){
  log.writeToLog(Importance::info,
		 logger::makeTextColored(Color::blue,
                   "[%d] Child fork signal-stop came before fork event.\n"),
		 traceesPid);
  int status;
  // Intercept any system call.
  // This should really be the parents pid. which we don't have readily avaliable.
  traceesPid = waitpid(-1, &status, 0);
  if(traceesPid == -1){
    throw runtime_error("waitpid failed:" + string { strerror(errno) });
  }

  if(! ptracer::isPtraceEvent(status, PTRACE_EVENT_FORK) &&
     ! ptracer::isPtraceEvent(status, PTRACE_EVENT_VFORK)){
    throw runtime_error("Expected fork or vfork event!\n");
  }
  return;
}
// =======================================================================================
void execution::handleClone(){
  // Nothing to do for now...
  log.writeToLog(Importance::inter,
		 logger::makeTextColored(Color::blue, "[%d] caught clone event!\n"),
		 traceesPid);
  return;
}
// =======================================================================================
void execution::handleExecve(){
  // Nothing to do for now... New process is already automatically ptraced by
  // our tracer.
  log.writeToLog(Importance::inter,
		 logger::makeTextColored(Color::blue, "[%d] Caught execve event!\n"),
		 traceesPid);
  return;
}
// =======================================================================================
bool execution::handleSeccomp(){
  // Fetch system call provided to us via seccomp.
  uint16_t syscallNum;
  ptracer::doPtrace(PTRACE_GETEVENTMSG, traceesPid, nullptr, &syscallNum);

  // INT16_MAX is sent by seccomp by convention as for system calls with no
  // rules.
  if(syscallNum == INT16_MAX){
    // Fetch real system call from register.
    tracer.updateState(traceesPid);
    syscallNum = tracer.getSystemCallNumber();
    throw runtime_error("No filter rule for system call: " +
			systemCallMappings[syscallNum]);
  }

  // TODO: Right now we update this information on every exit and entrance, as a
  // small optimization we might not want to...

  // Get registers from tracee.
  tracer.updateState(traceesPid);
  return handlePreSystemCall( states.at(traceesPid) );
}
// =======================================================================================
void execution::handleSignal(int sigNum){
  // Remember to deliver this signal to the tracee for next event! Happens in
  // getNextEvent.
  states.at(traceesPid).signalToDeliver = sigNum;
  auto msg = "[%d] Tracer: Received signal: %d. Forwading signal to tracee.\n";
  auto coloredMsg = logger::makeTextColored(Color::blue, msg);
  log.writeToLog(Importance::inter, coloredMsg, traceesPid, sigNum);
  return;
}
// =======================================================================================
unique_ptr<systemCall>
execution::getSystemCall(int syscallNumber, string syscallName){
    switch(syscallNumber){
    case SYS_access:
      return make_unique<accessSystemCall>(syscallNumber, syscallName);
    case SYS_alarm:
      return make_unique<alarmSystemCall>(syscallNumber, syscallName);
    case SYS_chdir:
      return make_unique<chdirSystemCall>(syscallNumber, syscallName);
    case SYS_chmod:
      return make_unique<chmodSystemCall>(syscallNumber, syscallName);
    case SYS_clock_gettime:
      return make_unique<clock_gettimeSystemCall>(syscallNumber, syscallName);
    case SYS_clone:
      return make_unique<cloneSystemCall>(syscallNumber, syscallName);
    case SYS_connect:
      return make_unique<connectSystemCall>(syscallNumber, syscallName);
    case SYS_execve:
      return make_unique<execveSystemCall>(syscallNumber, syscallName);
    case SYS_fstat:
      return make_unique<fstatSystemCall>(syscallNumber, syscallName);
    case SYS_newfstatat:
      return make_unique<newfstatatSystemCall>(syscallNumber, syscallName);
    case SYS_fstatfs:
      return make_unique<fstatfsSystemCall>(syscallNumber, syscallName);
    case SYS_futex:
      return make_unique<futexSystemCall>(syscallNumber, syscallName);
    case SYS_getcwd:
      return make_unique<getcwdSystemCall>(syscallNumber, syscallName);
    case SYS_getdents:
      return make_unique<getdentsSystemCall>(syscallNumber, syscallName);
    case SYS_getrandom:
	return make_unique<getrandomSystemCall>(syscallNumber, syscallName);
    case SYS_getrlimit:
      return make_unique<getrlimitSystemCall>(syscallNumber, syscallName);
    case SYS_getrusage:
      return make_unique<getrusageSystemCall>(syscallNumber, syscallName);
    case SYS_gettimeofday:
      return make_unique<gettimeofdaySystemCall>(syscallNumber, syscallName);
    case SYS_ioctl:
      return make_unique<ioctlSystemCall>(syscallNumber, syscallName);
    case SYS_nanosleep:
      return make_unique<nanosleepSystemCall>(syscallNumber, syscallName);
    case SYS_lstat:
      return make_unique<lstatSystemCall>(syscallNumber, syscallName);
    case SYS_open:
      return make_unique<openSystemCall>(syscallNumber, syscallName);
    case SYS_openat:
      return make_unique<openatSystemCall>(syscallNumber, syscallName);
    case SYS_pipe:
      return make_unique<pipeSystemCall>(syscallNumber, syscallName);
    case SYS_pselect6:
      return make_unique<pselect6SystemCall>(syscallNumber, syscallName);
    case SYS_poll:
      return make_unique<pollSystemCall>(syscallNumber, syscallName);
    case SYS_prlimit64:
      return make_unique<prlimit64SystemCall>(syscallNumber, syscallName);
    case SYS_read:
      return make_unique<readSystemCall>(syscallNumber, syscallName);
    case SYS_readlink:
      return make_unique<readlinkSystemCall>(syscallNumber, syscallName);
    case SYS_sendto:
      return make_unique<sendtoSystemCall>(syscallNumber, syscallName);
    case SYS_select:
      return make_unique<selectSystemCall>(syscallNumber, syscallName);
    case SYS_set_robust_list:
      return make_unique<set_robust_listSystemCall>(syscallNumber, syscallName);
    case SYS_statfs:
      return make_unique<statfsSystemCall>(syscallNumber, syscallName);
    case SYS_stat:
      return make_unique<statSystemCall>(syscallNumber, syscallName);
    case SYS_sysinfo:
      return make_unique<sysinfoSystemCall>(syscallNumber, syscallName);
    case SYS_tgkill:
      return make_unique<tgkillSystemCall>(syscallNumber, syscallName);
    case SYS_time:
      return make_unique<timeSystemCall>(syscallNumber, syscallName);
    case SYS_uname:
      return make_unique<unameSystemCall>(syscallNumber, syscallName);
    case SYS_unlink:
      return make_unique<unlinkSystemCall>(syscallNumber, syscallName);
    case SYS_unlinkat:
      return make_unique<unlinkatSystemCall>(syscallNumber, syscallName);
    case SYS_utimensat:
      return make_unique<utimensatSystemCall>(syscallNumber, syscallName);
    case SYS_vfork:
      return make_unique<vforkSystemCall>(syscallNumber, syscallName);
    case SYS_write:
      return make_unique<writeSystemCall>(syscallNumber, syscallName);
    case SYS_writev:
      return make_unique<writeSystemCall>(syscallNumber, syscallName);
    }

    // Generic system call. Throws error.
    throw runtime_error("Missing case for system call: " + syscallName
			+ " this is a bug!");
  }
// =======================================================================================
ptraceEvent execution::getNextEvent(pid_t currentPid, pid_t& traceesPid, int& status,
				    bool ptraceSystemcall){
  // At every doPtrace we have the choice to deliver a signal. We must deliver a signal
  // when an actual signal was returned (ptraceEvent::signal), otherwise the signal is
  // never delivered to the tracee! This field is updated in @handleSignal
  //
  // 64 bit value to avoid warning when casting to void* below.
  int64_t signalToDeliver = states.at(nextPid).signalToDeliver;
  // int64_t signalToDeliver = 0;
  // Reset signal field after for next event.
  states.at(nextPid).signalToDeliver = 0;

  // Usually we use PTRACE_CONT below because we are letting seccomp + bpf handle the
  // events. So unlike standard ptrace, we do not rely on system call events. Instead,
  // we wait for seccomp events. Note that seccomp + bpf only sends us (the tracer)
  // a ptrace event on pre-system call events. Sometimes we need the system call to be
  // called and then we change it's arguments. So we call PTRACE_SYSCALL instead.
  if(ptraceSystemcall){
    ptracer::doPtrace(PTRACE_SYSCALL, currentPid, 0, (void*) signalToDeliver);
  }else{
    // Tell the process that we just intercepted an event for to continue, with us tracking
    // it's system calls. If this is the first time this function is called, it will be the
    // starting process. Which we expect to be in a waiting state.
    ptracer::doPtrace(PTRACE_CONT, currentPid, 0, (void*) signalToDeliver);
  }

  // Intercept any system call.
  traceesPid = waitpid(-1, &status, 0);
  if(traceesPid == -1){
    throw runtime_error("waitpid failed:" + string { strerror(errno) });
  }

  // Check if tracee has exited.
  if (WIFEXITED(status)){
    return ptraceEvent::exit;
  }

  // Condition for PTRACE_O_TRACEEXEC
  if( ptracer::isPtraceEvent(status, PTRACE_EVENT_EXEC) ){
    return ptraceEvent::exec;
  }

  // Condition for PTRACE_O_TRACECLONE
  if( ptracer::isPtraceEvent(status, PTRACE_EVENT_CLONE) ){
    return ptraceEvent::clone;
  }

  // Condition for PTRACE_O_TRACEVFORK
  if( ptracer::isPtraceEvent(status, PTRACE_EVENT_VFORK) ){
    return ptraceEvent::vfork;
  }

  // Even though fork() is clone under the hood, any time that clone is used with
  // SIGCHLD, ptrace calls that event a fork *sigh*.
  // Also requires PTRACE_O_FORK flag.
  if( ptracer::isPtraceEvent(status, PTRACE_EVENT_FORK) ){
    return ptraceEvent::fork;
  }

#ifdef PTRACE_EVENT_STOP
  if( ptracer::isPtraceEvent(status, PTRACE_EVENT_STOP) ){
    throw runtime_error("Ptrace event stop.\n");
  }
#endif

  if( ptracer::isPtraceEvent(status, PTRACE_EVENT_EXIT) ){
    throw runtime_error("Ptrace event exit.\n");
  }

  if( ptracer::isPtraceEvent(status, PTRACE_EVENT_SECCOMP) ){
    return ptraceEvent::seccomp;
  }

  // This is a stop caused by a system call exit-pre/exit-post.
  // Check if WIFSTOPPED return true,
  // if yes, compare signal number to SIGTRAP | 0x80 (see ptrace(2)).
  if(WIFSTOPPED(status) && (WSTOPSIG(status) == (SIGTRAP | 0x80)) ){
    return ptraceEvent::syscall;
  }

  // Check if we intercepted a signal before it was delivered to the child.
  if(WIFSTOPPED(status)){
    return ptraceEvent::signal;
  }

  // Check if the child was terminated by a signal. This can happen after when we,
  //the tracer, intercept a signal of the tracee and deliver it.
  if(WIFSIGNALED(status)){
    return ptraceEvent::terminatedBySignal;
  }

  throw runtime_error("Uknown event on dettrace::getNextEvent()");
}
// =======================================================================================
