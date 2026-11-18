#ifndef _ofxhUtilities_h_
#define _ofxhUtilities_h_

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

#include <string>
#include <list>
#include <vector>
#if defined(_MSC_VER)
#include <float.h> // kept for the code that relies on it (IsNaN no longer uses _isnan)
#endif
#include <cmath> // kept for the code that relies on it (IsNaN no longer uses std::isnan)
#include <cstring> // memcpy (IsNaN, IsInfinite)

#include "ofxCore.h"
#include "ofxhPlatformDefines.h"

// macro that intercepts any exception that passes through a plugin's entry point, and transforms it into a message on the host using Host::vmessage()
#define CatchAllSetStatus(stat,host,plugin,msg)                         \
  catch ( const std::bad_alloc& ba ) {                                  \
    (stat) = kOfxStatErrMemory;                                         \
    if (host) {                                                         \
      try {                                                             \
        (host)->message(kOfxMessageError, "",                           \
                        "%s: Memory allocation error occured in plugin %s (%s)", \
                        (msg), (plugin)->pluginIdentifier, ba.what());  \
      } catch (...) {                                                   \
      }                                                                 \
    }                                                                   \
  } catch ( const std::exception &e ) {                                 \
    (stat) = kOfxStatFailed;                                            \
    if (host) {                                                         \
      try {                                                             \
        (host)->message(kOfxMessageError, "",                           \
                        "%s: Exception occured in plugin %s (%s)",      \
                        (msg), (plugin)->pluginIdentifier, e.what());   \
      } catch (...) {                                                   \
      }                                                                 \
    }                                                                   \
  } catch ( ... ) {                                                     \
    (stat) = kOfxStatFailed;                                            \
    if (host) {                                                         \
      try {                                                             \
        (host)->message(kOfxMessageError, "",                           \
                        "%s:Exception occured in plugin %s",            \
                        (msg), (plugin)->pluginIdentifier);             \
      } catch (...) {                                                   \
      }                                                                 \
    }                                                                   \
  }

namespace OFX {

  /// class that is a std::vector of std::strings
  typedef std::vector<std::string> StringVec;

  /// class that is a std::vector of std::strings
  inline void SetStringVecValue(StringVec &sv, const std::string &value, size_t index = 0) 
  {
    size_t size = sv.size();
    if(size <= index) {
      while(size < index) {
        sv.push_back("");
        ++size;
      }
      sv.push_back(value);
    }
    else 
      sv[index] = value;
  }

  /// get me deepest bit depth 
  std::string FindDeepestBitDepth(const std::string &s1, const std::string &s2);

  /// get the min value
  template<class T> inline T Minimum(const T &a, const T &b)
  {
    return a < b ? a : b;
  }

  /// get the min value
  template<class T> inline T Maximum(const T &a, const T &b)
  {
    return a > b ? a : b;
  }

  /// clamp the value
  template<class T> inline T Clamp(const T &v, const T &mn, const T &mx)
  {
    if(v < mn) return mn;
    if(v > mx) return mx;
    return v;
  }

  /// clamp the rect in v to the given bounds
  inline OfxRectD Clamp(const OfxRectD &v,
                        const OfxRectD &bounds)
  {
    OfxRectD r;
    r.x1 = Clamp(v.x1, bounds.x1, bounds.x2);
    r.x2 = Clamp(v.x2, bounds.x1, bounds.x2);
    r.y1 = Clamp(v.y1, bounds.y1, bounds.y2);
    r.y2 = Clamp(v.y2, bounds.y1, bounds.y2);
    return r;
  }

  /// get the union of the two rects
  inline OfxRectD Union(const OfxRectD &a,
                        const OfxRectD &b)
  {
    OfxRectD r;
    r.x1 = Minimum(a.x1, b.x1);
    r.x2 = Maximum(a.x2, b.x2);
    r.y1 = Minimum(a.y1, b.y1);
    r.y2 = Maximum(a.y2, b.y2);
    return r;
  }

  inline const char* StatStr(OfxStatus stat) {
    switch(stat) {
      case kOfxStatOK:
        return "kOfxStatOK";
      case kOfxStatFailed:
        return "kOfxStatFailed";
      case kOfxStatErrFatal:
        return "kOfxStatErrFatal";
      case kOfxStatErrUnknown:
        return "kOfxStatErrUnknown";
      case kOfxStatErrMissingHostFeature:
        return "kOfxStatErrMissingHostFeature";
      case kOfxStatErrUnsupported:
        return "kOfxStatErrUnsupported";
      case kOfxStatErrExists:
        return "kOfxStatErrExists";
      case kOfxStatErrFormat:
        return "kOfxStatErrFormat";
      case kOfxStatErrMemory:
        return "kOfxStatErrMemory";
      case kOfxStatErrBadHandle:
        return "kOfxStatErrBadHandle";
      case kOfxStatErrBadIndex:
        return "kOfxStatErrBadIndex";
      case kOfxStatErrValue:
        return "kOfxStatErrValue";
      case kOfxStatReplyYes:
        return "kOfxStatReplyYes";
      case kOfxStatReplyNo:
        return "kOfxStatReplyNo";
      case kOfxStatReplyDefault:
        return "kOfxStatReplyDefault";
      default:
        return "(unknown error code)";
    }
  }

  // IsNaN and IsInfinite test the bits of the IEEE 754 binary64 representation instead of calling
  // std::isnan/std::isinf (or _isnan/_finite): with -ffast-math or -Ofast (-ffinite-math-only), or
  // /fp:fast, the compiler assumes that no floating-point value is NaN or infinite, and folds
  // std::isnan(x), std::isinf(x) and x != x to false, so that the checks disappear. These functions
  // check values that cross the API, i.e. that come from another binary (a host or a plug-in),
  // which may have been built with other options, so they must work whatever the options this
  // file is compiled with. The bits are integers, but clang also deduces facts about them from the
  // double they come from (e.g. a fast-math function argument is marked as never infinite), so an
  // empty asm statement (gcc, clang, clang-cl), or volatile copies (MSVC), hide their origin from
  // the optimizer. The bits are read as two 32-bit words, which needs neither long long (C++98)
  // nor 64-bit registers (32-bit x86). The results are those of std::isnan and std::isinf (a float
  // argument is converted to double, which keeps NaN and infinity).
  inline void DoubleWords(double x, unsigned int& hi, unsigned int& lo)
  {
    unsigned int w[2];
    std::memcpy(w, &x, sizeof(w));
    const unsigned int one = 1;
    unsigned char firstByte;
    std::memcpy(&firstByte, &one, 1);
    const int h = (firstByte == 1) ? 1 : 0; // the index of the high word (1 if little-endian)
#if defined(__GNUC__) || defined(__clang__)
    __asm__("" : "+r"(w[0]), "+r"(w[1])); // an optimization barrier: the compiler can't know the words
#else
    volatile unsigned int v0 = w[0];
    volatile unsigned int v1 = w[1];
    w[0] = v0;
    w[1] = v1;
#endif
    hi = w[h] & 0x7fffffffU; // without the sign bit
    lo = w[1 - h];
  }
  inline bool IsNaN(double x)
  {
    unsigned int hi, lo;
    DoubleWords(x, hi, lo);
    return hi > 0x7ff00000U || (hi == 0x7ff00000U && lo != 0); // exponent all ones, mantissa != 0
  }
  inline bool IsInfinite(double x)
  {
    unsigned int hi, lo;
    DoubleWords(x, hi, lo);
    return hi == 0x7ff00000U && lo == 0; // exponent all ones, mantissa == 0
  }

# ifdef WINDOWS
  std::wstring  utf8_to_utf16(const std::string& s);
  std::string  utf16_to_utf8(const std::wstring& s);
# endif
}
#endif


