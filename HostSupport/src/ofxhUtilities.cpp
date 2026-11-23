// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include "ofxCore.h"
#include "ofxhUtilities.h"
#include "ofxhPlatformDefines.h"

#ifdef WINDOWS
#include <windows.h>
#endif

namespace OFX {

  /// rank of a bit depth, from shallowest to deepest (-1 if unknown)
  static int BitDepthRank(const std::string &s)
  {
    if (s == kOfxBitDepthNone) return 0;
    if (s == kOfxBitDepthByte) return 1;
    if (s == kOfxBitDepthShort) return 2;
    if (s == kOfxBitDepthHalf) return 3;
    if (s == kOfxBitDepthFloat) return 4;
    return -1;
  }

  /// get me deepest bit depth 
  std::string FindDeepestBitDepth(const std::string &s1, const std::string &s2)
  {
    const int r1 = BitDepthRank(s1);
    const int r2 = BitDepthRank(s2);
    if (r1 <= 0) {
      return s2; // none or unknown
    }
    if (r2 < 0) {
      return s1;
    }
    // Byte < Short < Half < Float. Half was previously never deeper than Byte,
    // and Short vs. Half depended on the argument order.
    return (r2 > r1) ? s2 : s1;
  }

# ifdef WINDOWS
  std::wstring utf8_to_utf16(const std::string& str)
  {
    std::wstring native;

    native.resize(MultiByteToWideChar (CP_UTF8, 0, str.data(), str.length(), NULL, 0));
    MultiByteToWideChar (CP_UTF8, 0, str.data(), str.length(), &native[0], (int)native.size());

    return native;
  }
  
  std::string  utf16_to_utf8(const std::wstring& str)
  {
      std::string utf8;

      utf8.resize(WideCharToMultiByte (CP_UTF8, 0, str.data(), str.length(), NULL, 0, NULL, NULL));
      WideCharToMultiByte (CP_UTF8, 0, str.data(), str.length(), &utf8[0], (int)utf8.size(), NULL, NULL);

      return utf8;
  }
# endif

}
