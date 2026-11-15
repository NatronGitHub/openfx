#ifndef OFXH_ARCHDIRS_H
#define OFXH_ARCHDIRS_H

/*
Software License :

Copyright (c) 2007-2026, The Open Effects Association Ltd. All rights reserved.

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

/// @file ofxhArchDirs.h
/// The ARCHITECTURE sub-directories of an OFX plug-in bundle (NAME.ofx.bundle/Contents/ARCHITECTURE/NAME.ofx)
/// that a host should look in, in order of preference. See the OpenFX packaging reference
/// (Documentation/sources/Reference/ofxPackaging.rst).
///
/// archDirsFor() is a pure function of the host's OS and architecture, so that the rules can be tested
/// on any platform; PluginCache computes its arguments for the running host.

#include <string>
#include <vector>

namespace OFX {
  namespace Host {
    namespace ArchDirs {

      enum OSEnum {
        eOSLinux,
        eOSFreeBSD,
        eOSMacOS,
        eOSWindows
      };

      /// the architecture the host was compiled for
      enum ArchEnum {
        eArchX86,     ///< 32-bit x86
        eArchX86_64,  ///< x86-64 (Windows: x64)
        eArchArm64EC, ///< Windows Arm64EC
        eArchArm64,   ///< native 64-bit Arm (Windows: native Arm64, not Arm64EC)
        eArchOther    ///< anything else: use the machine name
      };

      /// The bundle ARCHITECTURE directories to look in, most preferred first.
      ///
      /// @param os                    the host operating system
      /// @param arch                  the architecture the host was compiled for
      /// @param x64EmulatedOnArm64    Windows only: an x64 host is running under emulation on Arm64 hardware
      /// @param machine               the `uname -m` machine name (Linux, FreeBSD), used for architectures
      ///                              other than x86 and x86-64 (e.g. "aarch64", "riscv64")
      ///
      /// The rules, from the OpenFX packaging reference:
      /// - MacOS: "all MacOS plug-ins should be in MacOS ... There will not be a specific arm64 folder."
      ///   An x86-64 host still checks the deprecated MacOS-x86-64 folder first, then MacOS.
      /// - Windows: "An Arm64EC host should load plug-ins from the Win-arm64ec folder. ... an x64 host should
      ///   detect when it is running under Arm64 emulation and, if so, also check the Win-arm64ec folder when it
      ///   exists, preferring a native Arm64EC plug-in there over the emulated x64 plug-in in Win64."
      ///   An Arm64EC host can also load x64 plug-ins, so it falls back to Win64. A native Arm64 host uses
      ///   the reserved (not yet normative) Win-arm64 and Win-arm64x folders.
      /// - Linux (informative): "hosts should look in the subdir named by Linux-${uname -m} which for arm64
      ///   should be Linux-aarch64". The existing Linux-x86 and Linux-x86-64 names are kept for x86.
      ///
      /// Windows folder names are returned as HostSupport always used them ("win32", "win64"); Windows file
      /// names are case-insensitive.
      inline std::vector<std::string> archDirsFor(OSEnum os, ArchEnum arch, bool x64EmulatedOnArm64, const std::string &machine)
      {
        std::vector<std::string> dirs;
        switch (os) {
        case eOSMacOS:
          if (arch == eArchX86_64) {
            dirs.push_back("MacOS-x86-64"); // DEPRECATED, but still checked first by x86-64 hosts
          }
          dirs.push_back("MacOS");
          break;
        case eOSWindows:
          switch (arch) {
          case eArchX86:
            dirs.push_back("win32");
            break;
          case eArchX86_64:
            if (x64EmulatedOnArm64) {
              dirs.push_back("Win-arm64ec"); // prefer a native Arm64EC plug-in over an emulated x64 one
            }
            dirs.push_back("win64");
            break;
          case eArchArm64EC:
            dirs.push_back("Win-arm64ec");
            dirs.push_back("win64"); // Arm64EC processes can load (emulated) x64 DLLs
            break;
          case eArchArm64:
          case eArchOther:
            // reserved names, "not yet normative, pending a host that needs them"
            dirs.push_back("Win-arm64");
            dirs.push_back("Win-arm64x");
            break;
          }
          break;
        case eOSLinux:
        case eOSFreeBSD: {
          const char *prefix = (os == eOSLinux) ? "Linux-" : "FreeBSD-";
          if (arch == eArchX86) {
            dirs.push_back(std::string(prefix) + "x86");
          } else if (arch == eArchX86_64) {
            dirs.push_back(std::string(prefix) + "x86-64");
          } else if (!machine.empty()) {
            dirs.push_back(std::string(prefix) + machine);
          }
          break;
        }
        }
        return dirs;
      }

    } // namespace ArchDirs
  } // namespace Host
} // namespace OFX

#endif // OFXH_ARCHDIRS_H
