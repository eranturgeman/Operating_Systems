#include <cstdio>
#include "MapReduceFramework.h"
#include "Barrier.h"
#include <pthread.h>
#include <atomic>
#include <algorithm>
#include <set>

// ============================= CONSTANTS =============================
#define SYSTEM_ERROR_MSG "system error: "

// ============================= TYPEDEF & STRUCTS =============================
typedef std::vector<IntermediatePair> IntermediateVector;
typedef struct JobContext JobContext;
typedef struct ThreadContext ThreadContext;
template<class K2> struct K2PointersComparator;

/**
 * struct containing all data and resources required for a single wirker thread.
 * an instance will be created for each new thread participating in the task.
 */
typedef struct ThreadContext{
    size_t threadId;
    IntermediateVec* threadInterVec = nullptr;
    int jobIndex;
    JobContext* jobContext;
} ThreadContext;


/**
 * struct containing all data, structures, and objects required for a certain job.
 * an instance of this kind will be created for each new job to be performed by the program.
 */
typedef struct JobContext{
    const MapReduceClient* curClient = nullptr;
    JobState jobState = {UNDEFINED_STAGE, 0};
    int threadsAmount = 0;
    size_t inputSize = 0;
    std::set<K2*, K2PointersComparator<K2>>* mapPhaseUniqueKeys{};

    // vectors
    std::vector<IntermediateVector*> intermediateVectorsCollection;
    std::vector<std::vector<IntermediatePair>*> shuffledVector;
    const InputVec* curInputVec = nullptr;
    OutputVec* curOutputVec = nullptr;

    // atomics
    std::atomic_int processedCounter {0};
    std::atomic_int oldValue {0};
    std::atomic_flag isWaiting { false };

    // mutexes
    pthread_mutex_t interVecInsertMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t emitMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t stateChangeMutex = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_t shuffledMutex = PTHREAD_MUTEX_INITIALIZER;

    // barriers
    Barrier* mapToShuffleBarrier = nullptr;
    Barrier* shuffleToReduceBarrier = nullptr;

    // arrays
    pthread_t* arrayOfThreads = nullptr;
    std::vector<ThreadContext*>* arrayOfThreadsContext;

} JobContext;

// ============================= FUNCTION DECLARATIONS =============================
void emit2 (K2* key, V2* value, void* context);
void emit3 (K3* key, V3* value, void* context);
JobHandle startMapReduceJob(const MapReduceClient& client,
                            const InputVec& inputVec,
                            OutputVec& outputVec,
                            int multiThreadLevel);
void waitForJob(JobHandle job);
void getJobState(JobHandle job, JobState* state);
void closeJobHandle(JobHandle job);
void * entryPoint(void * arg);
void mapPhase(ThreadContext* threadContext);
void reducePhase(ThreadContext* threadContext);
JobContext* initJobContext(const MapReduceClient& client,
                           const InputVec& inputVec,
                           OutputVec& outputVec,
                           int threadsAmount);

void errorExit (JobContext *p_context, const char *string);
void mutexLock (pthread_mutex_t *mutex, JobContext* jobContext, const char *errorMsg);
void mutexUnlock (pthread_mutex_t *mutex, JobContext* jobContext, const char *errorMsg);
void shufflePhase (JobContext *p_context);
bool mapPhasePairComparator(IntermediatePair firstPair, IntermediatePair secondPair);
void freeResources (JobContext *jobContext);

// ============================= COMPARATORS =============================
/**
 * comparator func for intermediate pairs (by key)
 * @param firstPair
 * @param secondPair
 * @return
 */
bool mapPhasePairComparator(IntermediatePair firstPair, IntermediatePair secondPair){
    return *(firstPair.first) < *(secondPair.first);
}

/**
 * comparator for K2 values. required for the way we implemented our unique key set
 * @tparam K2
 */
template<class K2> struct K2PointersComparator{
    bool operator()(K2* left, K2* right){
        return *left < *right;
    }
};

// ============================= MAIN FUNCS IMPLEMENTATION =============================
/**
 * starts a MapReduce process. this function is the first one to be called when initiating the program
 * @param client the job that has to be performed
 * @param inputVec inputs for the job
 * @param outputVec vector to which the job outputs have to be inserted to
 * @param multiThreadLevel amount of worker threads the user demand
 * @return initialized jobContext
 */
JobHandle startMapReduceJob(const MapReduceClient& client,
                            const InputVec& inputVec,
                            OutputVec& outputVec,
                            int multiThreadLevel){

    if(multiThreadLevel <= 0){
        fprintf(stderr, "%s %s\n",SYSTEM_ERROR_MSG, "number of threads has to be > 0");
        exit(1);
    }

    JobContext* currentJobContext = initJobContext(client, inputVec, outputVec, multiThreadLevel);

    // creating new threads
    for(int i = 0; i < multiThreadLevel; i++){
        int res = pthread_create(currentJobContext -> arrayOfThreads + i, nullptr, entryPoint, (*(currentJobContext -> arrayOfThreadsContext))[i]);
        if (res == -1){
            delete[] currentJobContext -> arrayOfThreads;
            delete currentJobContext -> arrayOfThreadsContext;
            delete currentJobContext -> mapToShuffleBarrier;
            delete currentJobContext -> shuffleToReduceBarrier;
            delete currentJobContext -> mapPhaseUniqueKeys;
            delete currentJobContext;
            fprintf(stderr, "%s %s %d\n",SYSTEM_ERROR_MSG, "pthread creation failed for thread number: ", i);
            exit(1);
        }
    }
    return currentJobContext;
}

/**
 * this function will be called during client-> map phase to insert processed input pair into intermediate vector.
 * this is the user responsibility to call this function in his implemented map phase!
 * @param key pair's key
 * @param value pair's value
 * @param context ThreadContext* that contains JobContext as well
 */
void emit2 (K2* key, V2* value, void* context){
    ThreadContext* tc = (ThreadContext*) context;
    mutexLock(&(tc->jobContext->stateChangeMutex), tc->jobContext, "emitMutex lock failed");

    tc->jobContext->mapPhaseUniqueKeys->insert(key);
    tc->threadInterVec->push_back(IntermediatePair(key, value));

    mutexUnlock(&(tc->jobContext->stateChangeMutex), tc->jobContext, "emitMutex unlock failed");
}

/**
 * this function will be called during client->reduce phase to insert processed input pair into the output vector.
 * this is the user responsibility to call this function in his implemented reduce phase!
 * @param key pair's key
 * @param value pair's value
 * @param context ThreadContext* that contains JobContext as well
 */
void emit3 (K3* key, V3* value, void* context){
    ThreadContext* tc = (ThreadContext*) context;
    mutexLock(&(tc->jobContext->stateChangeMutex), tc->jobContext, "emitMutex lock failed");

    tc->jobContext->curOutputVec->push_back(OutputPair (key, value));

    mutexUnlock(&(tc->jobContext->stateChangeMutex), tc->jobContext, "emitMutex unlock failed");
}


/**
 * updates the given state object in the percentage of processed inputs according to the current stage the job is at.
 * @param job jobContext for current running job
 * @param state object to be updated
 */
void getJobState(JobHandle job, JobState* state){
    auto* jobContext = (JobContext*) job;
    mutexLock(&(jobContext->stateChangeMutex), jobContext, "failed to lock state change mutex");

    state->stage = jobContext->jobState.stage;
    auto alreadyDone = (int)jobContext->processedCounter.load();
    state->percentage = ((float)alreadyDone / (float)(jobContext->inputSize)) * 100;

    mutexUnlock(&(jobContext->stateChangeMutex), jobContext, "failed to unlock state change mutex");
}


/**
 * this function waits untill the provided job is finished
 * @param job job to wait for
 */
void waitForJob(JobHandle job){
    JobContext* jobContext = (JobContext*)job;
    if(jobContext->isWaiting.test_and_set()){
        return;
    }else{
        for(int i = 0; i < jobContext->threadsAmount; ++i){
            pthread_join(jobContext->arrayOfThreads[i], nullptr);
        }
    }
}


/**
 * releases all job's recourses. this function should be called ONLY AFTER THE JOB IS FINISHED
 * @param job job to be released
 */
void closeJobHandle(JobHandle job){
    waitForJob(job);
    freeResources((JobContext*)job);
}

// ============================= STAGES IMPLEMENTATION =============================
/**
 * implementation of map stage of the program - includes creating a vector for each working thread that performed
 * some work, sort the vector, and blocking all threads untill all of them are done with this stage
 * @param threadContext context of the current thread performing a job
 */
void mapPhase(ThreadContext* threadContext){
    IntermediateVector* threadInterVector = threadContext->threadInterVec;
    JobContext* jobContext = threadContext->jobContext;

    mutexLock(&(jobContext->stateChangeMutex), jobContext, "state change mutex lock failed");
    if(jobContext->processedCounter == 0){
        //first iteration of mapPhase
        jobContext->jobState.stage = MAP_STAGE;
        jobContext->jobState.percentage = 0;
    }
    mutexUnlock(&(jobContext->stateChangeMutex), jobContext, "state change mutex unlock failed");
    while(true){
        threadContext -> jobIndex = (int) jobContext -> oldValue++;
        if (threadContext -> jobIndex >= jobContext->inputSize)
        {
            break;
        }
        if(jobContext -> processedCounter.load() >= jobContext->inputSize){
            break;
        }
        const InputPair curInput = (*jobContext->curInputVec)[threadContext -> jobIndex];

        jobContext->curClient->map(curInput.first, curInput.second, threadContext);
        jobContext -> processedCounter++;
    }

    if(!threadInterVector->empty()){
        std::sort(threadInterVector->begin(),
                  threadInterVector->end(),
                  mapPhasePairComparator);
    }

    mutexLock(&(jobContext->interVecInsertMutex), jobContext, "interVecInsertMutex lock failed");
    if(!threadInterVector->empty()){
        jobContext->intermediateVectorsCollection.push_back(threadInterVector);
    }
    mutexUnlock(&(jobContext->interVecInsertMutex), jobContext, "interVecInsertMutex unlock failed");

    jobContext->mapToShuffleBarrier->barrier();
}


/**
 * this stage is dedicated for THREAD 0 ONLY.
 * after all threads are done with MAP stage- all their initially processed vectors are combined into a single,
 * sorted (reversed) vector of vectors. each inner vectors contains pairs with the SAME key VALUE
 * @param jobContext job to perform the action on
 */
void shufflePhase (JobContext *jobContext){
  // moving to SHUFFLE stage
  mutexLock(&(jobContext->stateChangeMutex), jobContext, "stateChangeMutex lock failed");

  jobContext->processedCounter = 0;
  jobContext->inputSize = 0;
  for(auto curVec: jobContext->intermediateVectorsCollection){
    jobContext->inputSize += curVec->size();
  }

  jobContext->jobState = {SHUFFLE_STAGE, 0};
  mutexUnlock(&(jobContext->stateChangeMutex), jobContext, "stateChangeMutex unlock failed");

  if(jobContext->mapPhaseUniqueKeys->empty()){
    return;
  }

  for(auto biggestKeyIter = jobContext->mapPhaseUniqueKeys->rbegin(); biggestKeyIter !=
      jobContext->mapPhaseUniqueKeys->rend(); ++biggestKeyIter){

    auto keyVal = *biggestKeyIter;

    auto* curVec = new (std::nothrow) std::vector<IntermediatePair>();
    if(curVec == nullptr){
      errorExit(jobContext, "inner shuffled vector allocation failed");
    }

    for(auto* threadVec: jobContext->intermediateVectorsCollection){
      while(!threadVec->empty()){
        auto pair = threadVec->back();
        if(!(*(pair.first) < *keyVal) && !(*keyVal < *(pair.first))){
          curVec->push_back(pair);
          jobContext->processedCounter++;
          threadVec->pop_back();
        }else {
          break;
        }
      }
    }
    jobContext -> shuffledVector.push_back(curVec);
  }

  // moving to REDUCE stage
  mutexLock(&(jobContext->stateChangeMutex), jobContext, "stateChangeMutex lock failed");

  jobContext->processedCounter = 0;
  jobContext->oldValue = 0;
  jobContext->jobState = {REDUCE_STAGE, 0};

  mutexUnlock(&(jobContext->stateChangeMutex), jobContext, "stateChangeMutex unlock failed");
}


/**
 * this stage is taking place ONLY AFTER SHUFFLE STAGE IS DONE.
 * the inner vectors in the shuffled vectors are being processed by the worker threads, and each running on each
 * vector the client->reduce function provided by the client
 * @param threadContext context of worker thread
 */
void reducePhase(ThreadContext* threadContext){
  JobContext* jobContext = threadContext->jobContext;

  while(true){
      threadContext -> jobIndex = (int) jobContext -> oldValue++;
      if (threadContext -> jobIndex >= jobContext->shuffledVector.size()){
          break;
      }
      if(jobContext -> processedCounter.load() >= jobContext->inputSize){
          break;
      }
      const IntermediateVector* curVecToProcess = jobContext->shuffledVector[threadContext -> jobIndex];

      jobContext->curClient->reduce(curVecToProcess, threadContext);
      size_t amountProcessed = curVecToProcess->size();
      jobContext->processedCounter.fetch_add((int)amountProcessed);
  }
}

// ============================= HELPER FUNCS IMPLEMENTATION =============================
/**
 * initializes all memory allocations and assignments. helper for StartMapReduceJob
 * @param client client provided to the program
 * @param inputVec input vector
 * @param outputVec output vector
 * @param threadsAmount amount of worker threads
 * @return partially initialized job context
 */
JobContext* initJobContext(const MapReduceClient& client,
                           const InputVec& inputVec,
                           OutputVec& outputVec,
                           int threadsAmount){
    //allocating JobContext
    JobContext* currentJobContext;
    try {
        currentJobContext = new JobContext();
    }catch (const std::bad_alloc& e){
        fprintf(stderr, "%s %s\n", SYSTEM_ERROR_MSG, "JobContext allocation failed");
        exit(1);
    }

    //initialize given fields
    currentJobContext -> curClient = &client;
    currentJobContext -> curInputVec = &inputVec;
    currentJobContext -> curOutputVec = &outputVec;
    currentJobContext -> threadsAmount = threadsAmount;
    currentJobContext-> inputSize = inputVec.size();
    currentJobContext -> jobState = {UNDEFINED_STAGE, 0};

    //allocating unique keys set
    try{
        currentJobContext->mapPhaseUniqueKeys = new std::set<K2*, K2PointersComparator<K2>>();
    }catch(const std::bad_alloc& e){
      pthread_mutex_destroy(&(currentJobContext->stateChangeMutex));
      pthread_mutex_destroy(&(currentJobContext->emitMutex));
      pthread_mutex_destroy(&(currentJobContext->interVecInsertMutex));
      pthread_mutex_destroy(&(currentJobContext->shuffledMutex));
      delete currentJobContext;
      fprintf(stderr, "%s %s\n", SYSTEM_ERROR_MSG, "set of unique keys allocation failed");
      exit(1);
    }

    // allocating arrays for threads \ threads contexts
    try{
        currentJobContext -> arrayOfThreads = new pthread_t[threadsAmount];
    }catch (const std::bad_alloc& e){
        pthread_mutex_destroy(&(currentJobContext->stateChangeMutex));
        pthread_mutex_destroy(&(currentJobContext->emitMutex));
        pthread_mutex_destroy(&(currentJobContext->interVecInsertMutex));
        pthread_mutex_destroy(&(currentJobContext->shuffledMutex));
        delete currentJobContext->mapPhaseUniqueKeys;
        delete currentJobContext;
        fprintf(stderr, "%s %s\n", SYSTEM_ERROR_MSG, "array of threads allocation failed");
        exit(1);
    }

    try{
        currentJobContext -> arrayOfThreadsContext = new std::vector<ThreadContext*>();
    }catch (const std::bad_alloc& e){
        pthread_mutex_destroy(&(currentJobContext->stateChangeMutex));
        pthread_mutex_destroy(&(currentJobContext->emitMutex));
        pthread_mutex_destroy(&(currentJobContext->interVecInsertMutex));
        pthread_mutex_destroy(&(currentJobContext->shuffledMutex));
        delete currentJobContext->mapPhaseUniqueKeys;
        delete[] currentJobContext -> arrayOfThreads;
        delete currentJobContext;
        fprintf(stderr, "%s %s\n", SYSTEM_ERROR_MSG, "array of threads contexts allocation failed");
        exit(1);
    }

    // creating barrier
    try{
        currentJobContext -> mapToShuffleBarrier = new Barrier(threadsAmount);
    }catch (const std::bad_alloc& e){
        pthread_mutex_destroy(&(currentJobContext->stateChangeMutex));
        pthread_mutex_destroy(&(currentJobContext->emitMutex));
        pthread_mutex_destroy(&(currentJobContext->interVecInsertMutex));
        pthread_mutex_destroy(&(currentJobContext->shuffledMutex));
        delete currentJobContext->mapPhaseUniqueKeys;
        delete currentJobContext -> arrayOfThreadsContext;
        delete[] currentJobContext -> arrayOfThreads;
        delete currentJobContext;
        fprintf(stderr, "%s %s\n", SYSTEM_ERROR_MSG, "map to shuffle barrier allocation failed");
        exit(1);
    }

    try{
        currentJobContext -> shuffleToReduceBarrier = new Barrier(threadsAmount);
    }catch (const std::bad_alloc& e){
        pthread_mutex_destroy(&(currentJobContext->stateChangeMutex));
        pthread_mutex_destroy(&(currentJobContext->emitMutex));
        pthread_mutex_destroy(&(currentJobContext->interVecInsertMutex));
        pthread_mutex_destroy(&(currentJobContext->shuffledMutex));
        delete currentJobContext->mapPhaseUniqueKeys;
        delete currentJobContext -> arrayOfThreadsContext;
        delete[] currentJobContext -> arrayOfThreads;
        delete currentJobContext -> mapToShuffleBarrier;
        delete currentJobContext;
        fprintf(stderr, "%s %s\n", SYSTEM_ERROR_MSG, "shuffle to reduce barrier allocation failed");
        exit(1);
    }

    // allocating thread contexts for all threads to be
    for(int i = 0; i < threadsAmount; ++i){
        try{
            auto* tc = new ThreadContext();
            tc->threadInterVec = new IntermediateVec();
            tc->threadId = i;
            tc ->jobContext = currentJobContext;
            currentJobContext->arrayOfThreadsContext->push_back(tc);
        }catch(const std::bad_alloc& e){
            for(auto tc: *(currentJobContext->arrayOfThreadsContext)){
                delete tc->threadInterVec;
                delete tc;
                tc = nullptr;
            }
            pthread_mutex_destroy(&(currentJobContext->stateChangeMutex));
            pthread_mutex_destroy(&(currentJobContext->emitMutex));
            pthread_mutex_destroy(&(currentJobContext->interVecInsertMutex));
            pthread_mutex_destroy(&(currentJobContext->shuffledMutex));
            delete currentJobContext->mapPhaseUniqueKeys;
            delete currentJobContext -> arrayOfThreads;
            delete[] currentJobContext -> arrayOfThreadsContext;
            delete currentJobContext -> mapToShuffleBarrier;
            delete currentJobContext -> shuffleToReduceBarrier;
            delete currentJobContext;
            fprintf(stderr, "%s %s\n", SYSTEM_ERROR_MSG, "thread context/thread vector allocation failed");
            exit(1);
        }
    }
    return currentJobContext;
}


/**
 * aggregates all program stages in their logical order
 * @param arg thread context
 * @return nullptr
 */
void * entryPoint(void * arg){
  ThreadContext* threadContext = (ThreadContext*) arg;

  mapPhase(threadContext);

  if(threadContext->threadId == 0){
    shufflePhase(threadContext->jobContext);
  }
  threadContext->jobContext->shuffleToReduceBarrier->barrier();
  reducePhase(threadContext);

  return nullptr;
}


/**
 * error handler function. prints an error message to stderr and releases all program resources.
 * @param jobContext the job to be freed
 * @param errorMsg error message (string)
 */
void errorExit (JobContext *jobContext, const char *errorMsg){
    fprintf(stderr, "%s %s\n", SYSTEM_ERROR_MSG, errorMsg);
    exit(1);
}


/**
 * releases all resources in the program
 * @param jobContext the job to be freed
 */
void freeResources (JobContext *jobContext){
    if(jobContext->arrayOfThreadsContext != nullptr){
        for(auto tc: *(jobContext->arrayOfThreadsContext)){
            delete tc->threadInterVec;
            tc->threadInterVec = nullptr;
            delete tc;
            tc = nullptr;
        }
        delete jobContext -> arrayOfThreadsContext;
    }

    delete[] jobContext -> arrayOfThreads;

    if(!(jobContext->shuffledVector.empty())){
        for(auto* innerVecPointer: jobContext->shuffledVector){
            delete innerVecPointer;
            innerVecPointer = nullptr;
        }
    }
    pthread_mutex_destroy(&(jobContext->stateChangeMutex));
    pthread_mutex_destroy(&(jobContext->emitMutex));
    pthread_mutex_destroy(&(jobContext->interVecInsertMutex));
    pthread_mutex_destroy(&(jobContext->shuffledMutex));
    delete jobContext->mapPhaseUniqueKeys;
    delete jobContext->mapToShuffleBarrier;
    delete jobContext -> shuffleToReduceBarrier;
    delete jobContext;
}


/**
 * locks a provided mutex, with a check if the lock was successful.
 * @param mutex mutex to lock
 * @param jobContext the job's context in case resources needs to be freed
 * @param errorMsg error message
 */
void mutexLock (pthread_mutex_t *mutex, JobContext* jobContext, const char *errorMsg){
    if(pthread_mutex_lock(mutex) != 0){
        errorExit(jobContext, errorMsg);
    }
}


/**
 * unlocks a provided mutex, with a check if the lock was successful.
 * @param mutex mutex to unlock
 * @param jobContext the job's context in case resources needs to be freed
 * @param errorMsg error message
 */
void mutexUnlock (pthread_mutex_t *mutex, JobContext* jobContext, const char *errorMsg){
    if(pthread_mutex_unlock(mutex) != 0){
        errorExit(jobContext, errorMsg);
    }
}
