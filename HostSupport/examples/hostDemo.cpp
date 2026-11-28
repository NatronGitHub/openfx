// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause


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
#include <functional> // the GPU renders (withGPU())
#include <memory>
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
#ifdef HOSTDEMO_OPENGL
#include "ofxhOpenGLContext.h"
#include "ofxhOpenGLRender.h"
#endif
#ifdef HOSTDEMO_OPENCL
#include "ofxhOpenCLDevice.h"
#include "ofxhOpenCLRender.h"
#endif
#ifdef HOSTDEMO_METAL
#include "ofxhMetalDevice.h"
#include "ofxhMetalRender.h"
#endif
#ifdef HOSTDEMO_CUDA
#include <functional>
#include <memory>
#include "ofxhCUDADevice.h"
#include "ofxhCUDARender.h"
#endif
   
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
        if (OFX::IsNaN(pix[c])) { // a bit test, which -ffast-math doesn't remove
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
  double min[4], max[4], mean[4]; ///< only meaningful if count > 0
  long count[4]; ///< the number of non-NaN values of each component
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
  double sum[4] = { 0., 0., 0., 0. };
  long* count = st.count;
  for (int c = 0; c < 4; ++c) {
    st.min[c] = st.max[c] = st.mean[c] = 0.;
    count[c] = 0;
  }
  long nanPixels = 0;
  unsigned long crc = 0;
  // the bytes of a component, in little-endian order whatever the host's byte order
  const unsigned short one = 1;
  const bool littleEndian = *reinterpret_cast<const unsigned char*>(&one) == 1;
  std::vector<unsigned char> row((size_t)(b.x2 - b.x1) * nComps * bpc);
  for (int y = b.y1; y < b.y2; ++y) {
    if (!row.empty()) {
      // (memcpy with a null pointer is undefined, even for 0 bytes)
      std::memcpy(row.data(), im.getPixelData(b.x1, y), row.size());
    }
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
        if (OFX::IsNaN(v)) { // a bit test, which -ffast-math doesn't remove (NaN marks unwritten pixels)
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
      if (st.count[c] == 0) {
        a.push(Json()); // no value: null
      } else {
        a.push((k == 0) ? st.min[c] : (k == 1) ? st.max[c] : st.mean[c]);
      }
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
  j["openCLSupported"] = propJson(p, kOfxImageEffectPropOpenCLSupported);
  // the depths of the plug-in's OpenGL textures (none: the host decides); null without
  // OFX_SUPPORTS_OPENGLRENDER
  j["openGLPixelDepths"] = propJson(p, kOfxOpenGLPropPixelDepth, true);
  j["cudaRenderSupported"] = propJson(p, kOfxImageEffectPropCudaRenderSupported);
  j["cudaStreamSupported"] = propJson(p, kOfxImageEffectPropCudaStreamSupported);
  j["metalRenderSupported"] = propJson(p, kOfxImageEffectPropMetalRenderSupported);
  // ("false" or "true"; upstream main, after OFX 1.5.1)
  j["metalTextureSupported"] = propJson(p, kOfxImageEffectPropMetalTextureSupported);
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
// If kOfxActionLoad or kOfxActionDescribe failed (loadFailed), no context is described.
// Returns false if a context couldn't be described.
static bool describePlugin(OFX::Host::ImageEffect::ImageEffectPlugin* plugin, bool loadFailed, Json& out)
{
  bool ok = true;
  out["schema"] = "hostDemo.describe";
  out["schemaVersion"] = 1;
  out["plugin"] = pluginJson(plugin);
  Json& contexts = out["contexts"] = Json::object();
  if (loadFailed) {
    std::cerr << "Error: the load or describe action failed: no context is described" << std::endl;
  }
  const std::set<std::string>& cs = plugin->getContexts();
  for (std::set<std::string>::const_iterator it = cs.begin(); it != cs.end(); ++it) {
    Json& c = contexts[*it] = Json::object();
    OFX::Host::ImageEffect::Descriptor* d = loadFailed ? NULL : plugin->getContext(*it);
    if (!d) {
      if (!loadFailed) {
        std::cerr << "Error: the describe in context action failed for the " << *it << " context" << std::endl;
      }
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

// the curve (keys) of a numeric parameter instance of hostDemo, or NULL
static const MyHost::MyCurve* curveOf(OFX::Host::Param::Instance* param)
{
  using namespace MyHost;
  if (MyIntegerInstance* p = dynamic_cast<MyIntegerInstance*>(param)) return &p->getCurve();
  if (MyDoubleInstance* p = dynamic_cast<MyDoubleInstance*>(param)) return &p->getCurve();
  if (MyBooleanInstance* p = dynamic_cast<MyBooleanInstance*>(param)) return &p->getCurve();
  if (MyChoiceInstance* p = dynamic_cast<MyChoiceInstance*>(param)) return &p->getCurve();
  if (MyRGBAInstance* p = dynamic_cast<MyRGBAInstance*>(param)) return &p->getCurve();
  if (MyRGBInstance* p = dynamic_cast<MyRGBInstance*>(param)) return &p->getCurve();
  if (MyDouble2DInstance* p = dynamic_cast<MyDouble2DInstance*>(param)) return &p->getCurve();
  if (MyInteger2DInstance* p = dynamic_cast<MyInteger2DInstance*>(param)) return &p->getCurve();
  if (MyDouble3DInstance* p = dynamic_cast<MyDouble3DInstance*>(param)) return &p->getCurve();
  if (MyInteger3DInstance* p = dynamic_cast<MyInteger3DInstance*>(param)) return &p->getCurve();
  return 0;
}

// a value of a curve as JSON, with the type of the parameter: a boolean, an integer, a number, or
// an array of them (multi-dimensional parameters)
static Json curveValueJson(const std::string& type, const double* v, int n)
{
  const bool isBool = type == kOfxParamTypeBoolean;
  const bool isInt = type == kOfxParamTypeInteger || type == kOfxParamTypeChoice ||
                     type == kOfxParamTypeInteger2D || type == kOfxParamTypeInteger3D;
  Json a = Json::array();
  for (int k = 0; k < n; ++k) {
    Json e = isBool ? Json(v[k] != 0.) : isInt ? Json((int)v[k]) : Json(v[k]);
    if (n == 1) {
      return e;
    }
    a.push(e);
  }
  return a;
}

// the values at time of the parameters that have keys, by name (for --report)
static Json keyedParamValuesJson(MyHost::MyEffectInstance* instance, OfxTime time)
{
  Json j = Json::object();
  const std::list<OFX::Host::Param::Instance*>& pl = instance->getParamList();
  for (std::list<OFX::Host::Param::Instance*>::const_iterator it = pl.begin(); it != pl.end(); ++it) {
    const MyHost::MyCurve* c = curveOf(*it);
    if (c && !c->getKeys().empty()) {
      double v[4];
      c->get(time, v);
      j[(*it)->getName()] = curveValueJson((*it)->getType(), v, c->getDimension());
    }
  }
  return j;
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
    j["ended"] = p[i].ends > 0;
    j["ends"] = p[i].ends;
    j["updates"] = p[i].updates;
    j["lastValue"] = (p[i].updates > 0) ? Json(p[i].lastValue) : Json(); // null if there was no update
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
    "                     file:rgb.pfm[+alpha.pfm][@x1,y1][#premult|#unpremult|#opaque], then\n"
    "                     optionally #rgb, #alpha or #rgba: the components of the input (default RGBA)\n"
    "  -i Clip@Plane=spec an extra plane of an input (with --multiplane): Plane is ForwardMotionVector,\n"
    "                     BackMotionVector, StereoDisparityLeft or StereoDisparityRight, and its two\n"
    "                     components are the first two of spec (any -i spec but none)\n"
    "  --frames t,...     render (or query) at these times (default: 0,1)\n"
    "  --scale s[,sy]     render scale, in (0,1] (default: 1)\n"
    "  --window x1,y1,x2,y2  render window, in pixels at the render scale, clipped to the RoD\n"
    "                     (default: the RoD clipped to the project). A plug-in or output clip\n"
    "                     without tiles (kOfxImageEffectPropSupportsTiles=0) renders its whole RoD,\n"
    "                     and gets whole-RoD input images\n"
    "  --query            only call the query actions (RoD, identity, frames needed, RoI)\n"
    "  -s name=value      set a parameter after createInstance, with instanceChanged (at time 0)\n"
    "  -s name@t=value    set a key at time t (numeric parameters that animate: linear\n"
    "                     interpolation, rounded for integers, stepped for booleans and choices),\n"
    "                     with instanceChanged at time t\n"
    "  -s button[@t]      press a push button (instanceChanged, at time t)\n"
    "  --report file.json write a JSON report of the run (even if it failed)\n"
    "  --describe file.json  describe the plug-in as JSON (no instance, no render)\n"
    "  --ofx14            be an OFX 1.4 host: only the OFX 1.4 host properties and suites,\n"
    "                     no OfxSetHost, no StrChoice or Bytes params, one view\n"
    "  --ofx11            be an OFX 1.1 host such as VEGAS Pro: only the OFX 1.1 host properties\n"
    "                     (API version 1.1) and suites, no interact suite, message suite V2 that refuses\n"
    "                     persistent messages, no tiles, no multi-resolution,\n"
    "                     no overlays, byte and float depths, a single clip PAR, no OfxSetHost, no\n"
    "                     kOfxImageEffectInstancePropSequentialRender, as --no-normalised-defaults, one view\n"
    "  --opengl-describe  with --describe only: say the host supports OpenGL render\n"
    "                     (kOfxImageEffectPropOpenGLRenderSupported \"true\"), to describe OpenGL-only plug-ins\n"
    "  --components c,... the host's supported components (kOfxImageEffectPropSupportedComponents):\n"
    "                     distinct rgba, rgb or alpha, in order (default: rgba,alpha)\n"
    "  --gpu=MODE         cpu (the default) or opengl: render with OpenGL the plug-ins that can (an\n"
    "                     offscreen context: EGL or GLX on Linux, CGL on macOS, WGL on Windows), on the CPU\n"
    "                     without a context, or if the OpenGL render fails (kOfxStatGPURenderFailed...)\n"
    "                     and the plug-in can; the --report renders get \"openGL\" and \"cpuRetry\".\n"
    "                     opencl: the same with OpenCL (the first GPU device, else the first device;\n"
    "                     OpenCL Buffers, or Images for a plug-in with Images only and RGBA clips);\n"
    "                     the --report renders get \"openCL\" and \"cpuRetry\".\n"
    "                     metal (macOS): the same with Metal (the system default device; Metal buffers,\n"
    "                     or textures for a plug-in with textures only and RGBA or Alpha clips);\n"
    "                     the --report renders get \"metal\" and \"cpuRetry\".\n"
    "                     cuda (Linux, Windows): the same with CUDA device memory (the first CUDA\n"
    "                     device), and a CUDA stream for the plug-ins that support streams;\n"
    "                     the --report renders get \"cuda\" and \"cpuRetry\"\n"
    "  --gpu-renderer=BACKEND:ID  with --gpu=opengl: the renderer of the context, one of --gpu-renderers\n"
    "                     (e.g. egl-device:0, glx:-1, cgl:<renderer ID>); no context on it is an error\n"
    "  --gpu-renderers    list the OpenGL renderers as BACKEND:ID and their names, and exit\n"
    "  --gpu-devices      list the GPU renderers and devices of the APIs of this build, under a heading\n"
    "                     per API, and exit: the OpenGL renderers (as --gpu-renderers), the OpenCL\n"
    "                     devices as PLATFORM:DEVICE, the Metal (macOS) and CUDA (not macOS) devices as\n"
    "                     their index (the ids of the ofxh module's init(): --gpu=opencl, metal and cuda\n"
    "                     take the default device); \"(none)\" under an empty heading, and why on stderr\n"
    "  --multiplane       be a multi-plane host (kFnOfxImageEffectPropMultiPlanar, the plane suites,\n"
    "                     the GetClipComponents action; Nuke-extension builds only)\n"
    "  --no-normalised-defaults  be a host without normalised parameter defaults: double params\n"
    "                     have no kOfxParamPropDefaultCoordinateSystem, their defaults are canonical\n"
    "  --no-std-paths     only look for plug-ins in OFX_PLUGIN_PATH\n"
    "  --no-cache         don't read or write the plug-in cache files (hostDemoPluginCache*.xml)\n"
    "  --quiet            print only the plug-in's messages on stdout\n"
    "  --allow-unwritten  don't fail on unwritten (NaN) output pixels (float only)\n"
    "Environment: OFX_HOSTDEMO_KEEP_LOADED=1 keeps the scanned plug-ins loaded (by default they\n"
    "are unloaded after the scan, and loaded again at their first use).\n"
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

// the largest key time of -s name@t=value (a finite time far from double overflow)
static const double kMaxKeyTime = 1e15;

// set the parameter value from the string "name=value[,value...]", or a key from
// "name@time=value[,value...]" (numeric parameters that animate), or press a push button ("name", or
// "name=" anything), and return its name in paramName, and the time in time (0 without one).
// Returns false and prints an error on failure.
static bool setParamFromString(OFX::Host::ImageEffect::Instance* instance, const std::string& assignment, bool quiet,
                               std::string& paramName, double& time)
{
  std::string::size_type eq = assignment.find('=');
  std::string name = assignment.substr(0, eq);
  std::string value = eq == std::string::npos ? std::string() : assignment.substr(eq + 1);
  bool hasTime = false;
  time = 0.;
  std::string::size_type at = name.find('@');
  if (at != std::string::npos) {
    if (!parseDouble(name.substr(at + 1), time) || !(std::fabs(time) < kMaxKeyTime)) {
      std::cerr << "Error: invalid key time in \"" << assignment << "\", expected name@time=value[,value...]" << std::endl;
      return false;
    }
    hasTime = true;
    name = name.substr(0, at);
  }
  OFX::Host::Param::Instance* param = name.empty() ? 0 : instance->getParam(name);
  const bool button = param && param->getType() == kOfxParamTypePushButton;
  if (!name.empty() && !param) {
    std::cerr << "Error: the plug-in has no parameter named \"" << name << "\"" << std::endl;
    return false;
  }
  if (name.empty() || (eq == std::string::npos && !button)) {
    std::cerr << "Error: invalid parameter assignment \"" << assignment << "\", expected name=value[,value...], "
              << "name@time=value[,value...] or a push button name" << std::endl;
    return false;
  }
  paramName = name;
  if (!param) {
    std::cerr << "Error: the plug-in has no parameter named \"" << name << "\"" << std::endl;
    return false;
  }
  const std::string& type = param->getType();
  const bool numeric = !(type == kOfxParamTypeString || type == kOfxParamTypeCustom || type == kOfxParamTypeStrChoice ||
                         type == kOfxParamTypeBytes || button);
  // (a push button may be pressed at a time: -s name@t)
  if (hasTime && !button && (!numeric || param->getProperties().getIntProperty(kOfxParamPropAnimates) == 0)) {
    std::cerr << "Error: parameter \"" << name << "\" doesn't animate (" << type << "): no key at a time" << std::endl;
    return false;
  }
  if (button) {
    // a press: only the instance changed action
    if (!quiet) {
      std::cerr << "Pressed button " << name << std::endl;
    }
    return true;
  }
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
      stat = hasTime ? p->set(time, b) : p->set(b);
    }
  } else if (type == kOfxParamTypeInteger) {
    OFX::Host::Param::IntegerInstance* p = dynamic_cast<OFX::Host::Param::IntegerInstance*>(param);
    parsed = parseInts(value, 1, i);
    if (parsed && p) {
      stat = hasTime ? p->set(time, i[0]) : p->set(i[0]);
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
      stat = hasTime ? p->set(time, i[0]) : p->set(i[0]);
    }
  } else if (type == kOfxParamTypeDouble) {
    OFX::Host::Param::DoubleInstance* p = dynamic_cast<OFX::Host::Param::DoubleInstance*>(param);
    parsed = parseDoubles(value, 1, d);
    if (parsed && p) {
      stat = hasTime ? p->set(time, d[0]) : p->set(d[0]);
    }
  } else if (type == kOfxParamTypeDouble2D) {
    OFX::Host::Param::Double2DInstance* p = dynamic_cast<OFX::Host::Param::Double2DInstance*>(param);
    parsed = parseDoubles(value, 2, d);
    if (parsed && p) {
      stat = hasTime ? p->set(time, d[0], d[1]) : p->set(d[0], d[1]);
    }
  } else if (type == kOfxParamTypeInteger2D) {
    OFX::Host::Param::Integer2DInstance* p = dynamic_cast<OFX::Host::Param::Integer2DInstance*>(param);
    parsed = parseInts(value, 2, i);
    if (parsed && p) {
      stat = hasTime ? p->set(time, i[0], i[1]) : p->set(i[0], i[1]);
    }
  } else if (type == kOfxParamTypeDouble3D) {
    OFX::Host::Param::Double3DInstance* p = dynamic_cast<OFX::Host::Param::Double3DInstance*>(param);
    parsed = parseDoubles(value, 3, d);
    if (parsed && p) {
      stat = hasTime ? p->set(time, d[0], d[1], d[2]) : p->set(d[0], d[1], d[2]);
    }
  } else if (type == kOfxParamTypeInteger3D) {
    OFX::Host::Param::Integer3DInstance* p = dynamic_cast<OFX::Host::Param::Integer3DInstance*>(param);
    parsed = parseInts(value, 3, i);
    if (parsed && p) {
      stat = hasTime ? p->set(time, i[0], i[1], i[2]) : p->set(i[0], i[1], i[2]);
    }
  } else if (type == kOfxParamTypeRGB) {
    OFX::Host::Param::RGBInstance* p = dynamic_cast<OFX::Host::Param::RGBInstance*>(param);
    parsed = parseDoubles(value, 3, d);
    if (parsed && p) {
      stat = hasTime ? p->set(time, d[0], d[1], d[2]) : p->set(d[0], d[1], d[2]);
    }
  } else if (type == kOfxParamTypeRGBA) {
    OFX::Host::Param::RGBAInstance* p = dynamic_cast<OFX::Host::Param::RGBAInstance*>(param);
    parsed = parseDoubles(value, 4, d);
    if (parsed && p) {
      stat = hasTime ? p->set(time, d[0], d[1], d[2], d[3]) : p->set(d[0], d[1], d[2], d[3]);
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
    if (hasTime) {
      std::cerr << "Set a key of parameter " << name << " at " << time << " = " << value << std::endl;
    } else {
      std::cerr << "Set parameter " << name << " = " << value << std::endl;
    }
  }
  return true;
}

// render the frame t (and view) and write the output image to base.ppm (or base.pfm). If rep is
// not NULL, the results of the actions are added to it.
// Returns false (after printing an error) on failure.
/// --gpu=opengl (HOSTDEMO_OPENGL builds): see below
struct OpenGLHost;

#ifdef HOSTDEMO_OPENGL
namespace GL = OFX::Host::OpenGL;

/// --gpu=opengl: the OpenGL context of hostDemo (one render at a time) and the instance's attachment
struct OpenGLHost {
  std::unique_ptr<GL::Context> context;
  std::unique_ptr<GL::Attachment> attachment;
  /// how the instance renders now (OpenGL, the CPU, or neither)
  GL::RenderMode mode(const MyHost::MyEffectInstance& instance) const
  {
    return GL::chooseRenderMode(instance, context && attachment && !attachment->failed());
  }
};

/// An action of the instance with OpenGL (action(openGLRender, contextData)) in a RenderSession, with
/// output (the output clip, for the render action) as the render target over window. glFailed: OpenGL
/// couldn't be used (the instance wasn't attached), or GL::openGLFailed(); cpuRetry: then the host
/// may render on the CPU (GL::cpuRetryAllowed(): the plug-in can).
static OfxStatus withOpenGL(OpenGLHost& gl, MyHost::MyEffectInstance& instance, MyHost::MyClipInstance* output,
                            OfxTime t, const OfxRectI& window, const std::function<OfxStatus(bool, void*)>& action,
                            bool& glFailed, bool& cpuRetry)
{
  GL::CurrentContext current(*gl.context);
  GL::RenderSession session(*gl.attachment, current);
  glFailed = true;
  cpuRetry = GL::chooseRenderMode(instance, false) == GL::RenderMode::CPU;
  if (!session.ok()) {
    return session.status();
  }
  OFX::Host::ImageEffect::Image* out = NULL;
  OfxStatus st = kOfxStatFailed;
  try {
    if (output) {
      out = output->getImage(t, NULL); // (the output image of this render)
      if (!out) {
        return kOfxStatFailed;
      }
      session.setOutput(*out, window);
    }
    // the session of the action, also when it throws (E.6)
    struct SessionScope {
      MyHost::MyEffectInstance& instance;
      SessionScope(MyHost::MyEffectInstance& i, GL::RenderSession* s) : instance(i) { instance.setOpenGLSession(s); }
      ~SessionScope() { instance.setOpenGLSession(NULL); }
    } scope(instance, &session);
    st = action(true, session.contextData());
    glFailed = GL::openGLFailed(session, st);
    cpuRetry = GL::cpuRetryAllowed(instance, session, st);
    if (out && st == kOfxStatOK && !glFailed) {
      session.readOutput();
    }
  } catch (const std::exception& e) {
    std::cerr << "Error: OpenGL render: " << e.what() << std::endl;
    glFailed = true;
  }
  if (out) {
    out->releaseReference();
  }
  return st;
}
#endif
/// --gpu=opencl, --gpu=metal and --gpu=cuda (HOSTDEMO_OPENCL, HOSTDEMO_METAL, HOSTDEMO_CUDA builds):
/// see below
struct OpenCLHost;
struct MetalHost;
struct CUDAHost;

#ifdef HOSTDEMO_OPENCL
namespace CL = OFX::Host::OpenCL;

/// --gpu=opencl: the OpenCL device of hostDemo (a context, and the queues lent to its renders)
struct OpenCLHost {
  std::unique_ptr<CL::Device> device;
  /// how the instance renders now: OpenCL Buffers (preferred when the plug-in can do both), OpenCL
  /// Images, the CPU, or neither
  CL::RenderMode mode(const MyHost::MyEffectInstance& instance) const
  {
    return CL::chooseRenderMode(instance, device != nullptr, device && device->info().imageSupport);
  }
};

/// (withGPU())
static bool gpuFailed(const CL::RenderSession& session, OfxStatus st) { return CL::openCLFailed(session, st); }
#endif

#ifdef HOSTDEMO_METAL
namespace MT = OFX::Host::Metal;

/// --gpu=metal: the Metal device of hostDemo, and the queues lent to its renders
struct MetalHost {
  std::unique_ptr<MT::Device> device;
  /// how the instance renders now: Metal buffers (preferred when the plug-in can do both), Metal
  /// textures, the CPU, or neither
  MT::RenderMode mode(const MyHost::MyEffectInstance& instance) const
  {
    return MT::chooseRenderMode(instance, device != nullptr, device != nullptr);
  }
};

/// (withGPU())
static bool gpuFailed(const MT::RenderSession& session, OfxStatus st) { return MT::metalFailed(session, st); }
#endif

#ifdef HOSTDEMO_CUDA
namespace CU = OFX::Host::CUDA;

/// --gpu=cuda: the CUDA device of hostDemo, and the streams lent to its renders
struct CUDAHost {
  std::unique_ptr<CU::Device> device;
  /// how the instance renders now: CUDA, the CPU, or neither (the CPU after a sticky CUDA error)
  CU::RenderMode mode(const MyHost::MyEffectInstance& instance) const
  {
    return CU::chooseRenderMode(instance, device && device->usable());
  }
};

/// (withGPU())
static bool gpuFailed(const CU::RenderSession& session, OfxStatus st) { return CU::cudaFailed(session, st); }
#endif

#if defined(HOSTDEMO_OPENCL) || defined(HOSTDEMO_METAL) || defined(HOSTDEMO_CUDA)
/// An action of the instance on a Queue of device (a CL:: or MT::Queue, a CU::Stream), in a Session
/// (CL::, MT:: or CU::RenderSession, made with sessionArgs: the OpenCL mode) (action()), with the
/// render window of output's image (the render action) copied to the GPU and read back after.
/// sent: the session accepted it (else the action wasn't sent); failed: the API's openCLFailed(),
/// metalFailed() or cudaFailed(), or not sent; cpuRetry: then the host may render on the CPU (the
/// API's cpuRetryAllowed()). api: "OpenCL", "Metal" or "CUDA", for the error.
template <class Queue, class Session, class Device, class... SessionArgs>
static OfxStatus withGPU(const char* api, Device& device, MyHost::MyEffectInstance& instance,
                         MyHost::MyClipInstance* output, OfxTime t, const OfxRectI& window,
                         const std::function<OfxStatus()>& action, bool& sent, bool& failed, bool& cpuRetry,
                         SessionArgs... sessionArgs)
{
  sent = false;
  failed = true;
  cpuRetry = OFX::Host::ImageEffect::canRenderOnCPU(instance);
  OFX::Host::ImageEffect::Image* out = NULL;
  OfxStatus st = kOfxStatFailed;
  {
    Queue queue(device);
    Session session(instance, queue, sessionArgs...);
    if (!session.ok()) {
      return kOfxStatFailed;
    }
    try {
      if (output) {
        out = output->getImage(t, NULL); // (the output image of this render, before the session)
        if (!out || !session.setOutput(*output, *out, window)) {
          if (out) {
            out->releaseReference();
          }
          return kOfxStatFailed;
        }
      }
      // the session of the action, also when it throws (E.6)
      struct SessionScope {
        MyHost::MyEffectInstance& instance;
        SessionScope(MyHost::MyEffectInstance& i, Session* s) : instance(i) { instance.setGPUSession(s); }
        ~SessionScope() { instance.setGPUSession(static_cast<Session*>(NULL)); }
      } scope(instance, &session);
      sent = true;
      st = action();
      instance.setGPUSession(static_cast<Session*>(NULL));
      if (out && st == kOfxStatOK && !session.failed()) {
        session.readOutput();
      }
      failed = gpuFailed(session, st);
      cpuRetry = cpuRetryAllowed(instance, session, st); // (the API's, by ADL)
    } catch (const std::exception& e) {
      std::cerr << "Error: " << api << " render: " << e.what() << std::endl;
      failed = true;
    }
  }
  if (out) {
    out->releaseReference();
  }
  return st;
}
#endif


static bool renderFrame(MyHost::MyEffectInstance* instance, MyHost::MyClipInstance* outputClip,
                        OfxTime t, int view, int nViews, const OfxPointD& renderScale,
                        const OfxRectI* window, bool query, bool sequential,
                        bool allowUnwritten, const std::string& base, OutputFormat format,
                        bool multiPlane, Json* rep, OpenGLHost* openGL, OpenCLHost* openCL, MetalHost* metal,
                        CUDAHost* cuda)
{
  const std::string fname = base + (format == eFormatPFM ? ".pfm" : ".ppm");
  (void)nViews;
  const double par = instance->getProjectPixelAspectRatio();
  Json dummy;
  Json& r = rep ? *rep : dummy;
  r["time"] = t;
  r["view"] = view;
  r["renderScale"] = pointJson(renderScale.x, renderScale.y);
  // the timeline is at the time of the frame (paramGetValue gives the values at that time)
  instance->timeLineGotoTime(t);
  // the values of the parameters with keys at this time
  r["params"] = keyedParamValuesJson(instance, t);

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
    // (each warning once per run, not per frame and view)
    static bool warnedWindow = false, warnedLarge = false;
    if (MyHost::imageFits(rodPixels)) {
      if (window && !warnedWindow && (renderWindow.x1 != rodPixels.x1 || renderWindow.y1 != rodPixels.y1 ||
                                      renderWindow.x2 != rodPixels.x2 || renderWindow.y2 != rodPixels.y2)) {
        warnedWindow = true;
        std::cerr << "Warning: the plug-in or its output clip doesn't support tiles: rendering the whole RoD, not --window" << std::endl;
      }
      renderWindow = rodPixels;
    } else if (!warnedLarge) {
      warnedLarge = true;
      std::cerr << "Warning: the plug-in or its output clip doesn't support tiles, but the RoD (" << rod.x1 << "," << rod.y1 << ","
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
    if (multiPlane) {
      id["identityPlane"] = stat == kOfxStatOK ? Json(idPlane) : Json();
    }
#endif
  }

#ifdef OFX_EXTENSIONS_NUKE
  // --multiplane: the planes the plug-in needs on each clip and produces on the output, and the
  // clip it passes the other planes through from (hostDemo renders the colour plane only)
  if (multiPlane) {
    OFX::Host::ImageEffect::ComponentsMap components;
    OFX::Host::ImageEffect::ClipInstance* passThroughClip = NULL;
    OfxTime passThroughTime = t;
    int passThroughView = view;
    stat = instance->getClipComponentsAction(t, view, components, passThroughClip, passThroughTime, passThroughView);
    Json& cc = r["clipComponents"] = actionJson(stat);
    if (!statusOK(stat)) {
      std::cerr << "Error: get clip components action failed at time " << t << " (status " << stat << ")" << std::endl;
      return false;
    }
    // (by clip name: the map is ordered by the clips' addresses)
    Json& ccc = cc["clips"] = Json::object();
    for (std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
      OFX::Host::ImageEffect::ComponentsMap::const_iterator found = components.find(it->second);
      if (found != components.end()) {
        Json& l = ccc[it->first] = Json::array();
        for (std::list<std::string>::const_iterator c = found->second.begin(); c != found->second.end(); ++c) {
          l.push(*c);
        }
      }
    }
    cc["passThroughClip"] = passThroughClip ? Json(passThroughClip->getName()) : Json();
    cc["passThroughTime"] = stat == kOfxStatOK ? Json(passThroughTime) : Json();
    cc["passThroughView"] = stat == kOfxStatOK ? Json(passThroughView) : Json();
  }
#else
  (void)multiPlane;
#endif

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
  auto render = [&](bool openGLRender, void* contextData) {
    (void)openGLRender;
    (void)contextData;
    return instance->renderAction(t, //OfxTime      time,
                                kOfxImageFieldNone, // const std::string &  field,
                                renderWindow, // const OfxRectI &renderRoI,
                                renderScale, // OfxPointD   renderScale,
                                sequential, // bool     sequentialRender,
                                false, // bool     interactiveRender,
#ifdef OFX_SUPPORTS_OPENGLRENDER
                                openGLRender, // bool     openGLRender,
#  ifdef OFX_EXTENSIONS_NATRON
                                contextData, // void*    contextData,
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
  };
  // --gpu=opencl, metal or cuda: gpuApi, the API, and gpuRun, the render with it (withGPU()); gpuNone: the
  // plug-in can't render on the CPU and can't use the host's device. gpuDone: rendered with it (or
  // failed to), below
  const char* gpuApi = NULL;
  std::function<OfxStatus(bool&, bool&, bool&)> gpuRun;
  bool gpuNone = false;
#if defined(HOSTDEMO_OPENCL) || defined(HOSTDEMO_METAL) || defined(HOSTDEMO_CUDA)
  const auto gpuAction = [&] { return render(false, NULL); };
#endif
#ifdef HOSTDEMO_OPENCL
  const CL::RenderMode clMode = openCL ? openCL->mode(*instance) : CL::RenderMode::CPU;
  if (clMode == CL::RenderMode::Buffers || clMode == CL::RenderMode::Images) {
    gpuApi = "OpenCL";
    gpuRun = [&](bool& sent, bool& failed, bool& retry) {
      return withGPU<CL::Queue, CL::RenderSession>("OpenCL", *openCL->device, *instance, outputClip, t, renderWindow,
                                                   gpuAction, sent, failed, retry, clMode);
    };
  } else if (openCL && clMode == CL::RenderMode::None) {
    gpuApi = "OpenCL";
    gpuNone = true;
  }
#else
  (void)openCL;
#endif
#ifdef HOSTDEMO_METAL
  const MT::RenderMode mtMode = metal ? metal->mode(*instance) : MT::RenderMode::CPU;
  if (mtMode == MT::RenderMode::Buffers || mtMode == MT::RenderMode::Textures) {
    gpuApi = "Metal";
    gpuRun = [&](bool& sent, bool& failed, bool& retry) {
      MT::AutoreleasePool pool; // (the plug-in's autoreleased Metal objects)
      return withGPU<MT::Queue, MT::RenderSession>("Metal", *metal->device, *instance, outputClip, t, renderWindow,
                                                   gpuAction, sent, failed, retry, mtMode);
    };
  } else if (metal && mtMode == MT::RenderMode::None) {
    gpuApi = "Metal";
    gpuNone = true;
  }
#else
  (void)metal;
#endif
#ifdef HOSTDEMO_CUDA
  const CU::RenderMode cuMode = cuda ? cuda->mode(*instance) : CU::RenderMode::CPU;
  if (cuMode == CU::RenderMode::CUDA) {
    gpuApi = "CUDA";
    gpuRun = [&](bool& sent, bool& failed, bool& retry) {
      return withGPU<CU::Stream, CU::RenderSession>("CUDA", *cuda->device, *instance, outputClip, t, renderWindow,
                                                    gpuAction, sent, failed, retry);
    };
  } else if (cuda && cuMode == CU::RenderMode::None) {
    gpuApi = "CUDA";
    gpuNone = true;
  }
#else
  (void)cuda;
#endif
  bool usedOpenGL = false, cpuRetry = false, usedGPU = false;
  const bool gpuDone = gpuApi != NULL;
  if (gpuNone) {
    std::cerr << "Error: the plug-in can't render on the CPU (kOfxImageEffectPropCPURenderSupported \"false\"), "
                 "and hostDemo has no " << gpuApi << " device it can use" << std::endl;
    stat = kOfxStatFailed;
  } else if (gpuDone) {
    bool sent = false, failed = false, retry = false;
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    // the view of this render first, as with OpenGL below
    outputClip->setView(view);
#endif
    stat = gpuRun(sent, failed, retry);
    usedGPU = sent && !failed;
    if (!usedGPU && retry) {
      // render again on the CPU, with a new output image
      cpuRetry = true;
      outputClip->releaseOutputImage();
      stat = render(false, NULL);
    } else if (!usedGPU && stat == kOfxStatOK) {
      stat = kOfxStatFailed;
    }
  }
#ifdef HOSTDEMO_OPENGL
  OpenGLHost* gl = openGL;
  const GL::RenderMode mode = gl ? gl->mode(*instance) : GL::RenderMode::CPU;
  if (gpuDone) {
    // (rendered with OpenCL, Metal or CUDA above)
  } else if (mode == GL::RenderMode::OpenGL) {
    bool glFailed = false, retry = false;
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    // the view of this render first: renderAction() sets it, which releases the output image of
    // another view, and the render target is made of this one before
    outputClip->setView(view);
#endif
    stat = withOpenGL(*gl, *instance, outputClip, t, renderWindow, render, glFailed, retry);
    usedOpenGL = !glFailed;
    if (glFailed && retry) {
      // render again on the CPU, with a new output image
      cpuRetry = true;
      outputClip->releaseOutputImage();
      stat = render(false, NULL);
    } else if (glFailed && stat == kOfxStatOK) {
      stat = kOfxStatFailed;
    }
  } else if (mode == GL::RenderMode::None) {
    std::cerr << "Error: the plug-in needs OpenGL (kOfxImageEffectPropOpenGLRenderSupported \"needed\"), "
                 "and hostDemo has no OpenGL context for it" << std::endl;
    stat = kOfxStatFailed;
  } else
#else
  if (gpuDone) {
    // (rendered with OpenCL, Metal or CUDA above)
  } else
#endif
  {
    stat = render(false, NULL);
  }
  instance->clearRenderState();
  // the render is OK if the action succeeded (kOfxStatOK only) and the image fetches too
  Json& renderJ = r["render"] = actionJson(stat);
  renderJ["ok"] = stat == kOfxStatOK && MyHost::gImageErrors == imageErrors;
  renderJ["imageFetchErrors"] = MyHost::gImageErrors - imageErrors;
  if (openGL) {
    // --gpu=opengl: whether OpenGL rendered this frame, and whether the CPU rendered it again
    renderJ["openGL"] = usedOpenGL;
    renderJ["cpuRetry"] = cpuRetry;
  }
  if (openCL) {
    // --gpu=opencl: whether OpenCL rendered this frame, and whether the CPU rendered it again
    renderJ["openCL"] = usedGPU;
    renderJ["cpuRetry"] = cpuRetry;
  }
  if (metal) {
    // --gpu=metal: whether Metal rendered this frame, and whether the CPU rendered it again
    renderJ["metal"] = usedGPU;
    renderJ["cpuRetry"] = cpuRetry;
  }
  if (cuda) {
    // --gpu=cuda: whether CUDA rendered this frame, and whether the CPU rendered it again
    renderJ["cuda"] = usedGPU;
    renderJ["cpuRetry"] = cpuRetry;
  }
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
  MyHost::Profile profile = MyHost::Profile::eDefault; ///< --ofx14, --ofx11
  bool ofx14And11 = false;     ///< both --ofx14 and --ofx11
  bool openglDescribe = false; ///< --opengl-describe
  bool gpuOpenGL = false;      ///< --gpu=opengl
  std::string gpuRenderer;     ///< --gpu-renderer=BACKEND:ID
  bool gpuRenderers = false;   ///< --gpu-renderers
  bool gpuDevices = false;     ///< --gpu-devices
  bool gpuOpenCL = false;      ///< --gpu=opencl
  bool gpuMetal = false;       ///< --gpu=metal
  bool gpuCUDA = false;        ///< --gpu=cuda
  bool multiPlane = false;     ///< --multiplane
  /// --components: kOfxImageEffectPropSupportedComponents of the host descriptor, and their short names
  std::vector<std::string> hostComponents, hostComponentNames;
  /// -i Clip@Plane=spec: by clip, then by short plane name
  std::map<std::string, std::map<std::string, MyHost::InputPlane> > inputPlanes;
  bool normalisedDefaults = true; ///< --no-normalised-defaults
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
    } else if (arg == "--ofx14" || arg == "--ofx11") {
      const MyHost::Profile p = arg == "--ofx14" ? MyHost::Profile::eOFX14 : MyHost::Profile::eOFX11;
      ofx14And11 = ofx14And11 || (profile != MyHost::Profile::eDefault && profile != p);
      profile = p;
    } else if (arg == "--opengl-describe") {
      openglDescribe = true;
    } else if (arg == "--gpu=opengl" || arg == "--gpu=opencl" || arg == "--gpu=metal" || arg == "--gpu=cuda" ||
               arg == "--gpu=cpu") {
      gpuOpenGL = arg == "--gpu=opengl";
      gpuOpenCL = arg == "--gpu=opencl";
      gpuMetal = arg == "--gpu=metal";
      gpuCUDA = arg == "--gpu=cuda";
    } else if (arg.compare(0, 15, "--gpu-renderer=") == 0) {
      gpuRenderer = arg.substr(15);
    } else if (arg == "--gpu-renderers") {
      gpuRenderers = true;
    } else if (arg == "--gpu-devices") {
      gpuDevices = true;
    } else if (arg == "--multiplane") {
#ifdef OFX_EXTENSIONS_NUKE
      multiPlane = true;
#else
      std::cerr << "Error: --multiplane needs a hostDemo built with OFX_EXTENSIONS_NUKE" << std::endl;
      return 1;
#endif
    } else if (arg == "--no-normalised-defaults") {
      normalisedDefaults = false;
    } else if (arg == "-p" || arg == "-m" || arg == "-c" || arg == "-o" || arg == "-s" ||
               arg == "-d" || arg == "-i" || arg == "--format" ||
               arg == "--report" || arg == "--describe" || arg == "--components" ||
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
      } else if (arg == "-i" && optarg.find('=') != std::string::npos && optarg.find('@') < optarg.find('=')) {
        // -i Clip@Plane=spec: an extra plane
        std::string clipName, planeName, error;
        MyHost::InputPlane plane;
        if (!MyHost::parseInputPlaneArg(optarg, clipName, planeName, plane, error)) {
          std::cerr << "Error: invalid input plane \"" << optarg << "\" (" << error
                    << "), expected Clip@ForwardMotionVector|BackMotionVector|StereoDisparityLeft|StereoDisparityRight=spec"
                    << std::endl;
          return 1;
        }
        if (!inputPlanes[clipName].insert(std::make_pair(planeName, plane)).second) {
          std::cerr << "Error: input plane \"" << clipName << "@" << planeName << "\" given twice (-i)" << std::endl;
          return 1;
        }
      } else if (arg == "-i") {
        std::string::size_type eq = optarg.find('=');
        MyHost::InputSpec spec;
        std::string error = "no clip name";
        if (eq == std::string::npos || eq == 0 || !MyHost::parseInputSpec(optarg.substr(eq + 1), spec, error)) {
          std::cerr << "Error: invalid input \"" << optarg << "\" (" << error
                    << "), expected Clip=default|solid:r,g,b,a|ramp|checker:N|rod:x1,y1,x2,y2|none|"
                       "file:rgb.pfm[+alpha.pfm][@x1,y1][#premult|#unpremult|#opaque], then optionally #rgb|#alpha|#rgba"
                    << std::endl;
          return 1;
        }
        if (inputSpecs.count(optarg.substr(0, eq))) {
          std::cerr << "Error: input clip \"" << optarg.substr(0, eq) << "\" given twice (-i)" << std::endl;
          return 1;
        }
        inputs.push_back(std::make_pair(optarg.substr(0, eq), spec));
        inputSpecs[optarg.substr(0, eq)] = optarg.substr(eq + 1);
      } else if (arg == "--components") {
        // the names of the components, and their OFX names
        static const char* const kComponents[3][2] = {
          {"rgba", kOfxImageComponentRGBA}, {"rgb", kOfxImageComponentRGB}, {"alpha", kOfxImageComponentAlpha}};
        hostComponents.clear();
        hostComponentNames.clear();
        for (const std::string& c : splitValues(optarg)) {
          int k = 0;
          while (k < 3 && c != kComponents[k][0]) {
            ++k;
          }
          if (k == 3 || std::find(hostComponentNames.begin(), hostComponentNames.end(), kComponents[k][0]) != hostComponentNames.end()) {
            std::cerr << "Error: invalid components \"" << optarg << "\", expected distinct rgba, rgb or alpha, comma-separated" << std::endl;
            return 1;
          }
          hostComponentNames.push_back(kComponents[k][0]);
          hostComponents.push_back(kComponents[k][1]);
        }
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
            ((long long)w[2] - w[0]) * ((long long)w[3] - w[1]) > MyHost::kMaxImagePixels) {
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
  for (auto& clipPlanes : inputPlanes) {
    for (auto& p : clipPlanes.second) {
      p.second.spec.fileScaleX = renderScale.x;
      p.second.spec.fileScaleY = renderScale.y;
    }
  }

  if (!reportPath.empty() && !describePath.empty()) {
    std::cerr << "Error: --report and --describe can't be used together (--describe creates no instance)" << std::endl;
    return 1;
  }
  // a host whose kOfxImageEffectPropOpenGLRenderSupported is "true" must render with OpenGL when
  // the plug-in needs it, which hostDemo can't: describe only
  if (openglDescribe && describePath.empty()) {
    std::cerr << "Error: --opengl-describe needs --describe (to render with OpenGL: --gpu=opengl)" << std::endl;
    return 1;
  }
  if (!gpuRenderer.empty() && !gpuOpenGL) {
    std::cerr << "Error: --gpu-renderer needs --gpu=opengl" << std::endl;
    return 1;
  }
#ifndef HOSTDEMO_OPENGL
  if (gpuOpenGL || gpuRenderers) {
    std::cerr << "Error: " << (gpuRenderers ? "--gpu-renderers" : "--gpu=opengl")
              << ": hostDemo was built without OFX_HOSTSUPPORT_OPENGL" << std::endl;
    return 1;
  }
#else
  // --gpu-renderer=BACKEND:ID: a backend name and a decimal int (optional sign), in the int range
  std::vector<GL::Backend> gpuBackends = GL::defaultBackends();
  int gpuRendererId = -1;
  if (!gpuRenderer.empty()) {
    const size_t colon = gpuRenderer.rfind(':');
    GL::Backend backend;
    const std::string idText = colon == std::string::npos ? std::string() : gpuRenderer.substr(colon + 1);
    const size_t sign = !idText.empty() && (idText[0] == '+' || idText[0] == '-') ? 1 : 0;
    const bool digits = idText.size() > sign &&
      idText.find_first_not_of("0123456789", sign) == std::string::npos;
    errno = 0;
    const long long id = digits ? std::strtoll(idText.c_str(), nullptr, 10) : 0;
    if (!digits || errno == ERANGE || id < INT_MIN || id > INT_MAX ||
        !GL::backendFromName(gpuRenderer.substr(0, colon), backend)) {
      std::cerr << "Error: --gpu-renderer: '" << gpuRenderer << "': expected BACKEND:ID (see --gpu-renderers)" << std::endl;
      return 1;
    }
    gpuBackends.assign(1, backend);
    gpuRendererId = static_cast<int>(id);
  }
  if (gpuRenderers || gpuDevices) {
    if (gpuDevices) {
      std::cout << "OpenGL renderers:\n";
    }
    const std::vector<GL::RendererInfo> renderers = GL::listRenderers(GL::Hooks());
    if (gpuDevices && renderers.empty()) {
      std::cout << "  (none)\n";
    }
    for (const GL::RendererInfo &r : renderers) {
      std::cout << GL::backendName(r.backend) << ":" << r.id << "\t" << r.renderer << " (" << r.vendor << ", "
                << r.version << ", GLSL " << (r.shadingLanguageVersion.empty() ? "-" : r.shadingLanguageVersion)
                << ", " << (r.videoMemoryBytes > 0 ? std::to_string(r.videoMemoryBytes >> 20) + " MiB" : "Unknown")
                << (r.software ? ", software" : "") << ")\n";
    }
    if (!gpuDevices) {
      std::cout.flush();
      return 0;
    }
  }
#endif
  if (gpuDevices) {
    // (the rest of --gpu-devices: the ids are those of the ofxh module's init(), see its README)
    // an empty list: why, on stderr (stdout is the same as the Python host's)
    std::string why;
    auto noDevice = [&why](const char *api) {
      if (!why.empty()) {
        std::cout.flush();
        std::cerr << "Warning: --gpu-devices: no " << api << " device (" << why << ")" << std::endl;
      }
    };
    (void)noDevice;  // (unused in builds without OpenCL, Metal and CUDA)
#ifdef HOSTDEMO_OPENCL
    std::cout << "OpenCL devices:\n";
    const std::vector<CL::DeviceInfo> clDevices = CL::listDevices(CL::Hooks(), &why);
    if (clDevices.empty()) {
      std::cout << "  (none)\n";
      noDevice("OpenCL");
    }
    for (const CL::DeviceInfo &d : clDevices) {
      std::cout << d.platform << ":" << d.device << "\t" << d.name << " (" << d.vendor << ", " << d.platformName
                << ", " << d.version << ", driver " << d.driverVersion << ", " << (d.globalMemSize >> 20) << " MiB"
                << (d.gpu ? ", GPU" : "") << (d.imageSupport ? ", images" : "") << ")\n";
    }
#endif
#ifdef HOSTDEMO_METAL
    std::cout << "Metal devices:\n";
    const std::vector<MT::DeviceInfo> mtDevices = MT::listDevices(MT::Hooks(), &why);
    if (mtDevices.empty()) {
      std::cout << "  (none)\n";
      noDevice("Metal");
    }
    for (const MT::DeviceInfo &d : mtDevices) {
      std::cout << d.index << "\t" << d.name << " (" << (d.recommendedMaxWorkingSetSize >> 20) << " MiB working set"
                << (d.unifiedMemory ? ", unified memory" : "") << (d.systemDefault ? ", system default" : "") << ")\n";
    }
#endif
#if defined(HOSTDEMO_CUDA) && !defined(__APPLE__)
    // (macOS has no CUDA driver: HostSupportCUDA builds there only for its checks on a fake driver)
    std::cout << "CUDA devices:\n";
    const std::vector<CU::DeviceInfo> cuDevices = CU::listDevices(CU::Hooks(), &why);
    if (cuDevices.empty()) {
      std::cout << "  (none)\n";
      noDevice("CUDA");
    }
    for (const CU::DeviceInfo &d : cuDevices) {
      std::cout << d.index << "\t" << d.name << " (" << d.uuid << ", compute " << d.computeMajor << "." << d.computeMinor
                << ", PCI " << d.pciBusId << ", " << (d.totalMem >> 20) << " MiB, driver " << d.driverVersion / 1000
                << "." << d.driverVersion % 1000 / 10 << ")\n";
    }
#endif
    std::cout.flush();
    return 0;
  }
#ifndef OFX_SUPPORTS_OPENGLRENDER
  if (openglDescribe) {
    std::cerr << "Error: --opengl-describe: hostDemo was built without OFX_SUPPORTS_OPENGLRENDER (no OpenGL render suite)" << std::endl;
    return 1;
  }
#endif
  if (!inputPlanes.empty() && !multiPlane) {
    std::cerr << "Error: -i Clip@Plane=spec needs --multiplane" << std::endl;
    return 1;
  }
  if (multiPlane && (profile == MyHost::Profile::eOFX14 || ofx14And11)) {
    std::cerr << "Error: --multiplane and --ofx14 can't be used together (OFX 1.4 has no plane suite)" << std::endl;
    return 1;
  }
  if (ofx14And11 || (profile == MyHost::Profile::eOFX11 && (multiPlane || openglDescribe || gpuOpenGL))) {
    std::cerr << "Error: --ofx11 can't be used with --ofx14, --multiplane, --opengl-describe or --gpu=opengl" << std::endl;
    return 1;
  }
  // (the OpenCL properties are OFX 1.5: an OFX 1.4 or 1.1 host has none)
  if (profile != MyHost::Profile::eDefault && gpuOpenCL) {
    std::cerr << "Error: --gpu=opencl can't be used with --ofx14 or --ofx11 (OpenCL render is OFX 1.5)" << std::endl;
    return 1;
  }
  // (after the option conflicts, as the Python host, which checks its ofxh build after parsing)
#ifndef HOSTDEMO_OPENCL
  if (gpuOpenCL) {
    std::cerr << "Error: --gpu=opencl: hostDemo was built without OFX_HOSTSUPPORT_OPENCL" << std::endl;
    return 1;
  }
#endif
  // (likewise for Metal, OFX 1.5 too)
  if (profile != MyHost::Profile::eDefault && gpuMetal) {
    std::cerr << "Error: --gpu=metal can't be used with --ofx14 or --ofx11 (Metal render is OFX 1.5)" << std::endl;
    return 1;
  }
#ifndef HOSTDEMO_METAL
  if (gpuMetal) {
    std::cerr << "Error: --gpu=metal: hostDemo was built without OFX_HOSTSUPPORT_METAL" << std::endl;
    return 1;
  }
#endif
  // (likewise for CUDA, OFX 1.5 too)
  if (profile != MyHost::Profile::eDefault && gpuCUDA) {
    std::cerr << "Error: --gpu=cuda can't be used with --ofx14 or --ofx11 (CUDA render is OFX 1.5)" << std::endl;
    return 1;
  }
#ifndef HOSTDEMO_CUDA
  if (gpuCUDA) {
    std::cerr << "Error: --gpu=cuda: hostDemo was built without OFX_HOSTSUPPORT_CUDA" << std::endl;
    return 1;
  }
#endif
  if (profile == MyHost::Profile::eOFX11 && inputDepth == kOfxBitDepthShort) {
    std::cerr << "Error: -d short: the --ofx11 host only supports byte and float depths" << std::endl;
    return 1;
  }

  //_CrtSetBreakAlloc(3168);
#if defined(_MSC_VER) && defined(_DEBUG)
  _CrtSetDbgFlag ( _CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF );
#endif
  // this must be called before the first call to getPluginCache()
  OFX::Host::PluginCache::useStdOFXPluginsLocation(useStdPaths);

#ifdef HOSTDEMO_OPENGL
  // --gpu=opengl: the context, created first: without one, the host doesn't say it supports OpenGL
  OpenGLHost gl;
  if (gpuOpenGL) {
    GL::Hooks hooks;
    std::string why;
    gl.context = GL::createContext(hooks, gpuBackends, gpuRendererId, &why);
    if (!gl.context && !gpuRenderer.empty()) {
      // (a renderer the user chose: no silent fallback to another one, or to the CPU)
      std::cerr << "Error: --gpu-renderer=" << gpuRenderer << ": no OpenGL context (" << why << ")" << std::endl;
      return 1;
    }
    if (!gl.context) {
      std::cerr << "Warning: --gpu=opengl: no OpenGL context (" << why << "): rendering on the CPU" << std::endl;
    }
  }
  const bool hostOpenGL = gl.context != nullptr;
#else
  const bool hostOpenGL = false;
#endif
#ifdef HOSTDEMO_OPENCL
  // --gpu=opencl: the device (by default the first GPU, else the first device), created first:
  // without one, the host doesn't say it supports OpenCL
  OpenCLHost cl;
  if (gpuOpenCL) {
    std::string why;
    cl.device = CL::createDevice(CL::Hooks(), -1, -1, &why);
    if (!cl.device) {
      std::cerr << "Warning: --gpu=opencl: no OpenCL device (" << why << "): rendering on the CPU" << std::endl;
    }
  }
  const bool hostOpenCL = cl.device != nullptr;
  const bool hostOpenCLImages = hostOpenCL && cl.device->info().imageSupport;
#else
  const bool hostOpenCL = false, hostOpenCLImages = false;
#endif
#ifdef HOSTDEMO_METAL
  // --gpu=metal: the device (the system default device, else the first one), created first:
  // without one, the host doesn't say it supports Metal
  MetalHost mt;
  if (gpuMetal) {
    std::string why;
    mt.device = MT::createDevice(MT::Hooks(), -1, &why);
    if (!mt.device) {
      std::cerr << "Warning: --gpu=metal: no Metal device (" << why << "): rendering on the CPU" << std::endl;
    }
  }
  const bool hostMetal = mt.device != nullptr;
#else
  const bool hostMetal = false;
#endif
#ifdef HOSTDEMO_CUDA
  // --gpu=cuda: the device (the first one), created first: without one, the host doesn't say it
  // supports CUDA
  CUDAHost cu;
  if (gpuCUDA) {
    std::string why;
    cu.device = CU::createDevice(CU::Hooks(), -1, &why);
    if (!cu.device) {
      std::cerr << "Warning: --gpu=cuda: no CUDA device (" << why << "): rendering on the CPU" << std::endl;
    }
  }
  const bool hostCUDA = cu.device != nullptr;
#else
  const bool hostCUDA = false;
#endif
  // set the version label in the global cache
  // (the plug-ins may describe themselves differently to an OFX 1.4 host, to a host with
  // OpenGL render, to a multi-plane host, or to a host with other components: another cache)
  std::string componentsSuffix;
  if (!hostComponentNames.empty() &&
      hostComponentNames != std::vector<std::string>{"rgba", "alpha"}) {
    componentsSuffix = "-components";
    for (const std::string& c : hostComponentNames) {
      componentsSuffix += "-" + c;
    }
  }
  const std::string cacheSuffix =
    (profile == MyHost::Profile::eDefault ? std::string() : std::string("-") + MyHost::profileName(profile)) +
    (openglDescribe || hostOpenGL ? "-opengl" : "") + (hostOpenCL ? (hostOpenCLImages ? "-opencl-images" : "-opencl") : "") +
    (hostMetal ? "-metal" : "") + (hostCUDA ? "-cuda" : "") +
    (multiPlane ? "-multiplane" : "") + componentsSuffix;
  OFX::Host::PluginCache::getPluginCache()->setCacheVersion("hostDemoV1" + cacheSuffix);
  const std::string cachePath = "hostDemoPluginCache" + cacheSuffix + ".xml";

  // create our derived image effect host which provides
  // a factory to make plugin instances and acts
  // as a description of the host application
  MyHost::Host myHost;
  if (profile == MyHost::Profile::eOFX14) {
    myHost.restrictToOFX14();
  } else if (profile == MyHost::Profile::eOFX11) {
    myHost.restrictToOFX11();
  }
  if (openglDescribe || hostOpenGL) {
    // (an OFX 1.4 host property: also with --ofx14)
    myHost.getProperties().setStringProperty(kOfxImageEffectPropOpenGLRenderSupported, "true");
  }
  if (hostOpenCL) {
    // "a host indicates it supports OpenCL acceleration by setting kOfxImageEffectPropOpenCLSupported
    // (Images) and/or kOfxImageEffectPropOpenCLRenderSupported (Buffers) on it's descriptor"
    // (ofxGPURender.h): Images if the device has them
    myHost.getProperties().setStringProperty(kOfxImageEffectPropOpenCLRenderSupported, "true");
    myHost.getProperties().setStringProperty(kOfxImageEffectPropOpenCLSupported, hostOpenCLImages ? "true" : "false");
#ifdef HOSTDEMO_OPENCL
    // the OpenCL program suite, "an optional suite the host can provide for building OpenCL
    // programs for the plug-in" (ofxGPURender.h): built for the context of the render's queue
    CL::Device* device = cl.device.get();
    myHost.setOpenCLProgramCompiler([device](const char* source, int fOptional, void* result) {
      return device->compileProgram(CL::currentQueue(), source, fOptional, result);
    });
#endif
  }
  if (hostMetal) {
    // kOfxImageEffectPropMetalRenderSupported, "Indicates whether a host or plug-in can support Metal
    // render", and kOfxImageEffectPropMetalTextureSupported, "The host sets it on the host
    // descriptor" (ofxGPURender.h): Metal buffers and textures
    myHost.getProperties().setStringProperty(kOfxImageEffectPropMetalRenderSupported, "true");
    myHost.getProperties().setStringProperty(kOfxImageEffectPropMetalTextureSupported, "true");
  }
  if (hostCUDA) {
    // kOfxImageEffectPropCudaRenderSupported, "Indicates whether a host or plug-in can support CUDA
    // render", and kOfxImageEffectPropCudaStreamSupported, "Indicates whether a host or plug-in can
    // support CUDA streams" (ofxGPURender.h, host descriptor): CUDA memory, and a stream for the
    // plug-ins that support streams too
    myHost.getProperties().setStringProperty(kOfxImageEffectPropCudaRenderSupported, "true");
    myHost.getProperties().setStringProperty(kOfxImageEffectPropCudaStreamSupported, "true");
  }
  if (!hostComponents.empty()) {
    // "This list of strings indicate what component types are supported by a host" (an OFX 1.4
    // host property: also with --ofx14 and --ofx11). The images themselves may have any of the
    // three, whatever this says.
    std::vector<const char*> v;
    for (const std::string& c : hostComponents) {
      v.push_back(c.c_str());
    }
    OFX::Host::Property::String* p = myHost.getProperties().fetchStringProperty(kOfxImageEffectPropSupportedComponents);
    if (p == NULL) { // (a property of every image effect host descriptor: ofxhImageEffect.cpp)
      std::cerr << "Error: --components: the host descriptor has no kOfxImageEffectPropSupportedComponents" << std::endl;
      return 1;
    }
    p->setValueN(v.data(), (int)v.size());
  }
#ifdef OFX_EXTENSIONS_NUKE
  if (multiPlane) {
    // "Indicates that a host or plugin can fetch more than a type of image from a clip"
    // (nuke/fnOfxExtensions.h): with the plane suites, which HostSupport gives in these builds
    myHost.getProperties().setIntProperty(kFnOfxImageEffectPropMultiPlanar, 1);
  }
#endif
  if (!normalisedDefaults) {
    myHost.disableNormalisedDefaults();
  }

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
  // kOfxActionLoad and kOfxActionDescribe before the report, so that it shows what the plug-in
  // describes, not a possibly stale plug-in cache. getPluginHandle() logs a failure, and the
  // plug-in then gets no further action (ofxCore.h, kOfxActionLoad: "kOfxStatFailed, the load
  // action failed, no further actions will be passed to the plug-in"). A plug-in with no context
  // (its Load or Describe failed during the scan) can't be used anyway: it isn't loaded again.
  const bool loadFailed = plugin &&
    plugin->getDescriptor().getProps().getDimension(kOfxImageEffectPropSupportedContexts) > 0 &&
    plugin->getPluginHandle() == NULL;

  if (!quiet) {
    imageEffectPluginCache.dumpToStdOut();
  }

  int ret = 0;
  // the --report document (see python/README.md)
  Json report = Json::object();
  report["schema"] = "hostDemo.report";
  report["schemaVersion"] = 1;
  report["ok"] = false;
  if (plugin) {
    report["plugin"] = pluginJson(plugin);
  } else {
    report["plugin"] = Json::object();
    report["plugin"]["id"] = pluginId;
  }
  report["context"] = context;
  {
    Json& host = report["host"] = Json::object();
    host["name"] = myHost.getProperties().getStringProperty(kOfxPropName);
    host["profile"] = MyHost::profileName(profile);
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
    if (!describePlugin(plugin, loadFailed, desc)) {
      ret = 1;
    }
    if (!writeJson(describePath, desc)) {
      ret = 1;
    }
  } else if (plugin->getContexts().find(context) == plugin->getContexts().end()) {
    std::cerr << "Error: plug-in \"" << pluginId << "\" does not support the " << context << " context." << std::endl;
    ret = 1;
  } else if (loadFailed) {
    std::cerr << "Error: could not create an instance of plug-in \"" << pluginId << "\" in the " << context << " context." << std::endl;
    ret = 1;
  } else {
    // create an instance of it in the requested context (filter by default)
    // the first arg is the context, the second is client data we are allowed to pass down the call chain

    OFX::Host::auto_ptr<OFX::Host::ImageEffect::Instance> instance(plugin->createInstance(context, NULL));
    // send kOfxActionDestroyInstance while the instance is still whole, before it is deleted (the
    // guard is destroyed before the auto_ptr): a plug-in may call the host during that action (e.g.
    // clearPersistentMessage()), which made a pure virtual call when the HostSupport destructor sent it
    struct DestroyInstanceGuard {
      OFX::Host::ImageEffect::Instance *instance;
      ~DestroyInstanceGuard() { if (instance) { instance->destroyInstanceAction(); } }
    } destroyInstanceGuard = { instance.get() };
    MyHost::MyEffectInstance* myInstance = dynamic_cast<MyHost::MyEffectInstance*>(instance.get());
#ifdef HOSTDEMO_OPENGL
    // the instance's OpenGL attachment: kOfxActionOpenGLContextDetached is sent at its end, before
    // kOfxActionDestroyInstance (this guard is destroyed before destroyInstanceGuard)
    struct DetachGuard {
      OpenGLHost &gl;
      ~DetachGuard() { gl.attachment.reset(); }
    } detachGuard = { gl };
    if (myInstance && gl.context) {
      gl.attachment.reset(new GL::Attachment(*myInstance));
    }
#endif

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
      for (auto ip = inputPlanes.begin(); ret == 0 && ip != inputPlanes.end(); ++ip) {
        MyHost::MyClipInstance* clip = dynamic_cast<MyHost::MyClipInstance*>(instance->getClip(ip->first));
        if (!clip || clip->isOutput()) {
          std::cerr << "Error: the plug-in has no input clip named \"" << ip->first << "\". Its clips are:";
          for (std::map<std::string, OFX::Host::ImageEffect::ClipInstance*>::const_iterator it = clips.begin(); it != clips.end(); ++it) {
            std::cerr << " " << it->first;
          }
          std::cerr << std::endl;
          ret = 1;
          break;
        }
        for (const auto& p : ip->second) {
          clip->setInputPlane(p.second);
        }
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
          if (multiPlane) {
            // its -i Clip@Plane=spec
            Json& planes = in["planes"] = Json::object();
            const auto ps = inputPlanes.find(it->first);
            if (ps != inputPlanes.end()) {
              for (const auto& p : ps->second) {
                planes[p.first] = p.second.text;
              }
            }
          }
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
      double time = 0.;
      // (the timeline is at the time of the change: a paramSetValue in instanceChanged on a
      // parameter with keys sets the key at that time)
      instance->timeLineGotoTime(0.);
      if (!setParamFromString(instance.get(), paramAssignments[k], quiet, name, time)) {
        ret = 1;
        break;
      }
      OfxPointD scale;
      scale.x = scale.y = 1.;
      instance->timeLineGotoTime(time);
      const OfxStatus st[3] = {
        instance->beginInstanceChangedAction(kOfxChangeUserEdited),
        instance->paramInstanceChangedAction(name, kOfxChangeUserEdited, time, scale),
        instance->endInstanceChangedAction(kOfxChangeUserEdited)
      };
      instance->timeLineGotoTime(0.);
      const char* names[3] = { "beginInstanceChanged", "instanceChanged", "endInstanceChanged" };
      for (int i = 0; i < 3; ++i) {
        Json& a = actions.push(namedActionJson(names[i], actionJson(st[i])));
        a["param"] = name;
        const std::string::size_type eq = paramAssignments[k].find('=');
        a["value"] = eq == std::string::npos ? Json() : Json(paramAssignments[k].substr(eq + 1));
        a["time"] = time;
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
#ifdef OFX_EXTENSIONS_NUKE
        if (multiPlane) {
          // kFnOfxImageEffectPropComponentsPresent: "the planes present", as a plug-in reads it
          Json& present = c["planesPresent"] = Json::array();
          const OFX::Host::Property::Set& cprops = it->second->getProps();
          const int n = cprops.getDimension(kFnOfxImageEffectPropComponentsPresent);
          for (int k = 0; k < n; ++k) {
            present.push(cprops.getStringProperty(kFnOfxImageEffectPropComponentsPresent, k));
          }
        }
#endif
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
      // Begin/EndSequenceRender, with OpenGL if the instance renders with it (kOfxImageEffectPropOpenGLEnabled
      // is in their inArgs too), else without
      auto sequence = [&](bool begin, bool openGLRender, void* contextData) {
        (void)openGLRender;
        (void)contextData;
        return (instance.get()->*(begin ? &OFX::Host::ImageEffect::Instance::beginRenderAction
                                        : &OFX::Host::ImageEffect::Instance::endRenderAction))(
          firstFrame, lastFrame, frameStep, false, renderScale, sequential, /*interactive=*/false,
#ifdef OFX_SUPPORTS_OPENGLRENDER
          openGLRender,
#ifdef OFX_EXTENSIONS_NATRON
          contextData,
#endif
#endif
          /*draftRender=*/false
#ifdef OFX_EXTENSIONS_NUKE
          , 0 /* view*/
#endif
          );
      };
      OpenGLHost* openGLArg = NULL; // renderFrame()'s (--gpu=opengl)
      OpenCLHost* openCLArg = NULL; // renderFrame()'s (--gpu=opencl)
      MetalHost* metalArg = NULL;   // renderFrame()'s (--gpu=metal)
      CUDAHost* cudaArg = NULL;     // renderFrame()'s (--gpu=cuda)
      // whether BeginSequenceRender ran with OpenGL: EndSequenceRender does the same (the pair
      // shares kOfxImageEffectPropOpenGLEnabled); likewise with OpenCL, in the same mode, Metal and
      // CUDA
      bool sequenceOpenGL = false;
#ifdef HOSTDEMO_OPENCL
      CL::RenderMode sequenceOpenCL = CL::RenderMode::CPU;
#endif
#ifdef HOSTDEMO_METAL
      MT::RenderMode sequenceMetal = MT::RenderMode::CPU;
#endif
#ifdef HOSTDEMO_CUDA
      bool sequenceCUDA = false;
#endif
      auto sequenceAction = [&](bool begin) {
#ifdef HOSTDEMO_OPENCL
        if (begin) {
          sequenceOpenCL = cl.device ? cl.mode(*myInstance) : CL::RenderMode::CPU;
        }
        if (sequenceOpenCL == CL::RenderMode::Buffers || sequenceOpenCL == CL::RenderMode::Images) {
          bool sent = false, clFailed = false, retry = false;
          const OfxRectI noWindow = { 0, 0, 0, 0 };
          const OfxStatus st = withGPU<CL::Queue, CL::RenderSession>("OpenCL", *cl.device, *myInstance, NULL, firstFrame,
                                                                     noWindow, [&] { return sequence(begin, false, NULL); },
                                                                     sent, clFailed, retry, sequenceOpenCL);
          if (sent) {
            return st;
          }
          // (refused by the session: without OpenCL, and End too)
          sequenceOpenCL = CL::RenderMode::CPU;
        }
#endif
#ifdef HOSTDEMO_METAL
        if (begin) {
          sequenceMetal = mt.device ? mt.mode(*myInstance) : MT::RenderMode::CPU;
        }
        if (sequenceMetal == MT::RenderMode::Buffers || sequenceMetal == MT::RenderMode::Textures) {
          bool sent = false, mtFailed = false, retry = false;
          const OfxRectI noWindow = { 0, 0, 0, 0 };
          MT::AutoreleasePool pool; // (the plug-in's autoreleased Metal objects)
          const OfxStatus st = withGPU<MT::Queue, MT::RenderSession>("Metal", *mt.device, *myInstance, NULL, firstFrame,
                                                                     noWindow, [&] { return sequence(begin, false, NULL); },
                                                                     sent, mtFailed, retry, sequenceMetal);
          if (sent) {
            return st;
          }
          // (refused by the session: without Metal, and End too)
          sequenceMetal = MT::RenderMode::CPU;
        }
#endif
#ifdef HOSTDEMO_CUDA
        if (begin) {
          sequenceCUDA = cu.device && cu.mode(*myInstance) == CU::RenderMode::CUDA;
        }
        if (sequenceCUDA) {
          bool sent = false, cuFailed = false, retry = false;
          const OfxRectI noWindow = { 0, 0, 0, 0 };
          const OfxStatus st = withGPU<CU::Stream, CU::RenderSession>("CUDA", *cu.device, *myInstance, NULL, firstFrame,
                                                                      noWindow, [&] { return sequence(begin, false, NULL); },
                                                                      sent, cuFailed, retry);
          if (sent) {
            return st;
          }
          // (refused by the session: without CUDA, and End too)
          sequenceCUDA = false;
        }
#endif
#ifdef HOSTDEMO_OPENGL
        if (begin) {
          sequenceOpenGL = gl.context && gl.mode(*myInstance) == GL::RenderMode::OpenGL;
        }
        if (sequenceOpenGL) {
          bool glFailed = false, retry = false, sent = false;
          const OfxRectI none = { 0, 0, 0, 0 };
          const OfxStatus st = withOpenGL(gl, *myInstance, NULL, firstFrame, none,
                                          [&](bool on, void* data) { sent = true; return sequence(begin, on, data); }, glFailed, retry);
          if (sent) {
            return st;
          }
          // (not attached: without OpenGL, and End too)
          sequenceOpenGL = false;
        }
#endif
        return sequence(begin, false, NULL);
      };
      OfxStatus stat = kOfxStatOK;
#ifdef HOSTDEMO_OPENGL
      openGLArg = gpuOpenGL ? &gl : NULL;
#endif
#ifdef HOSTDEMO_OPENCL
      openCLArg = gpuOpenCL ? &cl : NULL;
#endif
#ifdef HOSTDEMO_METAL
      metalArg = gpuMetal ? &mt : NULL;
#endif
#ifdef HOSTDEMO_CUDA
      cudaArg = gpuCUDA ? &cu : NULL;
#endif
      if (!query) {
        stat = sequenceAction(true);
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
        // render a stereoscopic frame: left and right views (one view with --ofx14 or --ofx11:
        // views are an extension)
        const int nViews = myHost.viewCount();
        for (int view = 0; ret == 0 && view < nViews; ++view) {
          std::ostringstream ss;
          ss << outputPrefix << "." << tname.str() << (nViews == 1 ? "" : view == 0 ? "l" : "r");
          Json* r = reportPath.empty() ? NULL : &renders.push(Json::object());
          if (!renderFrame(myInstance, outputClip, t, view, nViews, renderScale, win, query, sequential, allowUnwritten, ss.str(), format, multiPlane, r, openGLArg, openCLArg, metalArg, cudaArg)) {
            ret = 1;
          }
        }
#else
        std::ostringstream ss;
        ss << outputPrefix << "." << tname.str();
        Json* r = reportPath.empty() ? NULL : &renders.push(Json::object());
        if (!renderFrame(myInstance, outputClip, t, 0, 1, renderScale, win, query, sequential, allowUnwritten, ss.str(), format, multiPlane, r, openGLArg, openCLArg, metalArg, cudaArg)) {
          ret = 1;
        }
#endif
      }

      if (!query) {
        stat = sequenceAction(false);
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
      // the values at time 0 (for a parameter with keys; the keys are reported too)
      reportInstance->timeLineGotoTime(0.);
      Json& params = report["params"] = Json::object();
      const std::list<OFX::Host::Param::Instance*>& pl = reportInstance->getParamList();
      for (std::list<OFX::Host::Param::Instance*>::const_iterator it = pl.begin(); it != pl.end(); ++it) {
        Json& p = params[(*it)->getName()] = Json::object();
        p["type"] = (*it)->getType();
        p["value"] = paramValueJson(*it);
        // the keys (numeric parameters), [[time, value]...], in time order
        if (const MyHost::MyCurve* c = curveOf(*it)) {
          Json& keys = p["keys"] = Json::array();
          for (MyHost::MyCurve::Keys::const_iterator k = c->getKeys().begin(); k != c->getKeys().end(); ++k) {
            Json& key = keys.push(Json::array());
            key.push(k->first);
            key.push(curveValueJson((*it)->getType(), &k->second[0], c->getDimension()));
          }
        }
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
