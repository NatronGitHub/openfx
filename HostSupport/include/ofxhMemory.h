
// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef OFX_MEMORY_H
#define OFX_MEMORY_H

#include <memory>
#include <cstddef> // size_t

#include "ofxImageEffect.h" // OfxImageMemoryHandle

namespace OFX {

  namespace Host {
    /// std::unique_ptr, under the name the hosts use (std::auto_ptr was removed in C++17)
    template <typename T>
    using auto_ptr = std::unique_ptr<T>;

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
        // not copyable: the instance owns _ptr, which freeMem() and the destructor delete[], so
        // that a copy would free it twice (C.21, C.67)
        Instance(const Instance &) = delete;
        Instance &operator=(const Instance &) = delete;
      };

    } // Memory

  } // Host

} // OFX

#endif // OFX_MEMORY_H
