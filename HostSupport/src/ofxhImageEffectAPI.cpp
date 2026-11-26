// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include <assert.h>

#include <string>
#include <map>
#include <ctype.h>
#include <stdexcept>

// ofx
#include "ofxImageEffect.h"

// ofx host
#include "ofxhBinary.h"
#include "ofxhPropertySuite.h"
#include "ofxhClip.h"
#include "ofxhParam.h"
#include "ofxhMemory.h"
#include "ofxhImageEffect.h"
#include "ofxhPluginAPICache.h"
#include "ofxhPluginCache.h"
#include "ofxhHost.h"
#include "ofxhImageEffectAPI.h"
#include "ofxhXml.h"

// Disable the "this pointer used in base member initialiser list" warning in Windows
namespace OFX {

  namespace Host {

    namespace ImageEffect {

      /// our global host bobject, set when the cache is created
      OFX::Host::ImageEffect::Host *gImageEffectHost;

      /// ctor
#ifdef _MSC_VER
#pragma warning( disable : 4355 )
#endif
      ImageEffectPlugin::ImageEffectPlugin(PluginCache &pc, PluginBinary *pb, int pi, OfxPlugin *pl)
        : Plugin(pb, pi, pl)
        , _pc(pc)
        , _baseDescriptor(NULL)
        , _madeKnownContexts(false)
        , _pluginHandle()
        , _loadFailed(false)
        , _failedLoadGeneration(0)
      {
        _baseDescriptor = gImageEffectHost->makeDescriptor(this);
      }

      ImageEffectPlugin::ImageEffectPlugin(PluginCache &pc,
                                           PluginBinary *pb,
                                           int pi,
                                           const std::string &api,
                                           int apiVersion,
                                           const std::string &pluginId,
                                           const std::string &rawId,
                                           int pluginMajorVersion,
                                           int pluginMinorVersion)
        : Plugin(pb, pi, api, apiVersion, pluginId, rawId, pluginMajorVersion, pluginMinorVersion)
        , _pc(pc)
        , _baseDescriptor(NULL) 
        , _madeKnownContexts(false)
        , _pluginHandle()
        , _loadFailed(false)
        , _failedLoadGeneration(0)
      {        
        _baseDescriptor = gImageEffectHost->makeDescriptor(this);
      }

#ifdef _MSC_VER
#pragma warning( default : 4355 )
#endif

      ImageEffectPlugin::~ImageEffectPlugin()
      {
        // first: the descriptor handles are valid "until the end of the kOfxActionUnload action"
        unload(true);
        for(std::map<std::string, Descriptor *>::iterator it =  _contexts.begin(); it != _contexts.end(); ++it) {
          delete it->second;
        }
        _contexts.clear();
        delete _baseDescriptor;
      }

      APICache::PluginAPICacheI &ImageEffectPlugin::getApiHandler()
      {
        return _pc;
      }


      /// get the image effect descriptor
      Descriptor &ImageEffectPlugin::getDescriptor() {
        return *_baseDescriptor;
      }

      /// get the image effect descriptor const version
      const Descriptor &ImageEffectPlugin::getDescriptor() const {
        return *_baseDescriptor;
      }

      void ImageEffectPlugin::addContext(const std::string &context, Descriptor *ied)
      {
        _contexts[context] = ied;
        _knownContexts.insert(context);
        _madeKnownContexts = true;
      }

      void ImageEffectPlugin::addContext(const std::string &context)
      {
        _knownContexts.insert(context);
        _madeKnownContexts = true;
      }

      void ImageEffectPlugin::addContextInternal(const std::string &context) const
      {
        _knownContexts.insert(context);
        _madeKnownContexts = true;
      }

      void ImageEffectPlugin::saveXML(std::ostream &os) 
      {        
        APICache::propertySetXMLWrite(os, getDescriptor().getProps(), 6);
      }

      const std::set<std::string> &ImageEffectPlugin::getContexts() const {
        if (_madeKnownContexts) {
          return _knownContexts;
        } 
        else {
          const OFX::Host::Property::Set &eProps = getDescriptor().getProps();
          int size = eProps.getDimension(kOfxImageEffectPropSupportedContexts);
          for (int j=0;j<size;j++) {
            std::string context = eProps.getStringProperty(kOfxImageEffectPropSupportedContexts, j);
            addContextInternal(context);
          }
          return _knownContexts;
        }
      }

      /// copy the values of the properties of src into the properties of dst which have the same name
      /// and type (a variable-dimension property gets the dimension of the source)
      template<class P>
      static bool copyPropertyValues(Property::Property *dst, Property::Property *src)
      {
        P *d = dynamic_cast<P *>(dst);
        P *s = dynamic_cast<P *>(src);
        if (!d || !s) {
          return false;
        }
        const std::vector<typename P::Type> values = s->getValues();
        if (!d->isFixedSize()) {
          d->setValueN(NULL, 0);
        }
        for (size_t i = 0; i < values.size(); ++i) {
          d->setValue(values[i], (int)i);
        }
        return true;
      }

      /// which properties copyPropertySetValues() copies
      enum PropertyCopyEnum {
        ePropertyCopyAll,
        ePropertyCopyNoPointers, ///< not the pointers (e.g. kOfxImageEffectPluginPropOverlayInteractV1)
        ePropertyCopyPointers    ///< only the pointers
      };

      static void copyPropertySetValues(Property::Set &dst, const Property::Set &src, PropertyCopyEnum which = ePropertyCopyAll)
      {
        const Property::PropertyMap &srcProps = src.getProperties();
        for (Property::PropertyMap::const_iterator it = srcProps.begin(); it != srcProps.end(); ++it) {
          Property::Property *d = dst.fetchProperty(it->first);
          Property::Property *s = it->second;
          if (!d || d == s) {
            continue;
          }
          if (which != ePropertyCopyPointers &&
              (copyPropertyValues<Property::Int>(d, s) ||
               copyPropertyValues<Property::Double>(d, s) ||
               copyPropertyValues<Property::String>(d, s))) {
            continue;
          }
          if (which != ePropertyCopyNoPointers) {
            (void)copyPropertyValues<Property::Pointer>(d, s);
          }
        }
      }

      /// The pointers that the plug-in set on its base descriptor (overlay interact entry points...)
      /// point into the binary: after kOfxActionUnload, the binary may be unloaded. They get the
      /// values of a new descriptor (NULL), as the base descriptor of a plug-in only known from the
      /// cache. The base descriptor's overlay descriptor, described with the old entry point, is
      /// reset too.
      static void resetPointerProperties(ImageEffectPlugin *plugin, Descriptor &desc)
      {
        desc.resetOverlayDescriptor();
        try {
          auto_ptr<ImageEffect::Descriptor> fresh(gImageEffectHost->makeDescriptor(plugin));
          copyPropertySetValues(desc.getProps(), fresh->getProps(), ePropertyCopyPointers);
        } catch (...) {
          // no fresh descriptor: their default values (NULL)
          const Property::PropertyMap &props = desc.getProps().getProperties();
          for (Property::PropertyMap::const_iterator it = props.begin(); it != props.end(); ++it) {
            if (it->second->getType() == Property::ePointer) {
              try {
                it->second->reset();
              } catch (...) {
              }
            }
          }
        }
      }

      /// the pluginIdentifier of an OfxPlugin, saved for CatchAllSetStatus before kOfxActionUnload,
      /// after which the OfxPlugin may be deleted (the Support library deletes it)
      struct SavedPluginIdentifier {
        std::string id;
        const char *pluginIdentifier;
        explicit SavedPluginIdentifier(const OfxPlugin *op)
          : id(op && op->pluginIdentifier ? op->pluginIdentifier : "")
          , pluginIdentifier(NULL)
        {
          pluginIdentifier = id.c_str();
        }
      private:
        // pluginIdentifier points into id: not copyable
        SavedPluginIdentifier(const SavedPluginIdentifier &);
        SavedPluginIdentifier &operator=(const SavedPluginIdentifier &);
      };

      PluginHandle *ImageEffectPlugin::getPluginHandle() 
      {
        // ofxCore.h: kOfxActionLoad "will not be called again while the binary containing the
        // plug-in remains loaded", and kOfxActionDescribe "will not be called again, unless it fails
        // and returns one of the error codes where the host is allowed to attempt the action
        // again". The handle keeps the binary loaded, until unload(), which sends kOfxActionUnload:
        // the plug-in gets Load and Describe once per load of the binary. After a failure, "no
        // further actions will be passed to the plug-in" while the binary stays loaded; once it was
        // unloaded, the next call loads it again and tries again (e.g. after kOfxStatErrMemory).
        if (_pluginHandle.get()) {
          return _pluginHandle.get();
        }
        PluginBinary *binary = getBinary();
        if (_loadFailed && binary->isLoaded() && binary->getLoadGeneration() == _failedLoadGeneration) {
          return 0;
        }
        _loadFailed = false;

        _pluginHandle.reset(new OFX::Host::PluginHandle(this, _pc.getHost())); 
        
        OfxPlugin *op = _pluginHandle->getOfxPlugin();
        
        if (!op) {
          // OfxSetHost refused this host (silently: "it's not for this Host and it should be
          // skipped silently"), or the binary could not be loaded or no longer has this plug-in
          if (!binary->setHostFailed()) {
            std::cerr << "OFX: " << getIdentifier() << ": cannot get the plug-in from " << binary->getFilePath() << std::endl;
          }
          return loadFailed();
        }

        OfxStatus stat;
        try {
#         ifdef OFX_DEBUG_ACTIONS
            std::cout << "OFX: "<<op->pluginIdentifier<<"("<<(void*)op<<")->"<<kOfxActionLoad<<"()"<<std::endl;
#         endif
          stat = op->mainEntry(kOfxActionLoad, 0, 0, 0);
#         ifdef OFX_DEBUG_ACTIONS
            std::cout << "OFX: "<<op->pluginIdentifier<<"("<<(void*)op<<")->"<<kOfxActionLoad<<"()->"<<StatStr(stat)<<std::endl;
#         endif
        } CatchAllSetStatus(stat, gImageEffectHost, op, kOfxActionLoad);

        if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
          std::cerr << "OFX: " << getIdentifier() << ": " << kOfxActionLoad << " failed (" << StatStr(stat) << ")" << std::endl;
          return loadFailed();
        }

        // The base descriptor may hold the values read from the plug-in cache, or described during
        // a previous load of the binary: Describe must start from the initial state of a
        // descriptor, or the values that the plug-in appends (kOfxImageEffectPropSupportedContexts,
        // kOfxImageEffectPropSupportedPixelDepths...) would be duplicated. It is reset in place,
        // because the plug-in "is at liberty to cache the handle away for future reference until
        // the plug-in is unloaded", and the host may hold references to getDescriptor().
        // Properties that a fresh descriptor doesn't have (read from an older cache) are kept.
        Property::Set &props = getDescriptor().getProps();
        auto_ptr<Property::Set> previousProps; // restored if Describe fails
        try {
          previousProps.reset(new Property::Set(props));
          {
            auto_ptr<ImageEffect::Descriptor> fresh(gImageEffectHost->makeDescriptor(this));
            copyPropertySetValues(props, fresh->getProps());
          }
#         ifdef OFX_DEBUG_ACTIONS
            std::cout << "OFX: "<<op->pluginIdentifier<<"("<<(void*)op<<")->"<<kOfxActionDescribe<<"()"<<std::endl;
#         endif
          stat = op->mainEntry(kOfxActionDescribe, getDescriptor().getHandle(), 0, 0);
#         ifdef OFX_DEBUG_ACTIONS
            std::cout << "OFX: "<<op->pluginIdentifier<<"("<<(void*)op<<")->"<<kOfxActionDescribe<<"()->"<<StatStr(stat)<<std::endl;
#         endif
        } CatchAllSetStatus(stat, gImageEffectHost, op, kOfxActionDescribe);

        if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
          std::cerr << "OFX: " << getIdentifier() << ": " << kOfxActionDescribe << " failed (" << StatStr(stat) << ")" << std::endl;
          if (previousProps.get()) {
            try {
              // not the pointers: they are reset below
              copyPropertySetValues(props, *previousProps, ePropertyCopyNoPointers);
            } catch (...) {
            }
          }
          // kOfxActionLoad succeeded, so the plug-in may have allocated global
          // resources: it must get kOfxActionUnload before being dropped.
          // ("kOfxStatErrMissingHostFeature, in which the plugin will be unloaded and ignored")
          const SavedPluginIdentifier saved(op);
          OfxStatus unloadStat;
          try {
            unloadStat = op->mainEntry(kOfxActionUnload, 0, 0, 0);
          } CatchAllSetStatus(unloadStat, gImageEffectHost, &saved, kOfxActionUnload);
          (void)unloadStat;
          resetPointerProperties(this, getDescriptor());
          return loadFailed();
        }

        return _pluginHandle.get();
      }

      PluginHandle *ImageEffectPlugin::loadFailed()
      {
        _loadFailed = true;
        _failedLoadGeneration = getBinary()->getLoadGeneration();
        _pluginHandle.reset(NULL); // may unload the binary
        return 0;
      }

      Descriptor *ImageEffectPlugin::getContext(const std::string &context) 
      {
        std::map<std::string, Descriptor *>::iterator it = _contexts.find(context);

        if (it != _contexts.end()) {
          //printf("found context description.\n");
          return it->second;
        }

        if (_knownContexts.find(context) == _knownContexts.end()) {
          return 0;
        }

        //        printf("doing context description.\n");

        OFX::Host::Property::PropSpec inargspec[] = {
          { kOfxImageEffectPropContext, OFX::Host::Property::eString, 1, true, context.c_str() },
            Property::propSpecEnd
        };
        
        OFX::Host::Property::Set inarg(inargspec);

        PluginHandle *ph = getPluginHandle();
        if (!ph) {
          // kOfxActionLoad or kOfxActionDescribe failed: no further actions may be passed to the plug-in
          return 0;
        }
        auto_ptr<ImageEffect::Descriptor> newContext( gImageEffectHost->makeDescriptor(getDescriptor(), this));

        OfxStatus stat;
        try {
#         ifdef OFX_DEBUG_ACTIONS
            OfxPlugin *ofxp = ph->getOfxPlugin();
            const char* id = ofxp->pluginIdentifier;
            std::cout << "OFX: "<<id<<"("<<ofxp<<")->"<<kOfxImageEffectActionDescribeInContext<<"("<<context<<")"<<std::endl;
#         endif
          stat = ph->getOfxPlugin()->mainEntry(kOfxImageEffectActionDescribeInContext, newContext->getHandle(), inarg.getHandle(), 0);
#         ifdef OFX_DEBUG_ACTIONS
            std::cout << "OFX: "<<id<<"("<<ofxp<<")->"<<kOfxImageEffectActionDescribeInContext<<"("<<context<<")->"<<StatStr(stat)<<std::endl;
#         endif
        } CatchAllSetStatus(stat, gImageEffectHost, ph->getOfxPlugin(), kOfxImageEffectActionDescribeInContext);

        if (stat == kOfxStatOK || stat == kOfxStatReplyDefault) {
          _contexts[context] = newContext.release();
          return _contexts[context];
        }
        return 0;
      }

      ImageEffect::Instance* ImageEffectPlugin::createInstance(const std::string &context, void *clientData)
      {          

        // kOfxActionLoad and kOfxActionDescribe, if they were not called since the binary was
        // loaded (getContext() calls it too, before kOfxImageEffectActionDescribeInContext)
        if (!getPluginHandle()) {
          return 0;
        }

        Descriptor *desc = getContext(context);
        
        if (desc) {
          ImageEffect::Instance *instance = gImageEffectHost->newInstance(clientData,
                                                                          this,
                                                                          *desc,
                                                                          context);
          if (!instance) {
            return 0;
          }
          OfxStatus st = instance->populate();
          if (st != kOfxStatOK) {
            // clips or params could not be created: the instance is unusable
            delete instance;
            return 0;
          }
          return instance;
        }
        return 0;
      }

      void ImageEffectPlugin::unload(bool deleting) {
        if (_pluginHandle.get()) {
          // kOfxActionUnload: "No other actions will be called", until the binary is loaded again.
          // The context descriptors of this load are dropped: the next load describes the plug-in
          // and its contexts again (the plug-in may have cached their handles).
          OfxPlugin *op = _pluginHandle->getOfxPlugin();
          const SavedPluginIdentifier saved(op); // the plug-in may delete op in kOfxActionUnload
          OfxStatus stat;
          try {
#           ifdef OFX_DEBUG_ACTIONS
              std::cout << "OFX: "<<saved.pluginIdentifier<<"("<<(void*)op<<")->"<<kOfxActionUnload<<"()"<<std::endl;
#           endif
            stat = op->mainEntry(kOfxActionUnload, 0, 0, 0);
#           ifdef OFX_DEBUG_ACTIONS
              std::cout << "OFX: "<<saved.pluginIdentifier<<"("<<(void*)op<<")->"<<kOfxActionUnload<<"()->"<<StatStr(stat)<<std::endl;
#           endif
          } CatchAllSetStatus(stat, gImageEffectHost, &saved, kOfxActionUnload);
          (void)stat;
          _pluginHandle.reset(NULL); // may unload the binary
          for (std::map<std::string, Descriptor *>::iterator it = _contexts.begin(); it != _contexts.end(); ++it) {
            delete it->second;
          }
          _contexts.clear();
          if (!deleting) {
            // (not when the plug-in cache is deleted: the host may be gone)
            resetPointerProperties(this, getDescriptor());
          }
        }
      }

      PluginCache::PluginCache(OFX::Host::ImageEffect::Host* host)
        : PluginAPICacheI(kOfxImageEffectPluginApi, 1, 1)
        , _currentPlugin(NULL)
        , _currentProp(NULL)
        , _currentContext(NULL)
        , _currentParam(NULL)
        , _currentClip(NULL)
        , _host(host)
      {
        gImageEffectHost = host;
      }

      PluginCache::~PluginCache() {}       

      /// get the plugin by id.  vermaj and vermin can be specified.  if they are not it will
      /// pick the highest found version.
      ImageEffectPlugin *PluginCache::getPluginById(const std::string &id, int vermaj, int vermin)
      {
        // return the highest version one, which fits the pattern provided
        ImageEffectPlugin *sofar = 0;

        for (std::vector<ImageEffectPlugin *>::iterator i=_plugins.begin();i!=_plugins.end();i++) {
          ImageEffectPlugin *p = *i;

          if (p->getIdentifier() != id) {
            continue;
          }

          if (vermaj != -1 && p->getVersionMajor() != vermaj) {
            continue;
          }

          if (vermin != -1 && p->getVersionMinor() != vermin) {
            continue;
          }

          if (!sofar || p->trumps(sofar)) {
            sofar = p;
          }
        }

        return sofar;
      }

      /// whether we support this plugin.  
      bool PluginCache::pluginSupported(OFX::Host::Plugin *p, std::string &reason) const {
        return gImageEffectHost->pluginSupported(dynamic_cast<OFX::Host::ImageEffect::ImageEffectPlugin *>(p), reason);
      }

      /// get the plugin by label.  vermaj and vermin can be specified.  if they are not it will
      /// pick the highest found version.
      ImageEffectPlugin *PluginCache::getPluginByLabel(const std::string &label, int vermaj, int vermin)
      {
        // return the highest version one, which fits the pattern provided
        ImageEffectPlugin *sofar = 0;

        for (std::vector<ImageEffectPlugin *>::iterator i=_plugins.begin();i!=_plugins.end();i++) {
          ImageEffectPlugin *p = *i;

          if (p->getDescriptor().getProps().getStringProperty(kOfxPropLabel) != label) {
            continue;
          }

          if (vermaj != -1 && p->getVersionMajor() != vermaj) {
            continue;
          }

          if (vermin != -1 && p->getVersionMinor() != vermin) {
            continue;
          }

          if (!sofar || p->trumps(sofar)) {
            sofar = p;
          }
        }

        return sofar;
      }

      const std::vector<ImageEffectPlugin *>& PluginCache::getPlugins() const
      {
        return _plugins;
      }

      const std::map<std::string, ImageEffectPlugin *>& PluginCache::getPluginsByID() const
      {
        return _pluginsByID;
      }

      /// handle the case where the info needs filling in from the file.  runs the "describe" action on the plugin.
      void PluginCache::loadFromPlugin(Plugin *op) const {
        _host->loadingStatus(true, op->getRawIdentifier(), op->getVersionMajor(), op->getVersionMinor());

        ImageEffectPlugin *p = dynamic_cast<ImageEffectPlugin*>(op);
        assert(p);
        if (!p) {
          std::cerr << "bad handle for plugin " << op->getIdentifier() << std::endl;
          return;
        }

        // kOfxActionLoad, then kOfxActionDescribe (getPluginHandle() logs a failure), inside
        // loadingStatus(true, ...) above. By default, kOfxActionUnload follows, and the plug-in
        // cache unloads the binary after the scan of its plug-ins: the first use of the plug-in
        // loads it again. With Host::keepPluginsLoadedAfterScan(), the plug-in keeps its handle, and
        // the binary stays loaded until the plug-in is deleted (kOfxActionUnload). Either way,
        // ofxCore.h: Load "will not be called again while the binary containing the plug-in
        // remains loaded".
        if (!p->getPluginHandle()) {
          return;
        }

        ImageEffect::Descriptor &e = p->getDescriptor();
        Property::Set &eProps = e.getProps();

        int size = eProps.getDimension(kOfxImageEffectPropSupportedContexts);

        for (int j=0;j<size;j++) {
          std::string context = eProps.getStringProperty(kOfxImageEffectPropSupportedContexts, j);
          p->addContext(context);
        }

        // a statically linked plug-in can't be unloaded: it keeps its handle
        if (!_host->keepPluginsLoadedAfterScan() && !p->getBinary()->isStaticallyLinkedPlugin()) {
          p->unload();
        }
      }

      void PluginCache::unloadPlugin(Plugin *op) const {
        _host->loadingStatus(false, op->getRawIdentifier(), op->getVersionMajor(), op->getVersionMinor());
      }

      /// handler for preparing to read in a chunk of XML from the cache, set up context to do this
      void PluginCache::beginXmlParsing(Plugin *p) {
        _currentPlugin = dynamic_cast<ImageEffectPlugin*>(p);
      }

      /// XML handler : element begins (everything is stored in elements and attributes)       
      void PluginCache::xmlElementBegin(const std::string &el, const std::map<std::string, std::string> &map)
      {
        if (el == "apiproperties") {
          return;
        }

        if (el == "context") {
          _currentContext = gImageEffectHost->makeDescriptor(_currentPlugin->getBinary()->getBundlePath(), _currentPlugin);
          _currentPlugin->addContext(map.at("name"), _currentContext);
          return;
        }

        if (el == "param" && _currentContext) {
          std::string pname = map.at("name");
          std::string ptype = map.at("type");

          _currentParam = _currentContext->paramDefine(ptype.c_str(), pname.c_str());
          return;
        }

        if (el == "clip" && _currentContext) {
          std::string cname = map.at("name");

          _currentClip = new ClipDescriptor(cname);
          _currentContext->addClip(cname, _currentClip);
          return;
        }

        if (_currentContext && _currentParam) {
          APICache::propertySetXMLRead(el, map, _currentParam->getProperties(), _currentProp);
          return;
        }

        if (_currentContext && _currentClip) {
          APICache::propertySetXMLRead(el, map, _currentClip->getProps(), _currentProp);
          return;
        }

        if (!_currentContext && !_currentParam) {
          APICache::propertySetXMLRead(el, map, _currentPlugin->getDescriptor().getProps(), _currentProp);
          return;
        }

        // a corrupt cache: a parse error discards it, and the plug-ins are scanned again
        throw std::runtime_error("malformed plugin cache: unexpected element <" + el + ">");
      }

      void PluginCache::xmlCharacterHandler(const std::string &) {
      }

      void PluginCache::xmlElementEnd(const std::string &el) {
        if (el == "param") {
          _currentParam = 0;
        }

        if (el == "context") {
          _currentContext = 0;
        }
      }

      void PluginCache::endXmlParsing() {
        _currentPlugin = 0;
        // these point into the plugin descriptors, which may be deleted
        // if the cache is discarded
        _currentContext = 0;
        _currentParam = 0;
        _currentClip = 0;
        _currentProp = 0;
      }

      void PluginCache::saveXML(Plugin *ip, std::ostream &os) const {
        ImageEffectPlugin *p = dynamic_cast<ImageEffectPlugin*>(ip);
        if (p) {
          p->saveXML(os);
        }
      }

      void PluginCache::confirmPlugin(Plugin *p, const std::list<std::string>& pluginPath) {
        ImageEffectPlugin *plugin = dynamic_cast<ImageEffectPlugin*>(p);
        if (!plugin) {
          return;
        }
        _plugins.push_back(plugin);

        std::map<std::string, ImageEffectPlugin *>::iterator foundPlugin = _pluginsByID.find(plugin->getIdentifier());
        if (foundPlugin != _pluginsByID.end()) {
          if (plugin->trumps(foundPlugin->second)) {
            foundPlugin->second = plugin;
          }
        } else {
          _pluginsByID.insert(std::make_pair(plugin->getIdentifier(),plugin));
        }

        MajorPlugin maj(plugin);

        if (_pluginsByIDMajor.find(maj) != _pluginsByIDMajor.end()) {
          ImageEffectPlugin *otherPlugin = _pluginsByIDMajor[maj];
          if (plugin->trumps(otherPlugin)) {
            _pluginsByIDMajor[maj] = plugin;
          } else if(plugin->equals(otherPlugin)) {
            // A static plug-in is considered prioritary
            if (plugin->getBinary()->isStaticallyLinkedPlugin()) {
              _pluginsByIDMajor[maj] = plugin;
            } else {
              // if the plugin *before* otherPlugin in the plugin path, insert it.
              const std::string& thisPath = plugin->getBinary()->getBundlePath();
              const std::string& otherPath = otherPlugin->getBinary()->getBundlePath();
              int thisRank = -1;
              int otherRank = -1;
              int rank = 0;
              for (std::list<std::string>::const_iterator paths= pluginPath.begin();
                   paths != pluginPath.end();
                   ++paths, ++rank) {
                const std::string& p = *paths;
                // check if the bundle path for each plugin starts with the current path item
                if (!thisPath.compare(0, p.size(), p)) {
                  thisRank = rank;
                }
                if (!otherPath.compare(0, p.size(), p)) {
                  otherRank = rank;
                }
              }
              assert(thisRank != -1 && otherRank != -1);
              if (thisRank < otherRank) {
                _pluginsByIDMajor[maj] = plugin;
              }
            }
          }
        } else {
          _pluginsByIDMajor[maj] = plugin;
        }
      }

      Plugin *PluginCache::newPlugin(PluginBinary *pb,
        int pi,
        OfxPlugin *pl) {
          ImageEffectPlugin *plugin = new ImageEffectPlugin(*this, pb, pi, pl);
          return plugin;
      }

      Plugin *PluginCache::newPlugin(PluginBinary *pb,
        int pi,
        const std::string &api,
        int apiVersion,
        const std::string &pluginId,
        const std::string &rawId,
        int pluginMajorVersion,
        int pluginMinorVersion) 
      {
        ImageEffectPlugin *plugin = new ImageEffectPlugin(*this, pb, pi, api, apiVersion, pluginId, rawId, pluginMajorVersion, pluginMinorVersion);
        return plugin;
      }

      void PluginCache::dumpToStdOut()
      {
        if (_pluginsByID.empty())
          std::cout << "No Plug-ins Found." << std::endl;

        for(std::map<std::string, ImageEffectPlugin *>::const_iterator it =  _pluginsByID.begin(); it != _pluginsByID.end(); ++it)
        {
          std::cout << "Plug-in:" << it->first << std::endl;
          std::cout << "\t" << "Filepath: " << it->second->getBinary()->getFilePath();
          std::cout<< "(" << it->second->getIndex() << ")" << std::endl;

          std::cout << "Contexts:" << std::endl;
          const std::set<std::string>& contexts = it->second->getContexts();
          for (std::set<std::string>::const_iterator it2 = contexts.begin(); it2 != contexts.end(); ++it2)
            std::cout << "\t* " << *it2 << std::endl;
          const Descriptor& d =  it->second->getDescriptor();
          std::cout << "Inputs:" << std::endl;
          const std::map<std::string, ClipDescriptor*>& inputs = d.getClips();
          for (std::map<std::string, ClipDescriptor*>::const_iterator it2 = inputs.begin(); it2 != inputs.end(); ++it2)
            std::cout << "\t\t* " << it2->first << std::endl;
        }
      }

    } // ImageEffect
      
  } // Host

} // OFX

