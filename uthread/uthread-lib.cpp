Makefile                                                                                            0000644 0620316 0000050 00000001020 14225766455 011364  0                                                                                                    ustar   ofekk                           stud                                                                                                                                                                                                                   CC=g++
CXX=g++
RANLIB=ranlib

LIBSRC=uthreads.cpp
LIBOBJ=$(LIBSRC:.cpp=.o)

INCS=-I.
CFLAGS = -Wall -std=c++11 -g $(INCS)
CXXFLAGS = -Wall -std=c++11 -g $(INCS)

UTHREADLIB = libuthreads.a
TARGETS = $(UTHREADLIB)

TARFLAGS=-cvf
TARNAME=ex2.tar
TARSRCS=$(LIBSRC) Makefile README
TAR=tar

all: $(TARGETS)

$(TARGETS): $(LIBOBJ)
	$(AR) $(ARFLAGS) $@ $^
	$(RANLIB) $@

clean:
	$(RM) $(TARGETS) $(OSMLIB) $(OBJ) $(LIBOBJ) *~ *core

depend:
	makedepend -- $(CFLAGS) -- $(SRC) $(LIBSRC)

tar:
	$(TAR) $(TARFLAGS) $(TARNAME) $(TARSRCS)
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                uthreads.cpp                                                                                        0000644 0620316 0000050 00000040145 14225763403 012250  0                                                                                                    ustar   ofekk                           stud                                                                                                                                                                                                                   #include <queue>
#include <map>
#include <sys/time.h>
#include <signal.h>
#include <iostream>
#include <csetjmp>
#include <set>
#include <unordered_map>
#include <string.h>
#include <algorithm>


#ifdef __x86_64__
/* code for 64 bit Intel arch */

typedef unsigned long address_t;
#define JB_SP 6
#define JB_PC 7

/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
  address_t ret;
  asm volatile("xor    %%fs:0x30,%0\n"
               "rol    $0x11,%0\n"
               : "=g" (ret)
               : "0" (addr));
  return ret;
}

#else
/* code for 32 bit Intel arch */

typedef unsigned int address_t;
#define JB_SP 4
#define JB_PC 5


/* A translation is required when using an address of a variable.
   Use this as a black box in your code. */
address_t translate_address(address_t addr)
{
  address_t ret;
  asm volatile("xor    %%gs:0x18,%0\n"
               "rol    $0x9,%0\n"
               : "=g" (ret)
               : "0" (addr));
  return ret;
}


#endif

//====================== CONSTANTS ======================
enum State {RUNNING, READY, SLEEP, BLOCKED, BLOCKED_SLEEP};

#define MAX_THREAD_NUM 100 /* maximal number of threads */
#define STACK_SIZE 4096 /* stack size per thread (in bytes) */
#define ERROR (-1) /*error return value*/
#define SYS_ERROR_MSG "system error: "
#define FUNC_ERROR_MSG "thread library error: "
#define JMP_VAL 1

typedef void (*thread_entry_point)(void);

  //====================== DECLARATIONS ======================
  void abortProgram();
  int uthread_spawn(thread_entry_point entry_point);
  void switchThreads();
  void wakeUpThreads();
  void blockSigVTAlarm();
  void unblockSigVTAlarm();

  //====================== PRIMITIVE GLOBALS ======================
  int nextTid = 0;
  int threadAmount = 0;
  int totalQuantums = 0;

  //====================== CLASSES & STRUCTS ======================
  typedef struct threadDescriptor
  {
      int tid;
      State state;
      char* stack;
      sigjmp_buf env;
  } threadDescriptor;


  class Scheduler{
   public:
    struct itimerval timer;
    struct sigaction timerAction = {0};

    std::deque<threadDescriptor*> readyQueue;
    std::queue<threadDescriptor*> runningQueue;
    std::unordered_map<int, threadDescriptor*> blockedMap;
    std::unordered_map<int, std::deque<threadDescriptor*>> sleepingMap;


    Scheduler(): timer(), timerAction(){
      this -> timer.it_value.tv_sec = 0;
      this -> timer.it_value.tv_usec = 0;
      this -> timer.it_interval.tv_sec = 0;
      this -> timer.it_interval.tv_usec = 0;
    }

    void resetTimer(){
      int ret_val = setitimer(ITIMER_VIRTUAL, &timer, nullptr);
      if (ret_val == ERROR) {
        fprintf(stderr, "%s%s\n", SYS_ERROR_MSG, " failed to init timer");
        abortProgram();
        exit(1);
      }
      totalQuantums++;
    }

    void initializeTimer(int quantum_usecs){
      this -> timer.it_value.tv_usec = quantum_usecs; //TODO is that correct?
      resetTimer();
    }
  };

  // ==================== GLOBALS =====================
  Scheduler scheduler;
  std::set<int> tidPool;
  std::unordered_map<int, threadDescriptor*> allThreads;
  std::unordered_map<int, int> threadQuantumCount;
  std::unordered_map<int, int> sleepingIds; //maps tid to the quantum it should wake up at

  threadDescriptor* curThread = NULL;


  //================= HELPER FUNCTIONS ===============
  void sigALRM_handler(int){
    scheduler.readyQueue.push_back(allThreads[curThread -> tid]);
    allThreads[curThread -> tid] -> state = READY;
    scheduler.runningQueue.pop();
    switchThreads();
  }

  void wakeUpThreads(){
    if(scheduler.sleepingMap.count(totalQuantums + 1) == 0){
      return;
    }

    std::deque<threadDescriptor*> tmp = scheduler.sleepingMap[totalQuantums + 1];
    while(!tmp.empty()){
      threadDescriptor* curTd = tmp.front();
      tmp.pop_front();
      sleepingIds.erase(curTd->tid);
      if(curTd->state == SLEEP){
        curTd->state = READY;
        scheduler.readyQueue.push_back(curTd);
      }else{
        curTd->state = BLOCKED;
      }
    }
    scheduler.sleepingMap.erase(totalQuantums + 1);

  }

  void freeThread(threadDescriptor** toFree){
    free((*toFree)->stack);
    (*toFree)->stack = NULL;
    free(*toFree);
    (*toFree) = NULL;
  }

  void abortProgram()
  {
    for(auto pair : allThreads){
      threadDescriptor* cur_thread_ptr = pair.second;
      freeThread(&cur_thread_ptr);
    }
  }

  void setRunner(){
    threadDescriptor* currentThread = nullptr;
    if(!scheduler.readyQueue.empty()){
      do{
        currentThread = scheduler.readyQueue.front();
        scheduler.readyQueue.pop_front();
      } while(currentThread == NULL || currentThread->state == BLOCKED || currentThread->state == BLOCKED_SLEEP);

      allThreads[currentThread -> tid]->state = RUNNING;
      curThread = currentThread;
    }else{
      currentThread = curThread;
    }

    scheduler.runningQueue.push(currentThread);
    threadQuantumCount[currentThread->tid]++;
    scheduler.resetTimer();
  }

  void switchThreads(){
    blockSigVTAlarm();
    int ret_val = sigsetjmp(allThreads[curThread->tid]->env, 1);
    if (ret_val == JMP_VAL) {
      return;
    }

    setRunner();
    wakeUpThreads();

    unblockSigVTAlarm();
    siglongjmp(allThreads[curThread -> tid]->env, JMP_VAL);
  }

  void blockSigVTAlarm(){
    sigprocmask(SIG_BLOCK,&(scheduler.timerAction).sa_mask, NULL);
  }

  inline void unblockSigVTAlarm(){
    sigprocmask(SIG_UNBLOCK,&(scheduler.timerAction).sa_mask, NULL);
  }

  //================= FUNCTIONS ===============
  /**
   * @brief initializes the thread library.
   *
   * You may assume that this function is called before any other thread library function, and that it is called
   * exactly once.
   * The input to the function is the length of a quantum in micro-seconds.
   * It is an error to call this function with non-positive quantum_usecs.
   *
   * @return On success, return 0. On failure, return -1.
  */
  int uthread_init(int quantum_usecs){
    if(quantum_usecs <= 0){
      fprintf(stderr, "%s%s\n", FUNC_ERROR_MSG, " timer quantum time is not positive integer");
      return ERROR;
    }

    scheduler.initializeTimer(quantum_usecs);

    scheduler.timerAction.sa_handler = &sigALRM_handler;
    if(sigaction(SIGVTALRM, &(scheduler.timerAction), NULL) < 0){
      fprintf(stderr, "%s%s\n", SYS_ERROR_MSG, " failed to define sigaction to SIGVTALRM");
      exit(1);
    }

    sigemptyset(&(scheduler.timerAction).sa_mask);
    sigaddset(&(scheduler.timerAction).sa_mask, SIGVTALRM);

    int tid = nextTid++;

    threadDescriptor* td = (threadDescriptor*) malloc(sizeof(threadDescriptor));
    if(td == NULL){
      fprintf(stderr, "%s%s\n", SYS_ERROR_MSG, " failed to allocate threadDescriptor for new thread");
      exit (1);
    }
    td->tid = tid;
    td->state = RUNNING;
    td->stack = NULL;

    sigsetjmp(td->env,1);
    sigemptyset(&(td->env)->__saved_mask);

    scheduler.runningQueue.push(td);

    allThreads[tid] = td;
    threadQuantumCount[tid] = 1;
    ++threadAmount;
    curThread = td;
    return 0;
  }


  /**
   * @brief Creates a new thread, whose entry point is the function entry_point with the signature
   * void entry_point(void).
   *
   * The thread is added to the end of the READY threads list.
   * The uthread_spawn function should fail if it would cause the number of concurrent threads to exceed the
   * limit (MAX_THREAD_NUM).
   * Each thread should be allocated with a stack of size STACK_SIZE bytes.
   *
   * @return On success, return the ID of the created thread. On failure, return -1.
  */
  int uthread_spawn(thread_entry_point entry_point){
    blockSigVTAlarm();
    if(entry_point == NULL){
      fprintf(stderr, "%s%s\n", FUNC_ERROR_MSG, " entry point cannot be NULL");
      unblockSigVTAlarm();
      return ERROR;
    }
    if(threadAmount >= MAX_THREAD_NUM){
      fprintf(stderr, "%s%s\n", FUNC_ERROR_MSG, " MAX_THREAD_NUM exceeded");
      unblockSigVTAlarm();
      return ERROR;
    }

    char* stack = (char*) malloc(STACK_SIZE);
    if(stack == NULL){
      fprintf(stderr, "%s%s\n", SYS_ERROR_MSG, " failed to allocate stack for new thread");
      abortProgram();
      exit(1);
    }

    int tid;
    if(tidPool.empty()){
      tid = nextTid;
      ++nextTid;
    }else{
      tid = *(tidPool.begin());
      tidPool.erase(tidPool.begin());
    }

    address_t sp = (address_t) stack + STACK_SIZE - sizeof(address_t);
    address_t pc = (address_t) entry_point;

    threadDescriptor* td = (threadDescriptor*) malloc(sizeof(threadDescriptor));
    if(td == NULL){
      free(stack);
      fprintf(stderr, "%s%s\n", SYS_ERROR_MSG, " failed to allocate threadDescriptor for new thread");
      abortProgram();
      exit (1);
    }

    td->tid = tid;
    td->state = READY;
    td->stack = stack;

    sigsetjmp(td->env,1);
    ((td->env)->__jmpbuf)[JB_SP] = translate_address (sp);
    ((td->env)->__jmpbuf)[JB_PC] = translate_address(pc);
    sigemptyset(&(td->env)->__saved_mask);

    scheduler.readyQueue.push_back(td);

    allThreads[tid] = td;
    threadQuantumCount[tid] = 0;
    ++threadAmount;
    unblockSigVTAlarm();
    return tid;
  }


  /**
   * @brief Terminates the thread with ID tid and deletes it from all relevant control structures.
   *
   * All the resources allocated by the library for this thread should be released. If no thread with ID tid exists it
   * is considered an error. Terminating the main thread (tid == 0) will result in the termination of the entire
   * process using exit(0) (after releasing the assigned library memory).
   *
   * @return The function returns 0 if the thread was successfully terminated and -1 otherwise. If a thread terminates
   * itself or the main thread is terminated, the function does not return.
  */
  int uthread_terminate(int tid){
    blockSigVTAlarm();
    if(tid == 0){
      abortProgram();
      exit(0);
    }

    if(allThreads.count(tid) == 0){
      fprintf(stderr, "%s%s %d%s\n", FUNC_ERROR_MSG, " thread with tid", tid, " does not exists. ABORT TERMINATE!");
      unblockSigVTAlarm();
      return ERROR;
    }

    threadDescriptor* toKill = allThreads[tid];
    tidPool.insert(tid);
    --threadAmount;
    threadQuantumCount.erase(tid);
    if(tid == curThread->tid){
      scheduler.runningQueue.pop();
      switchThreads();
    }

    //taking out terminated from readyQueue
    auto pos1 = std::find(scheduler.readyQueue.begin(), scheduler.readyQueue.end(), toKill);
    if(pos1 != scheduler.readyQueue.end()){
      scheduler.readyQueue.erase(pos1);
    }

    //taking out terminated from sleepMap
    if(sleepingIds.count(toKill->tid)){
      auto sleepDequeToRemoveFrom = &scheduler.sleepingMap[sleepingIds[toKill->tid]];
      auto pos2 = std::find((*sleepDequeToRemoveFrom).begin(), (*sleepDequeToRemoveFrom).end(), toKill);
      if(pos2 != (*sleepDequeToRemoveFrom).end()){
        (*sleepDequeToRemoveFrom).erase(pos2);
      }
      if((*sleepDequeToRemoveFrom).empty()){
        scheduler.sleepingMap.erase(sleepingIds[toKill->tid]);
      }
      sleepingIds.erase(toKill->tid);
    }

    //taking out terminated from blockedMap
    if(scheduler.blockedMap.count(toKill->tid)){
      scheduler.blockedMap.erase(tid);
    }

    freeThread(&toKill);
    allThreads.erase(tid);
    unblockSigVTAlarm();
    return 0;
  }


  /**
   * @brief Blocks the thread with ID tid. The thread may be resumed later using uthread_resume.
   *
   * If no thread with ID tid exists it is considered as an error. In addition, it is an error to try blocking the
   * main thread (tid == 0). If a thread blocks itself, a scheduling decision should be made. Blocking a thread in
   * BLOCKED state has no effect and is not considered an error.
   *
   * @return On success, return 0. On failure, return -1.
  */
  int uthread_block(int tid){
    blockSigVTAlarm();
    if(tid == 0){
      fprintf(stderr, "%s%s\n", FUNC_ERROR_MSG, " main thread cannot be blocked!");
      unblockSigVTAlarm();
      return ERROR;
    }

    if(allThreads.count(tid) == 0){
      fprintf(stderr, "%s%s %d%s\n", FUNC_ERROR_MSG, " thread with tid", tid, " does not exists. ABORT BLOCK!");
      unblockSigVTAlarm();
      return ERROR;
    }

    threadDescriptor* toBlock = allThreads[tid];
    if(toBlock->state == SLEEP){
      toBlock->state = BLOCKED_SLEEP;
    }else{
      toBlock->state = BLOCKED;
    }
    scheduler.blockedMap[tid] = toBlock;
    if(curThread->tid == tid){
      scheduler.runningQueue.pop();
      switchThreads();
    }else{
      auto pos = std::find(scheduler.readyQueue.begin(), scheduler.readyQueue.end(), toBlock);
      if(pos != scheduler.readyQueue.end()){
        scheduler.readyQueue.erase(pos);
      }
    }
    unblockSigVTAlarm();
    return 0;
  }


  /**
   * @brief Resumes a blocked thread with ID tid and moves it to the READY state.
   *
   * Resuming a thread in a RUNNING or READY state has no effect and is not considered as an error. If no thread with
   * ID tid exists it is considered an error.
   *
   * @return On success, return 0. On failure, return -1.
  */
  int uthread_resume(int tid){
    blockSigVTAlarm();
    if(allThreads.count(tid) == 0){
      fprintf(stderr, "%s%s %d%s\n", FUNC_ERROR_MSG, " thread with tid", tid, " does not exists. ABORT BLOCK!");
      unblockSigVTAlarm();
      return ERROR;
    }

    threadDescriptor* toResume = allThreads[tid];
    if(toResume->state == BLOCKED){
      toResume->state = READY;
      scheduler.blockedMap.erase(tid);
      scheduler.readyQueue.push_front(toResume);
    }

    if(toResume->state == BLOCKED_SLEEP){
      toResume->state = SLEEP;
      scheduler.blockedMap.erase(tid);
    }
    unblockSigVTAlarm();
    return 0;
  }


  /**
   * @brief Blocks the RUNNING thread for num_quantums quantums.
   *
   * Immediately after the RUNNING thread transitions to the BLOCKED state a scheduling decision should be made.
   * After the sleeping time is over, the thread should go back to the end of the READY threads list.
   * The number of quantums refers to the number of times a new quantum starts, regardless of the reason. Specifically,
   * the quantum of the thread which has made the call to uthread_sleep isn’t counted.
   * It is considered an error if the main thread (tid==0) calls this function.
   *
   * @return On success, return 0. On failure, return -1.
  */
  int uthread_sleep(int num_quantums){
    blockSigVTAlarm();
    if(curThread->tid == 0){
      fprintf(stderr, "%s%s\n", FUNC_ERROR_MSG, " cant sent main thread (tid=0) to sleep");
      unblockSigVTAlarm();
      return ERROR;
    }

    int wakeUpTime = totalQuantums + 1 + num_quantums;
    curThread->state = SLEEP;
    scheduler.sleepingMap[wakeUpTime].push_back(curThread);
    scheduler.runningQueue.pop();
    sleepingIds[curThread->tid] = wakeUpTime;
    switchThreads();
    unblockSigVTAlarm();
    return 0;
  }


  /**
   * @brief Returns the thread ID of the calling thread.
   *
   * @return The ID of the calling thread.
  */
  int uthread_get_tid(){
    return curThread->tid;
  }


  /**
   * @brief Returns the total number of quantums since the library was initialized, including the current quantum.
   *
   * Right after the call to uthread_init, the value should be 1.
   * Each time a new quantum starts, regardless of the reason, this number should be increased by 1.
   *
   * @return The total number of quantums.
  */
  int uthread_get_total_quantums(){
    return totalQuantums;
  }


  /**
   * @brief Returns the number of quantums the thread with ID tid was in RUNNING state.
   *
   * On the first time a thread runs, the function should return 1. Every additional quantum that the thread starts should
   * increase this value by 1 (so if the thread with ID tid is in RUNNING state when this function is called, include
   * also the current quantum). If no thread with ID tid exists it is considered an error.
   *
   * @return On success, return the number of quantums of the thread with ID tid. On failure, return -1.
  */
  int uthread_get_quantums(int tid){
    blockSigVTAlarm();
    if(threadQuantumCount.count(tid) == 0){
      fprintf(stderr, "%s%s %d%s\n", FUNC_ERROR_MSG, " no thread with id ", tid, " was found");
      unblockSigVTAlarm();
      return ERROR;
    }
    unblockSigVTAlarm();
    return threadQuantumCount[tid];
  }                                                                                                                                                                                                                                                                                                                                                                                                                           README                                                                                              0000777 0620316 0000050 00000006254 14225771636 010626  0                                                                                                    ustar   ofekk                           stud                                                                                                                                                                                                                   eran.turgeman, ofekk
Eran Turgeman (208484147), Ofek Kaveh (208746602)
EX: 1

FILES:
uhreads.cpp - the main scheduler file with the ThreadDescryptor struct and the
Scheduler struct.

REMARKS:
None

ANSWERS:

Question1
If we wish to write a program that is able to run on every operating systems we should consider using
user level threads.
by using user-level-threads the programmer can provide his own scheduling algorithm (and determaine his own
progream flow).
also since the threads are unknown to the OS, it treats them all as one process and therefore migrating the program from
one system to another will not cause any problems.


Question2
the advanteges of using kernel-level threads over processes are:
1) switching between kernel-level-threads are mush faster that switching between processes
2) threads can share data, while processes need to activly send pass the data from one to another if they
wish to share something between them- and that takes time.

the disadvantages of kernerl-level-threads in compare to processes are:
1) there is no separation in parts of the data between threads (i.e. heap) and sometimes we want
complete separation
2) we wont have to save state in every switch (overhead)


Question3
the kill-pid leads to hardware interrupt fro mthe keyboard
1) the OS informs (using iterrupts) to a running process (the shell) that some key was pressed , untill the
wole command is written
2) when this command is running, since 'kill' is a system call and currently we are in user-mode, a trap
will be executed and we will move to kernel-mode
3) the OS wil lsend SIGTERM
4) the running process will "choose" (from predefined settings) wheather to ignore the signal or not
5) at the end we will return to user-mode


Question4
the difference between real time to virtual time is that real time is the time passing in real life,
unrealated to what is running.
virtual time it a time allocated per process, and continue running only when the process is running.
example for real-time: if we want to check the incoming traffic we get from the network within one hour,
no matter from which source- we will want to measure the time unrelated to the running process (since every
tab in the browser is a new process). in this case we can use real time.
example for virtual-time: if we want to run 3 different processes, but measure the time of
process one only. for this case we will use virtual time cause we dont wish to measue the overall time
passing in real life.

Question5 - Sigsetjmp
this function saves the state of a thread(pointers, registers in CPU) on which it called into a buffer
that will allow to reconstruce the thread state when we will resume its work.
since we ca nmask the signals, and sometimes we want to return to previous settings, the function has an options to
be asked to save those masked signals into the buffer as well.

Question5 - Siglongjmp
performs the jump to other thread that was previously saved with sigsetjmp.
the function has a second argument that defines the function return value. this allows us to diffecinciate
between two situations- when we just saved a thread data and when we jumped back to this thread.
                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    