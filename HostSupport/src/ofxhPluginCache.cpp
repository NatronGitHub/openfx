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
#include <cstdio> // fopen (binaryMatchesHost)
#include <algorithm> // std::find
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
#include "ofxhImageEffect.h" // gImageEffectHost, the default host for OfxSetHost
#include "ofxhImageEffectAPI.h" // ImageEffect::PluginCache (scanPluginFiles())
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
#if defined(_M_ARM64EC) || defined(__arm64ec__) // MSVC, llvm-mingw (which also defines __x86_64__)
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
  std::string machine; // the machine name for the compiled architecture (Linux, FreeBSD): see below
#if defined (__linux__) || defined (__FreeBSD__)
  // Use the compiled architecture for the common ones (a 32-bit host on a 64-bit kernel must not use the
  // kernel's name), and the kernel's machine name otherwise. The names must match the plug-in build
  // systems (Support/Plugins/Makefile.master, Examples/Makefile.master, cmake/OpenFX.cmake):
  // - Linux: `uname -m` names, as in the OFX packaging reference ("Linux-${uname -m} which for arm64
  //   should be Linux-aarch64"), and armv7l/armv6l for 32-bit Arm;
  // - FreeBSD (not covered by the spec): FreeBSD's MACHINE_ARCH, i.e. `uname -p` (aarch64, armv7,
  //   riscv64, powerpc64, powerpc64le), because `uname -m` is the platform (arm64, riscv, powerpc).
#  if defined(__linux__)
#    define OFXH_LINUX_OR_FREEBSD(onLinux, onFreeBSD) onLinux
#  else
#    define OFXH_LINUX_OR_FREEBSD(onLinux, onFreeBSD) onFreeBSD
#  endif
#  if defined(__aarch64__)
  machine = "aarch64";
#  elif defined(__arm__)
#    if defined(__ARM_ARCH) && (__ARM_ARCH >= 7)
  machine = OFXH_LINUX_OR_FREEBSD("armv7l", "armv7");
#    else
  machine = OFXH_LINUX_OR_FREEBSD("armv6l", "armv6");
#    endif
#  elif defined(__riscv) && defined(__riscv_xlen) && (__riscv_xlen == 64)
  machine = "riscv64";
#  elif defined(__powerpc64__) && defined(__LITTLE_ENDIAN__)
  machine = OFXH_LINUX_OR_FREEBSD("ppc64le", "powerpc64le");
#  elif defined(__powerpc64__)
  machine = OFXH_LINUX_OR_FREEBSD("ppc64", "powerpc64");
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
#  undef OFXH_LINUX_OR_FREEBSD
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

#if defined (__linux__) || defined (__FreeBSD__)
  // Linux: the spec's folder name is Linux-${uname -m}; FreeBSD: an extra name to search
  std::string unameMachine;
  {
    struct utsname ku;
    if (uname(&ku) == 0) {
      unameMachine = ku.machine;
    }
  }
  std::vector<std::string> dirs = archDirsFor(os, arch, x64EmulatedOnArm64, machine, sizeof(void *) == 8, unameMachine);
#else
  std::vector<std::string> dirs = archDirsFor(os, arch, x64EmulatedOnArm64, machine);
#endif
  return dirs;
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


// Does the plug-in binary at path match the host process (word size, CPU)? Reads the ELF, Mach-O (thin or
// fat) or PE header. Unreadable files and unknown formats count as a match, so that nothing that loaded
// before is rejected; a mismatch is only reported when the header says so.
// This lets the host follow the spec's folder order (e.g. Linux-${uname -m}, then MacOS) and still skip a
// binary it can't load (a 64-bit Linux-aarch64 plug-in in a 32-bit host on an aarch64 kernel, or an
// arm64-only MacOS binary on an Intel Mac), using the next folder instead.
static unsigned int rd16(const unsigned char *p, bool le) { return le ? (p[0] | (p[1] << 8)) : ((p[0] << 8) | p[1]); }
static unsigned long rd32(const unsigned char *p, bool le)
{
  return le ? ((unsigned long)p[0] | ((unsigned long)p[1] << 8) | ((unsigned long)p[2] << 16) | ((unsigned long)p[3] << 24))
            : (((unsigned long)p[0] << 24) | ((unsigned long)p[1] << 16) | ((unsigned long)p[2] << 8) | (unsigned long)p[3]);
}

static bool binaryMatchesHost(const std::string &path)
{
  unsigned char h[4096];
  size_t n = 0;
#if defined (WINDOWS)
  FILE *f = _wfopen(OFX::utf8_to_utf16(path).c_str(), L"rb");
#else
  FILE *f = fopen(path.c_str(), "rb");
#endif
  if (!f) {
    return true;
  }
  n = fread(h, 1, sizeof(h), f);
  fclose(f);
  const bool host64 = sizeof(void *) == 8;
  // ELF (Linux, FreeBSD)
  if (n >= 20 && h[0] == 0x7f && h[1] == 'E' && h[2] == 'L' && h[3] == 'F') {
    if ((h[4] == 2) != host64) {
      return false; // ELFCLASS64 vs ELFCLASS32
    }
    const unsigned int m = rd16(h + 18, h[5] == 1);
#  if defined(__x86_64__) || defined(_M_X64)
    return m == 62;   // EM_X86_64
#  elif defined(__i386__) || defined(_M_IX86)
    return m == 3;    // EM_386
#  elif defined(__aarch64__)
    return m == 183;  // EM_AARCH64
#  elif defined(__arm__)
    return m == 40;   // EM_ARM
#  elif defined(__riscv)
    return m == 243;  // EM_RISCV
#  elif defined(__powerpc64__)
    return m == 21;   // EM_PPC64
#  elif defined(__s390x__)
    return m == 22;   // EM_S390
#  elif defined(__loongarch64)
    return m == 258;  // EM_LOONGARCH
#  else
    (void)m;
    return true;
#  endif
  }
  // Mach-O: CPU types
#  if defined(__aarch64__) || defined(__arm64__)
  const unsigned long hostCpu = 0x0100000CUL; // CPU_TYPE_ARM64
#  elif defined(__x86_64__)
  const unsigned long hostCpu = 0x01000007UL; // CPU_TYPE_X86_64
#  elif defined(__i386__)
  const unsigned long hostCpu = 7UL;          // CPU_TYPE_I386
#  else
  const unsigned long hostCpu = 0UL;
#  endif
  if (n >= 8 && ((h[0] == 0xCF || h[0] == 0xCE) && h[1] == 0xFA && h[2] == 0xED && h[3] == 0xFE)) {
    return hostCpu == 0 || rd32(h + 4, true) == hostCpu; // thin, little-endian MH_MAGIC(_64)
  }
  if (n >= 8 && h[0] == 0xCA && h[1] == 0xFE && h[2] == 0xBA && (h[3] == 0xBE || h[3] == 0xBF)) {
    // fat (universal) binary, big-endian headers: fat_arch is 20 bytes, fat_arch_64 is 32 bytes
    const size_t entry = (h[3] == 0xBE) ? 20 : 32;
    const unsigned long count = rd32(h + 4, false);
    if (hostCpu == 0 || count == 0 || count > 64) {
      return true;
    }
    for (unsigned long i = 0; i < count && 8 + (i + 1) * entry <= n; ++i) {
      if (rd32(h + 8 + i * entry, false) == hostCpu) {
        return true;
      }
    }
    return false;
  }
  // PE (Windows): IMAGE_FILE_HEADER.Machine
  if (n >= 0x40 && h[0] == 'M' && h[1] == 'Z') {
    const unsigned long pe = rd32(h + 0x3C, true); // e_lfanew, from the file: don't add to it before checking
    if (pe <= n && n - pe >= 6 && h[pe] == 'P' && h[pe + 1] == 'E' && h[pe + 2] == 0 && h[pe + 3] == 0) {
      const unsigned int m = rd16(h + pe + 4, true);
#  if defined(_M_ARM64EC) || defined(__arm64ec__)
      return m == 0x8664 || m == 0xAA64; // x64 and Arm64EC images use AMD64; Arm64X uses ARM64
#  elif defined(__x86_64__) || defined(_M_X64)
      return m == 0x8664; // a pure ARM64 DLL can't load in an x64 process (an Arm64X image under
                          // emulation is still used through the first-existing fallback)
#  elif defined(__i386__) || defined(_M_IX86)
      return m == 0x14C;
#  elif defined(__aarch64__) || defined(_M_ARM64)
      return m == 0xAA64;
#  else
      (void)m;
      return true;
#  endif
    }
  }
  return true;
}

bool OFX::Host::PluginCache::_useStdOFXPluginsLocation = true;
OFX::Host::PluginCache* OFX::Host::PluginCache::gPluginCachePtr = 0;

// Define this to enable ofx plugin cache debug messages.
//#define CACHE_DEBUG

using namespace OFX::Host;


/// try to open the plugin bundle object and query it for plugins
void PluginBinary::loadPluginInfo(PluginCache *cache) {      
  if (_binary) {
    // also for a binary that can't be stat()ed (0, 0): a cache that recorded it valid is then
    // rewritten once, with the mtime and size of a failed stat(), and stays clean afterwards
    _fileModificationTime = _binary->getTime();
    _fileSize = _binary->getSize();
  }
  if (isInvalid()) {
    return;
  }
  if (_binary) {
    // Take a reference to keep the binary loaded during its scan: the plug-in cache releases it
    // after the scan (releaseScanReference()), else the destructor does. While a PluginHandle
    // holds the binary, it stays loaded (Host::keepPluginsLoadedAfterScan()).
    if (!_binary->isLoaded()) {
      _binary->ref();
      if (_binary->isLoaded()) {
        ++_loadGeneration;
      }
      _scanRef = true;
    }
  }

  
  if (_binary) {
    _getNumberOfPlugins = (OfxGetNumberOfPluginsFunc) _binary->findSymbol("OfxGetNumberOfPlugins");
    _getPluginFunc = (OfxGetPluginFunc) _binary->findSymbol("OfxGetPlugin");
    // OFX 1.5 (ofxCore.h): "The order of calls is then: 1) OfxSetHost, 2) OfxGetNumberOfPlugins,
    // 3) OfxGetPlugin", and the symbol may be absent. "Plug-in can return kOfxStatFailed to
    // indicate it has nothing to do here, it's not for this Host and it should be skipped
    // silently": the binary is then valid, with no plug-ins (cached as such, and scanned again if
    // it changes), and unloaded.
    OFX::Host::Host* host = cache ? cache->getScanHost() : NULL;
    // an OFX 1.4 host (Host::supportsOfxSetHost() false) doesn't look for OfxSetHost
    OfxSetHostFunc setHost = (host && !host->supportsOfxSetHost()) ? NULL : (OfxSetHostFunc) _binary->findSymbol("OfxSetHost");
    _setHostFailed = false;
    if (setHost && _getNumberOfPlugins && _getPluginFunc) {
      if (host) {
        host->loadingBinary(_filePath);
        _setHostFailed = ((*setHost)(host->getHandle()) == kOfxStatFailed);
        if (_setHostFailed) {
          _plugins.clear();
          _getNumberOfPlugins = 0; // they would point into the unloaded binary
          _getPluginFunc = 0;
          releaseScanReference(); // the reference taken above: the binary is unloaded
          return;
        }
      } else {
        std::cerr << "OFX: " << _filePath << ": no host to pass to OfxSetHost (call PluginCache::setScanHost() before the scan)" << std::endl;
      }
    }
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
  // the plug-ins released their handles: release the reference taken by loadPluginInfo(), if the
  // scan didn't, which unloads the binary
  releaseScanReference();
  // we own the Binary (created by the constructors): deleting it unloads the library (~Binary()
  // calls unload()), even if a reference was not released, e.g. by a leaked PluginHandle, whose
  // OfxPlugin* then points into the unloaded library
  delete _binary;
  _binary = 0;
}

void PluginBinary::releaseScanReference() {
  if (_binary && _scanRef) {
    _scanRef = false;
    // they point into the binary, which may be loaded at another address next time
    _getNumberOfPlugins = 0;
    _getPluginFunc = 0;
    _binary->unref();
  }
}

PluginHandle::PluginHandle(Plugin *p, OFX::Host::Host *host)
{
  _b = p->getBinary();
  // a fresh load of the binary (only known from the cache, or unloaded since the scan): OfxSetHost
  // must be called again, once, before OfxGetPlugin (OFX 1.5)
  const bool freshLoad = _b->_binary && !_b->_binary->isLoaded();
  if (_b->_binary) {
    _b->_binary->ref();
    if (freshLoad && _b->_binary->isLoaded()) {
      ++_b->_loadGeneration;
    }
  }
  _op = 0;
  OfxPlugin* (*getPlug)(int) = 0;
  if (_b->_getPluginFunc && !freshLoad) {
    // (after a fresh load, the binary may be at another address: look the symbol up again)
    getPlug = _b->_getPluginFunc;
  }
  if (!getPlug && _b->_binary) {
    getPlug = (OfxPlugin*(*)(int)) _b->_binary->findSymbol("OfxGetPlugin");
  }
  // OFX 1.5: OfxSetHost is called once per load of the binary, before OfxGetPlugin; a failure means
  // that its plug-ins are not for this host (the handle then has no OfxPlugin)
  if (freshLoad && getPlug) {
    OfxSetHostFunc setHostFunc = host->supportsOfxSetHost() ? (OfxSetHostFunc) _b->_binary->findSymbol("OfxSetHost") : NULL;
    if (setHostFunc) {
      host->loadingBinary(_b->getFilePath());
      _b->_setHostFailed = ((*setHostFunc)(host->getHandle()) == kOfxStatFailed);
    } else {
      _b->_setHostFailed = false;
    }
  }
  if (_b->_setHostFailed) {
    getPlug = 0;
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
    if (!_b->_binary->isLoaded()) {
      // they pointed into the unloaded binary
      _b->_getNumberOfPlugins = 0;
      _b->_getPluginFunc = 0;
    }
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
, _pluginHost(NULL)
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
      static const std::string bundleSuffix(".ofx.bundle");
      if (!name.empty() && name[0] == '@') {
        // "Any bundle or sub-directory name starting with the character '@' is to be ignored. Such
        // directories or bundles must be skipped by the host." (ofxPackaging.rst)
      } else if (name.size() > bundleSuffix.size() &&
                 name.compare(name.size() - bundleSuffix.size(), bundleSuffix.size(), bundleSuffix) == 0) {
        // a bundle is a directory whose name ends with ".ofx.bundle" (not "X.ofx.bundle.old")
        std::string barename = name.substr(0, name.length() - strlen(".bundle"));
        std::string bundlename = dir + DIRSEP + name;
        // The binary in the most preferred ARCHITECTURE directory that has one, or in the most
        // preferred one if none has it (it is then recorded as an invalid binary, as before).
        // E.g. an x86-64 macOS host checks MacOS then MacOS-x86-64, an x64 Windows host running
        // under Arm64 emulation checks Win-arm64ec then Win64 (see ofxhArchDirs.h).
        const std::string contents = dir + DIRSEP + name + DIRSEP "Contents" DIRSEP;
        // Use the first folder whose binary matches the host process; if none matches, the first folder
        // that has a binary (as before).
        std::string binpath = archDirs.empty() ? std::string() : contents + archDirs[0] + DIRSEP + barename;
        std::string firstExisting;
        for (std::size_t a = 0; a < archDirs.size(); ++a) {
          const std::string candidate = contents + archDirs[a] + DIRSEP + barename;
          if (binaryExists(candidate)) {
            if (firstExisting.empty()) {
              firstExisting = candidate;
            }
            if (binaryMatchesHost(candidate)) {
              binpath = candidate;
              firstExisting.clear();
              break;
            }
          }
        }
        if (!firstExisting.empty()) {
          binpath = firstExisting;
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
          // unload the binary, unless its plug-ins hold it (Host::keepPluginsLoadedAfterScan())
          pb->releaseScanReference();
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
        if (isdir && (recurse && !name.empty() && name[name.size() - 1] != '.')) {
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

  // a scan (re)builds the lists of plug-in directories and of supported plug-ins, and those of the
  // image effect API caches: a second scan would otherwise add every plug-in again, and keep the
  // plug-ins of the binaries it deletes (below)
  _pluginDirs.clear();
  _plugins.clear();
  for (std::list<PluginCacheSupportedApi>::iterator a = _apiHandlers.begin(); a != _apiHandlers.end(); ++a) {
    if (ImageEffect::PluginCache *ie = dynamic_cast<ImageEffect::PluginCache *>(a->handler)) {
      ie->clearConfirmedPlugins();
    }
  }
  
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
      // if it comes back, it is scanned again
      _knownBinFiles.erase(pb->getFilePath());
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

      if (binChanged) {
        // the binary was scanned: unload it, unless its plug-ins hold it (Host::keepPluginsLoadedAfterScan())
        pb->releaseScanReference();
        // and it is now up to date: a later scan doesn't load it again (which would make new
        // plug-ins, and leave the ones of this scan to the hosts that use them)
        pb->_binaryChanged = false;
      }
      
      i++;
    }
  }
}


namespace {
  /// state shared between readCache() and the XML parser callbacks
  struct XMLReadState {
    PluginCache *cache; ///< the cache that reads (not necessarily the singleton getPluginCache())
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
    static_cast<XMLReadState*>(userData)->cache->elementBeginCallback(userData, name, atts);
  } catch (const std::exception &e) {
    stopParserOnException(userData, e.what());
  } catch (...) {
    stopParserOnException(userData, "unknown exception");
  }
}

/// callback for XML parser
static void elementCharHandler(void *userData, const XML_Char *data, int len) {
  try {
    static_cast<XMLReadState*>(userData)->cache->elementCharCallback(userData, data, len);
  } catch (const std::exception &e) {
    stopParserOnException(userData, e.what());
  } catch (...) {
    stopParserOnException(userData, "unknown exception");
  }
}

/// callback for XML parser
static void elementEndHandler(void *userData, const XML_Char *name) {
  try {
    static_cast<XMLReadState*>(userData)->cache->elementEndCallback(userData, name);
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
  
  // (the plug-ins of a changed binary are scanned again, and a binary that can't be stat()ed has none)
  if (ename == "plugin" && _xmlCurrentBinary && !_xmlCurrentBinary->hasBinaryChanged() &&
      !_xmlCurrentBinary->isInvalid()) {
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
  state.cache = this;
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


OFX::Host::Host* PluginCache::getScanHost() const
{
  return _pluginHost ? _pluginHost : ImageEffect::gImageEffectHost;
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
