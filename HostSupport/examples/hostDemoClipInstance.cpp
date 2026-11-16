/*
Software License :

Copyright (c) 2007, The Open Effects Association Ltd. All rights reserved.

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
#include <iostream>
#include <fstream>
#include <cassert>
#include <algorithm> // std::fill (no std::min/std::max: windows.h may define min/max macros)
#include <cerrno>
#include <cfloat>
#include <climits>
#include <cmath>
#include <ctime>
#include <cstdio> // fopen (binary files)
#include <cstdlib>
#include <cstring> // memcpy
#include <limits>
#include <sstream>

// ofx
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxPixels.h"
#ifdef OFX_EXTENSIONS_NUKE
#include "nuke/fnOfxExtensions.h" // kFnOfxImagePlaneColour
#endif

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

// my host
#include "hostDemoHostDescriptor.h"
#include "hostDemoEffectInstance.h"
#include "hostDemoClipInstance.h"

// We are hard coding everything in our example, in a real host you
// need to enquire things from your host.
namespace MyHost {
  const double    kPalPixelAspect = double(768)/double(720);

  // 5x3 bitmaps for digits 0..9 and period
  const char digits[11][5][3] = {
    { {0,1,0},
      {1,0,1},
      {1,0,1},
      {1,0,1},
      {0,1,0} },
    { {0,1,0},
      {1,1,0},
      {0,1,0},
      {0,1,0},
      {0,1,0} },    
    { {0,1,0},
      {1,0,1},
      {0,0,1},
      {0,1,0},
      {1,1,1} },    
    { {1,1,0},
      {0,0,1},
      {0,1,1},
      {0,0,1},
      {1,1,0} },      
    { {0,1,0},
      {1,0,0},
      {1,0,1},
      {1,1,1},
      {0,0,1} },    
    { {1,1,1},
      {1,0,0},
      {1,1,0},
      {0,0,1},
      {1,1,0} },  
    { {0,1,1},
      {1,0,0},
      {1,1,0},
      {1,0,1},
      {0,1,0} },      
    { {1,1,1},
      {0,0,1},
      {0,1,0},
      {0,1,1},
      {0,1,0} },      
    { {0,1,0},
      {1,0,1},
      {0,1,0},
      {1,0,1},
      {0,1,0} },  
    { {0,1,0},
      {1,0,1},
      {0,1,1},
      {0,0,1},
      {1,1,0} },
    { {0,0,0},
      {0,0,0},
      {0,0,0},
      {0,0,0},
      {0,1,0} }
  };

  int gImageErrors = 0;

  bool parseFiniteDouble(const std::string& s, double& d)
  {
    if (s.empty()) {
      return false;
    }
    // only decimal notation: this rejects nan, inf, hexadecimal and spaces, which strtod accepts
    for (std::string::size_type k = 0; k < s.size(); ++k) {
      const char c = s[k];
      if (!((c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.' || c == 'e' || c == 'E')) {
        return false;
      }
    }
    char* end = NULL;
    errno = 0;
    const double v = std::strtod(s.c_str(), &end);
    if (errno == ERANGE || end == s.c_str() || end == NULL || *end != '\0' || !(v - v == 0.)) {
      return false;
    }
    d = v;
    return true;
  }

  bool parseInteger(const std::string& s, int& i)
  {
    if (s.empty()) {
      return false;
    }
    for (std::string::size_type k = 0; k < s.size(); ++k) {
      const char c = s[k];
      if (!((c >= '0' && c <= '9') || (k == 0 && (c == '+' || c == '-')))) {
        return false;
      }
    }
    char* end = NULL;
    errno = 0;
    const long l = std::strtol(s.c_str(), &end, 10);
    if (errno == ERANGE || end == s.c_str() || end == NULL || *end != '\0' || l < INT_MIN || l > INT_MAX) {
      return false;
    }
    i = (int)l;
    return true;
  }

  static bool isPFMSpace(unsigned char c)
  {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
  }

  // the next whitespace-separated header token of a PFM file, starting at pos
  static std::string pfmToken(const std::vector<unsigned char>& buf, size_t& pos)
  {
    while (pos < buf.size() && isPFMSpace(buf[pos])) {
      ++pos;
    }
    std::string t;
    while (pos < buf.size() && !isPFMSpace(buf[pos]) && t.size() < 64) {
      t += (char)buf[pos++];
    }
    return t;
  }

  bool readPFM(const std::string& path, PFMImage& image, std::string& error)
  {
    image = PFMImage();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
      error = "cannot open " + path;
      return false;
    }
    std::vector<unsigned char> buf;
    unsigned char chunk[65536];
    size_t n;
    while ((n = std::fread(chunk, 1, sizeof(chunk), f)) > 0) {
      buf.insert(buf.end(), chunk, chunk + n);
    }
    const bool readError = std::ferror(f) != 0;
    std::fclose(f);
    if (readError) {
      error = "cannot read " + path;
      return false;
    }
    // "PF" or "Pf", whitespace, width, whitespace, height, whitespace, scale (negative: only
    // little-endian files are read), a single whitespace character, then the raster
    if (buf.size() < 3 || buf[0] != 'P' || (buf[1] != 'F' && buf[1] != 'f') || !isPFMSpace(buf[2])) {
      error = path + ": not a PFM file (it must start with PF or Pf)";
      return false;
    }
    const int nComps = (buf[1] == 'F') ? 3 : 1;
    size_t pos = 2;
    int w = 0, h = 0;
    double scale = 0.;
    const std::string ws = pfmToken(buf, pos);
    const std::string hs = pfmToken(buf, pos);
    if (!parseInteger(ws, w) || !parseInteger(hs, h) || w <= 0 || h <= 0 || w > 65536 || h > 65536 ||
        (double)w * (double)h > (double)(1 << 24)) {
      error = path + ": invalid PFM size \"" + ws + " " + hs + "\" (the width and height must be positive, at most 65536, and at most 2^24 pixels)";
      return false;
    }
    const std::string ss = pfmToken(buf, pos);
    if (!parseFiniteDouble(ss, scale) || scale >= 0.) {
      error = path + ": invalid PFM scale \"" + ss + "\" (it must be negative: only little-endian PFM files are supported)";
      return false;
    }
    if (pos >= buf.size() || !isPFMSpace(buf[pos])) {
      error = path + ": no whitespace after the PFM scale";
      return false;
    }
    ++pos;
    const size_t expected = (size_t)w * (size_t)h * (size_t)nComps * 4;
    const size_t found = buf.size() - pos;
    if (found != expected) {
      std::ostringstream ss2;
      ss2 << path << ": a " << w << "x" << h << (nComps == 3 ? " PF" : " Pf") << " file has " << expected
          << " bytes of pixel data, but there are " << found << (found < expected ? " (truncated)" : " (trailing data)");
      error = ss2.str();
      return false;
    }
    image.width = w;
    image.height = h;
    image.nComps = nComps;
    image.data.resize(expected / 4);
    const unsigned char* p = &buf[pos];
    for (size_t i = 0; i < image.data.size(); ++i, p += 4) {
      const unsigned int u =
        ((unsigned int)p[3] << 24) | ((unsigned int)p[2] << 16) | ((unsigned int)p[1] << 8) | (unsigned int)p[0];
      float v;
      std::memcpy(&v, &u, 4);
      image.data[i] = v;
    }
    return true;
  }

  bool parseInputSpec(const std::string& s, InputSpec& spec, std::string& error)
  {
    spec = InputSpec();
    error.clear();
    if (s == "default") {
      spec.type = InputSpec::eDefault;
      return true;
    }
    if (s == "none") {
      spec.type = InputSpec::eNone;
      return true;
    }
    if (s == "ramp") {
      spec.type = InputSpec::eRamp;
      return true;
    }
    if (s.compare(0, 5, "file:") == 0) {
      spec.type = InputSpec::eFile;
      spec.path = s.substr(5);
      // "#premult", "#unpremult" or "#opaque": the premultiplication state
      const std::string::size_type hash = spec.path.rfind('#');
      if (hash != std::string::npos) {
        const std::string p = spec.path.substr(hash + 1);
        if (p == "premult" || p == "unpremult" || p == "opaque") {
          spec.premult = p == "premult" ? kOfxImagePreMultiplied : p == "unpremult" ? kOfxImageUnPreMultiplied : kOfxImageOpaque;
          spec.path.erase(hash);
        }
      }
      // "@x1,y1": the position of the file, if what follows the last '@' is two integers
      const std::string::size_type at = spec.path.rfind('@');
      if (at != std::string::npos) {
        const std::string pos = spec.path.substr(at + 1);
        const std::string::size_type comma = pos.find(',');
        int x1 = 0, y1 = 0;
        if (comma != std::string::npos && parseInteger(pos.substr(0, comma), x1) &&
            parseInteger(pos.substr(comma + 1), y1)) {
          if (x1 < -1000000 || x1 > 1000000 || y1 < -1000000 || y1 > 1000000) {
            error = "file position out of range";
            return false;
          }
          spec.fileX = x1;
          spec.fileY = y1;
          spec.path.erase(at);
        }
      }
      // "+alpha.pfm": the alpha file
      const std::string::size_type plus = spec.path.rfind(".pfm+");
      if (plus != std::string::npos) {
        spec.alphaPath = spec.path.substr(plus + 5);
        spec.path.erase(plus + 4);
      }
      if (spec.path.empty()) {
        error = "no file name";
        return false;
      }
      if (!readPFM(spec.path, spec.file, error)) {
        return false;
      }
      if (!spec.alphaPath.empty()) {
        if (!readPFM(spec.alphaPath, spec.alpha, error)) {
          return false;
        }
        if (spec.file.nComps != 3 || spec.alpha.nComps != 1 ||
            spec.alpha.width != spec.file.width || spec.alpha.height != spec.file.height) {
          error = spec.alphaPath + ": the alpha file must be a Pf file of the size of a PF file";
          return false;
        }
      }
      return true;
    }
    if (s.compare(0, 8, "checker:") == 0) {
      int n = 0;
      if (!parseInteger(s.substr(8), n) || n < 1 || n > 1000000) {
        error = "invalid checker size";
        return false;
      }
      spec.type = InputSpec::eChecker;
      spec.checkerSize = n;
      return true;
    }
    if (s.compare(0, 6, "solid:") == 0) {
      std::string v = s.substr(6);
      for (int i = 0; i < 4; ++i) {
        std::string::size_type comma = v.find(',');
        if ((i < 3) != (comma != std::string::npos)) {
          error = "solid takes 4 values";
          return false; // exactly 4 values
        }
        // the values must also fit in a float image
        if (!parseFiniteDouble(v.substr(0, comma), spec.colour[i]) || std::fabs(spec.colour[i]) > FLT_MAX) {
          error = "invalid number";
          return false;
        }
        v = (comma == std::string::npos) ? std::string() : v.substr(comma + 1);
      }
      spec.type = InputSpec::eSolid;
      return true;
    }
    error = "unknown input type";
    return false;
  }

  // is the full-resolution pixel (px,py) part of digit d drawn at (x0,y0) with the given scale?
  static bool digitPixel(int d, int x0, int y0, int scale, int px, int py)
  {
    if (px < x0 || py < y0) {
      return false;
    }
    int i = (px - x0) / scale;
    int j = (py - y0) / scale;
    if (i >= 3 || j >= 5) {
      return false;
    }
    return digits[d][j][i] != 0;
  }

  // is the full-resolution pixel (px,py) part of the time and view numbers drawn by the default pattern?
  static bool textPixel(OfxTime time, int view, int px, int py)
  {
    const int scale = 5;
    const int charwidth = 4*scale;
    // digits of |time| and view (a negative time draws the digits of its absolute value)
    double at = std::fabs(time);
    int x = 50;
    int y = 50;
    if (digitPixel((int(at)/10)%10, x, y, scale, px, py)) return true;
    x += charwidth;
    if (digitPixel(int(at)%10, x, y, scale, px, py)) return true;
    x += charwidth;
    if (digitPixel(10, x, y, scale, px, py)) return true;
    x += charwidth;
    if (digitPixel(int(at*10)%10, x, y, scale, px, py)) return true;
    x = 50;
    y += 8*scale;
    return digitPixel(std::abs(view)%10, x, y, scale, px, py);
  }

  // evaluate the input pattern at the full-resolution pixel coordinates (X,Y) (render scale 1),
  // in an image of full-resolution size W×H pixels
  static void evalPattern(const InputSpec& spec, OfxTime time, int view, double X, double Y, double W, double H,
                          double rgba[4])
  {
    switch (spec.type) {
      case InputSpec::eSolid:
        for (int c = 0; c < 4; ++c) {
          rgba[c] = spec.colour[c];
        }
        return;
      case InputSpec::eRamp:
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = (W > 0) ? X / W : 0.;
        return;
      case InputSpec::eChecker: {
        int i = (int)std::floor(X / spec.checkerSize);
        int j = (int)std::floor(Y / spec.checkerSize);
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = ((i + j) % 2 != 0) ? 1. : 0.;
        return;
      }
      case InputSpec::eFile: {
        // the PFM pixel that contains (X,Y): the file's pixels are pixels at the render scale
        // fileScale, starting at (fileX,fileY). PFM rows are bottom to top, like OFX images.
        // "PF" gives (r,g,b,1), "Pf" gives r=g=b=a=value. Outside the file, transparent black.
        const PFMImage& f = spec.file;
        const double fx = std::floor(X * spec.fileScaleX) - spec.fileX;
        const double fy = std::floor(Y * spec.fileScaleY) - spec.fileY;
        rgba[0] = rgba[1] = rgba[2] = rgba[3] = 0.;
        if (fx >= 0 && fy >= 0 && fx < f.width && fy < f.height) {
          const float* pix = &f.data[((size_t)fy * (size_t)f.width + (size_t)fx) * (size_t)f.nComps];
          if (f.nComps == 3) {
            rgba[0] = pix[0];
            rgba[1] = pix[1];
            rgba[2] = pix[2];
            rgba[3] = spec.alpha.nComps == 1 ? spec.alpha.data[(size_t)fy * (size_t)f.width + (size_t)fx] : 1.;
          } else {
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = pix[0];
          }
        }
        return;
      }
      case InputSpec::eNone:
      case InputSpec::eDefault:
      default:
        break;
    }
    (void)H;
    // the default pattern: the value grows with time, without wrapping (it is clamped
    // to [0,1] only in integer images): black at t=0 and white at t=OFXHOSTDEMOCLIPLENGTH.
    double v = time / OFXHOSTDEMOCLIPLENGTH;
    // the time and the view number are drawn in the reverse colour
    if (textPixel(time, view, (int)std::floor(X), (int)std::floor(Y))) {
      v = (v <= 1.) ? 1. - v : 0.;
    }
#ifdef OFX_EXTENSIONS_VEGAS
    rgba[0] = view == 0 ? v : 0.;
    rgba[1] = view == 1 ? v : 0.;
    rgba[2] = view == 1 ? v : 0.;
#else
    rgba[0] = rgba[1] = rgba[2] = v;
#endif
    rgba[3] = 1.;
  }

  static int componentCount(const std::string& components)
  {
    if (components == kOfxImageComponentRGBA) return 4;
    if (components == kOfxImageComponentRGB) return 3;
    if (components == kOfxImageComponentAlpha) return 1;
    return 0;
  }

  static int bytesPerComponent(const std::string& depth)
  {
    if (depth == kOfxBitDepthByte) return 1;
    if (depth == kOfxBitDepthShort) return 2;
    if (depth == kOfxBitDepthFloat) return 4;
    return 0;
  }

  // clamp to [0,1]; NaN gives 0, so that the conversion to an integer is always defined
  static double clamp01(double v)
  {
    if (!(v > 0.)) {
      return 0.;
    }
    return v > 1. ? 1. : v;
  }

  // local min/max: std::min and std::max break if windows.h (included by ofxhBinary.h) defines
  // the min and max macros, i.e. without NOMINMAX
  static inline double minD(double a, double b) { return a < b ? a : b; }
  static inline double maxD(double a, double b) { return a > b ? a : b; }
  static inline int minI(int a, int b) { return a < b ? a : b; }
  static inline int maxI(int a, int b) { return a > b ? a : b; }

  // the nearest int (NaN gives 0), so that converting huge values (e.g. an infinite RoD) is defined
  static int clampToInt(double v)
  {
    if (!(v == v)) {
      return 0;
    }
    if (v <= (double)INT_MIN) {
      return INT_MIN;
    }
    if (v >= (double)INT_MAX) {
      return INT_MAX;
    }
    return (int)v;
  }

  bool MyImage::isSupported(const std::string& depth, const std::string& components)
  {
    return componentCount(components) > 0 && bytesPerComponent(depth) > 0;
  }

  MyImage::MyImage(MyClipInstance &clip, OfxTime time, int view,
                   const OfxRectI& bounds, const OfxRectI& rod, const OfxPointD& renderScale,
                   bool isOutput)
    : OFX::Host::ImageEffect::Image(clip) /// this ctor sets the depth, components, premult and PAR from the clip
    , _data(NULL)
    , _depth(clip.getPixelDepth())
    , _components(clip.getComponents())
    , _nComps(componentCount(_components))
    , _bytesPerComp(bytesPerComponent(_depth))
    , _rowBytes(0)
    , _bounds(bounds)
  {
    assert(isSupported(_depth, _components));
    if (_bounds.x2 < _bounds.x1) _bounds.x2 = _bounds.x1;
    if (_bounds.y2 < _bounds.y1) _bounds.y2 = _bounds.y1;
    const int width = _bounds.x2 - _bounds.x1;
    const int height = _bounds.y2 - _bounds.y1;
    _rowBytes = width * _nComps * _bytesPerComp;
    const size_t size = (size_t)_rowBytes * (size_t)height;

    // make some memory (new[] returns memory suitably aligned for float)
    _data = new unsigned char[size > 0 ? size : 1];

    if (isOutput) {
      // fill with the sentinel, so that the pixels that the plug-in did not write show
      if (_depth == kOfxBitDepthFloat) {
        std::fill(reinterpret_cast<float*>(_data), reinterpret_cast<float*>(_data + size),
                  std::numeric_limits<float>::quiet_NaN());
      } else {
        std::fill(_data, _data + size, (unsigned char)0xCD);
      }
    } else {
      const InputSpec& spec = clip.getInput();
      // full-resolution size of the RoD, in pixels
      const double W = (rod.x2 - rod.x1) / renderScale.x;
      const double H = (rod.y2 - rod.y1) / renderScale.y;
      double rgba[4];
      double out[4];
      for (int y = _bounds.y1; y < _bounds.y2; ++y) {
        unsigned char* row = _data + (size_t)(y - _bounds.y1) * _rowBytes;
        // the pixel centre, in full-resolution pixel coordinates
        const double Y = (y + 0.5) / renderScale.y;
        for (int x = _bounds.x1; x < _bounds.x2; ++x) {
          const double X = (x + 0.5) / renderScale.x;
          // the patterns are unpremultiplied colours (getPremult() is kOfxImageUnPreMultiplied
          // for RGBA and Alpha); RGB images drop the alpha, and are opaque
          evalPattern(spec, time, view, X, Y, W, H, rgba);
          if (_nComps == 1) {
            out[0] = rgba[3];
          } else {
            for (int c = 0; c < _nComps; ++c) {
              out[c] = rgba[c];
            }
          }
          unsigned char* pix = row + (size_t)(x - _bounds.x1) * _nComps * _bytesPerComp;
          for (int c = 0; c < _nComps; ++c) {
            if (_bytesPerComp == 1) {
              pix[c] = (unsigned char)std::floor(clamp01(out[c]) * 255. + 0.5);
            } else if (_bytesPerComp == 2) {
              reinterpret_cast<unsigned short*>(pix)[c] = (unsigned short)std::floor(clamp01(out[c]) * 65535. + 0.5);
            } else {
              reinterpret_cast<float*>(pix)[c] = (float)out[c];
            }
          }
        }
      }
    }

    setDoubleProperty(kOfxImageEffectPropRenderScale, renderScale.x, 0);
    setDoubleProperty(kOfxImageEffectPropRenderScale, renderScale.y, 1);

    // data ptr
    setPointerProperty(kOfxImagePropData, _data);

    // bounds and rod
    setIntProperty(kOfxImagePropBounds, _bounds.x1, 0);
    setIntProperty(kOfxImagePropBounds, _bounds.y1, 1);
    setIntProperty(kOfxImagePropBounds, _bounds.x2, 2);
    setIntProperty(kOfxImagePropBounds, _bounds.y2, 3);

    setIntProperty(kOfxImagePropRegionOfDefinition, rod.x1, 0);
    setIntProperty(kOfxImagePropRegionOfDefinition, rod.y1, 1);
    setIntProperty(kOfxImagePropRegionOfDefinition, rod.x2, 2);
    setIntProperty(kOfxImagePropRegionOfDefinition, rod.y2, 3);

    // row bytes
    setIntProperty(kOfxImagePropRowBytes, _rowBytes);

    setStringProperty(kOfxImagePropField, kOfxImageFieldNone);
    std::ostringstream uid;
    uid << clip.getName() << ":" << time << ":" << view << (isOutput ? ":output" : "");
    setStringProperty(kOfxImagePropUniqueIdentifier, uid.str());
  }

  bool MyImage::getPixelRGB8(int x, int y, int rgb[3], bool& notWritten) const
  {
    notWritten = false;
    if (x < _bounds.x1 || x >= _bounds.x2 || y < _bounds.y1 || y >= _bounds.y2) {
      return false;
    }
    const unsigned char* pix = _data + (size_t)(y - _bounds.y1) * _rowBytes + (size_t)(x - _bounds.x1) * _nComps * _bytesPerComp;
    int v[4];
    for (int c = 0; c < _nComps; ++c) {
      if (_bytesPerComp == 1) {
        v[c] = pix[c];
      } else if (_bytesPerComp == 2) {
        v[c] = (reinterpret_cast<const unsigned short*>(pix)[c] * 255 + 32767) / 65535;
      } else {
        float f = reinterpret_cast<const float*>(pix)[c];
        if (f != f) { // NaN
          notWritten = true;
          v[c] = 0;
        } else {
          v[c] = (int)std::floor(clamp01(f) * 255. + 0.5);
        }
      }
    }
    if (_nComps == 1) {
      rgb[0] = rgb[1] = rgb[2] = v[0];
    } else {
      rgb[0] = v[0];
      rgb[1] = v[1];
      rgb[2] = v[2];
    }
    return true;
  }

  double MyImage::getComponent(int x, int y, int c) const
  {
    const unsigned char* pix = getPixelData(x, y);
    if (_bytesPerComp == 1) {
      return pix[c];
    } else if (_bytesPerComp == 2) {
      return reinterpret_cast<const unsigned short*>(pix)[c];
    }
    return reinterpret_cast<const float*>(pix)[c];
  }

  float MyImage::getComponentFloat(int x, int y, int c) const
  {
    const unsigned char* pix = getPixelData(x, y);
    if (_bytesPerComp == 1) {
      return pix[c] / 255.f;
    } else if (_bytesPerComp == 2) {
      return reinterpret_cast<const unsigned short*>(pix)[c] / 65535.f;
    }
    return reinterpret_cast<const float*>(pix)[c];
  }

  MyImage::~MyImage() 
  {
    delete [] _data;
  }

  MyClipInstance::MyClipInstance(MyEffectInstance* effect, OFX::Host::ImageEffect::ClipDescriptor *desc)
    : OFX::Host::ImageEffect::ClipInstance(effect, *desc)
    , _effect(effect)
    , _name(desc->getName())
    , _outputImage(NULL)
    , _input()
    , _unmappedDepth(kOfxBitDepthByte)
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    , _view(0)
#endif
    , _inRoDAction(false)
  {
  }

  MyClipInstance::~MyClipInstance()
  {
    releaseOutputImage();
  }

  void MyClipInstance::releaseOutputImage()
  {
    if(_outputImage) {
      _outputImage->releaseReference();
      _outputImage = NULL;
    }
  }
   
  /// Get the Raw Unmapped Pixel Depth from the host: 8 bits by default (hostDemo -d sets it),
  /// kOfxBitDepthNone if the clip is disconnected ("implying a clip is unconnected image")
  const std::string &MyClipInstance::getUnmappedBitDepth() const
  {
    static const std::string none(kOfxBitDepthNone);
    return getConnected() ? _unmappedDepth : none;
  }
    
  /// Get the Raw Unmapped Components from the host: RGBA, or kOfxImageComponentNone if the
  /// clip is disconnected ("implying a clip is unconnected, not valid for an image")
  const std::string &MyClipInstance::getUnmappedComponents() const
  {
    static const std::string rgba(kOfxImageComponentRGBA);
    static const std::string none(kOfxImageComponentNone);
    return getConnected() ? rgba : none;
  }

  /// The mapped pixel depth: kOfxBitDepthNone if the clip is disconnected
  const std::string &MyClipInstance::getPixelDepth() const
  {
    static const std::string none(kOfxBitDepthNone);
    return getConnected() ? OFX::Host::ImageEffect::ClipInstance::getPixelDepth() : none;
  }

  /// The mapped components: kOfxImageComponentNone if the clip is disconnected
  const std::string &MyClipInstance::getComponents() const
  {
    static const std::string none(kOfxImageComponentNone);
    return getConnected() ? OFX::Host::ImageEffect::ClipInstance::getComponents() : none;
  }

  // PreMultiplication -
  //
  //  kOfxImageOpaque - the image is opaque and so has no premultiplication state
  //  kOfxImagePreMultiplied - the image is premultiplied by it's alpha
  //  kOfxImageUnPreMultiplied - the image is unpremultiplied
  //
  // The output clip has the premultiplication set by the clip preferences. An RGB input (or
  // a disconnected one) is opaque: "an image with no alpha is opaque". RGBA and Alpha inputs are
  // unpremultiplied: the synthetic patterns are unpremultiplied colours.
  const std::string &MyClipInstance::getPremult() const
  {
    static const std::string opaque(kOfxImageOpaque);
    static const std::string unpremult(kOfxImageUnPreMultiplied);
    if (isOutput()) {
      return _effect->getOutputPreMultiplication();
    }
    if (!getConnected() || getComponents() == kOfxImageComponentRGB) {
      return opaque;
    }
    if (_input.type == InputSpec::eFile && !_input.premult.empty()) {
      return _input.premult;
    }
    return unpremult;
  }


#ifdef OFX_EXTENSIONS_NATRON
  // Format -
  // The format of the clip or image (in pixel coordinates)
  OfxRectI MyClipInstance::getFormat() const
  {
    OfxRectI v;
    v.x1 = v.y1 = 0;
    if (!isOutput() && _input.type == InputSpec::eFile) {
      // a PFM file: its bounds, in pixels at render scale 1
      v.x1 = (int)std::floor(_input.fileX / _input.fileScaleX);
      v.y1 = (int)std::floor(_input.fileY / _input.fileScaleY);
      v.x2 = (int)std::ceil((_input.fileX + _input.file.width) / _input.fileScaleX);
      v.y2 = (int)std::ceil((_input.fileY + _input.file.height) / _input.fileScaleY);
      return v;
    }
    /// our clip is pretending to be progressive PAL SD, so return 0<=x<768, 0<=y<576
    v.x2 = 768;
    v.y2 = 576;
    return v;
  }
#endif

  // Pixel Aspect Ratio -
  //
  //  The pixel aspect ratio of a clip or image.
  double MyClipInstance::getAspectRatio() const
  {
    /// our clip is pretending to be progressive PAL SD, so return 1.06666
    return kPalPixelAspect;
  }
  
  // Frame Rate -
  double MyClipInstance::getFrameRate() const
  {
    /// our clip is pretending to be progressive PAL SD, so return 25
    return 25.0;
  }
  
  // Frame Range (startFrame, endFrame) -
  //
  //  The frame range over which a clip has images.
  void MyClipInstance::getFrameRange(double &startFrame, double &endFrame) const
  {
    // pretend we have a second's worth of PAL SD
    startFrame = 0;
    endFrame = 25;
  }

  /// Field Order - Which spatial field occurs temporally first in a frame.
  /// \returns 
  ///  - kOfxImageFieldNone - the clip material is unfielded
  ///  - kOfxImageFieldLower - the clip material is fielded, with image rows 0,2,4.... occuring first in a frame
  ///  - kOfxImageFieldUpper - the clip material is fielded, with image rows line 1,3,5.... occuring first in a frame
  const std::string &MyClipInstance::getFieldOrder() const
  {
    /// our clip is pretending to be progressive PAL SD, so return kOfxImageFieldNone
    static const std::string v(kOfxImageFieldNone);
    return v;
  }
        
  // Connected -
  //
  //  Says whether the clip is actually connected at the moment.
  bool MyClipInstance::getConnected() const
  {
    return isOutput() || _input.type != InputSpec::eNone;
  }
  
  // Unmapped Frame Rate -
  //
  //  The unmaped frame range over which an output clip has images.
  double MyClipInstance::getUnmappedFrameRate() const
  {
    /// our clip is pretending to be progressive PAL SD, so return 25
    return 25;
  }
  
  // Unmapped Frame Range -
  //
  //  The unmaped frame range over which an output clip has images.
  // this is applicable only to hosts and plugins that allow a plugin to change frame rates
  void MyClipInstance::getUnmappedFrameRange(double &unmappedStartFrame, double &unmappedEndFrame) const
  {
    // pretend we have a second's worth of PAL SD
    unmappedStartFrame = 0;
    unmappedEndFrame = 25;
  }

  // Continuous Samples -
  //
  //  0 if the images can only be sampled at discreet times (eg: the clip is a sequence of frames),
  //  1 if the images can only be sampled continuously (eg: the clip is infact an animating roto spline and can be rendered anywhen). 
  bool MyClipInstance::getContinuousSamples() const
  {
    return false;
  }


  /// the rod of the clip in canonical coordinates (see getRoD)
  OfxRectD MyClipInstance::getRegionOfDefinition(OfxTime time) const
  {
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    return getRoD(time, _view);
#else
    return getRoD(time, 0);
#endif
  }

  OfxRectD MyClipInstance::getRoD(OfxTime time, int view) const
  {
    (void)view;
    OfxRectD v;
    v.x1 = v.y1 = v.x2 = v.y2 = 0.;
    if (isOutput()) {
      // the output RoD is the effect's RoD (the guard protects against a plug-in that
      // asks for the output RoD in its own RoD action)
      if (_inRoDAction) {
        return v;
      }
      _inRoDAction = true;
      OfxRectD rod = v;
      OfxStatus stat = _effect->getRegionOfDefinitionAction(time, _effect->getRenderScale(),
#ifdef OFX_EXTENSIONS_NUKE
                                                            view,
#endif
                                                            rod);
      _inRoDAction = false;
      if (stat == kOfxStatOK || stat == kOfxStatReplyDefault) {
        v = rod;
      }
      return v;
    }
    if (!getConnected()) {
      return v; // a disconnected clip has an empty RoD
    }
    if (_input.type == InputSpec::eFile) {
      // a PFM file: one file pixel per clip pixel at the render scale fileScale, from
      // (fileX,fileY), so the RoD is its pixel bounds divided by that scale, times the clip's
      // PAR (the project's, since hostDemo doesn't support multiple clip PARs) in x
      const double par = getAspectRatio();
      v.x1 = _input.fileX / _input.fileScaleX * par;
      v.y1 = _input.fileY / _input.fileScaleY;
      v.x2 = (_input.fileX + _input.file.width) / _input.fileScaleX * par;
      v.y2 = (_input.fileY + _input.file.height) / _input.fileScaleY;
      return v;
    }
    /// our clip is pretending to be progressive PAL SD, so return 0<=x<768, 0<=y<576
    v.x2 = 768;
    v.y2 = 576;
    return v;
  }
  
  // intersection of two rectangles (may be empty, i.e. x2 <= x1 or y2 <= y1)
  static OfxRectD intersect(const OfxRectD& a, const OfxRectD& b)
  {
    OfxRectD r;
    r.x1 = maxD(a.x1, b.x1);
    r.y1 = maxD(a.y1, b.y1);
    r.x2 = minD(a.x2, b.x2);
    r.y2 = minD(a.y2, b.y2);
    return r;
  }

  OfxRectI intersect(const OfxRectI& a, const OfxRectI& b)
  {
    OfxRectI r;
    r.x1 = maxI(a.x1, b.x1);
    r.y1 = maxI(a.y1, b.y1);
    r.x2 = minI(a.x2, b.x2);
    r.y2 = minI(a.y2, b.y2);
    return r;
  }

  OfxRectI canonicalToPixelEnclosing(const OfxRectD& r, const OfxPointD& renderScale, double par)
  {
    // the epsilon absorbs rounding errors, e.g. 768 / (768/720) = 720.0000000001
    const double eps = 1e-6;
    OfxRectI p;
    p.x1 = clampToInt(std::floor(r.x1 * renderScale.x / par + eps));
    p.y1 = clampToInt(std::floor(r.y1 * renderScale.y + eps));
    p.x2 = clampToInt(std::ceil(r.x2 * renderScale.x / par - eps));
    p.y2 = clampToInt(std::ceil(r.y2 * renderScale.y - eps));
    return p;
  }

  MyImage* MyClipInstance::fetchImage(OfxTime time, int view, const OfxRectD *optionalBounds)
  {
    if (!getConnected()) {
      return NULL;
    }
    const std::string& depth = getPixelDepth();
    const std::string& components = getComponents();
    if (!MyImage::isSupported(depth, components)) {
      std::cerr << "Error: clip " << _name << ": hostDemo can't make images with pixel depth \"" << depth
                << "\" and components \"" << components << "\"" << std::endl;
      ++gImageErrors;
      return NULL;
    }
    const OfxPointD& renderScale = _effect->getRenderScale();
    const double par = getAspectRatio();
    const OfxRectD rodCanonical = getRoD(time, view);
    const OfxRectI rod = canonicalToPixelEnclosing(rodCanonical, renderScale, par);

    if (isOutput()) {
      if (!_outputImage) {
        // a new image for each render, covering the render window, i.e. the output RoD
        // clipped to the project (outside of render, the whole RoD)
        OfxRectI bounds = _effect->isRendering() ? _effect->getRenderWindow() : intersect(rod, _effect->getProjectPixelRect(renderScale));
        _outputImage = new MyImage(*this, time, view, bounds, rod, renderScale, /*isOutput=*/true);
      }
      // add another reference to the member image for this fetch: as we have a
      // ref count of 1 due to construction, the image is not deleted when the
      // plug-in releases it, so that hostDemo can save it after the render.
      _outputImage->addReference();
      return _outputImage;
    }

    // Input clip: fetch on demand, the image is deleted when the plug-in releases it.
    // The bounds are optionalBounds if given, else the RoI the plug-in asked for in this
    // render, else the RoD; clipped to the RoD.
    OfxRectD region = rodCanonical;
    OfxRectD roi;
    if (optionalBounds) {
      region = *optionalBounds;
    } else if (_effect->getRegionOfInterest(this, roi)) {
      region = roi;
    }
    region = intersect(region, rodCanonical);
    if (region.x2 <= region.x1 || region.y2 <= region.y1) {
      return NULL; // nothing to fetch
    }
    OfxRectI bounds = intersect(canonicalToPixelEnclosing(region, renderScale, par), rod);
    if (bounds.x2 <= bounds.x1 || bounds.y2 <= bounds.y1) {
      return NULL;
    }
    return new MyImage(*this, time, view, bounds, rod, renderScale, /*isOutput=*/false);
  }

  /// override this to fill in the image at the given time.
  /// The bounds of the image on the image plane should be 
  /// 'appropriate', typically the value returned in getRegionsOfInterest
  /// on the effect instance. Outside a render call, the optionalBounds should
  /// be 'appropriate' for the.
  /// If bounds is not null, fetch the indicated section of the canonical image plane.
  OFX::Host::ImageEffect::Image* MyClipInstance::getImage(OfxTime time, const OfxRectD *optionalBounds)
  {
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    return fetchImage(time, _view, optionalBounds);
#else
    return fetchImage(time, 0, optionalBounds);
#endif
  }

#ifdef OFX_EXTENSIONS_NUKE
  /// override this to fill in the given image plane at the given time.
  /// The bounds of the image on the image plane should be
  /// 'appropriate', typically the value returned in getRegionsOfInterest
  /// on the effect instance.
  /// Outside a render call, the optionalBounds should
  /// be 'appropriate' for the image.
  /// If bounds is not null, fetch the indicated section of the canonical image plane.
  ///
  /// This function implements both V1 of the image plane suite and V2. In the V1 the parameter view was not present and
  /// will be passed -1, indicating that you should on your own retrieve the correct index of the view at which the render called was issues
  /// by using thread local storage. In V2 the view index will be correctly set with a value >= 0.
  ///
  OFX::Host::ImageEffect::Image* MyClipInstance::getImagePlane(OfxTime time, int view, const std::string& plane,const OfxRectD *optionalBounds)
  {
    // only the colour plane
    if (plane != kFnOfxImagePlaneColour) {
      return NULL;
    }
    return fetchImage(time, view < 0 ? _view : view, optionalBounds);
  }

  /// override this to return the rod on the clip for the given view
  OfxRectD MyClipInstance::getRegionOfDefinition(OfxTime time, int view) const
  {
    return getRoD(time, view < 0 ? _view : view);
  }
#endif

#ifdef OFX_EXTENSIONS_VEGAS
  /// override this to fill in the image at the given time from a specific view
  /// (using the standard callback gets you the current view being rendered, @see getImage).
  /// The bounds of the image on the image plane should be
  /// 'appropriate', typically the value returned in getRegionsOfInterest
  /// on the effect instance. Outside a render call, the optionalBounds should
  /// be 'appropriate' for the.
  /// If bounds is not null, fetch the indicated section of the canonical image plane.
  OFX::Host::ImageEffect::Image* MyClipInstance::getStereoscopicImage(OfxTime time, int view, const OfxRectD *optionalBounds)
  {
    return fetchImage(time, view, optionalBounds);
  }
#endif

#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
  /// set the default view returned by getImage()
  void MyClipInstance::setView(int view) {
    if (view != _view) {
      releaseOutputImage();
      _view = view;
    }
  }
#endif
} // MyHost
