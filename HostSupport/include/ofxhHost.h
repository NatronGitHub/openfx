#ifndef OFX_HOST_H
#define OFX_HOST_H

/*
Software License :

Copyright (c) 2007-2009, The Open Effects Association Ltd.  All Rights Reserved.

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

#if defined(OFX_EXTENSIONS_NATRON) && !defined(OFX_EXTENSIONS_NUKE)
#error "OFX_EXTENSIONS_NATRON needs OFX_EXTENSIONS_NUKE: the Natron extensions use the Nuke ones (views, planes, transforms)"
#endif

#include <map>
#include <string>
#include <cstdarg>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxTimeLine.h"
#include "ofxhPropertySuite.h"

namespace OFX {

  namespace Host {

    /// a plugin what we use
    class Plugin;
   
    /// a param descriptor 
    namespace Param {
      class Descriptor;
    }
    
    /// our own internal property on the host descriptor, holding a pointer to the Host object
#define kOfxHostSupportHostPointer "sf.openfx.net.OfxHostSupportHostPointer"

    /// Base class for all objects passed to a plugin by the 'setHost' function 
    /// passed back by any plug-in.
    class Host {
    protected :
      OfxHost       _host;
      Property::Set _properties;

    public:
      Host();
      virtual ~Host() {}

      
      /// get the props on this host
      Property::Set &getProperties() {return _properties; }

      /// fetch a suite
      /// The base class returns the following suites
      ///    PropertySuite
      ///    MemorySuite
      virtual const void *fetchSuite(const char *suiteName, int suiteVersion);

      /// Called just before the OfxSetHost of a binary (OFX 1.5), with the binary's file path, before
      /// any of its plug-ins is known: a host whose properties (e.g. kOfxPropName) depend on the
      /// plug-in can set them for this binary here. The default does nothing.
      virtual void loadingBinary(const std::string& /*filePath*/) {}

      /// Whether the plug-in cache calls the OfxSetHost of the binaries (OFX 1.5) before their
      /// OfxGetPlugin. Return false to behave as an OFX 1.4 host, which only passes the host to
      /// OfxPlugin::setHost. The default is true. The plug-in cache records what the plug-ins said
      /// in OfxSetHost and describe: a host whose answer changes (e.g. with a command-line option)
      /// must also change its plug-in cache version (PluginCache::setCacheVersion()) or file.
      virtual bool supportsOfxSetHost() const { return true; }
      
      /// get the C API handle that is passed across the API to represent this host
      OfxHost *getHandle();

      /// override this to handle do post-construction initialisation on a Param::Descriptor
      virtual void initParamDescriptor(Param::Descriptor *) { }

      /// is my magic number valid?
      bool verifyMagic() { return true; }

      /// message (called when an exception occurs, calls vmessage)
      OfxStatus message(const char* type,
                        const char* id,
                        const char* format,
                        ...);

      /// vmessage
      virtual OfxStatus vmessage(const char* type,
                                 const char* id,
                                 const char* format,
                                 va_list args) = 0;

      /// setPersistentMessage
      virtual OfxStatus setPersistentMessage(const char* type,
                                             const char* id,
                                             const char* format,
                                             va_list args) = 0;
      /// clearPersistentMessage
      virtual OfxStatus clearPersistentMessage() = 0;
    };
    
  }
}

#endif

