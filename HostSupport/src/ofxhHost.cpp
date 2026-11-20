/*
Software License :

Copyright (c) 2007-2009, The Open Effects Association Ltd. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.
    * Neither the name The Open Effects Association Ltd, nor the names of its 
      contributors may be used to endorse or promote products derived from this
      software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include <limits.h>
#include <math.h>
#include <float.h>
#include <string.h>
#include <stdlib.h> // malloc, free
#include <cstddef> // std::max_align_t

// ofx
#include "ofxCore.h"
#include "ofxProperty.h"
#include "ofxMultiThread.h"
#include "ofxMemory.h"
#ifdef OFX_SUPPORTS_OPENGLRENDER
#include "ofxOpenGLRender.h"
#endif

#include "ofxhHost.h"

typedef OfxPlugin* (*OfxGetPluginType)(int);

namespace OFX {
  
  namespace Host {

    ////////////////////////////////////////////////////////////////////////////////
    /// simple memory suite 
    namespace Memory {
      // Each block starts with a header that holds a magic number, so that memoryFree() can return
      // "::kOfxStatErrBadHandle allocatedData was not a valid pointer returned by
      // OfxMemorySuiteV1::memoryAlloc" (ofxMemory.h) instead of passing any pointer to free().
      // memoryFree(NULL) is kOfxStatErrBadHandle too: NULL is never returned by memoryAlloc.
      // A plug-in that frees memoryAlloc's memory with free() (which was already wrong) now passes
      // a pointer that malloc() didn't return. As with the magic number of property sets, a pointer
      // that is not a block is still read (the header before it), and a freed block may be
      // detected (kMemoryFreedMagic) only until its memory is reused.
      //
      // "Allocated memory will be aligned for any use." (ofxMemory.h): malloc() returns memory
      // aligned for any fundamental type, alignof(std::max_align_t), and the header size is a
      // multiple of that, so that the data keeps the alignment. It is at least 16 bytes, the
      // alignment of malloc() on 64-bit systems (also where alignof(std::max_align_t) is 8, e.g.
      // MSVC), for SSE types. Large blocks are no longer page-aligned (they start after the header).
#if (__cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L)) // MSVC: __cplusplus is 199711L without /Zc:__cplusplus
      static const size_t kMemoryHeaderSize = alignof(std::max_align_t) > 16 ? alignof(std::max_align_t) : 16;
      static_assert(kMemoryHeaderSize % alignof(std::max_align_t) == 0,
                    "the memory suite header must keep the alignment of malloc()");
      static_assert(kMemoryHeaderSize >= sizeof(unsigned int), "the memory suite header holds the magic number");
#else
      static const size_t kMemoryHeaderSize = 16; // (no std::max_align_t before C++11)
#endif
      static const unsigned int kMemoryMagic = 0x0f8e3a17; // a live block
      static const unsigned int kMemoryFreedMagic = 0x0f8e3a18; // a freed block

      static OfxStatus memoryAlloc(void */*handle*/, size_t bytes, void **data)
      {
        if (!data) {
          return kOfxStatErrBadHandle;
        }
        *data = NULL;
        // "::kOfxStatErrMemory the request could not be met and no memory was allocated"
        if (bytes > (size_t)-1 - kMemoryHeaderSize) {
          return kOfxStatErrMemory;
        }
        unsigned char *block = (unsigned char *)malloc(kMemoryHeaderSize + bytes);
        if (!block) {
          return kOfxStatErrMemory;
        }
        memcpy(block, &kMemoryMagic, sizeof(kMemoryMagic));
        *data = block + kMemoryHeaderSize;
        return kOfxStatOK;
      }
      
      static OfxStatus memoryFree(void *data)
      {
        if (!data) {
          return kOfxStatErrBadHandle;
        }
        unsigned char *block = (unsigned char *)data - kMemoryHeaderSize;
        unsigned int magic;
        memcpy(&magic, block, sizeof(magic));
        if (magic != kMemoryMagic) {
          return kOfxStatErrBadHandle;
        }
        memcpy(block, &kMemoryFreedMagic, sizeof(kMemoryFreedMagic));
        free(block);
        return kOfxStatOK;
      }
      
      static const struct OfxMemorySuiteV1 gMallocSuite = {
        memoryAlloc,
        memoryFree
      };
    }

  }

}

namespace OFX {
  namespace Host {
    // kOfxHostSupportHostPointer (ofxhHost.h) stores away our private pointer to our host descriptor.
    // The image effect host properties are all read only ("Write Access: host", propset_ImageEffectHost
    // in Documentation/sources/Reference/ofxPropertySetsGenerated.rst). The
    // property suite never lets a plug-in write kOfxHostSupportHostPointer, whatever the
    // read-only policy: fetchSuite() dereferences it for every plug-in.
    static const Property::PropSpec hostStuffs[] = {
      { kOfxPropAPIVersion, Property::eInt, 0, true, "" },
      { kOfxPropType, Property::eString, 1, true, kOfxTypeImageEffectHost },
      { kOfxPropName, Property::eString, 1, true, "UNKNOWN" },
      { kOfxPropLabel, Property::eString, 1, true, "UNKNOWN" },
      { kOfxPropVersion, Property::eInt, 0, true, "0" },
      { kOfxPropVersionLabel, Property::eString, 1, true, "" },
      { kOfxHostSupportHostPointer,    Property::ePointer,    0,    true,    NULL },
      Property::propSpecEnd
    };    

    static const void *fetchSuite(OfxPropertySetHandle hostProps, const char *suiteName, int suiteVersion)
    {
      // "returns NULL if the API is unknown": a bad host handle (not a property set, checked
      // with its magic number) or a NULL suite name give NULL, as an unknown suite does
      Property::Set* properties = reinterpret_cast<Property::Set*>(hostProps);

      if (!properties || !properties->verifyMagic() || !suiteName)
        return 0;

      try {
        Host* host = (Host*)properties->getPointerProperty(kOfxHostSupportHostPointer);

        if(host)
          return host->fetchSuite(suiteName,suiteVersion);
        else
          return 0;
      } catch (...) {
        // e.g. a property set without kOfxHostSupportHostPointer: C++ exceptions must not
        // propagate into the plug-in
        return 0;
      }
    }

    // Base Host
    Host::Host() : _properties(hostStuffs) 
    {
      _host.host = _properties.getHandle();
      _host.fetchSuite = OFX::Host::fetchSuite;

      // record the host descriptor in the propert set
      _properties.setPointerProperty(kOfxHostSupportHostPointer,this);
    }

    OfxHost *Host::getHandle() {
      return &_host;
    }

    OfxStatus Host::message(const char* type,
                            const char* id,
                            const char* format,
                            ...) {
      try {
        OfxStatus stat;
        va_list args;
        va_start(args,format);
        stat = vmessage(type,id,format,args);
        va_end(args);
        return stat;
      } catch (...) {
        return kOfxStatFailed;
      }
    }

    const void *Host::fetchSuite(const char *suiteName, int suiteVersion)
    {
      if (strcmp(suiteName, kOfxPropertySuite)==0  && suiteVersion == 1) {
        return Property::GetSuite(suiteVersion);
      }
      else if (strcmp(suiteName, kOfxMemorySuite)==0 && suiteVersion == 1) {
        return (void*)&Memory::gMallocSuite;
      }  
    
      ///printf("fetchSuite failed with host = %p, name = %s, version = %i\n", this, suiteName, suiteVersion);
      return NULL;
    }

  } // Host

} // OFX 
