
// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef OFXH_IMAGE_EFFECT_API_H
#define OFXH_IMAGE_EFFECT_API_H

#include <string>
#include <map>
#include <set>
#include <memory>

#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxhImageEffect.h"
#include "ofxhHost.h"
#include "ofxhPluginCache.h" // Plugin

namespace OFX {
  
  namespace Host {
    
    namespace ImageEffect {

      class PluginCache;

      /// subclass of Plugin representing an ImageEffect plugin.  used to store API-specific
      /// data
      class ImageEffectPlugin : public Plugin {

        PluginCache &_pc;

        // this comes off Descriptor's property set after a describe
        // context independent
        Descriptor *_baseDescriptor; ///< owned; made by the host's factory, Host::makeDescriptor()
        
        /// map to store contexts in
        std::map<std::string, Descriptor *> _contexts;

        mutable std::set<std::string> _knownContexts;
        mutable bool _madeKnownContexts;

        auto_ptr<PluginHandle> _pluginHandle;

        /// OfxSetHost refused this host, or kOfxActionLoad or kOfxActionDescribe failed:
        /// "no further actions will be passed to the plug-in", so getPluginHandle() doesn't try again
        /// while the binary stays loaded (same PluginBinary::getLoadGeneration())
        bool _loadFailed;
        int _failedLoadGeneration;

        /// record a failure of getPluginHandle(), release the handle, return NULL
        PluginHandle *loadFailed();

        friend class PluginCache; // loadFromPlugin() calls unload()

        void addContextInternal(const std::string &context) const;

      public:
		ImageEffectPlugin(PluginCache &pc, PluginBinary *pb, int pi, OfxPlugin *pl);

        ImageEffectPlugin(PluginCache &pc,
                          PluginBinary *pb,
                          int pi,
                          const std::string &api,
                          int apiVersion,
                          const std::string &pluginId,
                          const std::string &rawId,
                          int pluginMajorVersion,
                          int pluginMinorVersion);

        virtual ~ImageEffectPlugin();

        /// return the API handler this plugin was constructed by
        APICache::PluginAPICacheI &getApiHandler();


        /// get the base image effect descriptor
        Descriptor &getDescriptor();

        /// get the base image effect descriptor, const version
        const Descriptor &getDescriptor() const;

        /// get the image effect descriptor for the context
        Descriptor *getContext(const std::string &context);

        void addContext(const std::string &context);
        void addContext(const std::string &context, Descriptor *ied);

        virtual void saveXML(std::ostream &os);

        /// the contexts, as read from the plug-in cache: they may be stale (see PluginHandle). A
        /// context that the plug-in no longer supports still gets kOfxImageEffectActionDescribeInContext:
        /// if it fails, getContext() returns NULL. New contexts stay hidden until the cache is cleared.
        const std::set<std::string>& getContexts() const;

        /// Load the plug-in, if it isn't yet: load the binary if needed (with its OfxSetHost), then
        /// kOfxActionLoad, then kOfxActionDescribe on the base descriptor, which is first reset in
        /// place to its initial state (it may hold the values read from the plug-in cache, or
        /// described during a previous load). The handle keeps the binary loaded until
        /// kOfxActionUnload: when the plug-in is deleted, or after the scan (see
        /// Host::keepPluginsLoadedAfterScan()). Returns NULL if OfxSetHost, Load or Describe failed
        /// (Load and Describe failures are logged on std::cerr, after a failed Describe the plug-in
        /// gets kOfxActionUnload), and then NULL again while the binary stays loaded. Once the
        /// binary was unloaded, the next call loads it and tries again: e.g. a Describe that
        /// returned kOfxStatErrMemory ("describe may be called again after a memory purge").
        ///
        /// Not thread-safe. The call that loads the plug-in (no handle yet), and the unload (after
        /// the scan, or PluginCache::clearPluginCache()), change state that all the plug-ins of the
        /// binary share (PluginBinary): the host must serialize them with every other call of
        /// getPluginHandle(), getContext() and createInstance(), for all the plug-ins of the binary,
        /// and not call these during the scan. Once the plug-in is loaded, getPluginHandle() only
        /// reads; getContext() still describes a new context, so serialize it per plug-in.
        PluginHandle *getPluginHandle();

        /// this is called to make an instance of the effect
        /// the client data ptr is what is passed back to the client creation function
        ImageEffect::Instance* createInstance(const std::string &context, void *clientDataPtr);

      private:
        /// if the plug-in is loaded: kOfxActionUnload, then release the handle (which may unload the
        /// binary) and the context descriptors of this load, and, unless the plug-in is being
        /// deleted, reset the pointer properties of the base descriptor and its overlay descriptor
        void unload(bool deleting = false);
      };

      class MajorPlugin {
        std::string _id;
        int _major;

      public:
        MajorPlugin(const std::string &id, int major) : _id(id), _major(major) {
        }

        MajorPlugin(ImageEffectPlugin *iep) : _id(iep->getIdentifier()), _major(iep->getVersionMajor()) {
        }

        const std::string &getId() const {
          return _id;
        }

        int getMajor() const {
          return _major;
        }

        bool operator<(const MajorPlugin &other) const {
          if (_id < other._id)
            return true;

          if (_id > other._id)
            return false;
          
          if (_major < other._major)
            return true;

          return false;
        }
      };

      /// implementation of the specific Image Effect handler API cache.
      class PluginCache : public APICache::PluginAPICacheI {
      public:      

      private:
        /// all plugins
        std::vector<ImageEffectPlugin *> _plugins;

        /// latest version of each plugin by ID
        std::map<std::string, ImageEffectPlugin *> _pluginsByID;

        /// latest minor version of each plugin by (ID,major)
        std::map<MajorPlugin, ImageEffectPlugin *> _pluginsByIDMajor;

        /// xml parsing state
        ImageEffectPlugin *_currentPlugin;
        /// xml parsing state
        Property::Property *_currentProp;
        
        Descriptor *_currentContext;
        Param::Descriptor *_currentParam;
        ClipDescriptor *_currentClip;

        /// pointer to our image effect host
        OFX::Host::ImageEffect::Host* _host;

      public:  

        explicit PluginCache(OFX::Host::ImageEffect::Host* host);

        virtual ~PluginCache();        

        /// get the plugin by id.  vermaj and vermin can be specified.  if they are not it will
        /// pick the highest found version.
        ImageEffectPlugin *getPluginById(const std::string &id, int vermaj=-1, int vermin=-1);

        /// get the plugin by label.  vermaj and vermin can be specified.  if they are not it will
        /// pick the highest found version.
        ImageEffectPlugin *getPluginByLabel(const std::string &label, int vermaj=-1, int vermin=-1);

        OFX::Host::ImageEffect::Host *getHost() {
          return _host;
        }

        const std::vector<ImageEffectPlugin *>& getPlugins() const;

        const std::map<std::string, ImageEffectPlugin *>& getPluginsByID() const;

        const std::map<MajorPlugin, ImageEffectPlugin *>& getPluginsByIDMajor() const
        {
          return _pluginsByIDMajor;
        }

        /// handle the case where the info needs filling in from the file.  runs the "describe" action on the plugin.
        void loadFromPlugin(Plugin *p) const;

        /// hook run before deleting the ImageEffect
        void unloadPlugin(Plugin *p) const;

        /// handler for preparing to read in a chunk of XML from the cache, set up context to do this
        void beginXmlParsing(Plugin *p);

        /// XML handler : element begins (everything is stored in elements and attributes)       
        virtual void xmlElementBegin(const std::string &el, const std::map<std::string, std::string> &map);

        virtual void xmlCharacterHandler(const std::string &);
        
        virtual void xmlElementEnd(const std::string &el);
        
        virtual void endXmlParsing();
        
        virtual void saveXML(Plugin *ip, std::ostream &os) const;

        void confirmPlugin(Plugin *p, const std::list<std::string>& pluginPath);

        virtual void clearConfirmedPlugins() {
          _plugins.clear();
          _pluginsByID.clear();
          _pluginsByIDMajor.clear();
        }

        virtual bool pluginSupported(Plugin *p, std::string &reason) const;

        Plugin *newPlugin(PluginBinary *pb,
                          int pi,
                          OfxPlugin *pl);

        Plugin *newPlugin(PluginBinary *pb,
                          int pi,
                          const std::string &api,
                          int apiVersion,
                          const std::string &pluginId,
                          const std::string &rawId,
                          int pluginMajorVersion,
                          int pluginMinorVersion);

        void dumpToStdOut();
      };
    
    } // ImageEffect

  } // Host

} // OFX

#endif
