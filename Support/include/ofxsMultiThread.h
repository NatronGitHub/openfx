#ifndef _ofxsMultiThread_H_
#define _ofxsMultiThread_H_
// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file This file contains core code that wraps OFX 'objects' with C++ classes.

This file only holds code that is visible to a plugin implementation, and so hides much
of the direct OFX objects and any library side only functions.
*/

#include "ofxsCore.h"

typedef struct OfxMutex* OfxMutexHandle;

namespace OFX {

  /** @brief Multi thread namespace */
  namespace MultiThread {

    /** @brief Class that wraps up SMP multi-processing */
    class Processor {
    private :
      /** @brief Function to pass to the multi thread suite */
      static void staticMultiThreadFunction(unsigned int threadIndex, unsigned int threadMax, void *customArg);

    public  :
      /** @brief ctor */
      Processor();

      /** @brief dtor */
      virtual ~Processor();            

      /** @brief function that will be called in each thread. ID is from 0..nThreads-1 nThreads are the number of threads it is being run over */
      virtual void multiThreadFunction(unsigned int threadID, unsigned int nThreads) = 0;

      /** @brief call this to kick off multi threading

      The nCPUs is 0, the maximum allowable number of CPUs will be used.

      Called from a thread spawned by multiThread (e.g. from the multiThreadFunction() of
      another Processor), it runs multiThreadFunction(0, 1) in the calling thread, because
      the spec forbids recursive multiThread calls. getThreadIndex() then still returns the
      outer thread's index, so don't use it to index per-thread data of the inner Processor.
      */
      virtual void multiThread(unsigned int nCPUs = 0);
    };

    /** @brief Has the current thread been spawned from an MP */
    bool isSpawnedThread(void);

    /** @brief The number of CPUs that can be used for MP-ing */
    unsigned int getNumCPUs(void);

    /** @brief The index of the current thread. From 0 to numCPUs() - 1 */
    unsigned int getThreadIndex(void);

    /** @brief Utility function to compute the subrange of a given thread */
    void getThreadRange(unsigned int threadID, unsigned int nThreads, int ibegin, int iend, int* ibegin_range, int* iend_range);

#ifdef OFX_USE_MULTITHREAD_MUTEX
    /** @brief An OFX mutex */
    class Mutex {
    protected :
      OfxMutexHandle _handle; /**< @brief The handle */

    private :
      // not copyable: the destructor destroys the mutex (C.21, R.1)
      Mutex(const Mutex &); // not defined: not copyable
      Mutex &operator=(const Mutex &); // not defined: not copyable

    public :
      /** @brief ctor */
      Mutex(int lockCount = 0);

      /** @brief dtor */
      virtual ~Mutex(void);

      /** @brief lock it, blocks until lock is gained */
      void lock();

      /** @brief unlock it */
      void unlock();

      /** @brief attempt to lock, non-blocking

      \brief returns
      - true if the lock was achieved
      - false if it could not
      */
      bool tryLock();
    };
#endif

    /// a class to wrap around a mutex which is exception safe
    /// it locks the mutex on construction and unlocks it on destruction
    template<class M>
    class AutoMutexT {
    private :
      M* _mutex;
      bool val;

      // not copyable: each copy would unlock the mutex (C.21, R.1)
      AutoMutexT(const AutoMutexT &); // not defined: not copyable
      AutoMutexT &operator=(const AutoMutexT &); // not defined: not copyable
    public :
      /// ctor, acquires the lock
      explicit AutoMutexT(M &m)
        : _mutex(&m)
      {
        if (_mutex) {
          _mutex->lock();
        }
        val = true;
      }

      /// ctor, acquires the lock
      explicit AutoMutexT(M* m)
        : _mutex(m)
      {
        if (_mutex) {
          _mutex->lock();
        }
        val = true;
      }

      /// dtor, releases the lock. It doesn't throw: an exception that leaves a destructor calls
      /// std::terminate() (destructors are noexcept). Mutex::unlock() logs its failures.
      ~AutoMutexT()
      {
        try {
          unlock();
        } catch (...) {
          // the unlock failed: nothing more can be done here
        }
      }

      void unlock()
      {
        if (val) {
          // cleared first: if unlock() throws, the destructor doesn't try again
          val = false;
          if (_mutex) {
            _mutex->unlock();
          }
        }
      }


      void relock()
      {
        if (!val) {
          if (_mutex) {
            _mutex->lock();
          }
          val = true;
        }
      }
    };
#ifdef OFX_USE_MULTITHREAD_MUTEX
    typedef AutoMutexT<Mutex> AutoMutex;
#endif
  };
};

#endif
