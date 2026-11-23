#ifndef OFX_HOST_H
#define OFX_HOST_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

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

    private :
      // not copyable (C++98 style: private and not defined): _host.host is the handle of this
      // host's _properties, so that a copy would hand out the properties of the original (C.21)
      Host(const Host &);
      Host &operator=(const Host &);

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

      /// Whether the plug-ins found by the scan of the plug-in files (PluginCache::scanPluginFiles(),
      /// for a binary that is not in the plug-in cache or has changed) stay loaded until the plug-in
      /// cache is deleted (PluginCache::clearPluginCache()). By default (false), each plug-in of the
      /// binary gets kOfxActionLoad, kOfxActionDescribe and kOfxActionUnload, and the binary is
      /// unloaded after the scan: the first use of a plug-in (ImageEffect::ImageEffectPlugin::
      /// getPluginHandle(), e.g. from createInstance()) loads it again, as for a plug-in known from
      /// the cache (OfxSetHost, kOfxActionLoad, kOfxActionDescribe). Return true to keep each
      /// scanned plug-in loaded instead (one kOfxActionLoad and kOfxActionDescribe, and
      /// kOfxActionUnload when the plug-in cache is deleted): this avoids loading the binaries
      /// twice, but keeps every scanned plug-in in memory. Statically linked plug-ins are never
      /// unloaded. Used by the image effect API (ImageEffect::PluginCache), with the host passed to
      /// its constructor.
      virtual bool keepPluginsLoadedAfterScan() const { return false; }
      
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

