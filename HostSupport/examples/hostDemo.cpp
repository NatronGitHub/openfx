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


#include <algorithm> // std::reverse (no std::min/std::max: windows.h may define min/max macros)
#include <cerrno>
#include <climits>
#include <cstdio> // fopen (binary files)
#include <iomanip> // setprecision
#include <iostream>
#include <limits>
#include <fstream>
#include <cassert>
#include <stdexcept>
#include <sstream> // stringstream
#include <string>
#include <vector>
#include <cstdlib> // strtol, strtod
#include <cstring> // strcmp, memcpy
#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h> // _CrtSetDbgFlag
#endif
#include <cmath> // ceil
#include <list>
#include <map>
#include <utility> // pair

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
#include "hostDemoParamInstance.h"
   
////////////////////////////////////////////////////////////////////////////////
// By default, this example code renders the example 'invert' plugin built
// with the OFX C++ support library. Other filter or general plug-ins can be
// rendered with the -p and -c options, and their parameters can be set from
// the command line (run "hostDemo -h" for the usage).
//
//
// It's job is purely to show you what functions you need to fill in to provide
// an OFX image effect plugin host. It is (as we say in England) a 'noddy' example.
// 
// Run this in a debugger and step through to see what it does. Also be sure to
// build the invert example and set OFX_PLUGIN_PATH so that it can see it.
//
// It works by hard coding progressive PAL SD imagery to the input clips. The output covers the
// RoD returned by the plug-in, clipped to the PAL SD project frame.
// By default, the input images are black at t=0 and white at t=1 (and should be
// inverted by the invert plugin); -i sets other inputs, or disconnects a clip.
// Images have the depth and components that the clip preferences map each clip to.
//
// Inputs may also be read from PFM files (-i Clip=file:path.pfm); the output is written as PPM
// (8 or 16 bits) or PFM files, and --stats prints a summary and a checksum of each output image.

// The output file formats (--format)
enum OutputFormat {
  eFormatPPM,   ///< plain PPM (P3), 8 bits, top row first
  eFormatPPM16, ///< binary PPM (P6), maxval 65535 (big-endian), top row first
  eFormatPFM    ///< PFM, little-endian (scale -1.0), bottom row first
};

// the number of pixels of the image that hold a NaN component, i.e. the NaN sentinel of
// unwritten pixels (float images only: the 0xCD sentinel of byte and short images can't be told
// apart from real data)
static long countNaNPixels(const MyHost::MyImage& im)
{
  if (im.getBytesPerComponent() != 4) {
    return 0;
  }
  const OfxRectI& b = im.getBounds();
  const int nComps = im.getNComponents();
  long n = 0;
  for (int y = b.y1; y < b.y2; ++y) {
    for (int x = b.x1; x < b.x2; ++x) {
      const float* pix = reinterpret_cast<const float*>(im.getPixelData(x, y));
      for (int c = 0; c < nComps; ++c) {
        if (pix[c] != pix[c]) {
          ++n;
          break;
        }
      }
    }
  }
  return n;
}

// clamp to [0,1]; NaN gives 0
static double clamp01(double v)
{
  if (!(v > 0.)) {
    return 0.;
  }
  return v > 1. ? 1. : v;
}

// Write the image (its bounds, i.e. the render window) to fname as a plain PPM, top row first
// (OFX images are bottom-up). Returns false (after printing an error) if the file couldn't be written.
// Short and float images are converted to 8 bits (float values are clamped to [0,1]), and Alpha
// images are written as grey levels. Unwritten (NaN) float pixels are written as 0.
static bool exportToPPM(const std::string& fname, const MyHost::MyImage& im)
{
  std::ofstream op(fname.c_str());
  if (!op) {
    std::cerr << "Error: cannot open " << fname << " for writing" << std::endl;
    return false;
  }
  const OfxRectI& b = im.getBounds();
  // Netpbm allows comments in the header before the maxval, not after it
  op << "P3" << std::endl;
  op << "# " << im.getDepth() << " " << im.getComponentsString() << ", converted to 8 bits" << std::endl;
  op << "# bounds " << b.x1 << " " << b.y1 << " " << b.x2 << " " << b.y2 << std::endl;
  op << b.x2 - b.x1 << " " << b.y2 - b.y1 << std::endl;
  op << "255" << std::endl;
  for (int y = b.y2 - 1; y >= b.y1; --y) {
    for (int x = b.x1; x < b.x2; ++x) {
      int rgb[3] = { 0, 0, 0 };
      bool nan = false;
      im.getPixelRGB8(x, y, rgb, nan);
      op << rgb[0] << " " << rgb[1] << " " << rgb[2] << "\n";
    }
  }
  op.close();
  if (!op) {
    std::cerr << "Error: cannot write " << fname << std::endl;
    return false;
  }
  return true;
}

// close the binary file f, written to fname. Returns false (after printing an error) on failure.
static bool closeBinary(FILE* f, const std::string& fname)
{
  const bool err = std::ferror(f) != 0;
  if (std::fclose(f) != 0 || err) {
    std::cerr << "Error: cannot write " << fname << std::endl;
    return false;
  }
  return true;
}

// Write the image as a 16-bit binary PPM: P6, maxval 65535, big-endian ("the most significant
// byte is first"), top row first. Byte values are multiplied by 257, float values are clamped to
// [0,1] (NaN gives 0), and Alpha images are written as grey levels.
static bool exportToPPM16(const std::string& fname, const MyHost::MyImage& im)
{
  FILE* f = std::fopen(fname.c_str(), "wb");
  if (!f) {
    std::cerr << "Error: cannot open " << fname << " for writing" << std::endl;
    return false;
  }
  const OfxRectI& b = im.getBounds();
  const int w = b.x2 - b.x1;
  std::fprintf(f, "P6\n# %s %s, converted to 16 bits\n# bounds %d %d %d %d\n%d %d\n65535\n",
               im.getDepth().c_str(), im.getComponentsString().c_str(), b.x1, b.y1, b.x2, b.y2, w, b.y2 - b.y1);
  const int nComps = im.getNComponents();
  const int bpc = im.getBytesPerComponent();
  std::vector<unsigned char> row((size_t)w * 6);
  for (int y = b.y2 - 1; y >= b.y1; --y) {
    for (int x = b.x1; x < b.x2; ++x) {
      for (int c = 0; c < 3; ++c) {
        const double v = im.getComponent(x, y, nComps == 1 ? 0 : c);
        unsigned int u;
        if (bpc == 1) {
          u = (unsigned int)v * 257;
        } else if (bpc == 2) {
          u = (unsigned int)v;
        } else {
          u = (unsigned int)std::floor(clamp01(v) * 65535. + 0.5);
        }
        unsigned char* p = &row[((size_t)(x - b.x1) * 3 + c) * 2];
        p[0] = (unsigned char)(u >> 8);
        p[1] = (unsigned char)(u & 0xff);
      }
    }
    std::fwrite(&row[0], 1, row.size(), f);
  }
  return closeBinary(f, fname);
}

// Write component c0 (nOut = 1, "Pf") or components 0..2 (nOut = 3, "PF") of the image as a PFM:
// little-endian (scale -1.0), rows "in left to right, bottom to top order"
// (http://www.pauldebevec.com/Research/HDR/PFM/), the opposite of PPM. The values are floats:
// byte and short values are divided by 255 and 65535, float values are unchanged (NaN included).
static bool writePFM(const std::string& fname, const MyHost::MyImage& im, int nOut, int c0)
{
  FILE* f = std::fopen(fname.c_str(), "wb");
  if (!f) {
    std::cerr << "Error: cannot open " << fname << " for writing" << std::endl;
    return false;
  }
  const OfxRectI& b = im.getBounds();
  const int w = b.x2 - b.x1;
  std::fprintf(f, "%s\n%d %d\n-1.0\n", nOut == 3 ? "PF" : "Pf", w, b.y2 - b.y1);
  std::vector<unsigned char> row((size_t)w * nOut * 4);
  for (int y = b.y1; y < b.y2; ++y) {
    for (int x = b.x1; x < b.x2; ++x) {
      for (int c = 0; c < nOut; ++c) {
        const float v = im.getComponentFloat(x, y, c0 + c);
        unsigned int u;
        std::memcpy(&u, &v, 4);
        unsigned char* p = &row[((size_t)(x - b.x1) * nOut + c) * 4];
        p[0] = (unsigned char)(u & 0xff);
        p[1] = (unsigned char)((u >> 8) & 0xff);
        p[2] = (unsigned char)((u >> 16) & 0xff);
        p[3] = (unsigned char)(u >> 24);
      }
    }
    std::fwrite(&row[0], 1, row.size(), f);
  }
  return closeBinary(f, fname);
}

// Write the image as PFM: RGB and RGBA images to base.pfm ("PF", the RGB components), and the alpha
// of RGBA images to base.alpha.pfm ("Pf"); Alpha images to base.pfm ("Pf").
static bool exportToPFM(const std::string& base, const MyHost::MyImage& im)
{
  const int nComps = im.getNComponents();
  if (nComps == 1) {
    return writePFM(base + ".pfm", im, 1, 0);
  }
  if (!writePFM(base + ".pfm", im, 3, 0)) {
    return false;
  }
  return nComps != 4 || writePFM(base + ".alpha.pfm", im, 1, 3);
}

// CRC-32 (ISO-HDLC, as in zlib and PNG: polynomial 0xEDB88320 reflected, initial value and final
// XOR 0xFFFFFFFF), updated with n bytes
static unsigned long crc32Update(unsigned long crc, const unsigned char* p, size_t n)
{
  static unsigned long table[256];
  static bool init = false;
  if (!init) {
    for (unsigned long i = 0; i < 256; ++i) {
      unsigned long c = i;
      for (int k = 0; k < 8; ++k) {
        c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
      }
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc & 0xFFFFFFFFUL;
  for (size_t i = 0; i < n; ++i) {
    crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  }
  return ~crc & 0xFFFFFFFFUL;
}

// print a value of the --stats line: integers for byte and short images, else a float with
// enough digits to round-trip ("nan" if there was no value)
static void printStatValue(std::ostream& os, double v, bool integer)
{
  if (v != v) {
    os << "nan";
  } else if (integer) {
    os << (long)v;
  } else {
    std::ostringstream ss; // %.9g
    ss << std::setprecision(9) << v;
    os << ss.str();
  }
}

// Print the --stats line of the image on stdout (see usage())
static void printStats(const MyHost::MyImage& im, OfxTime t, int view)
{
  const OfxRectI& b = im.getBounds();
  const int nComps = im.getNComponents();
  const int bpc = im.getBytesPerComponent();
  const double nan = std::numeric_limits<double>::quiet_NaN();
  double mn[4] = { nan, nan, nan, nan };
  double mx[4] = { nan, nan, nan, nan };
  double sum[4] = { 0., 0., 0., 0. };
  long count[4] = { 0, 0, 0, 0 };
  long nanPixels = 0;
  unsigned long crc = 0;
  // the bytes of a component, in little-endian order whatever the host's byte order
  const unsigned short one = 1;
  const bool littleEndian = *reinterpret_cast<const unsigned char*>(&one) == 1;
  std::vector<unsigned char> row((size_t)(b.x2 - b.x1) * nComps * bpc);
  for (int y = b.y1; y < b.y2; ++y) {
    std::memcpy(row.empty() ? NULL : &row[0], im.getPixelData(b.x1, y), row.size());
    if (!littleEndian && bpc > 1) {
      for (size_t i = 0; i + bpc <= row.size(); i += bpc) {
        std::reverse(row.begin() + i, row.begin() + i + bpc);
      }
    }
    if (!row.empty()) {
      crc = crc32Update(crc, &row[0], row.size());
    }
    for (int x = b.x1; x < b.x2; ++x) {
      bool isNaN = false;
      for (int c = 0; c < nComps; ++c) {
        const double v = im.getComponent(x, y, c);
        if (v != v) {
          isNaN = true;
          continue;
        }
        if (count[c] == 0 || v < mn[c]) {
          mn[c] = v;
        }
        if (count[c] == 0 || v > mx[c]) {
          mx[c] = v;
        }
        sum[c] += v;
        ++count[c];
      }
      if (isNaN) {
        ++nanPixels;
      }
    }
  }
  std::ostringstream os;
  os << "stats frame=" << t << " view=" << view << " w=" << b.x2 - b.x1 << " h=" << b.y2 - b.y1
     << " bounds=" << b.x1 << "," << b.y1 << "," << b.x2 << "," << b.y2
     << " depth=" << (bpc == 1 ? "byte" : bpc == 2 ? "short" : "float")
     << " comps=" << (nComps == 4 ? "RGBA" : nComps == 3 ? "RGB" : "Alpha");
  const char* names[3] = { " min=", " max=", " mean=" };
  for (int k = 0; k < 3; ++k) {
    os << names[k];
    for (int c = 0; c < nComps; ++c) {
      if (c > 0) {
        os << ",";
      }
      const double v = (k == 0) ? mn[c] : (k == 1) ? mx[c] : (count[c] > 0 ? sum[c] / count[c] : nan);
      printStatValue(os, v, bpc != 4 && k != 2);
    }
  }
  os << " nan=" << nanPixels << " crc32=" << std::hex << std::setw(8) << std::setfill('0') << crc;
  std::cout << os.str() << std::endl;
}

static void usage(const char* prog)
{
  std::cout <<
    "Usage: " << prog << " [-h] [-p pluginId] [-m major] [-c filter|general] [-o prefix]\n"
    "       [--format ppm|ppm16|pfm] [--stats] [-d byte|short|float] [-i Clip=spec]...\n"
    "       [--no-std-paths] [--no-cache | --cache path] [--quiet] [--allow-unwritten]\n"
    "       [-s name=value[,value...]]... [name=value[,value...]]...\n"
    "Render a filter or general OFX plug-in found in OFX_PLUGIN_PATH, on synthetic\n"
    "PAL SD input clips (or PFM files), and write the output frames as PPM or PFM files.\n"
    "  -h          print this help and exit\n"
    "  -p id       plug-in identifier (default: net.sf.openfx.invertPlugin)\n"
    "  -m major    plug-in major version (default: the highest version found)\n"
    "  -c context  filter or general (default: filter)\n"
    "  -o prefix   output file prefix (default: Output): writes <prefix>.<t>[l|r].ppm (or\n"
    "              .pfm). The output covers the render window: the RoD returned by the\n"
    "              plug-in, clipped to the project frame.\n"
    "  --format f  output file format (default: ppm):\n"
    "                ppm    plain PPM (P3), 8 bits, top row first. Short and float images are\n"
    "                       converted to 8 bits (float is clamped to [0,1]).\n"
    "                ppm16  binary PPM (P6), maxval 65535, big-endian, top row first. Byte\n"
    "                       values are multiplied by 257, float is clamped to [0,1].\n"
    "                pfm    PFM, little-endian (scale -1.0), bottom row first (as in OFX\n"
    "                       images; PFM rows are \"bottom to top\"). Byte and short values are\n"
    "                       converted to float in [0,1], float values (NaN included) are kept.\n"
    "                       RGB and RGBA images are written as PF (RGB): the alpha of an RGBA\n"
    "                       image goes to <prefix>.<t>[l|r].alpha.pfm (Pf). Alpha images are Pf.\n"
    "              In PPM files, Alpha images are written as grey and unwritten (NaN) pixels as 0.\n"
    "  --stats     for each output image, print a line on stdout (even with --quiet):\n"
    "                stats frame=T view=V w=W h=H bounds=x1,y1,x2,y2 depth=byte|short|float\n"
    "                comps=RGBA|RGB|Alpha min=c0[,c1...] max=... mean=... nan=N crc32=XXXXXXXX\n"
    "              with one value per component, in the native range of the depth (integers\n"
    "              for byte and short min and max, %.9g floats otherwise, \"nan\" if no value).\n"
    "              NaN values are left out of min, max and mean; nan is the number of pixels\n"
    "              with a NaN component. crc32 is the CRC-32 (as in zlib) of the pixel bytes,\n"
    "              row by row from the bottom row, within the bounds (so it doesn't depend on\n"
    "              the row padding), with each component in little-endian byte order.\n"
    "  -d depth    pixel depth of the input clips before clip preferences: byte, short or\n"
    "              float (default: byte). The images have the depth and components that\n"
    "              getClipPreferences maps each clip to.\n"
    "  -i Clip=spec\n"
    "              content of the input clip named Clip (e.g. Source), where spec is:\n"
    "                default        a grey level t (black at t=0, white at t=1, above 1\n"
    "                               in float images), with the time drawn in the reverse colour\n"
    "                solid:r,g,b,a  a constant colour\n"
    "                ramp           r=g=b=a from 0 (left) to 1 (right)\n"
    "                checker:N      N-pixel squares, alternating 0 and 1 (r=g=b=a)\n"
    "                file:path.pfm  a PFM image (PF or Pf, either byte order), the same at all\n"
    "                               times. Each file pixel is a clip pixel (at render scale 1):\n"
    "                               the clip's RoD is the file size in pixels, at the project\n"
    "                               PAR (768/720), i.e. 0,0,width*PAR,height in canonical\n"
    "                               coordinates. PF gives (r,g,b,1), Pf gives r=g=b=a=value.\n"
    "                               The values are converted to the clip's depth (clamped to\n"
    "                               [0,1] for byte and short) and components. Malformed files\n"
    "                               are rejected.\n"
    "                none           the clip is disconnected (only for optional clips)\n"
    "              Numbers must be finite decimals (no nan, inf or hexadecimal).\n"
    "  --no-std-paths\n"
    "              only look for plug-ins in OFX_PLUGIN_PATH, not in the standard\n"
    "              locations (/Library/OFX/Plugins, /usr/OFX/Plugins...)\n"
    "  --no-cache  don't read or write the plug-in cache\n"
    "  --cache path\n"
    "              read and write the plug-in cache at path (default:\n"
    "              hostDemoPluginCache.xml in the current directory)\n"
    "  --quiet     print nothing on stdout but the plug-in's messages and the --stats lines\n"
    "  --allow-unwritten\n"
    "              don't fail if output pixels were not written by the plug-in. The output\n"
    "              image is filled with NaN (float) or 0xCD bytes (byte, short) before\n"
    "              rendering, but only NaN can be detected: use -d float for that check.\n"
    "  -s name=value[,value...]\n"
    "              set a parameter before rendering (the option name -s may be omitted).\n"
    "              Multi-dimensional parameters take comma-separated values, e.g.\n"
    "              value=0.5,0.5,0.5,0.5 for an RGBA. Booleans take 0/1/true/false,\n"
    "              Choice takes the option index or label, String/StrChoice/Custom\n"
    "              take the whole value (commas included). Numbers must be finite decimals.\n"
    "The exit status is 0 on success, and non-zero on any failure, including a\n"
    "disconnected mandatory clip and (for float output) unwritten pixels.\n";
}

static std::vector<std::string> splitValues(const std::string& s)
{
  std::vector<std::string> v;
  std::string::size_type start = 0;
  for (;;) {
    std::string::size_type comma = s.find(',', start);
    if (comma == std::string::npos) {
      v.push_back(s.substr(start));
      break;
    }
    v.push_back(s.substr(start, comma - start));
    start = comma + 1;
  }
  return v;
}

static bool parseDouble(const std::string& s, double& d)
{
  return MyHost::parseFiniteDouble(s, d);
}

static bool parseInt(const std::string& s, int& i)
{
  return MyHost::parseInteger(s, i);
}

static bool parseBool(const std::string& s, bool& b)
{
  if (s == "1" || s == "true" || s == "yes" || s == "on") {
    b = true;
    return true;
  }
  if (s == "0" || s == "false" || s == "no" || s == "off") {
    b = false;
    return true;
  }
  return false;
}

// parse n doubles, separated by commas
static bool parseDoubles(const std::string& s, int n, double* d)
{
  std::vector<std::string> v = splitValues(s);
  if ((int)v.size() != n) {
    return false;
  }
  for (int i = 0; i < n; ++i) {
    if (!parseDouble(v[i], d[i])) {
      return false;
    }
  }
  return true;
}

// parse n ints, separated by commas
static bool parseInts(const std::string& s, int n, int* d)
{
  std::vector<std::string> v = splitValues(s);
  if ((int)v.size() != n) {
    return false;
  }
  for (int i = 0; i < n; ++i) {
    if (!parseInt(v[i], d[i])) {
      return false;
    }
  }
  return true;
}

// set the parameter value from the string "name=value[,value...]".
// Returns false and prints an error on failure.
static bool setParamFromString(OFX::Host::ImageEffect::Instance* instance, const std::string& assignment, bool quiet)
{
  std::string::size_type eq = assignment.find('=');
  if (eq == std::string::npos || eq == 0) {
    std::cerr << "Error: invalid parameter assignment \"" << assignment << "\", expected name=value[,value...]" << std::endl;
    return false;
  }
  std::string name = assignment.substr(0, eq);
  std::string value = assignment.substr(eq + 1);
  OFX::Host::Param::Instance* param = instance->getParam(name);
  if (!param) {
    std::cerr << "Error: the plug-in has no parameter named \"" << name << "\"" << std::endl;
    return false;
  }
  const std::string& type = param->getType();
  OfxStatus stat = kOfxStatErrUnsupported;
  bool parsed = false;
  double d[4];
  int i[3];
  if (type == kOfxParamTypeString || type == kOfxParamTypeCustom || type == kOfxParamTypeStrChoice) {
    OFX::Host::Param::StringInstance* p = dynamic_cast<OFX::Host::Param::StringInstance*>(param);
    OFX::Host::Param::StrChoiceInstance* c = dynamic_cast<OFX::Host::Param::StrChoiceInstance*>(param);
    parsed = true;
    if (p) {
      stat = p->set(value.c_str());
    } else if (c) {
      stat = c->set(value.c_str());
    }
  } else if (type == kOfxParamTypeBytes) {
    OFX::Host::Param::BytesInstance* p = dynamic_cast<OFX::Host::Param::BytesInstance*>(param);
    parsed = true;
    if (p) {
      stat = p->set(reinterpret_cast<const unsigned char*>(value.data()), value.size());
    }
  } else if (type == kOfxParamTypeBoolean) {
    OFX::Host::Param::BooleanInstance* p = dynamic_cast<OFX::Host::Param::BooleanInstance*>(param);
    bool b = false;
    parsed = parseBool(value, b);
    if (parsed && p) {
      stat = p->set(b);
    }
  } else if (type == kOfxParamTypeInteger) {
    OFX::Host::Param::IntegerInstance* p = dynamic_cast<OFX::Host::Param::IntegerInstance*>(param);
    parsed = parseInts(value, 1, i);
    if (parsed && p) {
      stat = p->set(i[0]);
    }
  } else if (type == kOfxParamTypeChoice) {
    OFX::Host::Param::ChoiceInstance* p = dynamic_cast<OFX::Host::Param::ChoiceInstance*>(param);
    parsed = parseInts(value, 1, i);
    if (!parsed) {
      // look for the option label
      const OFX::Host::Property::Set& props = param->getProperties();
      int n = props.getDimension(kOfxParamPropChoiceOption);
      for (int k = 0; k < n; ++k) {
        if (props.getStringProperty(kOfxParamPropChoiceOption, k) == value) {
          i[0] = k;
          parsed = true;
          break;
        }
      }
    }
    if (parsed && p) {
      stat = p->set(i[0]);
    }
  } else if (type == kOfxParamTypeDouble) {
    OFX::Host::Param::DoubleInstance* p = dynamic_cast<OFX::Host::Param::DoubleInstance*>(param);
    parsed = parseDoubles(value, 1, d);
    if (parsed && p) {
      stat = p->set(d[0]);
    }
  } else if (type == kOfxParamTypeDouble2D) {
    OFX::Host::Param::Double2DInstance* p = dynamic_cast<OFX::Host::Param::Double2DInstance*>(param);
    parsed = parseDoubles(value, 2, d);
    if (parsed && p) {
      stat = p->set(d[0], d[1]);
    }
  } else if (type == kOfxParamTypeInteger2D) {
    OFX::Host::Param::Integer2DInstance* p = dynamic_cast<OFX::Host::Param::Integer2DInstance*>(param);
    parsed = parseInts(value, 2, i);
    if (parsed && p) {
      stat = p->set(i[0], i[1]);
    }
  } else if (type == kOfxParamTypeDouble3D) {
    OFX::Host::Param::Double3DInstance* p = dynamic_cast<OFX::Host::Param::Double3DInstance*>(param);
    parsed = parseDoubles(value, 3, d);
    if (parsed && p) {
      stat = p->set(d[0], d[1], d[2]);
    }
  } else if (type == kOfxParamTypeInteger3D) {
    OFX::Host::Param::Integer3DInstance* p = dynamic_cast<OFX::Host::Param::Integer3DInstance*>(param);
    parsed = parseInts(value, 3, i);
    if (parsed && p) {
      stat = p->set(i[0], i[1], i[2]);
    }
  } else if (type == kOfxParamTypeRGB) {
    OFX::Host::Param::RGBInstance* p = dynamic_cast<OFX::Host::Param::RGBInstance*>(param);
    parsed = parseDoubles(value, 3, d);
    if (parsed && p) {
      stat = p->set(d[0], d[1], d[2]);
    }
  } else if (type == kOfxParamTypeRGBA) {
    OFX::Host::Param::RGBAInstance* p = dynamic_cast<OFX::Host::Param::RGBAInstance*>(param);
    parsed = parseDoubles(value, 4, d);
    if (parsed && p) {
      stat = p->set(d[0], d[1], d[2], d[3]);
    }
  } else {
    std::cerr << "Error: cannot set parameter \"" << name << "\" of type " << type << std::endl;
    return false;
  }
  if (!parsed) {
    std::cerr << "Error: invalid value \"" << value << "\" for parameter \"" << name << "\" of type " << type << std::endl;
    return false;
  }
  if (stat != kOfxStatOK) {
    std::cerr << "Error: could not set parameter \"" << name << "\" (status " << stat << ")" << std::endl;
    return false;
  }
  if (!quiet) {
    std::cerr << "Set parameter " << name << " = " << value << std::endl;
  }
  return true;
}

// render the frame t (and view), write the output image to base.ppm (or base.pfm) and print its
// stats if asked. Returns false (after printing an error) on failure.
static bool renderFrame(MyHost::MyEffectInstance* instance, MyHost::MyClipInstance* outputClip,
                        OfxTime t, int view, int nViews, const OfxPointD& renderScale,
                        bool allowUnwritten, const std::string& base, OutputFormat format, bool stats)
{
  const std::string fname = base + (format == eFormatPFM ? ".pfm" : ".ppm");
  (void)view;
  (void)nViews;
  const double par = instance->getProjectPixelAspectRatio();

  // the output RoD is what the plug-in's getRegionOfDefinition action returns
  OfxRectD rod;
  OfxStatus stat = instance->getRegionOfDefinitionAction(t, renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                                         view,
#endif
                                                         rod);
  if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
    std::cerr << "Error: get region of definition action failed at time " << t << " (status " << stat << ")" << std::endl;
    return false;
  }

  // The render window is in pixel coordinates (render scale and PAR applied): the output RoD,
  // clipped to the project frame
  OfxRectI renderWindow = MyHost::intersect(MyHost::canonicalToPixelEnclosing(rod, renderScale, par),
                                            instance->getProjectPixelRect(renderScale));
  if (renderWindow.x2 <= renderWindow.x1 || renderWindow.y2 <= renderWindow.y1) {
    std::cerr << "Warning: the output RoD (" << rod.x1 << "," << rod.y1 << "," << rod.x2 << "," << rod.y2
              << ") at time " << t << " doesn't intersect the project: nothing to render, "
              << fname << " not written" << std::endl;
    return true;
  }

  /// RoI is in canonical coords
  OfxRectD regionOfInterest;
  regionOfInterest.x1 = renderWindow.x1 * par / renderScale.x;
  regionOfInterest.y1 = renderWindow.y1 / renderScale.y;
  regionOfInterest.x2 = renderWindow.x2 * par / renderScale.x;
  regionOfInterest.y2 = renderWindow.y2 / renderScale.y;

  // get the RoI for each input clip: the input images are fetched with these bounds
  // (clipped to the RoD), unless the plug-in asks for specific bounds
  std::map<OFX::Host::ImageEffect::ClipInstance *, OfxRectD> rois;
  stat = instance->getRegionOfInterestAction(t, renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                                       view,
#endif
                                                       regionOfInterest, rois);
  if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
    std::cerr << "Error: get regions of interest action failed at time " << t << " (status " << stat << ")" << std::endl;
    return false;
  }

  // each render gets a new output image, filled with the sentinel
  instance->setRenderState(renderScale, renderWindow, rois);
  outputClip->releaseOutputImage();
  const int imageErrors = MyHost::gImageErrors;

#ifdef OFX_EXTENSIONS_NUKE
  std::list<std::string> planes;
  planes.push_back(kFnOfxImagePlaneColour);
#endif
  stat = instance->renderAction(t, //OfxTime      time,
                                kOfxImageFieldNone, // const std::string &  field,
                                renderWindow, // const OfxRectI &renderRoI,
                                renderScale, // OfxPointD   renderScale,
                                true, // bool     sequentialRender,
                                false, // bool     interactiveRender,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                false, // bool     openGLRender,
#  ifdef OFX_EXTENSIONS_NATRON
                                NULL, // void*    contextData,
#  endif
#endif
                                false // bool     draftRender
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
                                ,
                                view // int view
#endif
#ifdef OFX_EXTENSIONS_VEGAS
                                ,
                                nViews // int nViews
#endif
#ifdef OFX_EXTENSIONS_NUKE
                                ,
                                planes // const std::list<std::string>& planes
#endif
                                );
  instance->clearRenderState();

  // get the output image buffer
  MyHost::MyImage *outputImage = outputClip->getOutputImage();
  if (stat != kOfxStatOK || !outputImage) {
    std::cerr << "Error: render action failed at time " << t << " (status " << stat
              << (outputImage ? "" : ", no output image") << ")" << std::endl;
    return false;
  }
  if (MyHost::gImageErrors != imageErrors) {
    std::cerr << "Error: image fetches failed at time " << t << std::endl;
    return false;
  }
  bool ok;
  if (format == eFormatPFM) {
    ok = exportToPFM(base, *outputImage);
  } else if (format == eFormatPPM16) {
    ok = exportToPPM16(fname, *outputImage);
  } else {
    ok = exportToPPM(fname, *outputImage);
  }
  if (!ok) {
    return false;
  }
  if (stats) {
    printStats(*outputImage, t, view);
  }
  const long notWritten = countNaNPixels(*outputImage);
  if (notWritten > 0) {
    const OfxRectI& b = outputImage->getBounds();
    std::cerr << (allowUnwritten ? "Warning: " : "Error: ") << fname << ": " << notWritten << " of "
              << (long)(b.x2 - b.x1) * (long)(b.y2 - b.y1)
              << " pixels were not written by the plug-in (they hold NaN"
              << (format == eFormatPFM ? ")" : "), written as 0") << std::endl;
    return allowUnwritten;
  }
  return true;
}

static int run(int argc, char **argv)
{
  std::string pluginId = "net.sf.openfx.invertPlugin";
  int pluginMajor = -1;
  std::string context = kOfxImageEffectContextFilter;
  std::string outputPrefix = "Output";
  std::string inputDepth = kOfxBitDepthByte;
  std::vector<std::string> paramAssignments;
  std::vector<std::pair<std::string, MyHost::InputSpec> > inputs;
  bool useStdPaths = true;
  bool useCache = true;
  std::string cachePath = "hostDemoPluginCache.xml";
  bool quiet = false;
  bool allowUnwritten = false;
  bool stats = false;
  OutputFormat format = eFormatPPM;

  for (int a = 1; a < argc; ++a) {
    std::string arg = argv[a];
    if (arg == "-h" || arg == "--help") {
      usage(argv[0]);
      return 0;
    } else if (arg == "--no-std-paths") {
      useStdPaths = false;
    } else if (arg == "--no-cache") {
      useCache = false;
    } else if (arg == "--quiet") {
      quiet = true;
    } else if (arg == "--allow-unwritten") {
      allowUnwritten = true;
    } else if (arg == "--stats") {
      stats = true;
    } else if (arg == "-p" || arg == "-m" || arg == "-c" || arg == "-o" || arg == "-s" ||
               arg == "-d" || arg == "-i" || arg == "--cache" || arg == "--format") {
      if (a + 1 >= argc) {
        std::cerr << "Error: option " << arg << " requires an argument" << std::endl;
        usage(argv[0]);
        return 1;
      }
      std::string optarg = argv[++a];
      if (arg == "-p") {
        pluginId = optarg;
      } else if (arg == "-m") {
        if (!parseInt(optarg, pluginMajor) || pluginMajor < 0) {
          std::cerr << "Error: invalid major version \"" << optarg << "\"" << std::endl;
          return 1;
        }
      } else if (arg == "-c") {
        if (optarg == "filter" || optarg == kOfxImageEffectContextFilter) {
          context = kOfxImageEffectContextFilter;
        } else if (optarg == "general" || optarg == kOfxImageEffectContextGeneral) {
          context = kOfxImageEffectContextGeneral;
        } else {
          std::cerr << "Error: unsupported context \"" << optarg << "\", expected filter or general" << std::endl;
          return 1;
        }
      } else if (arg == "-o") {
        outputPrefix = optarg;
      } else if (arg == "--format") {
        if (optarg == "ppm") {
          format = eFormatPPM;
        } else if (optarg == "ppm16") {
          format = eFormatPPM16;
        } else if (optarg == "pfm") {
          format = eFormatPFM;
        } else {
          std::cerr << "Error: unsupported format \"" << optarg << "\", expected ppm, ppm16 or pfm" << std::endl;
          return 1;
        }
      } else if (arg == "-d") {
        if (optarg == "byte" || optarg == kOfxBitDepthByte) {
          inputDepth = kOfxBitDepthByte;
        } else if (optarg == "short" || optarg == kOfxBitDepthShort) {
          inputDepth = kOfxBitDepthShort;
        } else if (optarg == "float" || optarg == kOfxBitDepthFloat) {
          inputDepth = kOfxBitDepthFloat;
        } else {
          std::cerr << "Error: unsupported depth \"" << optarg << "\", expected byte, short or float" << std::endl;
          return 1;
        }
      } else if (arg == "-i") {
        std::string::size_type eq = optarg.find('=');
        MyHost::InputSpec spec;
        std::string error = "no clip name";
        if (eq == std::string::npos || eq == 0 || !MyHost::parseInputSpec(optarg.substr(eq + 1), spec, error)) {
          std::cerr << "Error: invalid input \"" << optarg << "\" (" << error
                    << "), expected Clip=default|solid:r,g,b,a|ramp|checker:N|file:path.pfm|none" << std::endl;
          return 1;
        }
        inputs.push_back(std::make_pair(optarg.substr(0, eq), spec));
      } else if (arg == "--cache") {
        useCache = true;
        cachePath = optarg;
      } else {
        paramAssignments.push_back(optarg);
      }
    } else if (!arg.empty() && arg[0] != '-' && arg.find('=') != std::string::npos) {
      paramAssignments.push_back(arg);
    } else {
      std::cerr << "Error: unknown argument \"" << arg << "\"" << std::endl;
      usage(argv[0]);
      return 1;
    }
  }

  //_CrtSetBreakAlloc(3168);
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetDbgFlag ( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
#endif
  // this must be called before the first call to getPluginCache()
  OFX::Host::PluginCache::useStdOFXPluginsLocation(useStdPaths);

  // set the version label in the global cache
  OFX::Host::PluginCache::getPluginCache()->setCacheVersion("hostDemoV1");

  // create our derived image effect host which provides
  // a factory to make plugin instances and acts
  // as a description of the host application
  MyHost::Host myHost;

  // make an image effect plugin cache. This is what knows about
  // all the plugins.
  OFX::Host::ImageEffect::PluginCache imageEffectPluginCache(&myHost);

  // register the image effect cache with the global plugin cache
  imageEffectPluginCache.registerInCache(*OFX::Host::PluginCache::getPluginCache());

  if (useCache) {
    // try to read an old cache
    std::ifstream ifs(cachePath.c_str());
    if (ifs) {
      try {
        OFX::Host::PluginCache::getPluginCache()->readCache(ifs);
      } catch (const std::exception &e) {
        std::cerr << "Error while reading XML cache: " << e.what() << std::endl;
      }
    }
  }
  OFX::Host::PluginCache::getPluginCache()->scanPluginFiles();

  if (useCache) {
    /// flush out the current cache
    std::ofstream of(cachePath.c_str());
    OFX::Host::PluginCache::getPluginCache()->writePluginCache(of);
    of.close();
    if (!of) {
      std::cerr << "Warning: could not write the plug-in cache " << cachePath << std::endl;
    }
  }

  // get the plugin (by default, the invert example plugin which uses the OFX C++ support code)
  OFX::Host::ImageEffect::ImageEffectPlugin* plugin = imageEffectPluginCache.getPluginById(pluginId, pluginMajor);

  if (!quiet) {
    imageEffectPluginCache.dumpToStdOut();
  }

  int ret = 0;
  if(!plugin) {
    std::cerr << "Error: plug-in \"" << pluginId << "\"";
    if (pluginMajor >= 0) {
      std::cerr << " with major version " << pluginMajor;
    }
    std::cerr << " not found. Check that OFX_PLUGIN_PATH contains the folder of its .ofx.bundle." << std::endl;
    ret = 1;
  } else if (plugin->getContexts().find(context) == plugin->getContexts().end()) {
    std::cerr << "Error: plug-in \"" << pluginId << "\" does not support the " << context << " context." << std::endl;
    ret = 1;
  } else {
    // create an instance of it in the requested context (filter by default)
    // the first arg is the context, the second is client data we are allowed to pass down the call chain

    OFX::Host::auto_ptr<OFX::Host::ImageEffect::Instance> instance(plugin->createInstance(context, NULL));
    MyHost::MyEffectInstance* myInstance = dynamic_cast<MyHost::MyEffectInstance*>(instance.get());

    if(!myInstance) {
      std::cerr << "Error: could not create an instance of plug-in \"" << pluginId << "\" in the " << context << " context." << std::endl;
      ret = 1;
    }

    // set up the input clips: depth and content (or disconnected)
    if (ret == 0) {
      const std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>& clips = myInstance->getClipInstances();
      for (std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
        MyHost::MyClipInstance* clip = dynamic_cast<MyHost::MyClipInstance*>(it->second);
        if (clip && !clip->isOutput()) {
          clip->setUnmappedBitDepth(inputDepth);
        }
      }
      for (size_t k = 0; k < inputs.size(); ++k) {
        MyHost::MyClipInstance* clip = dynamic_cast<MyHost::MyClipInstance*>(instance->getClip(inputs[k].first));
        if (!clip || clip->isOutput()) {
          std::cerr << "Error: the plug-in has no input clip named \"" << inputs[k].first << "\". Its clips are:";
          for (std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
            std::cerr << " " << it->first;
          }
          std::cerr << std::endl;
          ret = 1;
          break;
        }
        clip->setInput(inputs[k].second);
      }
      myInstance->setDefaultDepth(inputDepth);
    }

    // "Any clip that is not optional will *always* be connected during a render action"
    // (kOfxImageClipPropOptional): a mandatory clip can't be disconnected
    if (ret == 0 && !instance->checkClipConnectionStatus()) {
      const std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>& clips = myInstance->getClipInstances();
      std::cerr << "Error: these clips are not optional, they can't be disconnected (-i Clip=none):";
      for (std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
        if (!it->second->isOptional() && !it->second->getConnected()) {
          std::cerr << " " << it->first;
        }
      }
      std::cerr << std::endl;
      ret = 1;
    }

    // set the parameter values given on the command line, before calling the create instance action
    for (size_t k = 0; ret == 0 && k < paramAssignments.size(); ++k) {
      if (!setParamFromString(instance.get(), paramAssignments[k], quiet)) {
        ret = 1;
      }
    }

    // now we need to call the create instance action. Only call this once you have initialised all the params
    // and clips to their correct values. So if you are loading a saved plugin state, set up your params from
    // that state, _then_ call create instance.
    if (ret == 0) {
      OfxStatus stat = instance->createInstanceAction();
      if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
        std::cerr << "Error: create instance action failed (status " << stat << ")" << std::endl;
        ret = 1;
      }
    }

    // now we need to to call getClipPreferences on the instance so that it does the clip component/depth
    // logic and caches away the components and depth on each clip.
    if (ret == 0 && !instance->getClipPreferences()) {
      std::cerr << "Error: get clip preferences action failed" << std::endl;
      ret = 1;
    }

    // get the output clip
    MyHost::MyClipInstance* outputClip = NULL;
    if (ret == 0) {
      outputClip = dynamic_cast<MyHost::MyClipInstance*>(instance->getClip(kOfxImageEffectOutputClipName));
      if (!outputClip) {
        std::cerr << "Error: the plug-in has no output clip" << std::endl;
        ret = 1;
      }
    }

    if(ret == 0)
    {
      // current render scale of 1
      OfxPointD renderScale;
      renderScale.x = renderScale.y = 1.0;

      int numFramesToRender = OFXHOSTDEMOCLIPLENGTH;

      // say we are about to render a bunch of frames 
      OfxStatus stat = instance->beginRenderAction(0, numFramesToRender, 1.0, false, renderScale, /*sequential=*/true, /*interactive=*/false,
#                                        ifdef OFX_SUPPORTS_OPENGLRENDER
                                         /*openGLRender=*/false,
#                                        ifdef OFX_EXTENSIONS_NATRON
                                         /*contextData=*/NULL,
#                                        endif
#                                        endif
                                         /*draftRender=*/false
#                                        ifdef OFX_EXTENSIONS_NUKE
                                         , 0 /* view*/
#                                        endif
                                         );
      if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
        std::cerr << "Error: begin render action failed (status " << stat << ")" << std::endl;
        ret = 1;
      }

      for(int t = 0; ret == 0 && t <= numFramesToRender; ++t) 
      {
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
        // render a stereoscopic frame: left and right views
        for (int view = 0; ret == 0 && view < 2; ++view) {
          std::ostringstream ss;
          ss << outputPrefix << "." << t << (view == 0 ? "l" : "r");
          if (!renderFrame(myInstance, outputClip, t, view, 2, renderScale, allowUnwritten, ss.str(), format, stats)) {
            ret = 1;
          }
        }
#else
        std::ostringstream ss;
        ss << outputPrefix << "." << t;
        if (!renderFrame(myInstance, outputClip, t, 0, 1, renderScale, allowUnwritten, ss.str(), format, stats)) {
          ret = 1;
        }
#endif
      }

      stat = instance->endRenderAction(0, numFramesToRender, 1.0, false, renderScale, /*sequential=*/true, /*interactive=*/false,
#                               ifdef OFX_SUPPORTS_OPENGLRENDER
                                /*openGLRender=*/false,
#                               ifdef OFX_EXTENSIONS_NATRON
                                /*contextData=*/NULL,
#                               endif
#                               endif
                                /*draftRender=*/false
#                               ifdef OFX_EXTENSIONS_NUKE
                                , 0 /* view*/
#                               endif
                                );
      if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
        std::cerr << "Error: end render action failed (status " << stat << ")" << std::endl;
        ret = 1;
      }
      // release the last output image before the instance is destroyed
      outputClip->releaseOutputImage();
    }
  }
  OFX::Host::PluginCache::clearPluginCache();
  return ret;
}

int main(int argc, char **argv)
{
  try {
    return run(argc, argv);
  } catch (const std::exception& e) {
    std::cerr << "Error: exception: " << e.what() << std::endl;
  } catch (...) {
    std::cerr << "Error: unknown exception" << std::endl;
  }
  return 1;
}
