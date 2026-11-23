
// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef OFX_MEMORY_H
#define OFX_MEMORY_H

#include <memory>
#include <cstddef> // size_t

#include "ofxImageEffect.h" // OfxImageMemoryHandle

namespace OFX {

  namespace Host {
#if (__cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L)) // MSVC: __cplusplus is 199711L without /Zc:__cplusplus
    template <typename T>
    using auto_ptr = std::unique_ptr<T>;
#else
    using std::auto_ptr;
#endif

    namespace Memory {

      class Instance {
      public:
        Instance();

        virtual ~Instance();        
        virtual bool alloc(size_t nBytes);
        virtual OfxImageMemoryHandle getHandle();
        virtual bool freeMem(); // returns true if object can be deleted
        virtual void* getPtr();
        virtual void lock();
        virtual void unlock();

        virtual bool verifyMagic() { return true; }

      protected:
        char*   _ptr;
        int     _locked;

      private:
        // not copyable (C++98 style: private and not defined): the instance owns _ptr, which
        // freeMem() and the destructor delete[], so that a copy would free it twice (C.21, C.67)
        Instance(const Instance &);
        Instance &operator=(const Instance &);
      };

    } // Memory

  } // Host

} // OFX

#endif // OFX_MEMORY_H
