// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/*
  plugin-unload-test BINARY...: checks that each plug-in binary (the .ofx file in a bundle) is
  really unloaded by dlclose(), so that a host that unloads a plug-in and loads it again gets a new
  copy of its static state.

  Each binary is loaded, its OfxGetNumberOfPlugins() and OfxGetPlugin() are called (no host:
  setHost() and the actions are not), it is closed, and it must then be gone: dlopen() with
  RTLD_NOLOAD doesn't find it. OfxGetNumberOfPlugins() must not be negative (thread-scope-test.ofx,
  Support/tests/ThreadScopeBundle.cpp, returns -1 if its ThreadScope checks fail). A binary may
  be given twice, to check that it works again when it is loaded again.

  On macOS, the binary must also have no thread-local variables (MH_HAS_TLV_DESCRIPTORS): dyld
  never unloads such an image (MachOAnalyzer::neverUnload()), and the Support library used to give
  one to every plug-in (#385). dyld doesn't unload images that have Objective-C metadata either:
  for those (the Metal examples), the unload check is skipped, with a message.

  Prints one line per binary, and exits with 0 if all passed.
*/

#include <cstdio>

#include <dlfcn.h>

#ifdef __APPLE__
#include <mach-o/getsect.h>
#include <mach-o/loader.h>
#endif

#include "ofxCore.h"

namespace {

typedef int (*GetNumberOfPlugins)(void);
typedef OfxPlugin* (*GetPlugin)(int);

#ifdef __APPLE__
// whether the image has Objective-C metadata, which dyld never unloads
bool hasObjC(const struct mach_header_64* h)
{
  unsigned long size = 0;
  return getsectiondata(h, "__DATA", "__objc_imageinfo", &size) ||
         getsectiondata(h, "__DATA_CONST", "__objc_imageinfo", &size);
}
#endif

// returns whether the checks passed
bool checkUnload(const char* path)
{
  void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    std::printf("FAILED: %s: dlopen: %s\n", path, dlerror());
    return false;
  }
  GetNumberOfPlugins getNumberOfPlugins = reinterpret_cast<GetNumberOfPlugins>(dlsym(handle, "OfxGetNumberOfPlugins"));
  GetPlugin getPlugin = reinterpret_cast<GetPlugin>(dlsym(handle, "OfxGetPlugin"));
  if (!getNumberOfPlugins || !getPlugin) {
    std::printf("FAILED: %s: no OfxGetNumberOfPlugins or OfxGetPlugin\n", path);
    dlclose(handle);
    return false;
  }
  const int n = getNumberOfPlugins();
  for (int i = 0; i < n; ++i) {
    getPlugin(i);
  }

  // (a negative number: thread-scope-test.ofx's checks of ThreadScope failed)
  bool ok = n >= 0;
  if (!ok) {
    std::printf("FAILED: %s: OfxGetNumberOfPlugins() returned %d\n", path, n);
  }
  bool checkUnloaded = true;
#ifdef __APPLE__
  Dl_info info;
  if (!dladdr(reinterpret_cast<void*>(getNumberOfPlugins), &info) || !info.dli_fbase) {
    std::printf("FAILED: %s: dladdr\n", path);
    dlclose(handle);
    return false;
  }
  const struct mach_header_64* h = static_cast<const struct mach_header_64*>(info.dli_fbase);
  if (h->flags & MH_HAS_TLV_DESCRIPTORS) {
    std::printf("FAILED: %s has thread-local variables (MH_HAS_TLV_DESCRIPTORS): macOS never unloads it\n", path);
    ok = false;
  }
  if (hasObjC(h)) {
    std::printf("%s: Objective-C metadata, which macOS never unloads: unload not checked\n", path);
    checkUnloaded = false;
  }
#endif

  if (dlclose(handle) != 0) {
    std::printf("FAILED: %s: dlclose: %s\n", path, dlerror());
    return false;
  }
  if (checkUnloaded) {
    if (void* again = dlopen(path, RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD)) {
      std::printf("FAILED: %s is still loaded after dlclose()\n", path);
      dlclose(again);
      ok = false;
    } else if (ok) {
      std::printf("%s: %d plug-in(s), unloaded\n", path, n);
    }
  }
  return ok;
}

} // namespace

int main(int argc, char* argv[])
{
  if (argc < 2) {
    std::printf("usage: %s BINARY...\n", argv[0]);
    return 2;
  }
  int failures = 0;
  for (int i = 1; i < argc; ++i) {
    if (!checkUnload(argv[i])) {
      ++failures;
    }
  }
  if (failures) {
    std::printf("plugin-unload-test: %d of %d binaries failed\n", failures, argc - 1);
    return 1;
  }
  std::printf("plugin-unload-test: all %d binaries passed\n", argc - 1);
  return 0;
}
