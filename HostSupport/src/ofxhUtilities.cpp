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
