// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include "ofxsSupportPrivate.h"
#include "ofxsLog.h"
#include <atomic>
#include <new> // bad_alloc
#include <vector>
#ifdef OFX_USE_MULTITHREAD_MUTEX
#include <condition_variable>
#include <mutex>
#include <thread>
#endif

namespace OFX {

  namespace MultiThread {

    ////////////////////////////////////////////////////////////////////////////////
    // SMP class

    /** @brief ctor */
    Processor::Processor(void)
    {
    }

    /** @brief dtor */
    Processor::~Processor()
    {
    }

    namespace {
      /** @brief argument passed through the multi thread suite to staticMultiThreadFunction */
      struct ThreadArgs {
        Processor *processor;
        // one slot per thread, so that no synchronization is needed
        std::vector<OfxStatus> status;
      };
    }

    /** @brief Function to pass to the multi thread suite

     This is called from host threads through a C function pointer, so no
     exception may escape from it: exceptions are caught here, recorded per
     thread, and rethrown from multiThread() in the calling thread.
     */
    void Processor::staticMultiThreadFunction(unsigned int threadIndex, unsigned int threadMax, void *customArg)
    {
      ThreadArgs *args = (ThreadArgs *) customArg;
      OfxStatus stat = kOfxStatOK;
      try {
        // and call my thread function
        args->processor->multiThreadFunction(threadIndex, threadMax);
      }
      // same mapping as in OFX::Private::mainEntryStr()
      catch (const OFX::Exception::Suite &ex) {
        stat = ex.status();
      }
      catch (const OFX::Exception::HostInadequate &) {
        stat = kOfxStatErrMissingHostFeature;
      }
      catch (const OFX::Exception::PropertyUnknownToHost &) {
        stat = kOfxStatErrMissingHostFeature;
      }
      catch (const std::bad_alloc&) {
        stat = kOfxStatErrMemory;
      }
      catch (...) {
        stat = kOfxStatFailed;
      }
      if (stat != kOfxStatOK) {
        if (threadIndex < args->status.size()) {
          args->status[threadIndex] = stat;
        } else {
          // the host launched more threads than requested, should not happen
          OFX::Log::error(true, "multiThread: thread index %u out of range [0..%u)", threadIndex, (unsigned)args->status.size());
        }
      }
    }

    /** @brief Function to pass to the multi thread suite */
    void Processor::multiThread(unsigned int nCPUs)
    {
      // if 0, use all the CPUs we can
      if(nCPUs == 0)
        nCPUs = OFX::MultiThread::getNumCPUs();

      // Run in the calling thread with 1 CPU, or when called from a thread spawned by
      // multiThread (e.g. from the multiThreadFunction() of another Processor): the spec
      // (ofxMultiThread.h) says "This function cannot be called recursively." and returns
      // "::kOfxStatErrExists, failed in an attempt to call multiThread recursively".
      // multiThreadIsSpawnedThread returns "1 if the thread was spawned by multiThread".
      if(nCPUs == 1 || OFX::MultiThread::isSpawnedThread()) {
        multiThreadFunction(0, 1);
      }
      else {
        // OK do it
          OfxStatus stat = kOfxStatFailed;
          ThreadArgs args;
          args.processor = this;
          args.status.resize(nCPUs, kOfxStatOK);
          if(OFX::Private::gThreadSuite && OFX::Private::gThreadSuite->multiThread){
              stat = OFX::Private::gThreadSuite->multiThread(staticMultiThreadFunction, nCPUs, (void *)&args);
          } else {
              // a host without the thread suite or its multiThread function (getNumCPUs() is then
              // 1 without multiThreadNumCPUs): run single-threaded rather than fail or crash
              static std::atomic<bool> logged(false);
              OFX::Log::warning(!logged.exchange(true), "multiThread: the host has no multiThread function: running single-threaded");
              multiThreadFunction(0, 1);
              return;
          }

        // did we do it?
        throwSuiteStatusException(stat);

        // rethrow the first failure from the threads, if any
        for (std::size_t i = 0; i < args.status.size(); ++i) {
          if (args.status[i] != kOfxStatOK) {
            throwSuiteStatusException(args.status[i]);
            // kOfxStatReply* statuses are not errors for throwSuiteStatusException()
          }
        }
      }
    }

    ////////////////////////////////////////////////////////////////////////////////
    // futility functions

    /** @brief Has the current thread been spawned from an MP */
    bool isSpawnedThread(void)
    {
      if(OFX::Private::gThreadSuite && OFX::Private::gThreadSuite->multiThreadIsSpawnedThread){
          int v = OFX::Private::gThreadSuite->multiThreadIsSpawnedThread();
          return v != 0;
      }else{
          return false;
      }
    }

    /** @brief The number of CPUs that can be used for MP-ing */
    unsigned  int getNumCPUs(void)
    {
      unsigned int n = 1;
      OfxStatus stat = (OFX::Private::gThreadSuite && OFX::Private::gThreadSuite->multiThreadNumCPUs) ?
                       OFX::Private::gThreadSuite->multiThreadNumCPUs(&n) : kOfxStatFailed;

      if(stat != kOfxStatOK) n = 1;
      return n;
    }
      
    /** @brief The index of the current thread. From 0 to numCPUs() - 1 */
    unsigned int getThreadIndex(void)
    {
      unsigned int n = 0;
      OfxStatus stat = (OFX::Private::gThreadSuite && OFX::Private::gThreadSuite->multiThreadIndex) ?
                       OFX::Private::gThreadSuite->multiThreadIndex(&n) : kOfxStatFailed;
      if(stat != kOfxStatOK) n = 0;
      return n;
    }

    /** @brief Utility function to compute the subrange of a given thread */
    void getThreadRange(unsigned int threadID, unsigned int nThreads, int ibegin, int iend, int* ibegin_range, int* iend_range)
    {
      unsigned int di = iend - ibegin;
      // the following is equivalent to std::ceil(di/(double)nThreads); but doesn't require <cmath>
      unsigned int r = (di + nThreads - 1) / nThreads;

      if (r == 0) {
        // there are more threads than lines to process
        r = 1;
      }
      if ((int)threadID * r >= di) {
        // empty range
        *ibegin_range = *iend_range = iend;
        return;
      }
      *ibegin_range = ibegin + threadID * r;
      unsigned int step = (threadID + 1) * r;
      *iend_range = ibegin + (step < di ? step : di);
    }

#ifdef OFX_USE_MULTITHREAD_MUTEX
    ////////////////////////////////////////////////////////////////////////////////
    // MUTEX class

    // the host's mutex functions, if it has them all: Sony Catalyst Edit's thread suite has none
    // (openfx-misc's README-hosts.txt)
    static bool hostHasMutexFunctions()
    {
      const OfxMultiThreadSuiteV1* t = OFX::Private::gThreadSuite;
      return t && t->mutexCreate && t->mutexDestroy && t->mutexLock && t->mutexUnLock && t->mutexTryLock;
    }

    /** @brief the mutex used without the host's mutex functions: a lock count, as the OFX mutex
        (ofxMultiThread.h: "A successful lock causes the mutex's lock count to be increased by one
        and to block any other calls to lock the mutex until it is unlocked"; the initial count
        "can be negative"). The mutex is free while the count is <= 0, and else held by the thread
        that took the last lock that made it positive (the creating thread for a positive initial
        count). Locks of the holding thread nest. Destroying it while locked is harmless. */
    struct Mutex::Fallback {
      std::mutex m;
      std::condition_variable cv;
      int count;
      std::thread::id owner;

      explicit Fallback(int lockCount) : count(lockCount)
      {
        if (count > 0) {
          owner = std::this_thread::get_id();
        }
      }
      bool available(std::thread::id me) const { return count <= 0 || owner == me; }
      void take(std::thread::id me)
      {
        if (++count > 0) {
          owner = me;
        }
      }
      void lock()
      {
        const std::thread::id me = std::this_thread::get_id();
        std::unique_lock<std::mutex> l(m);
        cv.wait(l, [&] { return available(me); });
        take(me);
      }
      bool tryLock()
      {
        const std::thread::id me = std::this_thread::get_id();
        std::lock_guard<std::mutex> l(m);
        if (!available(me)) {
          return false;
        }
        take(me);
        return true;
      }
      void unlock()
      {
        std::lock_guard<std::mutex> l(m);
        if (--count <= 0) {
          owner = std::thread::id();
          cv.notify_all();
        }
      }
    };

    /** @brief ctor */
    Mutex::Mutex(int lockCount)
      : _handle(NULL)
    {
      if (!hostHasMutexFunctions()) {
        // (also a static Mutex, constructed before the suites are fetched: SupportExt's g_glLoadOnceMutex)
        static std::atomic<bool> logged(false);
        OFX::Log::warning(!logged.exchange(true), "Mutex: no thread suite yet, or the host has no mutex functions: "
                          "using Support's own mutex");
        _fallback.reset(new Fallback(lockCount));
        return;
      }
      OfxStatus stat = OFX::Private::gThreadSuite->mutexCreate(&_handle, lockCount);
      throwSuiteStatusException(stat);
    }

    /** @brief dtor */
    Mutex::~Mutex(void)
    {
      if (_fallback) {
        return;
      }
      if (OFX::Private::gThreadSuite && OFX::Private::gThreadSuite->mutexDestroy) {
        OfxStatus stat = OFX::Private::gThreadSuite->mutexDestroy(_handle);
        (void)stat;
      }
    }

    /** @brief lock it, blocks until lock is gained */
    void Mutex::lock()
    {
      if (_fallback) {
        _fallback->lock();
        return;
      }
      OfxStatus stat = (OFX::Private::gThreadSuite && OFX::Private::gThreadSuite->mutexLock) ?
                       OFX::Private::gThreadSuite->mutexLock(_handle) : kOfxStatReplyDefault;
      throwSuiteStatusException(stat);
    }

    /** @brief unlock it */
    void Mutex::unlock()
    {
      if (_fallback) {
        _fallback->unlock();
        return;
      }
      OfxStatus stat = (OFX::Private::gThreadSuite && OFX::Private::gThreadSuite->mutexUnLock) ?
                       OFX::Private::gThreadSuite->mutexUnLock(_handle) : kOfxStatReplyDefault;
      // log it too: ~AutoMutexT() can't let the exception out
      OFX::Log::error(stat != kOfxStatOK && stat != kOfxStatReplyDefault, "Mutex::unlock: mutexUnLock failed (%s).", OFX::mapStatusToString(stat));
      throwSuiteStatusException(stat);
    }

    /** @brief attempt to lock, non-blocking */
    bool Mutex::tryLock()
    {
      if (_fallback) {
        return _fallback->tryLock();
      }
      OfxStatus stat = (OFX::Private::gThreadSuite && OFX::Private::gThreadSuite->mutexTryLock) ?
                       OFX::Private::gThreadSuite->mutexTryLock(_handle) : kOfxStatReplyDefault;
      return stat == kOfxStatOK;
    }
#endif

  }
}
