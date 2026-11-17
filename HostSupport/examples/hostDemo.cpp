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
#include <iomanip> // setw
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
#include <set>
#include <map>
#include <utility> // pair

// ofx
#include "ofxCore.h"
#include "ofxImageEffect.h"
#include "ofxPixels.h"
#include "ofxGPURender.h" // kOfxImageEffectPropCudaRenderSupported...
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
#include "ofxhUtilities.h" // StatStr

// my host
#include "hostDemoJson.h"
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
// or PFM files, and --report writes a JSON summary of the run.

// The output file formats (--format)
enum OutputFormat {
  eFormatPPM,   ///< plain PPM (P3), 8 bits, top row first
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

// the numbers of an output image, for the report (see the README)
struct ImageStats {
  OfxRectI bounds;
  int nComps;
  int bytesPerComponent;
  double min[4], max[4], mean[4]; ///< NaN if there is no value
  long nanPixels;
  unsigned long crc32;
};

static const char* statsDepthName(int bytesPerComponent)
{
  return bytesPerComponent == 1 ? "byte" : bytesPerComponent == 2 ? "short" : "float";
}

static const char* statsComponentsName(int nComps)
{
  return nComps == 4 ? "RGBA" : nComps == 3 ? "RGB" : "Alpha";
}

static ImageStats computeStats(const MyHost::MyImage& im)
{
  ImageStats st;
  const OfxRectI& b = im.getBounds();
  st.bounds = b;
  const int nComps = im.getNComponents();
  const int bpc = im.getBytesPerComponent();
  st.nComps = nComps;
  st.bytesPerComponent = bpc;
  const double nan = std::numeric_limits<double>::quiet_NaN();
  double sum[4] = { 0., 0., 0., 0. };
  long count[4] = { 0, 0, 0, 0 };
  for (int c = 0; c < 4; ++c) {
    st.min[c] = st.max[c] = st.mean[c] = nan;
  }
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
        if (count[c] == 0 || v < st.min[c]) {
          st.min[c] = v;
        }
        if (count[c] == 0 || v > st.max[c]) {
          st.max[c] = v;
        }
        sum[c] += v;
        ++count[c];
      }
      if (isNaN) {
        ++nanPixels;
      }
    }
  }
  for (int c = 0; c < nComps; ++c) {
    if (count[c] > 0) {
      st.mean[c] = sum[c] / count[c];
    }
  }
  st.nanPixels = nanPixels;
  st.crc32 = crc;
  return st;
}

static std::string crc32String(unsigned long crc)
{
  std::ostringstream os;
  os << std::hex << std::setw(8) << std::setfill('0') << crc;
  return os.str();
}

////////////////////////////////////////////////////////////////////////////////
// JSON for --report and --describe (see python/README.md)

using MyHost::Json;

static Json rectJson(const OfxRectD& r)
{
  Json j = Json::array();
  j.push(r.x1); j.push(r.y1); j.push(r.x2); j.push(r.y2);
  return j;
}

static Json rectJson(const OfxRectI& r)
{
  Json j = Json::array();
  j.push(r.x1); j.push(r.y1); j.push(r.x2); j.push(r.y2);
  return j;
}

static Json pointJson(double x, double y)
{
  Json j = Json::array();
  j.push(x); j.push(y);
  return j;
}

static bool statusOK(OfxStatus stat)
{
  return stat == kOfxStatOK || stat == kOfxStatReplyDefault;
}

// the result of an action: "ok" (it succeeded: kOfxStatOK or kOfxStatReplyDefault), "status" (the
// name of the status code)
static Json actionJson(OfxStatus stat)
{
  Json j = Json::object();
  j["ok"] = statusOK(stat);
  j["status"] = OFX::StatStr(stat);
  return j;
}

// the result of an action for which HostSupport only says whether it succeeded: the status is null
static Json actionJson(bool ok)
{
  Json j = Json::object();
  j["ok"] = ok;
  j["status"] = Json();
  return j;
}

// an entry of the "actions" list
static Json namedActionJson(const char* action, const Json& result)
{
  Json j = Json::object();
  j["action"] = action;
  j["ok"] = result.get("ok");
  j["status"] = result.get("status");
  return j;
}

// the numbers of an output image, and the files it was written to
static Json statsJson(const ImageStats& st, const std::vector<std::string>& files)
{
  Json j = Json::object();
  const OfxRectI& b = st.bounds;
  j["w"] = b.x2 - b.x1;
  j["h"] = b.y2 - b.y1;
  j["bounds"] = rectJson(b);
  j["depth"] = statsDepthName(st.bytesPerComponent);
  j["comps"] = statsComponentsName(st.nComps);
  const char* names[3] = { "min", "max", "mean" };
  for (int k = 0; k < 3; ++k) {
    Json& a = j[names[k]] = Json::array();
    for (int c = 0; c < st.nComps; ++c) {
      a.push((k == 0) ? st.min[c] : (k == 1) ? st.max[c] : st.mean[c]); // NaN (no value) is null
    }
  }
  j["nan"] = st.nanPixels;
  j["crc32"] = crc32String(st.crc32);
  Json& f = j["files"] = Json::array();
  for (size_t i = 0; i < files.size(); ++i) {
    f.push(files[i]);
  }
  return j;
}

// the values of the property name of props: a scalar if it has one value and !forceArray, else an
// array; null if the property doesn't exist (or is a pointer)
static Json propJson(const OFX::Host::Property::Set& props, const char* name, bool forceArray = false)
{
  OFX::Host::Property::Property* p = props.fetchProperty(name);
  if (!p || p->getType() == OFX::Host::Property::ePointer) {
    return Json();
  }
  const int n = p->getDimension();
  Json a = Json::array();
  for (int i = 0; i < n; ++i) {
    switch (p->getType()) {
    case OFX::Host::Property::eInt:
      a.push(props.getIntProperty(name, i));
      break;
    case OFX::Host::Property::eDouble:
      a.push(props.getDoubleProperty(name, i));
      break;
    default:
      a.push(props.getStringProperty(name, i));
      break;
    }
  }
  return (n == 1 && !forceArray) ? a.at(0) : a;
}

// an int property as a boolean (null if it doesn't exist)
static Json propBoolJson(const OFX::Host::Property::Set& props, const char* name)
{
  OFX::Host::Property::Property* p = props.fetchProperty(name);
  if (!p || p->getType() != OFX::Host::Property::eInt || p->getDimension() < 1) {
    return Json();
  }
  return Json(props.getIntProperty(name) != 0);
}

// the plug-in identification, for --report and --describe
static Json pluginJson(OFX::Host::ImageEffect::ImageEffectPlugin* plugin)
{
  Json j = Json::object();
  j["id"] = plugin->getIdentifier();
  j["versionMajor"] = plugin->getVersionMajor();
  j["versionMinor"] = plugin->getVersionMinor();
  const OFX::Host::ImageEffect::Descriptor& d = plugin->getDescriptor();
  j["label"] = d.getLabel();
  j["grouping"] = d.getPluginGrouping();
  j["filePath"] = propJson(d.getProps(), kOfxPluginPropFilePath);
  return j;
}

// the effect flags of a descriptor, for --describe
static Json effectFlagsJson(const OFX::Host::Property::Set& p)
{
  Json j = Json::object();
  j["supportedPixelDepths"] = propJson(p, kOfxImageEffectPropSupportedPixelDepths, true);
  j["supportsTiles"] = propBoolJson(p, kOfxImageEffectPropSupportsTiles);
  j["supportsMultiResolution"] = propBoolJson(p, kOfxImageEffectPropSupportsMultiResolution);
  j["temporalClipAccess"] = propBoolJson(p, kOfxImageEffectPropTemporalClipAccess);
  j["supportsMultipleClipDepths"] = propBoolJson(p, kOfxImageEffectPropSupportsMultipleClipDepths);
  j["supportsMultipleClipPARs"] = propBoolJson(p, kOfxImageEffectPropSupportsMultipleClipPARs);
  j["fieldRenderTwiceAlways"] = propBoolJson(p, kOfxImageEffectPluginPropFieldRenderTwiceAlways);
  j["singleInstance"] = propBoolJson(p, kOfxImageEffectPluginPropSingleInstance);
  j["hostFrameThreading"] = propBoolJson(p, kOfxImageEffectPluginPropHostFrameThreading);
  j["renderThreadSafety"] = propJson(p, kOfxImageEffectPluginRenderThreadSafety);
  j["sequentialRender"] = propJson(p, kOfxImageEffectInstancePropSequentialRender);
  j["clipPreferencesSlaveParams"] = propJson(p, kOfxImageEffectPropClipPreferencesSlaveParam, true);
  // GPU rendering: "false", "true" or "needed"
  j["openGLRenderSupported"] = propJson(p, kOfxImageEffectPropOpenGLRenderSupported);
  j["openCLRenderSupported"] = propJson(p, kOfxImageEffectPropOpenCLRenderSupported);
  j["cudaRenderSupported"] = propJson(p, kOfxImageEffectPropCudaRenderSupported);
  j["cudaStreamSupported"] = propJson(p, kOfxImageEffectPropCudaStreamSupported);
  j["metalRenderSupported"] = propJson(p, kOfxImageEffectPropMetalRenderSupported);
  j["cpuRenderSupported"] = propJson(p, kOfxImageEffectPropCPURenderSupported);
  return j;
}

// a clip descriptor, for --describe
static Json clipJson(const OFX::Host::ImageEffect::ClipDescriptor& c)
{
  const OFX::Host::Property::Set& p = c.getProps();
  Json j = Json::object();
  j["name"] = c.getName();
  j["label"] = c.getLabel();
  // the hint and secret properties of clips are Natron extensions: null if the build doesn't have them
  j["hint"] = propJson(p, kOfxParamPropHint);
  j["optional"] = c.isOptional();
  j["isMask"] = c.isMask();
  j["supportedComponents"] = propJson(p, kOfxImageEffectPropSupportedComponents, true);
  j["temporalClipAccess"] = c.temporalAccess();
  j["supportsTiles"] = c.supportsTiles();
  j["fieldExtraction"] = c.getFieldExtraction();
  j["secret"] = propBoolJson(p, kOfxParamPropSecret);
  return j;
}

// a parameter descriptor, for --describe. page is the page parameter that lists it as a child.
static Json paramJson(const OFX::Host::Param::Descriptor& d, const std::string& page)
{
  const OFX::Host::Property::Set& p = d.getProperties();
  Json j = Json::object();
  j["name"] = d.getName();
  j["type"] = d.getType();
  j["label"] = d.getLabel();
  j["hint"] = d.getHint();
  j["scriptName"] = d.getScriptName();
  j["secret"] = d.getSecret();
  j["enabled"] = d.getEnabled();
  j["parent"] = d.getParentName();
  j["page"] = page;
  // the dimension is the number of values of the default (1 for strings, booleans, choices...)
  OFX::Host::Property::Property* def = p.fetchProperty(kOfxParamPropDefault);
  j["dimension"] = def ? def->getDimension() : 0;
  j["default"] = propJson(p, kOfxParamPropDefault);
  j["min"] = propJson(p, kOfxParamPropMin);
  j["max"] = propJson(p, kOfxParamPropMax);
  j["displayMin"] = propJson(p, kOfxParamPropDisplayMin);
  j["displayMax"] = propJson(p, kOfxParamPropDisplayMax);
  j["doubleType"] = propJson(p, kOfxParamPropDoubleType);
  j["choiceOptions"] = propJson(p, kOfxParamPropChoiceOption, true);
  j["animates"] = propBoolJson(p, kOfxParamPropAnimates);
  j["evaluateOnChange"] = propBoolJson(p, kOfxParamPropEvaluateOnChange);
  j["children"] = propJson(p, kOfxParamPropPageChild, true); // pages only
  return j;
}

// --describe: the plug-in's contexts, and in each context its effect flags, clips and parameters.
// Returns false if a context couldn't be described.
static bool describePlugin(OFX::Host::ImageEffect::ImageEffectPlugin* plugin, Json& out)
{
  bool ok = true;
  out["schema"] = "hostDemo.describe";
  out["schemaVersion"] = 1;
  out["plugin"] = pluginJson(plugin);
  Json& contexts = out["contexts"] = Json::object();
  const std::set<std::string>& cs = plugin->getContexts();
  for (std::set<std::string>::const_iterator it = cs.begin(); it != cs.end(); ++it) {
    Json& c = contexts[*it] = Json::object();
    OFX::Host::ImageEffect::Descriptor* d = plugin->getContext(*it);
    if (!d) {
      std::cerr << "Error: the describe in context action failed for the " << *it << " context" << std::endl;
      c["ok"] = false;
      ok = false;
      continue;
    }
    c["ok"] = true;
    c["flags"] = effectFlagsJson(d->getProps());
    Json& clips = c["clips"] = Json::array();
    const std::vector<OFX::Host::ImageEffect::ClipDescriptor*>& cl = d->getClipsByOrder();
    for (size_t k = 0; k < cl.size(); ++k) {
      clips.push(clipJson(*cl[k]));
    }
    // the page of each parameter
    const std::list<OFX::Host::Param::Descriptor*>& params = d->getParamList();
    std::map<std::string, std::string> pageOf;
    for (std::list<OFX::Host::Param::Descriptor*>::const_iterator p = params.begin(); p != params.end(); ++p) {
      if ((*p)->getType() == kOfxParamTypePage) {
        const OFX::Host::Property::Set& props = (*p)->getProperties();
        const int n = props.getDimension(kOfxParamPropPageChild);
        for (int i = 0; i < n; ++i) {
          pageOf.insert(std::make_pair(props.getStringProperty(kOfxParamPropPageChild, i), (*p)->getName()));
        }
      }
    }
    Json& ps = c["params"] = Json::array();
    for (std::list<OFX::Host::Param::Descriptor*>::const_iterator p = params.begin(); p != params.end(); ++p) {
      std::map<std::string, std::string>::const_iterator pg = pageOf.find((*p)->getName());
      ps.push(paramJson(**p, pg == pageOf.end() ? std::string() : pg->second));
    }
  }
  out["ok"] = ok;
  return ok;
}

// the current value of a parameter instance, for --report: a number, a boolean, a string, an array
// (multi-dimensional parameters), or null (groups, pages, push buttons, bytes, or on failure)
static Json paramValueJson(OFX::Host::Param::Instance* param)
{
  using namespace OFX::Host::Param;
  double d[4];
  int i[3];
  bool b;
  std::string s;
  Json a = Json::array();
  if (StringInstance* p = dynamic_cast<StringInstance*>(param)) { // String and Custom
    if (p->get(s) == kOfxStatOK) return Json(s);
  } else if (StrChoiceInstance* p = dynamic_cast<StrChoiceInstance*>(param)) {
    if (p->get(s) == kOfxStatOK) return Json(s);
  } else if (BooleanInstance* p = dynamic_cast<BooleanInstance*>(param)) {
    if (p->get(b) == kOfxStatOK) return Json(b);
  } else if (IntegerInstance* p = dynamic_cast<IntegerInstance*>(param)) {
    if (p->get(i[0]) == kOfxStatOK) return Json(i[0]);
  } else if (ChoiceInstance* p = dynamic_cast<ChoiceInstance*>(param)) {
    if (p->get(i[0]) == kOfxStatOK) return Json(i[0]);
  } else if (DoubleInstance* p = dynamic_cast<DoubleInstance*>(param)) {
    if (p->get(d[0]) == kOfxStatOK) return Json(d[0]);
  } else if (Double2DInstance* p = dynamic_cast<Double2DInstance*>(param)) {
    if (p->get(d[0], d[1]) == kOfxStatOK) { a.push(d[0]); a.push(d[1]); return a; }
  } else if (Integer2DInstance* p = dynamic_cast<Integer2DInstance*>(param)) {
    if (p->get(i[0], i[1]) == kOfxStatOK) { a.push(i[0]); a.push(i[1]); return a; }
  } else if (Double3DInstance* p = dynamic_cast<Double3DInstance*>(param)) {
    if (p->get(d[0], d[1], d[2]) == kOfxStatOK) { a.push(d[0]); a.push(d[1]); a.push(d[2]); return a; }
  } else if (Integer3DInstance* p = dynamic_cast<Integer3DInstance*>(param)) {
    if (p->get(i[0], i[1], i[2]) == kOfxStatOK) { a.push(i[0]); a.push(i[1]); a.push(i[2]); return a; }
  } else if (RGBInstance* p = dynamic_cast<RGBInstance*>(param)) {
    if (p->get(d[0], d[1], d[2]) == kOfxStatOK) { a.push(d[0]); a.push(d[1]); a.push(d[2]); return a; }
  } else if (RGBAInstance* p = dynamic_cast<RGBAInstance*>(param)) {
    if (p->get(d[0], d[1], d[2], d[3]) == kOfxStatOK) { a.push(d[0]); a.push(d[1]); a.push(d[2]); a.push(d[3]); return a; }
  }
  return Json();
}

// the messages and progresses of the instance
static void fillMessagesJson(const MyHost::MyEffectInstance& instance, Json& report)
{
  Json& messages = report["messages"] = Json::array();
  const std::vector<MyHost::MessageRecord>& m = instance.getMessages();
  for (size_t i = 0; i < m.size(); ++i) {
    Json& j = messages.push(Json::object());
    j["kind"] = m[i].kind;
    j["type"] = m[i].type;
    j["id"] = m[i].id;
    j["text"] = m[i].text;
  }
  Json& progress = report["progress"] = Json::array();
  const std::vector<MyHost::ProgressRecord>& p = instance.getProgress();
  for (size_t i = 0; i < p.size(); ++i) {
    Json& j = progress.push(Json::object());
    j["message"] = p[i].message;
    j["id"] = p[i].id;
    j["started"] = true;
    j["ended"] = p[i].ended;
    j["updates"] = p[i].updates;
    j["lastValue"] = p[i].lastValue; // null if there was no update
  }
}

// write the JSON document to path. Returns false (after printing an error) on failure.
static bool writeJson(const std::string& path, const Json& doc)
{
  std::ofstream os(path.c_str(), std::ios::out | std::ios::binary);
  if (!os) {
    std::cerr << "Error: cannot open " << path << " for writing" << std::endl;
    return false;
  }
  os.imbue(std::locale::classic());
  doc.write(os);
  os << "\n";
  os.close();
  if (!os) {
    std::cerr << "Error: cannot write " << path << std::endl;
    return false;
  }
  return true;
}

static void usage(const char* prog)
{
  std::cout <<
    "Usage: " << prog << " [options] [[-s] name=value[,value...]]...\n"
    "Render an OFX plug-in found in OFX_PLUGIN_PATH (default: at t=0 and 1, on PAL SD inputs).\n"
    "  -h                 print this help and exit\n"
    "  -p id              plug-in identifier (default: net.sf.openfx.invertPlugin)\n"
    "  -m major           plug-in major version (default: the highest one)\n"
    "  -c filter|general  context (default: filter)\n"
    "  -o prefix          write <prefix>.<t>[l|r].ppm|pfm (default: Output)\n"
    "  --format ppm|pfm   output format: plain 8-bit PPM (default), or float PFM\n"
    "  -d byte|short|float  input depth before clip preferences (default: byte)\n"
    "  -i Clip=spec       input: default|solid:r,g,b,a|ramp|checker:N|rod:x1,y1,x2,y2|none, or\n"
    "                     file:rgb.pfm[+alpha.pfm][@x1,y1][#premult|#unpremult|#opaque]\n"
    "  --frames t,...     render (or query) at these times (default: 0,1)\n"
    "  --scale s[,sy]     render scale, in (0,1] (default: 1)\n"
    "  --window x1,y1,x2,y2  render window, in pixels at the render scale, clipped to the RoD\n"
    "                     (default: the RoD clipped to the project). A plug-in or output clip\n"
    "                     without tiles (kOfxImageEffectPropSupportsTiles=0) renders its whole RoD,\n"
    "                     and gets whole-RoD input images\n"
    "  --query            only call the query actions (RoD, identity, frames needed, RoI)\n"
    "  -s name=value      set a parameter after createInstance, with instanceChanged\n"
    "  --report file.json write a JSON report of the run (even if it failed)\n"
    "  --describe file.json  describe the plug-in as JSON (no instance, no render)\n"
    "  --no-std-paths     only look for plug-ins in OFX_PLUGIN_PATH\n"
    "  --no-cache         don't read or write hostDemoPluginCache.xml\n"
    "  --quiet            print only the plug-in's messages on stdout\n"
    "  --allow-unwritten  don't fail on unwritten (NaN) output pixels (float only)\n"
    "Formats and JSON schemas: HostSupport/examples/python/README.md. Exit status: 0 on success.\n";
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

// set the parameter value from the string "name=value[,value...]", and return its name in paramName.
// Returns false and prints an error on failure.
static bool setParamFromString(OFX::Host::ImageEffect::Instance* instance, const std::string& assignment, bool quiet,
                               std::string& paramName)
{
  std::string::size_type eq = assignment.find('=');
  if (eq == std::string::npos || eq == 0) {
    std::cerr << "Error: invalid parameter assignment \"" << assignment << "\", expected name=value[,value...]" << std::endl;
    return false;
  }
  std::string name = assignment.substr(0, eq);
  std::string value = assignment.substr(eq + 1);
  paramName = name;
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

// render the frame t (and view) and write the output image to base.ppm (or base.pfm). If rep is
// not NULL, the results of the actions are added to it.
// Returns false (after printing an error) on failure.
static bool renderFrame(MyHost::MyEffectInstance* instance, MyHost::MyClipInstance* outputClip,
                        OfxTime t, int view, int nViews, const OfxPointD& renderScale,
                        const OfxRectI* window, bool query, bool sequential,
                        bool allowUnwritten, const std::string& base, OutputFormat format,
                        Json* rep)
{
  const std::string fname = base + (format == eFormatPFM ? ".pfm" : ".ppm");
  (void)nViews;
  const double par = instance->getProjectPixelAspectRatio();
  Json dummy;
  Json& r = rep ? *rep : dummy;
  r["time"] = t;
  r["view"] = view;
  r["renderScale"] = pointJson(renderScale.x, renderScale.y);

  // the regions of definition of the connected input clips
  const std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>& clips = instance->getClipInstances();
  Json& inputs = r["inputs"] = Json::object();
  for (std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
    MyHost::MyClipInstance* clip = dynamic_cast<MyHost::MyClipInstance*>(it->second);
    if (clip && !clip->isOutput()) {
      Json& in = inputs[it->first] = Json::object();
      in["connected"] = clip->getConnected();
      in["regionOfDefinition"] = clip->getConnected() ? rectJson(clip->getRoD(t, view)) : Json();
    }
  }

  // the output RoD is what the plug-in's getRegionOfDefinition action returns
  OfxRectD rod;
  rod.x1 = rod.y1 = rod.x2 = rod.y2 = 0.;
  OfxStatus stat = instance->getRegionOfDefinitionAction(t, renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                                         view,
#endif
                                                         rod);
  Json& rodJ = r["regionOfDefinition"] = actionJson(stat);
  if (!statusOK(stat)) {
    std::cerr << "Error: get region of definition action failed at time " << t << " (status " << stat << ")" << std::endl;
    return false;
  }
  rodJ["rod"] = rectJson(rod);

  // The render window is in pixel coordinates (render scale and PAR applied): --window, or
  // else the project frame, clipped to the output RoD.
  // kOfxImageEffectPropSupportsTiles: "If a clip or plugin does not support tiled images, then the
  // host should supply full RoD images to the effect whenever it fetches one": for an effect or
  // output clip without tiles, the render window is the whole RoD (unless hostDemo can't make an
  // image that large, e.g. an infinite RoD).
  {
    Json& tiles = r["supportsTiles"] = Json::object();
    tiles["effect"] = instance->supportsTiles();
    Json& tc = tiles["clips"] = Json::object();
    for (std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
      tc[it->first] = it->second->supportsTiles();
    }
  }
  const bool outputTiles = instance->supportsTiles() && outputClip->supportsTiles();
  const OfxRectI rodPixels = MyHost::canonicalToPixelEnclosing(rod, renderScale, par);
  OfxRectI renderWindow = MyHost::intersect(rodPixels, window ? *window : instance->getProjectPixelRect(renderScale));
  if (!outputTiles) {
    if (MyHost::imageFits(rodPixels)) {
      if (window && (renderWindow.x1 != rodPixels.x1 || renderWindow.y1 != rodPixels.y1 ||
                     renderWindow.x2 != rodPixels.x2 || renderWindow.y2 != rodPixels.y2)) {
        std::cerr << "Warning: the plug-in doesn't support tiles: rendering the whole RoD, not --window" << std::endl;
      }
      renderWindow = rodPixels;
    } else {
      std::cerr << "Warning: the plug-in doesn't support tiles, but its RoD (" << rod.x1 << "," << rod.y1 << ","
                << rod.x2 << "," << rod.y2 << ") is too large for hostDemo: rendering "
                << (window ? "--window" : "the project") << " instead" << std::endl;
    }
  }
  if (renderWindow.x2 <= renderWindow.x1 || renderWindow.y2 <= renderWindow.y1) {
    std::cerr << "Warning: the output RoD (" << rod.x1 << "," << rod.y1 << "," << rod.x2 << "," << rod.y2
              << ") at time " << t << " doesn't intersect the " << (window ? "render window" : "project")
              << ": nothing to render" << (query ? "" : ", " + fname + " not written") << std::endl;
    r["renderWindow"] = Json();
    r["output"] = Json();
    return true;
  }
  r["renderWindow"] = rectJson(renderWindow);

  // is the effect an identity for this render window? hostDemo renders it anyway (a host would
  // fetch the image of the identity clip instead), so that the output can be checked.
  {
    OfxTime idTime = t;
    std::string idClip;
#ifdef OFX_EXTENSIONS_NUKE
    int idView = view;
    std::string idPlane = kFnOfxImagePlaneColour;
#endif
    stat = instance->isIdentityAction(idTime, kOfxImageFieldNone, renderWindow, renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                      idView, idPlane,
#endif
                                      idClip);
    Json& id = r["isIdentity"] = actionJson(stat);
    if (!statusOK(stat)) {
      std::cerr << "Error: is identity action failed at time " << t << " (status " << stat << ")" << std::endl;
      return false;
    }
    // kOfxStatOK: "the effect is an identity", kOfxStatReplyDefault: it isn't
    id["identity"] = stat == kOfxStatOK;
    id["clip"] = stat == kOfxStatOK ? Json(idClip) : Json();
    id["time"] = stat == kOfxStatOK ? Json(idTime) : Json();
#ifdef OFX_EXTENSIONS_NUKE
    id["identityView"] = stat == kOfxStatOK ? Json(idView) : Json();
#endif
  }

  // the frames needed from each input clip (HostSupport only calls the action if the plug-in
  // asks for temporal clip access; the default is the frame itself)
  {
    OFX::Host::ImageEffect::RangeMap ranges;
    stat = instance->getFrameNeededAction(t, ranges);
    Json& fn = r["framesNeeded"] = actionJson(stat);
    if (!statusOK(stat)) {
      std::cerr << "Error: get frames needed action failed at time " << t << " (status " << stat << ")" << std::endl;
      return false;
    }
    Json& fc = fn["clips"] = Json::object();
    for (OFX::Host::ImageEffect::RangeMap::const_iterator it = ranges.begin(); it != ranges.end(); ++it) {
      Json& l = fc[it->first->getName()] = Json::array();
      for (size_t k = 0; k < it->second.size(); ++k) {
        l.push(pointJson(it->second[k].min, it->second[k].max));
      }
    }
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
  Json& roiJ = r["regionsOfInterest"] = actionJson(stat);
  if (!statusOK(stat)) {
    std::cerr << "Error: get regions of interest action failed at time " << t << " (status " << stat << ")" << std::endl;
    return false;
  }
  roiJ["regionOfInterest"] = rectJson(regionOfInterest);
  Json& roiClips = roiJ["clips"] = Json::object();
  for (std::map<OFX::Host::ImageEffect::ClipInstance *, OfxRectD>::const_iterator it = rois.begin(); it != rois.end(); ++it) {
    roiClips[it->first->getName()] = rectJson(it->second);
  }
  if (query) {
    // --query: no render
    r["render"] = Json();
    r["output"] = Json();
    return true;
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
                                sequential, // bool     sequentialRender,
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
  // the render is OK if the action succeeded (kOfxStatOK only) and the image fetches too
  Json& renderJ = r["render"] = actionJson(stat);
  renderJ["ok"] = stat == kOfxStatOK && MyHost::gImageErrors == imageErrors;
  renderJ["imageFetchErrors"] = MyHost::gImageErrors - imageErrors;
  r["output"] = Json();

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
  std::vector<std::string> files;
  if (format == eFormatPFM) {
    ok = exportToPFM(base, *outputImage);
    files.push_back(base + ".pfm");
    if (outputImage->getNComponents() == 4) {
      files.push_back(base + ".alpha.pfm");
    }
  } else {
    ok = exportToPPM(fname, *outputImage);
    files.push_back(fname);
  }
  if (!ok) {
    return false;
  }
  if (rep) {
    r["output"] = statsJson(computeStats(*outputImage), files);
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
  std::map<std::string, std::string> inputSpecs; ///< the -i spec text of each clip, for --report
  std::string reportPath;   ///< --report
  std::string describePath; ///< --describe
  bool useStdPaths = true;
  bool useCache = true;
  const std::string cachePath = "hostDemoPluginCache.xml";
  bool quiet = false;
  bool allowUnwritten = false;
  OutputFormat format = eFormatPPM;
  std::vector<double> frames;  ///< --frames (default: 0 and OFXHOSTDEMOCLIPLENGTH)
  OfxPointD renderScale;       ///< --scale
  renderScale.x = renderScale.y = 1.;
  bool haveWindow = false;     ///< --window
  OfxRectI window;
  window.x1 = window.y1 = window.x2 = window.y2 = 0;
  bool query = false;          ///< --query

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
    } else if (arg == "--query") {
      query = true;
    } else if (arg == "-p" || arg == "-m" || arg == "-c" || arg == "-o" || arg == "-s" ||
               arg == "-d" || arg == "-i" || arg == "--format" ||
               arg == "--report" || arg == "--describe" ||
               arg == "--frames" || arg == "--scale" || arg == "--window") {
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
        } else if (optarg == "pfm") {
          format = eFormatPFM;
        } else {
          std::cerr << "Error: unsupported format \"" << optarg << "\", expected ppm or pfm" << std::endl;
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
                    << "), expected Clip=default|solid:r,g,b,a|ramp|checker:N|rod:x1,y1,x2,y2|none|file:rgb.pfm[+alpha.pfm][@x1,y1][#premult|#unpremult|#opaque]" << std::endl;
          return 1;
        }
        if (inputSpecs.count(optarg.substr(0, eq))) {
          std::cerr << "Error: input clip \"" << optarg.substr(0, eq) << "\" given twice (-i)" << std::endl;
          return 1;
        }
        inputs.push_back(std::make_pair(optarg.substr(0, eq), spec));
        inputSpecs[optarg.substr(0, eq)] = optarg.substr(eq + 1);
      } else if (arg == "--report") {
        reportPath = optarg;
      } else if (arg == "--describe") {
        describePath = optarg;
      } else if (arg == "--frames") {
        const std::vector<std::string> v = splitValues(optarg);
        frames.clear();
        for (size_t k = 0; k < v.size(); ++k) {
          double f = 0.;
          if (!parseDouble(v[k], f) || std::find(frames.begin(), frames.end(), f) != frames.end()) {
            std::cerr << "Error: invalid frames \"" << optarg << "\", expected distinct times t0,t1,..." << std::endl;
            return 1;
          }
          frames.push_back(f);
        }
        // the output files are named after the time with 12 significant digits: distinct times
        // must give distinct names
        std::set<std::string> names;
        for (size_t k = 0; k < frames.size(); ++k) {
          std::ostringstream tname;
          tname.precision(12);
          tname << frames[k];
          if (!names.insert(tname.str()).second) {
            std::cerr << "Error: invalid frames \"" << optarg << "\": " << tname.str()
                      << " is given twice to 12 significant digits (the output file names)" << std::endl;
            return 1;
          }
        }
      } else if (arg == "--scale") {
        double s[2];
        if (parseDoubles(optarg, 1, s)) {
          s[1] = s[0];
        } else if (!parseDoubles(optarg, 2, s)) {
          s[0] = 0.;
        }
        if (!(s[0] > 0. && s[0] <= 1. && s[1] > 0. && s[1] <= 1.)) {
          std::cerr << "Error: invalid render scale \"" << optarg << "\", expected s or sx,sy in (0,1]" << std::endl;
          return 1;
        }
        renderScale.x = s[0];
        renderScale.y = s[1];
      } else if (arg == "--window") {
        int w[4];
        if (!parseInts(optarg, 4, w) || w[0] >= w[2] || w[1] >= w[3] ||
            (long long)w[2] - w[0] > 1000000 || (long long)w[3] - w[1] > 1000000 ||
            ((long long)w[2] - w[0]) * ((long long)w[3] - w[1]) > (1LL << 28)) {
          std::cerr << "Error: invalid render window \"" << optarg << "\", expected x1,y1,x2,y2 with x1<x2 and y1<y2, "
                    << "at most 1000000 pixels wide or high and 2^28 pixels in all" << std::endl;
          return 1;
        }
        haveWindow = true;
        window.x1 = w[0];
        window.y1 = w[1];
        window.x2 = w[2];
        window.y2 = w[3];
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

  if (frames.empty()) {
    frames.push_back(0.);
    frames.push_back(OFXHOSTDEMOCLIPLENGTH);
  }
  // the pixels of a file input are pixels at the render scale
  for (size_t k = 0; k < inputs.size(); ++k) {
    inputs[k].second.fileScaleX = renderScale.x;
    inputs[k].second.fileScaleY = renderScale.y;
  }

  if (!reportPath.empty() && !describePath.empty()) {
    std::cerr << "Error: --report and --describe can't be used together (--describe creates no instance)" << std::endl;
    return 1;
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
  // the --report document (see python/README.md)
  Json report = Json::object();
  report["schema"] = "hostDemo.report";
  report["schemaVersion"] = 1;
  report["ok"] = false;
  report["plugin"] = Json::object();
  report["plugin"]["id"] = pluginId;
  report["context"] = context;
  {
    Json& host = report["host"] = Json::object();
    host["name"] = myHost.getProperties().getStringProperty(kOfxPropName);
    host["profile"] = "default"; // --host (Part A, PR 4)
  }
  report["depth"] = inputDepth;
  {
    Json& fr = report["frames"] = Json::array();
    for (size_t k = 0; k < frames.size(); ++k) {
      fr.push(frames[k]);
    }
  }
  report["renderScale"] = pointJson(renderScale.x, renderScale.y);
  report["window"] = haveWindow ? rectJson(window) : Json();
  report["query"] = query;
  report["inputs"] = Json::object();
  Json& actions = report["actions"] = Json::array();
  report["clipPreferences"] = Json();
  report["timeDomain"] = Json();
  Json& renders = report["renders"] = Json::array();
  MyHost::MyEffectInstance* reportInstance = NULL; ///< the instance whose messages and params are reported

  if(!plugin) {
    std::cerr << "Error: plug-in \"" << pluginId << "\"";
    if (pluginMajor >= 0) {
      std::cerr << " with major version " << pluginMajor;
    }
    std::cerr << " not found. Check that OFX_PLUGIN_PATH contains the folder of its .ofx.bundle." << std::endl;
    ret = 1;
  } else if (!describePath.empty()) {
    // describe the plug-in in all its contexts, without creating an instance
    Json desc = Json::object();
    if (!describePlugin(plugin, desc)) {
      ret = 1;
    }
    if (!writeJson(describePath, desc)) {
      ret = 1;
    }
  } else if (plugin->getContexts().find(context) == plugin->getContexts().end()) {
    std::cerr << "Error: plug-in \"" << pluginId << "\" does not support the " << context << " context." << std::endl;
    report["plugin"] = pluginJson(plugin);
    ret = 1;
  } else {
    report["plugin"] = pluginJson(plugin);
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
      // the inputs used
      for (std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
        MyHost::MyClipInstance* clip = dynamic_cast<MyHost::MyClipInstance*>(it->second);
        if (clip && !clip->isOutput()) {
          Json& in = report["inputs"][it->first] = Json::object();
          std::map<std::string, std::string>::const_iterator sp = inputSpecs.find(it->first);
          in["spec"] = sp == inputSpecs.end() ? std::string("default") : sp->second;
          in["connected"] = clip->getConnected();
          in["optional"] = clip->isOptional();
          in["isMask"] = clip->isMask();
          in["unmappedDepth"] = clip->getUnmappedBitDepth();
          in["unmappedComponents"] = clip->getUnmappedComponents();
        }
      }
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

    // now we need to call the create instance action. A host that loads a saved project sets the
    // parameters from that state before this. hostDemo sets the command-line parameters after it,
    // as a user would, so that the plug-in's instance changed logic runs.
    if (ret == 0) {
      reportInstance = myInstance;
      OfxStatus stat = instance->createInstanceAction();
      actions.push(namedActionJson("createInstance", actionJson(stat)));
      if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
        std::cerr << "Error: create instance action failed (status " << stat << ")" << std::endl;
        ret = 1;
      }
    }

    // now we need to to call getClipPreferences on the instance so that it does the clip component/depth
    // logic and caches away the components and depth on each clip.
    if (ret == 0) {
      const bool ok = instance->getClipPreferences();
      actions.push(namedActionJson("getClipPreferences", actionJson(ok)));
      if (!ok) {
        std::cerr << "Error: get clip preferences action failed" << std::endl;
        ret = 1;
      }
    }

    // set the parameter values given on the command line, one at a time: each change is followed by
    // the instance changed action (kOfxChangeUserEdited, at time 0 and render scale 1), between
    // begin and end instance changed actions
    for (size_t k = 0; ret == 0 && k < paramAssignments.size(); ++k) {
      std::string name;
      if (!setParamFromString(instance.get(), paramAssignments[k], quiet, name)) {
        ret = 1;
        break;
      }
      OfxPointD scale;
      scale.x = scale.y = 1.;
      const OfxStatus st[3] = {
        instance->beginInstanceChangedAction(kOfxChangeUserEdited),
        instance->paramInstanceChangedAction(name, kOfxChangeUserEdited, 0., scale),
        instance->endInstanceChangedAction(kOfxChangeUserEdited)
      };
      const char* names[3] = { "beginInstanceChanged", "instanceChanged", "endInstanceChanged" };
      for (int i = 0; i < 3; ++i) {
        Json& a = actions.push(namedActionJson(names[i], actionJson(st[i])));
        a["param"] = name;
        a["value"] = paramAssignments[k].substr(paramAssignments[k].find('=') + 1);
        if (!statusOK(st[i])) {
          std::cerr << "Error: " << names[i] << " action failed for parameter " << name << " (status " << st[i] << ")" << std::endl;
          ret = 1;
        }
      }
    }

    // a change of a clip preferences slave parameter (kOfxImageEffectPropClipPreferencesSlaveParam)
    // makes the clip preferences dirty: run the action again
    if (ret == 0 && instance->areClipPrefsDirty()) {
      const bool ok = instance->getClipPreferences();
      actions.push(namedActionJson("getClipPreferences", actionJson(ok)));
      if (!ok) {
        std::cerr << "Error: get clip preferences action failed" << std::endl;
        ret = 1;
      }
    }

    // the clip preferences: what each clip is mapped to, and the output properties
    if (ret == 0) {
      Json& cp = report["clipPreferences"] = Json::object();
      Json& cpc = cp["clips"] = Json::object();
      const std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>& clips = myInstance->getClipInstances();
      for (std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
        Json& c = cpc[it->first] = Json::object();
        c["depth"] = it->second->getPixelDepth();
        c["components"] = it->second->getComponents();
        c["pixelAspectRatio"] = it->second->getAspectRatio();
        c["unmappedDepth"] = it->second->getUnmappedBitDepth();
        c["unmappedComponents"] = it->second->getUnmappedComponents();
        c["connected"] = it->second->getConnected();
      }
      cp["outputPremultiplication"] = instance->getOutputPreMultiplication();
      cp["outputFielding"] = instance->getOutputFielding();
      cp["outputFrameRate"] = instance->getOutputFrameRate();
      cp["continuousSamples"] = instance->continuousSamples();
      cp["frameVarying"] = instance->isFrameVarying();
    }

    // the time domain ("Only effects instantiated in the General Context can have this called on them")
    if (ret == 0 && context == kOfxImageEffectContextGeneral) {
      OfxRangeD range;
      range.min = range.max = 0.;
      OfxStatus stat = instance->getTimeDomainAction(range);
      Json& td = report["timeDomain"] = actionJson(stat);
      td["range"] = stat == kOfxStatOK ? pointJson(range.min, range.max) : Json();
      actions.push(namedActionJson("getTimeDomain", actionJson(stat)));
      if (!statusOK(stat)) {
        std::cerr << "Error: get time domain action failed (status " << stat << ")" << std::endl;
        ret = 1;
      }
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
      const double firstFrame = *std::min_element(frames.begin(), frames.end());
      const double lastFrame = *std::max_element(frames.begin(), frames.end());
      // kOfxImageEffectPropSequentialRenderStatus and kOfxImageEffectPropFrameStep: the frames are
      // a sequential render if they increase with a constant step (any single frame is one)
      double frameStep = 1.;
      bool sequential = true;
      if (frames.size() > 1) {
        frameStep = frames[1] - frames[0];
        for (size_t k = 1; k < frames.size(); ++k) {
          if (!(frames[k] - frames[k - 1] > 0.) || std::fabs(frames[k] - frames[k - 1] - frameStep) > 1e-9) {
            sequential = false;
          }
        }
        if (!sequential) {
          frameStep = 1.;
        }
      }
      const OfxRectI* win = haveWindow ? &window : NULL;

      // say we are about to render a bunch of frames (not for --query: the query actions may
      // be called outside a render sequence)
      OfxStatus stat = kOfxStatOK;
      if (!query) {
        stat = instance->beginRenderAction(firstFrame, lastFrame, frameStep, false, renderScale, sequential, /*interactive=*/false,
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
        actions.push(namedActionJson("beginSequenceRender", actionJson(stat)));
        if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
          std::cerr << "Error: begin render action failed (status " << stat << ")" << std::endl;
          ret = 1;
        }
      }

      for (size_t f = 0; ret == 0 && f < frames.size(); ++f)
      {
        const OfxTime t = frames[f];
        std::ostringstream tname;
        tname.precision(12);
        tname << t;
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
        // render a stereoscopic frame: left and right views
        for (int view = 0; ret == 0 && view < 2; ++view) {
          std::ostringstream ss;
          ss << outputPrefix << "." << tname.str() << (view == 0 ? "l" : "r");
          Json* r = reportPath.empty() ? NULL : &renders.push(Json::object());
          if (!renderFrame(myInstance, outputClip, t, view, 2, renderScale, win, query, sequential, allowUnwritten, ss.str(), format, r)) {
            ret = 1;
          }
        }
#else
        std::ostringstream ss;
        ss << outputPrefix << "." << tname.str();
        Json* r = reportPath.empty() ? NULL : &renders.push(Json::object());
        if (!renderFrame(myInstance, outputClip, t, 0, 1, renderScale, win, query, sequential, allowUnwritten, ss.str(), format, r)) {
          ret = 1;
        }
#endif
      }

      if (!query) {
        stat = instance->endRenderAction(firstFrame, lastFrame, frameStep, false, renderScale, sequential, /*interactive=*/false,
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
        actions.push(namedActionJson("endSequenceRender", actionJson(stat)));
        if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
          std::cerr << "Error: end render action failed (status " << stat << ")" << std::endl;
          ret = 1;
        }
      }
      // release the last output image before the instance is destroyed
      outputClip->releaseOutputImage();
    }

    // the final parameter values and states, the messages and the progresses (before the
    // instance is destroyed)
    if (reportInstance) {
      Json& params = report["params"] = Json::object();
      const std::list<OFX::Host::Param::Instance*>& pl = reportInstance->getParamList();
      for (std::list<OFX::Host::Param::Instance*>::const_iterator it = pl.begin(); it != pl.end(); ++it) {
        Json& p = params[(*it)->getName()] = Json::object();
        p["type"] = (*it)->getType();
        p["value"] = paramValueJson(*it);
        p["enabled"] = (*it)->getEnabled();
        p["secret"] = (*it)->getSecret();
      }
      fillMessagesJson(*reportInstance, report);
    }
  }
  if (!reportPath.empty()) {
    report["ok"] = ret == 0;
    report["exitStatus"] = ret;
    if (!writeJson(reportPath, report)) {
      ret = 1;
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
