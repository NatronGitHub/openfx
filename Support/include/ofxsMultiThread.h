#ifndef _ofxsMultiThread_H_
#define _ofxsMultiThread_H_
// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file This file contains core code that wraps OFX 'objects' with C++ classes.

This file only holds code that is visible to a plugin implementation, and so hides much
of the direct OFX objects and any library side only functions.
*/

#include "ofxsCore.h"
#include <atomic> // ThreadScope
#include <system_error> // ThreadScope
#ifndef _WIN32
#include <cerrno> // ThreadScope
#include <new> // ThreadScope
#include <pthread.h> // ThreadScope
#endif
#ifdef OFX_USE_MULTITHREAD_MUTEX
#include <memory> // unique_ptr (Mutex)
#endif

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
      /** @brief used instead of the host's mutex when the host has no mutex functions (ofxsMultiThread.cpp) */
      struct Fallback;
      std::unique_ptr<Fallback> _fallback;

    private :
      // not copyable: the destructor destroys the mutex (C.21, R.1)
      Mutex(const Mutex &) = delete;
      Mutex &operator=(const Mutex &) = delete;

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
      AutoMutexT(const AutoMutexT &) = delete;
      AutoMutexT &operator=(const AutoMutexT &) = delete;
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

/// OFX::MultiThread::ThreadScope exists
#define OFXS_SUPPORT_HAS_THREAD_SCOPE 1

// the visibility of ThreadScope's members on ELF and Mach-O (MinGW and Cygwin warn on it; Windows
// uses thread_local)
#if defined(__GNUC__) && !defined(_WIN32) && !defined(__CYGWIN__)
#define OFXS_THREAD_SCOPE_HIDDEN __attribute__((visibility("hidden")))
#else
#define OFXS_THREAD_SCOPE_HIDDEN
#endif

    /** @brief A per-thread value for the duration of a scope, without thread-local variables
        on POSIX: macOS never unloads a binary that has them (dyld), so plug-ins should not use
        thread_local (#385).

    A ThreadScope<T> object makes value the calling thread's ThreadScope<T>::current() until it is
    destroyed, which restores the previous one: scopes nest on each thread, also when an exception
    leaves them. Each type T is an independent kind: make T a type dedicated to the kind, usually
    the class that holds the ThreadScope (as below): e.g. a CImgAbortScope that holds the effect
    and a ThreadScope<CImgAbortScope>, not a ThreadScope<OFX::ImageEffect>, which another use in
    the binary could share. The value is owned by the caller, usually that object. Outside any scope, current() is nullptr. Construct and destroy a ThreadScope on the
    same thread, as a local variable or a member of one.

    For example, the render time of the render action being processed by this thread:
    @code
    class RenderTimeScope {
    public:
      explicit RenderTimeScope(double t) : _time(t), _scope(this) {}
      static double current() {
        const RenderTimeScope* s = OFX::MultiThread::ThreadScope<RenderTimeScope>::current();
        return s ? s->_time : 0.;
      }
    private:
      double _time;
      OFX::MultiThread::ThreadScope<RenderTimeScope> _scope; // last: pushed when _time is set
    };
    @endcode

    On POSIX (Linux, macOS) it uses a pthread key per T, created at the first scope (the
    constructor throws std::bad_alloc if memory is short, std::system_error for other failures,
    and the next scope tries again) and deleted when the binary is unloaded. The keys are a
    per-process resource shared with the host and the other plug-ins (PTHREAD_KEYS_MAX, 512 on
    macOS): use a few kinds, not one per object. At exit, the static destructors run while host
    threads may still be in an action: the key of a kind is then deleted only if no scope of
    that kind is live, and otherwise left to the process's end, so a scope that ends later is
    harmless. A scope that begins after the binary started unloading, or after exit, is not
    supported, including the next scope of an action already in flight (it may throw, or see
    nullptr). On Windows, which unloads DLLs that have thread-local variables, it uses a
    thread_local pointer. No allocation per scope, except the thread's storage for the key the
    first time a thread uses it (pthread_setspecific).

    There is one key per kind and per plug-in binary, whatever the visibility of T and of the
    build: the members of ThreadScope are hidden on ELF and Mach-O (with default visibility, GCC on
    Linux would make its statics STB_GNU_UNIQUE: shared by all the binaries that use the same T,
    which glibc then never unloads). The class itself keeps the default visibility, so that a T
    with default visibility can hold one without a GCC warning.
    */
    template <class T>
    class ThreadScope {
    public:
      /// makes value the current one of the calling thread; throws std::bad_alloc or
      /// std::system_error if the key or the thread's storage can't be created (the current value
      /// is unchanged then)
      OFXS_THREAD_SCOPE_HIDDEN explicit ThreadScope(T* value)
        : _previous(push(value))
      {
      }

      /// restores the previous value (that of the enclosing scope, or nullptr)
      OFXS_THREAD_SCOPE_HIDDEN ~ThreadScope() noexcept
      {
#ifdef _WIN32
        slot() = _previous;
#else
        // (can't fail: the thread's storage for the key exists since the constructor)
        pthread_setspecific(_key, _previous);
        _live.fetch_sub(1, std::memory_order_release);
#endif
      }

      // not copyable or movable: the destructor restores the previous value (C.21)
      ThreadScope(const ThreadScope&) = delete;
      ThreadScope& operator=(const ThreadScope&) = delete;

      /// the value of the innermost scope of the calling thread, or nullptr
      OFXS_THREAD_SCOPE_HIDDEN static T* current() noexcept
      {
#ifdef _WIN32
        return slot();
#else
        // no live scope (on any thread): the key may not exist yet
        if (_live.load(std::memory_order_acquire) == 0) {
          return nullptr;
        }
        return static_cast<T*>(pthread_getspecific(_key));
#endif
      }

    private:
      // returns the previous value
      OFXS_THREAD_SCOPE_HIDDEN static T* push(T* value)
      {
#ifdef _WIN32
        T* previous = slot();
        slot() = value;
        return previous;
#else
        static const Key key; // (thread-safe, and tried again if it throws)
        (void)key;
        _live.fetch_add(1, std::memory_order_acq_rel); // (from here, current() reads _key)
        T* previous = static_cast<T*>(pthread_getspecific(_key));
        const int err = pthread_setspecific(_key, value);
        if (err != 0) {
          _live.fetch_sub(1, std::memory_order_release);
          throwError(err, "pthread_setspecific");
        }
        return previous;
#endif
      }

#ifndef _WIN32
      // std::bad_alloc for ENOMEM, which Support's action handlers return as kOfxStatErrMemory
      [[noreturn]] OFXS_THREAD_SCOPE_HIDDEN static void throwError(int err, const char* what)
      {
        if (err == ENOMEM) {
          throw std::bad_alloc();
        }
        throw std::system_error(err, std::generic_category(), what);
      }
#endif

#ifdef _WIN32
      static T*& slot() noexcept
      {
        static thread_local T* value = nullptr;
        return value;
      }
#else
      // creates _key, and deletes it when the binary is unloaded or at exit, unless a scope is
      // still live. The key and the count are trivially destructible statics, so that a scope
      // can still use them after this destructor has run.
      struct OFXS_THREAD_SCOPE_HIDDEN Key {
        Key()
        {
          const int err = pthread_key_create(&_key, nullptr); // (no value is left at a thread's end)
          if (err != 0) {
            throwError(err, "pthread_key_create");
          }
        }
        ~Key()
        {
          if (_live.load(std::memory_order_acquire) == 0) {
            pthread_key_delete(_key);
          }
        }
        Key(const Key&) = delete;
        Key& operator=(const Key&) = delete;
      };

      OFXS_THREAD_SCOPE_HIDDEN static inline pthread_key_t _key{};
      OFXS_THREAD_SCOPE_HIDDEN static inline std::atomic<int> _live{0}; // the live scopes, on all threads
#endif

      T* const _previous;
    };
  };
};

#endif
