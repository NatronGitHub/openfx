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

#include <assert.h>

#include <map>
#include <string>
#include <iostream>
#include <fstream>
#include <sstream> // stringstream
#include <stdexcept>

#include <string.h>
#include <stdlib.h>

#include "expat.h"

// ofx
#include "ofxCore.h"
#include "ofxImageEffect.h"

// ofx host
#include "ofxhBinary.h"
#include "ofxhPropertySuite.h"
#include "ofxhMemory.h"
#include "ofxhPluginAPICache.h"
#include "ofxhPluginCache.h"
#include "ofxhHost.h"
#include "ofxhXml.h"
#include "ofxhUtilities.h"

#if defined (__linux__) || defined (__FreeBSD__)

#define DIRLIST_SEP_CHARS ":;"
#define DIRSEP "/"
#include <dirent.h>
#include <sys/stat.h>
#include <sys/utsname.h>

#elif defined (__APPLE__)

#define DIRLIST_SEP_CHARS ";:"
#define DIRSEP "/"
#include <dirent.h>
#include <sys/stat.h>

#elif defined (WINDOWS)
#define DIRLIST_SEP_CHARS ";"
#define DIRSEP "\\"

#include "shlobj.h"
#endif

#include "ofxhArchDirs.h"

#if defined (WINDOWS) && (defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)) && !defined(_M_ARM64EC)
/// Is this x64 host running under emulation on Arm64 hardware?
/// IsWow64Process2 (Windows 10 1709 and later) is looked up at run time, so that this builds with older SDKs
/// and MinGW and runs on older Windows (which cannot emulate x64 on Arm64 anyway).
static bool isX64EmulatedOnArm64()
{
  typedef BOOL (WINAPI *IsWow64Process2Fn)(HANDLE, USHORT *, USHORT *);
  HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
  if (!kernel32) {
    return false;
  }
  IsWow64Process2Fn isWow64Process2 = (IsWow64Process2Fn)(void *)GetProcAddress(kernel32, "IsWow64Process2");
  if (!isWow64Process2) {
    return false;
  }
  USHORT processMachine = 0;
  USHORT nativeMachine = 0;
  if (!isWow64Process2(GetCurrentProcess(), &processMachine, &nativeMachine)) {
    return false;
  }
  return nativeMachine == 0xAA64; // IMAGE_FILE_MACHINE_ARM64
}
#endif

/// The bundle ARCHITECTURE directories this host looks in, most preferred first (see ofxhArchDirs.h).
static std::vector<std::string> getArchDirs()
{
  using namespace OFX::Host::ArchDirs;

  // the architecture this host was compiled for
#if defined(_M_ARM64EC)
  const ArchEnum arch = eArchArm64EC;
#elif defined(__x86_64) || defined(__x86_64__) || defined(_M_X64) || defined(_M_AMD64)
  const ArchEnum arch = eArchX86_64;
#elif defined(__i386) || defined(__i386__) || defined(_M_IX86)
  const ArchEnum arch = eArchX86;
#elif defined(__aarch64__) || defined(_M_ARM64)
  const ArchEnum arch = eArchArm64;
#else
  const ArchEnum arch = eArchOther;
#endif

  bool x64EmulatedOnArm64 = false;
  std::string machine; // `uname -m` for the compiled architecture (Linux, FreeBSD)
#if defined (__linux__) || defined (__FreeBSD__)
  // Use the compiled architecture for the common ones (a 32-bit host on a 64-bit kernel must not use the
  // kernel's name), and the kernel's machine name otherwise.
#  if defined(__aarch64__)
#    if defined(__linux__)
  machine = "aarch64";
#    else
  machine = "arm64";
#    endif
#  elif defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen == 64)
  machine = "riscv64";
#  elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
  machine = "ppc64le";
#  elif defined(__powerpc64__)
  machine = "ppc64";
#  elif defined(__s390x__)
  machine = "s390x";
#  elif defined(__loongarch64)
  machine = "loongarch64";
#  else
  struct utsname u;
  if (uname(&u) == 0) {
    machine = u.machine;
  }
#  endif
#endif

#if defined (__linux__)
  const OSEnum os = eOSLinux;
#elif defined (__FreeBSD__)
  const OSEnum os = eOSFreeBSD;
#elif defined (__APPLE__)
  const OSEnum os = eOSMacOS;
#else
  const OSEnum os = eOSWindows;
#  if (defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)) && !defined(_M_ARM64EC)
  x64EmulatedOnArm64 = isX64EmulatedOnArm64();
#  endif
#endif

  return archDirsFor(os, arch, x64EmulatedOnArm64, machine);
}

/// does the plug-in binary at this path exist?
static bool binaryExists(const std::string &path)
{
#if defined (WINDOWS)
  std::wstring wpath = OFX::utf8_to_utf16(path);
  DWORD attr = GetFileAttributesW(wpath.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
#else
  struct stat st;
  return stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode);
#endif
}


bool OFX::Host::PluginCache::_useStdOFXPluginsLocation = true;
OFX::Host::PluginCache* OFX::Host::PluginCache::gPluginCachePtr = 0;

// Define this to enable ofx plugin cache debug messages.
//#define CACHE_DEBUG

using namespace OFX::Host;


/// try to open the plugin bundle object and query it for plugins
void PluginBinary::loadPluginInfo(PluginCache *cache) {      
  if (isInvalid()) {
    return;
  }

  if (_binary) {
    _fileModificationTime = _binary->getTime();
    _fileSize = _binary->getSize();
    
    // Take a reference to load the binary only once per session. It will
    // eventually be unloaded in the destructor (see below).
    // This avoid lots of useless calls to dlopen()/dlclose().
    if (!_binary->isLoaded()) {
      _binary->ref();
    }
  }

  
  if (_binary) {
    _getNumberOfPlugins = (OfxGetNumberOfPluginsFunc) _binary->findSymbol("OfxGetNumberOfPlugins");
    _getPluginFunc = (OfxGetPluginFunc) _binary->findSymbol("OfxGetPlugin");
  }
  
  
  if (_getNumberOfPlugins == 0 || _getPluginFunc == 0) {
    if (_binary) {
      _binary->setInvalid(true);
    }
    
  } else {
    int pluginCount = (*_getNumberOfPlugins)();
    _plugins.clear();
    if (pluginCount < 0) {
      // a broken binary must not abort the whole scan
      pluginCount = 0;
    }
    _plugins.reserve(pluginCount);
    
    for (int i=0;i<pluginCount;i++) {
      OfxPlugin *plug = (*_getPluginFunc)(i);
      if (!plug || !plug->pluginApi || !plug->pluginIdentifier || !plug->setHost || !plug->mainEntry) {
        std::cerr << "OFX: " << _filePath << ": OfxGetPlugin(" << i << ") returned an invalid plug-in, ignoring it" << std::endl;
        continue;
      }
      
      // "if the plug-in's API or its version are not supported, the plug-in
      // is ignored and we skip to the next one"
      APICache::PluginAPICacheI *api = cache->findApiHandler(plug->pluginApi, plug->apiVersion);
      if (!api) {
        continue;
      }
      
      _plugins.push_back(api->newPlugin(this, i, plug));
    }
  }
}

PluginBinary::~PluginBinary() {
  std::vector<Plugin*>::iterator i = _plugins.begin();
  while (i != _plugins.end()) {
    const APICache::PluginAPICacheI &api = (*i)->getApiHandler();
    api.unloadPlugin(*i);
    delete *i;
    i++;
  }
  // release the last reference to the binary, which should unload it
  // if this reference was taken by loadPluginInfo().
  if (_binary) {
    if (_binary->isLoaded()) {
      _binary->unref();
    }
    assert(!_binary->isLoaded());
  }
}

PluginHandle::PluginHandle(Plugin *p, OFX::Host::Host *host)
{
  _b = p->getBinary();
  if (_b->_binary) {
    _b->_binary->ref();
  }
  _op = 0;
  OfxPlugin* (*getPlug)(int) = 0;
  if (_b->_getPluginFunc) {
    getPlug = _b->_getPluginFunc;
  }
  if (!getPlug && _b->_binary) {
    getPlug = (OfxPlugin*(*)(int)) _b->_binary->findSymbol("OfxGetPlugin");
  }
  if (getPlug) {
    _op = getPlug(p->getIndex());
    if (_op) {         
      _op->setHost(host->getHandle());
    }
  }
}

PluginHandle::~PluginHandle() {
  if (_b->_binary) {
    _b->_binary->unref();
  }
}


#if defined (WINDOWS)
const std::wstring&
PluginCache::getStdOFXPluginPath(const std::string &hostId)
{
  static std::wstring ret;
  static int gotIt = 0;
  if(!gotIt) {
	gotIt = 1;
    wchar_t buffer[MAX_PATH];
    SHGetFolderPathW(NULL, CSIDL_PROGRAM_FILES_COMMON, NULL, SHGFP_TYPE_CURRENT, buffer);
	std::wstring str;
	str.append(L"\\OFX\\");
	str.append(OFX::utf8_to_utf16(hostId));
	wcscat_s(buffer, MAX_PATH, str.c_str());
	ret = std::wstring(buffer);
  }
  return ret;	   
}
#endif

static
std::string OFXGetEnv(const char* e)
{
#if defined(WINDOWS) && !defined(__MINGW32__)
  size_t requiredSize;
  getenv_s(&requiredSize, 0, 0, e);
  std::vector<char> buffer(requiredSize);
  if(requiredSize >0)
    {
      getenv_s(&requiredSize, &buffer.front(), requiredSize, e);
      return &buffer.front();
    }
  return "";
#else
  if(getenv(e))
    return getenv(e);
#endif
  return "";
}

PluginCache* PluginCache::getPluginCache()
{
  if(!gPluginCachePtr)
    gPluginCachePtr = new PluginCache();
  return gPluginCachePtr;
}

void PluginCache::clearPluginCache()
{
  delete gPluginCachePtr;
  gPluginCachePtr = 0;
}

PluginCache::~PluginCache()
{
  for(std::list<PluginBinary *>::iterator it=_binaries.begin(); it != _binaries.end(); ++it) {
    delete (*it);
  }
  _binaries.clear();
}

PluginCache::PluginCache()
: _hostSpec(NULL)
#ifdef OFX_USE_STATIC_PLUGINS
, _staticBinary(NULL)
#endif
, _xmlCurrentBinary(NULL)
, _xmlCurrentPlugin(NULL)
{
  _cacheVersion = "";
  _ignoreCache = false;
  _dirty = false;
  _enablePluginSeek = true;
  
  std::string s = OFXGetEnv("OFX_PLUGIN_PATH");
  
    
  while (s.length()) {
      
    int spos = int(s.find_first_of(DIRLIST_SEP_CHARS));
    
    std::string path;
    
    if (spos != -1) {
      path = s.substr(0, spos);
      s = s.substr(spos+1);
    } 
    else {
      path = s;
      s = "";
    }
    
    _pluginPath.push_back(path);
  }

  if (!_useStdOFXPluginsLocation)
    return;

#if defined(WINDOWS)

  std::wstring wpath = getStdOFXPluginPath();
  std::string path = OFX::utf16_to_utf8(wpath);

  _pluginPath.push_back(path);
  _pluginPath.push_back("C:\\Program Files\\Common Files\\OFX\\Plugins");
#endif
#if defined(__linux__) || defined(__FreeBSD__)
  _pluginPath.push_back("/usr/OFX/Plugins");
#endif
#if defined(__APPLE__)
  _pluginPath.push_back("/Library/OFX/Plugins");
#endif
}

void PluginCache::setPluginHostPath(const std::string &hostId) {
#if defined(WINDOWS)
  std::wstring wpath = getStdOFXPluginPath(hostId);
  std::string path = OFX::utf16_to_utf8(wpath);

  _pluginPath.push_back(path);
  _pluginPath.push_back("C:\\Program Files\\Common Files\\OFX\\" + hostId);
#endif
#if defined(__linux__) || defined(__FreeBSD__)
  _pluginPath.push_back("/usr/OFX/" + hostId);
#endif
#if defined(__APPLE__)
  _pluginPath.push_back("/Library/OFX/" + hostId);
#endif
}

void PluginCache::scanDirectory(std::set<std::string> &foundBinFiles, const std::string &dir, bool recurse)
{
#ifdef CACHE_DEBUG
  printf("looking in %s for plugins\n", dir.c_str());
#endif
  
#if defined (WINDOWS)
  WIN32_FIND_DATAW findData;
  HANDLE findHandle;
#else
  DIR *d = opendir(dir.c_str());
  if (!d) {
    return;
  }
#endif
  
  _pluginDirs.push_back(dir.c_str());

  const std::vector<std::string> archDirs = getArchDirs();
  
#if defined (UNIX)
  while (dirent *de = readdir(d))
#elif defined (WINDOWS)
    {

      std::wstring ws = OFX::utf8_to_utf16((dir + "\\*"));
      findHandle = FindFirstFileW(ws.c_str(), &findData);
    }
  if (findHandle == INVALID_HANDLE_VALUE) 
    {
      return;
    }

  while (1)
#endif
    {
#if defined (UNIX)
      std::string name = de->d_name;
      bool isdir = true;
#else
      std::wstring wname = findData.cFileName;
      std::string name = OFX::utf16_to_utf8(wname);

      bool isdir = (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
#endif
      if (name.find(".ofx.bundle") != std::string::npos) {
        std::string barename = name.substr(0, name.length() - strlen(".bundle"));
        std::string bundlename = dir + DIRSEP + name;
        // The binary in the most preferred ARCHITECTURE directory that has one, or in the most
        // preferred one if none has it (it is then recorded as an invalid binary, as before).
        // E.g. an x86-64 macOS host checks MacOS-x86-64 then MacOS, an x64 Windows host running
        // under Arm64 emulation checks Win-arm64ec then win64 (see ofxhArchDirs.h).
        const std::string contents = dir + DIRSEP + name + DIRSEP "Contents" DIRSEP;
        std::string binpath = archDirs.empty() ? std::string() : contents + archDirs[0] + DIRSEP + barename;
        for (std::size_t a = 0; a < archDirs.size(); ++a) {
          const std::string candidate = contents + archDirs[a] + DIRSEP + barename;
          if (binaryExists(candidate)) {
            binpath = candidate;
            break;
          }
        }

        if (binpath.empty()) {
          // unknown host architecture: there is no binary we could load
        } else if (_knownBinFiles.find(binpath) == _knownBinFiles.end()) {
#ifdef CACHE_DEBUG
          printf("found non-cached binary %s\n", binpath.c_str());
#endif
          _dirty = true;

          // the binary was not in the cache
          PluginBinary *pb = new PluginBinary(binpath, bundlename, this);
          _binaries.push_back(pb);
          _knownBinFiles.insert(binpath);
          foundBinFiles.insert(binpath);

          for (int j=0;j<pb->getNPlugins();j++) {
            Plugin *plug = &pb->getPlugin(j);
            const APICache::PluginAPICacheI &api = plug->getApiHandler();
            api.loadFromPlugin(plug);
          }
        } else {
#ifdef CACHE_DEBUG
          printf("found cached binary %s\n", binpath.c_str());
#endif
        }
        // insert the final path in the list of found files
        if (!binpath.empty()) {
          foundBinFiles.insert(binpath);
        }
      } else {
        if (isdir && (recurse && !name.empty() && name[0] != '@' && name[name.size() - 1] != '.')) {
          scanDirectory(foundBinFiles, dir + DIRSEP + name, recurse);
        }
      }
#if defined(WINDOWS)
      int rval = FindNextFileW(findHandle, &findData);
      
      if (rval == 0) {
        break;
      }
#endif
    }
  
#if defined(UNIX)
  closedir(d);
#else
  FindClose(findHandle);
#endif
}

std::string PluginCache::seekPluginFile(const std::string &baseName) const {
  // Exit early if disabled
  if (!_enablePluginSeek)
    return "";
  
  for (std::list<std::string>::const_iterator paths= _pluginDirs.begin();
       paths != _pluginDirs.end();
       paths++) {
    std::string candidate = *paths + DIRSEP + baseName;
    FILE *f = fopen(candidate.c_str(), "r");
    if (f) {
      fclose(f);
      return candidate;
    }
  }
  return "";
}

void PluginCache::scanPluginFiles()
{
  std::set<std::string> foundBinFiles;
  
  for (std::list<std::string>::iterator paths= _pluginPath.begin();
       paths != _pluginPath.end();
       paths++) {
    scanDirectory(foundBinFiles, *paths, _nonrecursePath.find(*paths) == _nonrecursePath.end());
  }

#ifdef OFX_USE_STATIC_PLUGINS
  // Now add all static plug-ins

  // If already loaded from the cache, the static binary must point to the same host binary file
  assert(!_staticBinary || _staticBinary->getFilePath() == _hostAppBinFilePath);
  if (!_staticBinary) {
    _staticBinary = new PluginBinary(_hostAppBinFilePath, &OfxGetNumberOfPlugins,&OfxGetPlugin,this, 0, 0);
    if (_staticBinary->isInvalid()) {
      std::cerr << "WARNING: ignoring statically linked plug-ins because host application binary file path (" << _hostAppBinFilePath << ") is wrongly set" << std::endl;
      delete _staticBinary;
      _staticBinary = 0;
    } else {
      _dirty = true;
      for (int j=0;j<_staticBinary->getNPlugins();j++) {
         Plugin *plug = &_staticBinary->getPlugin(j);
         const APICache::PluginAPICacheI &api = plug->getApiHandler();
         api.loadFromPlugin(plug);
      }
      _binaries.push_back(_staticBinary);
    }
  }
#endif
  
  std::list<PluginBinary *>::iterator i=_binaries.begin();
  while (i!=_binaries.end()) {
    PluginBinary *pb = *i;
    
    if (!pb->isStaticallyLinkedPlugin() && foundBinFiles.find(pb->getFilePath()) == foundBinFiles.end()) {
      
      // the binary was in the cache, but was not on the path
      
      _dirty = true;
      i = _binaries.erase(i);
      delete pb;
      
    } else {
      
      bool binChanged = pb->hasBinaryChanged();
      
      // the binary was in the cache, but the binary has changed and thus we need to reload
      if (binChanged) {
        // The static PluginBinary already had loadPluginInfo called anyway at this point
        if (!pb->isStaticallyLinkedPlugin()) {
          pb->loadPluginInfo(this);
        }
        _dirty = true;
      }
      
      for (int j=0;j<pb->getNPlugins();j++) {
        Plugin *plug = &pb->getPlugin(j);
        APICache::PluginAPICacheI &api = plug->getApiHandler();
        
        if (binChanged) {
          api.loadFromPlugin(plug);
        }
        
        std::string reason;
        
        if (api.pluginSupported(plug, reason)) {
          _plugins.push_back(plug);
          api.confirmPlugin(plug, _pluginPath);
        } else {
          std::cerr << "ignoring plugin " << plug->getIdentifier() <<
            " as unsupported (" << reason << ")" << std::endl;
        }
      }
      
      i++;
    }
  }
}


namespace {
  /// state shared between readCache() and the XML parser callbacks
  struct XMLReadState {
    XML_Parser parser;
    bool failed;
    std::string error;
  };

  /// C++ exceptions must not propagate through the expat C code: record the
  /// error, stop the parser, and let readCache() report it.
  void stopParserOnException(void *userData, const char *what) {
    XMLReadState *state = static_cast<XMLReadState*>(userData);
    if (!state->failed) {
      state->failed = true;
      state->error = what;
    }
    XML_StopParser(state->parser, XML_FALSE);
  }
}

/// callback for XML parser
static void elementBeginHandler(void *userData, const XML_Char *name, const XML_Char **atts) {
  try {
    PluginCache::getPluginCache()->elementBeginCallback(userData, name, atts);
  } catch (const std::exception &e) {
    stopParserOnException(userData, e.what());
  } catch (...) {
    stopParserOnException(userData, "unknown exception");
  }
}

/// callback for XML parser
static void elementCharHandler(void *userData, const XML_Char *data, int len) {
  try {
    PluginCache::getPluginCache()->elementCharCallback(userData, data, len);
  } catch (const std::exception &e) {
    stopParserOnException(userData, e.what());
  } catch (...) {
    stopParserOnException(userData, "unknown exception");
  }
}

/// callback for XML parser
static void elementEndHandler(void *userData, const XML_Char *name) {
  try {
    PluginCache::getPluginCache()->elementEndCallback(userData, name);
  } catch (const std::exception &e) {
    stopParserOnException(userData, e.what());
  } catch (...) {
    stopParserOnException(userData, "unknown exception");
  }
}

/// 64-bit safe conversions for the binary modification time and size in the cache:
/// writing them as int truncated mtimes after 2038 and sizes above 2GB, so that such
/// binaries were considered changed (and described again) at every launch.
static long long cacheStringToInt64(const std::string &s)
{
  return strtoll(s.c_str(), NULL, 10);
}

static std::string cacheInt64ToString(long long v)
{
  std::ostringstream o;
  o << v;
  return o.str();
}

static bool mapHasAll(const std::map<std::string, std::string> &attmap, const char **atts) {
  while (*atts) {
    if (attmap.find(*atts) == attmap.end()) {
      return false;
    }
    atts++;
  }
  return true;
}

void PluginCache::elementBeginCallback(void */*userData*/, const XML_Char *name, const XML_Char **atts) {
  if (_ignoreCache) {
    return;
  }
  
  std::string ename = name;
  std::map<std::string, std::string> attmap;
  
  while (*atts) {
    attmap[atts[0]] = atts[1];
    atts += 2;
  }
  
  /// XXX: validate in general
  
  if (ename == "cache") {
    std::string cacheversion = attmap["version"];
    if (cacheversion != _cacheVersion) {
#ifdef CACHE_DEBUG
      printf("mismatched version, ignoring cache (got '%s', wanted '%s')\n",
             cacheversion.c_str(),
             _cacheVersion.c_str());
#endif
      _ignoreCache = true;
    }
  }
  
  if (ename == "binary") {
    // "static_bin" is optional: caches written before Nov. 2017 (edc1d81a) don't have it
    const char *binAtts[] = {"path", "bundle_path", "mtime", "size", NULL};
    
    if (!mapHasAll(attmap, binAtts)) {
      throw std::runtime_error("malformed plugin cache: <binary> element with missing attributes");
    }

#ifdef OFX_USE_STATIC_PLUGINS
    bool isStaticLinkedCachedBinary = attmap.count("static_bin") && OFX::Host::Property::stringToInt(attmap["static_bin"]);
#endif

    std::string fname = attmap["path"];
    std::string bname = attmap["bundle_path"];
    time_t mtime = (time_t)cacheStringToInt64(attmap["mtime"]);
    off_t size = (off_t)cacheStringToInt64(attmap["size"]);

    PluginBinary* pb;
#ifdef OFX_USE_STATIC_PLUGINS
    if (isStaticLinkedCachedBinary) {
      // only 1 static binary allowed!
      if (_staticBinary) {
        _ignoreCache = true;
        return;
      }
      // We need to provide the 2 function pointers for the static binary
      pb = new PluginBinary(_hostAppBinFilePath, &OfxGetNumberOfPlugins,&OfxGetPlugin, this, &fname, &mtime, &size);
      _staticBinary = pb;
    } else
#endif
    {
      pb = new PluginBinary(fname, bname, mtime, size);
    }
    _xmlCurrentBinary = pb;
    _binaries.push_back(_xmlCurrentBinary);
    _knownBinFiles.insert(fname);
    
    return;
  }
  
  if (ename == "plugin" && _xmlCurrentBinary && !_xmlCurrentBinary->hasBinaryChanged()) {
    const char *plugAtts[] = {"api", "name", "index", "api_version", "major_version", "minor_version", NULL};
    
    if (!mapHasAll(attmap, plugAtts)) {
      throw std::runtime_error("malformed plugin cache: <plugin> element with missing attributes");
    }
    
    std::string api = attmap["api"];
    std::string rawIdentifier = attmap["name"];
    
    std::string identifier = rawIdentifier;
    
    // Who says the pluginIdentifier is case-insensitive? OFX 1.3 spec doesn't mention this.
    // http://openfx.sourceforge.net/Documentation/1.3/ofxProgrammingReference.html#id472588
    //for (size_t i=0;i<identifier.size();i++) {
    //  identifier[i] = tolower(identifier[i]);
    //}
    
    int idx = OFX::Host::Property::stringToInt(attmap["index"]);
    int api_version = OFX::Host::Property::stringToInt(attmap["api_version"]);
    int major_version = OFX::Host::Property::stringToInt(attmap["major_version"]);
    int minor_version = OFX::Host::Property::stringToInt(attmap["minor_version"]);
    
    APICache::PluginAPICacheI *apiCache = findApiHandler(api, api_version);
    if (apiCache) {
      
      Plugin *pe = apiCache->newPlugin(_xmlCurrentBinary, idx, api, api_version, identifier, rawIdentifier, major_version, minor_version);
      _xmlCurrentBinary->addPlugin(pe);
      _xmlCurrentPlugin = pe;
      apiCache->beginXmlParsing(pe);
    }
    
    return;
  }
  
  if (_xmlCurrentPlugin) {
    APICache::PluginAPICacheI &api = _xmlCurrentPlugin->getApiHandler();
    api.xmlElementBegin(name, attmap);
  }
  
}

void PluginCache::elementCharCallback(void */*userData*/, const XML_Char *data, int size)
{
  if (_ignoreCache) {
    return;
  }
  
  std::string s(data, size);
  if (_xmlCurrentPlugin) {
    APICache::PluginAPICacheI &api = _xmlCurrentPlugin->getApiHandler();
    api.xmlCharacterHandler(s);
  } else {
    /// XXX: we only want whitespace
  }
}

void PluginCache::elementEndCallback(void */*userData*/, const XML_Char *name) {
  if (_ignoreCache) {
    return;
  }
  
  std::string ename = name;
  
  /// XXX: validation?
  
  if (ename == "plugin") {
    if (_xmlCurrentPlugin) {
      APICache::PluginAPICacheI &api = _xmlCurrentPlugin->getApiHandler();
      api.endXmlParsing();
    }
    _xmlCurrentPlugin = 0;
    return;
  }
  
  if (ename == "bundle") {
    _xmlCurrentBinary = 0;
    return;
  }
  
  if (_xmlCurrentPlugin) {
    APICache::PluginAPICacheI &api = _xmlCurrentPlugin->getApiHandler();
    api.xmlElementEnd(name);
  }
}

void PluginCache::readCache(std::istream &ifs) {
  // Remember the state before reading, so that a cache that cannot be read
  // completely is discarded entirely: a partially read cache would otherwise
  // be trusted by scanPluginFiles() for the binaries it lists.
  const std::size_t nBinaries = _binaries.size();
  const std::set<std::string> knownBinFiles = _knownBinFiles;
#ifdef OFX_USE_STATIC_PLUGINS
  PluginBinary *staticBinary = _staticBinary;
#endif

  XML_Parser xP = XML_ParserCreate(NULL);
  if (!xP) {
    throw std::runtime_error("Error creating XML parser");

  }
  XMLReadState state;
  state.parser = xP;
  state.failed = false;
  XML_SetUserData(xP, &state);
  XML_SetElementHandler(xP, elementBeginHandler, elementEndHandler);
  XML_SetCharacterDataHandler(xP, elementCharHandler);
  std::string errorDesc;
  int done = false;
  char buf[4096];
  while (errorDesc.empty() && !done && !ifs.eof()) {
    if (!ifs.good()) {
      errorDesc = "Error reading cache stream";
      break;
    }
    ifs.read(buf, sizeof(buf));
    int n = (int)ifs.gcount();
    done = (n != sizeof(buf));

    if (XML_Parse(xP, buf, n, done) == XML_STATUS_ERROR) {
      std::stringstream ss;
#if XML_MAJOR_VERSION > 2 || (XML_MAJOR_VERSION == 2 && XML_MINOR_VERSION >= 9)
      // expat >= 2.9.0 deprecates the XML_Size variants (prone to 32-bit wrap-around)
      uint64_t errorLine = XML_GetCurrentLineNumber64(xP);
      uint64_t errorCol = XML_GetCurrentColumnNumber64(xP);
#else
      XML_Size errorLine = XML_GetCurrentLineNumber(xP);
      XML_Size errorCol = XML_GetCurrentColumnNumber(xP);
#endif
      if (state.failed) {
        ss << "Error reading plugin cache at line " << errorLine << ":" << errorCol;
        ss << ": " << state.error;
      } else {
        XML_Error errorCode = XML_GetErrorCode(xP);
        const XML_LChar *errorString = XML_ErrorString(errorCode);
        ss << "XML parsing error at line " << errorLine << ":" << errorCol;
        ss << ": " << errorString;
      }
      errorDesc = ss.str();
    }
  }
  XML_ParserFree(xP);

  if (!errorDesc.empty()) {
    // roll back everything that was read from the cache
    if (_xmlCurrentPlugin) {
      _xmlCurrentPlugin->getApiHandler().endXmlParsing();
    }
    _xmlCurrentPlugin = 0;
    _xmlCurrentBinary = 0;
    while (_binaries.size() > nBinaries) {
      delete _binaries.back();
      _binaries.pop_back();
    }
    _knownBinFiles = knownBinFiles;
#ifdef OFX_USE_STATIC_PLUGINS
    _staticBinary = staticBinary;
#endif
    throw std::runtime_error(errorDesc);
  }
}

void PluginCache::writePluginCache(std::ostream &os) const {
#ifdef CACHE_DEBUG
  printf("writing pluginCache with version = %s\n", _cacheVersion.c_str());
#endif
  
  os << "<cache version=\"" << _cacheVersion << "\">\n";
  for (std::list<PluginBinary *>::const_iterator i=_binaries.begin();i!=_binaries.end();i++) {
    PluginBinary *b = *i;
    
    std::vector<Plugin*> plugins(b->getNPlugins());
    for (std::size_t j = 0; j < plugins.size(); ++j) {
      plugins[j] = &b->getPlugin((int)j);
    }
    
    os << "<bundle>\n";
    os << "  <binary "
       << XML::attribute("static_bin", int(b->isStaticallyLinkedPlugin()))
       << XML::attribute("bundle_path", b->getBundlePath())
       << XML::attribute("path", b->getFilePath())
       << XML::attribute("mtime", cacheInt64ToString((long long)b->getFileModificationTime()))
       << XML::attribute("size", cacheInt64ToString((long long)b->getFileSize())) << "/>\n";
    
    for (int j=0;j<(int)plugins.size();j++) {
      Plugin *p = plugins[j];
      
      
      os << "  <plugin " 
         << XML::attribute("name", p->getRawIdentifier()) 
         << XML::attribute("index", p->getIndex()) 
         << XML::attribute("api", p->getPluginApi())
         << XML::attribute("api_version", p->getApiVersion())
         << XML::attribute("major_version", p->getVersionMajor())
         << XML::attribute("minor_version", p->getVersionMinor())
         << ">\n";
      																      
      const APICache::PluginAPICacheI &api = p->getApiHandler();
      os << "    <apiproperties>\n"; 
      api.saveXML(p, os);
      os << "    </apiproperties>\n";											    
				
      os << "  </plugin>\n";
    }
    os << "</bundle>\n";
  }
  os << "</cache>\n";
}


APICache::PluginAPICacheI *PluginCache::findApiHandler(const std::string &api, int version) {
  std::list<PluginCacheSupportedApi>::iterator i = _apiHandlers.begin();
  while (i != _apiHandlers.end()) {
    if (i->matches(api, version)) {
      return i->handler;
    }
    i++;
  }
  return 0;
}
