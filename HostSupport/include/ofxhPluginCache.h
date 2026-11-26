#ifndef OFX_PLUGIN_CACHE_H
#define OFX_PLUGIN_CACHE_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include <string>
#include <vector>
#include <list>
#include <memory> // unique_ptr
#include <set>
#include <iostream>

#include <stdio.h>

#include "expat.h"

#include "ofxCore.h"
#include "ofxhPropertySuite.h"
#include "ofxhPluginAPICache.h"
#include "ofxhBinary.h"

#ifdef WINDOWS
#include "tchar.h"
#endif

namespace OFX {

  namespace Host {

    typedef int(*OfxGetNumberOfPluginsFunc)(void);
    typedef OfxPlugin*(*OfxGetPluginFunc)(int);
    typedef OfxStatus(*OfxSetHostFunc)(const OfxHost*); ///< optional OFX 1.5 entry point, see ofxCore.h
    
    class Host;

    // forward delcarations
    class PluginDesc;   
    class Plugin;
    class PluginBinary;
    class PluginCache;

    /// C++ version of the information kept inside an OfxPlugin struct
    class PluginDesc  {
    protected :
      std::string _pluginApi;  ///< the API I implement
      int _apiVersion;         ///< the version of the API

      std::string _identifier; ///< the identifier of the plugin
      std::string _rawIdentifier; ///< the original identifier of the plugin
      int _versionMajor;       ///< the plugin major version
      int _versionMinor;       ///< the plugin minor version

    public:

      const std::string &getPluginApi() const {
        return _pluginApi;
      }
      
      int getApiVersion() const {
        return _apiVersion;
      }
      
      const std::string &getIdentifier() const {
        return _identifier;
      }

      const std::string &getRawIdentifier() const {
        return _rawIdentifier;
      }
      
      int getVersionMajor() const {
        return _versionMajor;
      }
      
      int getVersionMinor() const  {
        return _versionMinor;
      }

      PluginDesc() : _apiVersion(-1) {
      }

      virtual ~PluginDesc() {}

      PluginDesc(const std::string &api,
                 int apiVersion,
                 const std::string &identifier,
                 const std::string &rawIdentifier,
                 int versionMajor,
                 int versionMinor)
        : _pluginApi(api)
        , _apiVersion(apiVersion)
        , _identifier(identifier)
        , _rawIdentifier(rawIdentifier)
        , _versionMajor(versionMajor)
        , _versionMinor(versionMinor)
      {
      }


      /// constructor for the case where we have already loaded the plugin binary and 
      /// are populating this object from it. The identifier is kept as is: the spec doesn't say
      /// that it is case-insensitive (OfxPlugin::pluginIdentifier, ofxCore.h).
      explicit PluginDesc(OfxPlugin *ofxPlugin)
        : _pluginApi(ofxPlugin->pluginApi)
        , _apiVersion(ofxPlugin->apiVersion)
        , _identifier(ofxPlugin->pluginIdentifier)
        , _rawIdentifier(ofxPlugin->pluginIdentifier)
        , _versionMajor(ofxPlugin->pluginVersionMajor)
        , _versionMinor(ofxPlugin->pluginVersionMinor)
      {
      }

    };
    
    /// class that we use to manipulate a plugin
    class Plugin : public PluginDesc {
    /// owned by the PluginBinary it lives inside
    /// Plugins can only be pass about either by pointer or reference
    private :
      // not copyable
      Plugin(const Plugin&) = delete;
      Plugin &operator= (const Plugin&) = delete;

    protected :
      PluginBinary *_binary; ///< the file I live inside
      int           _index;  ///< where I live inside that file
    public :
      Plugin();

      PluginBinary *getBinary()
      {
        return _binary;
      }

      const PluginBinary *getBinary() const
      {
        return _binary;
      }

      int getIndex() const
      {
        return _index;
      }

      /// construct this based on the struct returned by the getNthPlugin() in the binary
      Plugin(PluginBinary *bin, int idx, OfxPlugin *o) : PluginDesc(o), _binary(bin), _index(idx)
      {
      }
      
      /// construct me from the cache
      Plugin(PluginBinary *bin, int idx, const std::string &api,
             int apiVersion, const std::string &identifier, 
             const std::string &rawIdentifier,
             int majorVersion, int minorVersion)
        : PluginDesc(api, apiVersion, identifier, rawIdentifier, majorVersion, minorVersion)
        , _binary(bin)
        , _index(idx) 
      {
      }

      virtual ~Plugin() {
      }

      virtual APICache::PluginAPICacheI &getApiHandler() = 0;

      bool trumps(const Plugin *other) const {
        int myMajor = getVersionMajor();
        int theirMajor = other->getVersionMajor();

        int myMinor = getVersionMinor();
        int theirMinor = other->getVersionMinor();

        if (myMajor > theirMajor) {
          return true;
        }
        
        if (myMajor == theirMajor && myMinor > theirMinor) {
          return true;
        }

        return false;
      }

      bool equals(const Plugin *other) const {
        int myMajor = getVersionMajor();
        int theirMajor = other->getVersionMajor();

        int myMinor = getVersionMinor();
        int theirMinor = other->getVersionMinor();

        if (myMajor == theirMajor && myMinor == theirMinor) {
          return true;
        }

        return false;
      }
    };

    class PluginHandle;

    /// class that represents a binary file which holds plugins
    class PluginBinary {
    /// has a set of plugins inside it and which it owns
    /// These are owned by a PluginCache
      friend class PluginHandle;
      friend class PluginCache; // scanPluginFiles() resets _binaryChanged

      // not copyable: a PluginBinary owns _binary and its plug-ins
      PluginBinary(const PluginBinary &) = delete;
      PluginBinary &operator=(const PluginBinary &) = delete;

    protected :
      std::unique_ptr<Binary> _binary; ///< our binary object, abstracted layer ontop of OS calls, defined in ofxhBinary.h, may not be valid if function pointers below are already set
      OfxGetNumberOfPluginsFunc _getNumberOfPlugins;
      OfxGetPluginFunc _getPluginFunc;
      std::string _filePath;          ///< full path to the file
      std::string _bundlePath;        ///< path to the .bundle directory
      std::vector<Plugin *> _plugins; ///< my plugins
      time_t _fileModificationTime;   ///< used as a time stamp to check modification times, used for caching
      off_t _fileSize;                ///< file size last time we check, used for caching
      bool _binaryChanged;            ///< whether the timestamp/filesize in this cache is different from that in the actual binary
      bool _binaryInvalid;            ///< whether we could open and stat the binary or not
      bool _setHostFailed;            ///< OfxSetHost returned kOfxStatFailed at the last load of the binary
      bool _scanRef;                  ///< loadPluginInfo() holds a reference to _binary, see releaseScanReference()
      int _loadGeneration;            ///< incremented each time _binary is loaded (dlopen)

    public :

      /// create one from the cache.  this will invoke the Binary() constructor which
      /// will stat() the file.
      explicit PluginBinary(const std::string &file, const std::string &bundlePath, time_t mtime, off_t size)
        : _binary(new Binary(file))
        , _getNumberOfPlugins(0)
        , _getPluginFunc(0)
        , _filePath(file)
        , _bundlePath(bundlePath)
        , _fileModificationTime(mtime)
        , _fileSize(size)
        , _binaryChanged(false)
        , _binaryInvalid(false)
        , _setHostFailed(false)
        , _scanRef(false)
        , _loadGeneration(0)
      {
        if (_binary->isInvalid()) {
          // the binary can't be stat()ed (e.g. it was removed, or is a dangling symlink): the
          // plug-ins the cache has for it are not read (readCache() skips them), and a scan
          // finds none (loadPluginInfo() does nothing). It is "changed" only if the cache has
          // another mtime or size than a failed stat() gives (0), so that a cache that already
          // records it as such isn't rewritten at each launch (dirty()).
          _binaryInvalid = true;
        }
        if (_fileModificationTime != _binary->getTime() || _fileSize != _binary->getSize()) {
          _binaryChanged = true;
        }
      }


      /// constructor which will open a library file, call things inside it, and then 
      /// create Plugin objects as appropriate for the plugins exported therefrom
      explicit PluginBinary(const std::string &file, const std::string &bundlePath, PluginCache *cache)
        : _binary(new Binary(file))
        , _getNumberOfPlugins(0)
        , _getPluginFunc(0)
        , _filePath(file)
        , _bundlePath(bundlePath)
        , _binaryChanged(false)
        , _binaryInvalid(false)
        , _setHostFailed(false)
        , _scanRef(false)
        , _loadGeneration(0)
      {
        if (_binary->isInvalid()) {
          _binaryInvalid = true;
        }
        loadPluginInfo(cache);
      }
      
       /// create the plug-in object from function pointers directly, this is useful for statically linked plug-ins.
      explicit PluginBinary(const std::string& hostAppBinFilePath,
                            OfxGetNumberOfPluginsFunc getNumberOfPlugins,
                            OfxGetPluginFunc getPlugins,
                            PluginCache *cache,
                            std::string* cachedHostAppBinFilePath = 0,
                            time_t* cachedFileModificationTime = 0,
                            off_t* cachedFileSize = 0)
      : _binary()
      , _getNumberOfPlugins(getNumberOfPlugins)
      , _getPluginFunc(getPlugins)
      , _filePath(hostAppBinFilePath)
      , _bundlePath(hostAppBinFilePath)
      , _plugins()
      , _fileModificationTime()
      , _fileSize()
      , _binaryChanged(true)
      , _binaryInvalid(false)
      , _setHostFailed(false)
      , _scanRef(false)
      , _loadGeneration(0)
      {
        _binaryInvalid = !Binary::getFileModTimeAndSize(hostAppBinFilePath, _fileModificationTime, _fileSize);
        if (cachedHostAppBinFilePath && cachedFileModificationTime && cachedFileSize) {
          if (_filePath == *cachedHostAppBinFilePath && _fileModificationTime == *cachedFileModificationTime && _fileSize == *cachedFileSize) {
            _binaryChanged = false;
          }
        }
        if (_binaryChanged) {
          loadPluginInfo(cache);
        }
      }

    
      /// dtor
      virtual ~PluginBinary();

      
      bool isStaticallyLinkedPlugin() const
      {
        return !_binary;
      }

      time_t getFileModificationTime() const {
      	return _fileModificationTime;
      }
    
      off_t getFileSize() const {
      	return _fileSize;
      }

      const std::string &getFilePath() const {
        return _filePath;
      }
      
      const std::string &getBundlePath() const {
        return _bundlePath;
      }
      
      bool hasBinaryChanged() const {
        return _binaryChanged;
      }

      bool isLoaded() const {
        return _binary ? _binary->isLoaded() : true;
      }

      /// incremented each time the binary is loaded (a statically linked binary is always loaded)
      int getLoadGeneration() const {
        return _loadGeneration;
      }

      /// whether OfxSetHost returned kOfxStatFailed at the last load of the binary
      bool setHostFailed() const {
        return _setHostFailed;
      }

      /// Release the reference to the binary that loadPluginInfo() took, so that it is unloaded once
      /// no PluginHandle holds it. Called by the plug-in cache after the scan of the binary.
      void releaseScanReference();
        
      bool isInvalid() const {
        return _binaryInvalid;
      }

      void addPlugin(Plugin *pe) {
        _plugins.push_back(pe);
      }

      /// load the binary and make its plug-ins; if it throws, there are no plug-ins, the binary is
      /// unloaded, and it is recorded as changed
      void loadPluginInfo(PluginCache *);

    private :
      /// unload (dropping exceptions) and delete the plug-ins, and clear the list; keep the Binary
      void deletePlugins();

    public :

      /// how many plugins?
      int getNPlugins() const {return (int)_plugins.size(); }

      /// get a plugin 
      Plugin &getPlugin(int idx) {return *_plugins[idx];}

      /// get a plugin 
      const Plugin &getPlugin(int idx) const {return *_plugins[idx];}
    };

    /// wrapper class for Plugin/PluginBinary.  use in a RAIA fashion to make sure the binary gets unloaded when needed and not before.
    class PluginHandle {
      PluginBinary *_b;
      OfxPlugin *_op;

      // not copyable: the destructor releases the reference to the binary that the constructor
      // took
      PluginHandle(const PluginHandle &) = delete;
      PluginHandle &operator=(const PluginHandle &) = delete;

    public:
      PluginHandle(Plugin *p, OFX::Host::Host *_host);
      virtual ~PluginHandle();

      /// NULL if the binary can't be loaded, OfxSetHost refused the host, or the binary no longer
      /// has this plug-in (a stale plug-in cache, see the PluginHandle constructor): the host should
      /// report it and offer to clear the cache.
      OfxPlugin *getOfxPlugin() {
        return _op;
      }

      OfxPlugin *operator->() {
        return _op;
      }
    };
    
    /// for later 
    struct PluginCacheSupportedApi {
      std::string api;
      int minVersion;
      int maxVersion;
      APICache::PluginAPICacheI *handler;

      PluginCacheSupportedApi(const std::string &_api, int _minVersion, int _maxVersion, APICache::PluginAPICacheI *_handler) :
        api(_api), minVersion(_minVersion), maxVersion(_maxVersion), handler(_handler)
      {
      }
      
      bool matches(const std::string &_api, int _version) const
      {
        if (_api == api && _version >= minVersion && _version <= maxVersion) {
          return true;
        }
        return false;
      }
    };

    /// Where we keep our plugins.
    ///
    /// A process should have one PluginCache that loads a given plug-in binary: two caches
    /// with the same binary each send kOfxActionLoad and kOfxActionUnload to it, independently
    /// (the library itself stays mapped, since dlopen counts references), so one cache can
    /// unload the plug-in while an instance from the other cache is alive.
    class PluginCache {
    protected :
      OFX::Host::Property::PropSpec* _hostSpec;
      OFX::Host::Host* _pluginHost; ///< the host passed to OfxSetHost when scanning a binary, see setScanHost()

      std::list<std::string>    _pluginPath;  ///< list of directories to look in
      std::set<std::string>     _nonrecursePath; ///< list of directories to look in (non-recursively)
      std::list<std::string>    _pluginDirs;  ///< list of directories we found
      std::list<PluginBinary *> _binaries; ///< all the binaries we know about, we own these
#ifdef OFX_USE_STATIC_PLUGINS
      PluginBinary* _staticBinary; /// < the statically linked binary
#endif
      std::list<Plugin *>       _plugins;  ///< all the plugins inside the binaries, we don't own these, populated from _binaries
      std::set<std::string>     _knownBinFiles;
#ifdef OFX_USE_STATIC_PLUGINS
      std::string               _hostAppBinFilePath; ///< file path of the host application binary, used to timestamp statically linked plug-ins
#endif

      PluginBinary *_xmlCurrentBinary;
      Plugin *_xmlCurrentPlugin;

      std::list<PluginCacheSupportedApi> _apiHandlers;

      void scanDirectory(std::set<std::string> &foundBinFiles, const std::string &dir, bool recurse);

      bool _ignoreCache;
      std::string _cacheVersion;

      bool _dirty;
      bool _enablePluginSeek;       ///< Turn off to make all seekPluginFile() calls return an empty string

      static bool _useStdOFXPluginsLocation;
      static PluginCache* gPluginCachePtr; ///< singleton plugin cache

    public:
      /// ctor, which inits _pluginPath to default locations and not much else
      PluginCache();

      /// dtor
      ~PluginCache();

      /// The host passed to a binary's OfxSetHost (OFX 1.5, optional entry point) when the binary is
      /// scanned for plug-ins. If not set, the image effect host (ImageEffect::gImageEffectHost) is used;
      /// it must exist before the scan. OfxSetHost is called once per load of a binary, after
      /// Host::loadingBinary(). A binary whose OfxSetHost returns kOfxStatFailed has no plug-ins for this
      /// host, and is cached as such until it changes: a host whose OfxSetHost answers may change (e.g.
      /// its name or version) should include them in setCacheVersion(). When a plug-in is loaded later,
      /// from the cache, OfxSetHost gets the host the plug-in is loaded for (e.g. the image effect host),
      /// so the scan host should be that same host.
      void setScanHost(OFX::Host::Host* host) { _pluginHost = host; }
      OFX::Host::Host* getScanHost() const;

      /// call this before the first call to getPluginCache() in order to ignore the standard OFX plugin path
      static void useStdOFXPluginsLocation(bool val) { _useStdOFXPluginsLocation = val; }

      /// get our plugin cache
      static PluginCache* getPluginCache();

      /// clear our plugin cache
      static void clearPluginCache();
        
#if defined (WINDOWS)
      /// The directory OFX/hostId (with backslashes) of the Common Files folder, CSIDL_PROGRAM_FILES_COMMON:
      /// by default the standard plug-in directory, OFX/Plugins. Computed for each call. Windows only,
      /// since the location of CSIDL_PROGRAM_FILES_COMMON varies.
      static std::wstring getStdOFXPluginPath(const std::string &hostId = "Plugins");
#endif
        
      /// get the list in which plugins are sought
      const std::list<std::string> &getPluginPath() {
        return _pluginPath;
      }

      /// was the cache outdated?
      bool dirty() const {
        return _dirty;
      }

      /// add a file to the plugin path
      void addFileToPath(const std::string &f, bool recurse=true) {
        _pluginPath.push_back(f);
        if (!recurse) {
          _nonrecursePath.insert(f);
        }
      }

      /// prepend a file to the plugin path
      void prependFileToPath(const std::string &f, bool recurse=true) {
        _pluginPath.push_front(f);
        if (!recurse) {
          _nonrecursePath.insert(f);
        }
      }
#ifdef OFX_USE_STATIC_PLUGINS
      void setHostApplicationBinPath(const std::string& f) {
        _hostAppBinFilePath = f;
      }
#endif

      /// specify which subdirectory of /usr/OFX or equivilant
      /// (as well as 'Plugins') to look in for plugins.
      void setPluginHostPath(const std::string &hostId);

      /// set the version string to write to the cache, 
      /// and also that we expect on cachess read in
      void setCacheVersion(const std::string &cacheVersion) {
        _cacheVersion = cacheVersion;
      }

      // populate the cache.  must call scanPluginFiles() after to check for changes.
      void readCache(std::istream &is);

      // seek a particular file on the OFX plugin path
      std::string seekPluginFile(const std::string &baseName) const;
      
      /// Sets behaviour of seekPluginFile().
      /// Enable (the default): normal operation; disable: returns an empty string instead
      void setPluginSeekEnabled(bool enabled) { _enablePluginSeek = enabled; }

      /// scan for plugins
      void scanPluginFiles();

      // write the plugin cache output file to the given stream
      void writePluginCache(std::ostream &os) const;
      
      // callback function for the XML
      void elementBeginCallback(void *userData, const XML_Char *name, const XML_Char **attrs);
      void elementCharCallback(void *userData, const XML_Char *data, int len);
      void elementEndCallback(void *userData, const XML_Char *name);

      /// register an API cache handler
      void registerAPICache(const std::string &api, int min, int max, APICache::PluginAPICacheI *apiCache) {
        _apiHandlers.push_back(PluginCacheSupportedApi(api, min, max, apiCache));
      }
      
      /// find the API cache handler for the given api/apiverson
      APICache::PluginAPICacheI* findApiHandler(const std::string &api, int apiver);

      /// obtain a list of plugins to walk through
      const std::list<Plugin *> &getPlugins() const {
        return _plugins;
      }
    };

  }
}

#endif
