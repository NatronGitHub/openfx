#ifndef OFXH_GPU_LIBRARY_H
#define OFXH_GPU_LIBRARY_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// Internal to HostSupport's GPU components that load their driver at run time
// (openfx::HostSupportOpenCL, openfx::HostSupportCUDA), not installed: the library and its
// symbols.

#include <memory>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800 // (headers that define it for Windows 8 and later only)
#endif
#else
#include <dlfcn.h>
#endif

namespace OFX {
  namespace Host {
    namespace GPU {

      /// The first of names (nullptr-terminated) that loads, never unloaded: drivers may not
      /// survive an unload (Windows: never freed; elsewhere RTLD_NODELETE). nullptr if none: then
      /// error is "<names[0]> not found". On Windows, only from System32
      /// (LOAD_LIBRARY_SEARCH_SYSTEM32), where the drivers install nvcuda.dll and OpenCL.dll: not a
      /// copy planted next to the host or in the current directory.
      inline std::shared_ptr<void> openLibrary(const char *const *names, std::string &error)
      {
        for (const char *const *n = names; *n; ++n) {
#if defined(_WIN32)
          if (HMODULE h = LoadLibraryExA(*n, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
            return std::shared_ptr<void>(h, [](void *) {});
          }
#else
          if (void *h = dlopen(*n, RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE)) {
            return std::shared_ptr<void>(h, [](void *p) { dlclose(p); });
          }
#endif
        }
        error = std::string(names[0] ? names[0] : "the library") + " not found";
        return nullptr;
      }

      /// the symbol name of library (from openLibrary()), nullptr if none
      inline void *librarySymbol(void *library, const char *name)
      {
#if defined(_WIN32)
        return reinterpret_cast<void *>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
        return dlsym(library, name);
#endif
      }

    } // GPU
  } // Host
} // OFX

#endif // OFXH_GPU_LIBRARY_H
