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


#include <cerrno>
#include <climits>
#include <iostream>
#include <fstream>
#include <cassert>
#include <stdexcept>
#include <sstream> // stringstream
#include <string>
#include <vector>
#include <cstdlib> // strtol, strtod
#include <cstring> // strcmp
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
// It works by hard coding progressive PAL SD imagery to input and output clips.
// By default, the input images are black at t=0 and white at t=1 (and should be
// inverted by the invert plugin); -i sets other inputs, or disconnects a clip.
// Images have the depth and components that the clip preferences map each clip to.
//
// There is no file input; the output is written as PPM files.

// returns false (after printing an error) if the file couldn't be written.
// Short and float images are converted to 8 bits (float values are clamped to [0,1]),
// Alpha images are written as grey levels, and pixels outside the image bounds are black.
bool exportToPPM(const std::string& fname, MyHost::MyImage* im)
{
  if (!im) {
    // the plug-in did not fetch the output image (e.g. a test plug-in that doesn't render)
    std::cerr << "No output image to export to " << fname << std::endl;
    return true; // not an error
  }
  std::ofstream op(fname.c_str());
  if (!op) {
    std::cerr << "Error: cannot open " << fname << " for writing" << std::endl;
    return false;
  }
  OfxRectI rod = im->getROD();
  op << "P3" << "\t# FORMAT" << std::endl;
  op << rod.x2 - rod.x1 << "\t#WIDTH" << std::endl;
  op << rod.y2 - rod.y1 << "\t#HEIGHT" <<std::endl;
  op << "255" << "\t# " << im->getDepth() << " " << im->getComponentsString() << ", converted to 8 bits" << std::endl;
  long notWrittenCount = 0;
  for (int y = rod.y1; y< rod.y2; ++y)
  {
    for (int x = rod.x1; x < rod.x2; ++x)
    {
      int rgb[3];
      bool notWritten = false;
      if (im->getPixelRGB8(x, y, rgb, notWritten)) {
        op << rgb[0] << " " << rgb[1] << " " << rgb[2] << " " << std::endl;
      } else {
        op << "0 0 0" << std::endl;
      }
      if (notWritten) {
        ++notWrittenCount;
      }
    }
  }
  op.close();
  if (!op) {
    std::cerr << "Error: cannot write " << fname << std::endl;
    return false;
  }
  if (notWrittenCount > 0) {
    std::cerr << "Warning: " << fname << ": " << notWrittenCount << " pixels were not written by the plug-in (NaN), written as 0" << std::endl;
  }
  return true;
}

static void usage(const char* prog)
{
  std::cout <<
    "Usage: " << prog << " [-h] [-p pluginId] [-m major] [-c filter|general] [-o prefix]\n"
    "       [-d byte|short|float] [-i Clip=spec]... [--no-std-paths] [--no-cache | --cache path]\n"
    "       [--quiet] [-s name=value[,value...]]... [name=value[,value...]]...\n"
    "Render a filter or general OFX plug-in found in OFX_PLUGIN_PATH, on synthetic\n"
    "PAL SD input clips, and write the output frames as PPM files.\n"
    "  -h          print this help and exit\n"
    "  -p id       plug-in identifier (default: net.sf.openfx.invertPlugin)\n"
    "  -m major    plug-in major version (default: the highest version found)\n"
    "  -c context  filter or general (default: filter)\n"
    "  -o prefix   output file prefix (default: Output): writes <prefix>.<t>[l|r].ppm.\n"
    "              Short and float images are converted to 8 bits (float is clamped to\n"
    "              [0,1]), Alpha is written as grey.\n"
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
    "                none           the clip is disconnected\n"
    "              Unwritten output pixels hold NaN (float) or 0xCD bytes (byte, short).\n"
    "  --no-std-paths\n"
    "              only look for plug-ins in OFX_PLUGIN_PATH, not in the standard\n"
    "              locations (/Library/OFX/Plugins, /usr/OFX/Plugins...)\n"
    "  --no-cache  don't read or write the plug-in cache\n"
    "  --cache path\n"
    "              read and write the plug-in cache at path (default:\n"
    "              hostDemoPluginCache.xml in the current directory)\n"
    "  --quiet     don't print the plug-in cache on stdout\n"
    "  -s name=value[,value...]\n"
    "              set a parameter before rendering (the option name -s may be omitted).\n"
    "              Multi-dimensional parameters take comma-separated values, e.g.\n"
    "              value=0.5,0.5,0.5,0.5 for an RGBA. Booleans take 0/1/true/false,\n"
    "              Choice takes the option index or label, String/StrChoice/Custom\n"
    "              take the whole value (commas included).\n"
    "The exit status is 0 on success, and non-zero on any failure.\n";
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
  if (s.empty()) {
    return false;
  }
  char* end = NULL;
  d = std::strtod(s.c_str(), &end);
  return end != NULL && *end == '\0';
}

static bool parseInt(const std::string& s, int& i)
{
  if (s.empty()) {
    return false;
  }
  char* end = NULL;
  errno = 0;
  long l = std::strtol(s.c_str(), &end, 10);
  if (errno == ERANGE || end == NULL || *end != '\0' || l < INT_MIN || l > INT_MAX) {
    return false;
  }
  i = (int)l;
  return true;
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
static bool setParamFromString(OFX::Host::ImageEffect::Instance* instance, const std::string& assignment)
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
  std::cout << "Set parameter " << name << " = " << value << std::endl;
  return true;
}

// render the frame t (and view) and write the output image to fname.
// Returns false (after printing an error) on failure.
static bool renderFrame(MyHost::MyEffectInstance* instance, MyHost::MyClipInstance* outputClip,
                        OfxTime t, int view, int nViews, const OfxPointD& renderScale,
                        const std::string& fname)
{
  (void)view;
  (void)nViews;
  // The render window is in pixel coordinates (render scale and PAR applied): the whole project
  const double par = instance->getProjectPixelAspectRatio();
  double projectWidth, projectHeight; // canonical coordinates
  instance->getProjectSize(projectWidth, projectHeight);
  const double eps = 1e-6;
  OfxRectI renderWindow;
  renderWindow.x1 = renderWindow.y1 = 0;
  renderWindow.x2 = (int)std::ceil(projectWidth * renderScale.x / par - eps);
  renderWindow.y2 = (int)std::ceil(projectHeight * renderScale.y - eps);

  /// RoI is in canonical coords
  OfxRectD regionOfInterest;
  regionOfInterest.x1 = regionOfInterest.y1 = 0;
  regionOfInterest.x2 = renderWindow.x2 * par / renderScale.x;
  regionOfInterest.y2 = renderWindow.y2 / renderScale.y;

  // get the RoI for each input clip: the input images are fetched with these bounds
  // (clipped to the RoD), unless the plug-in asks for specific bounds
  std::map<OFX::Host::ImageEffect::ClipInstance *, OfxRectD> rois;
  OfxStatus stat = instance->getRegionOfInterestAction(t, renderScale,
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
  return exportToPPM(fname, outputImage);
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
    } else if (arg == "-p" || arg == "-m" || arg == "-c" || arg == "-o" || arg == "-s" ||
               arg == "-d" || arg == "-i" || arg == "--cache") {
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
        if (eq == std::string::npos || eq == 0 || !MyHost::parseInputSpec(optarg.substr(eq + 1), spec)) {
          std::cerr << "Error: invalid input \"" << optarg << "\", expected Clip=default|solid:r,g,b,a|ramp|checker:N|none" << std::endl;
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
#ifdef _WIN32
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
    }

    // set the parameter values given on the command line, before calling the create instance action
    for (size_t k = 0; ret == 0 && k < paramAssignments.size(); ++k) {
      if (!setParamFromString(instance.get(), paramAssignments[k])) {
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
          ss << outputPrefix << "." << t << (view == 0 ? "l" : "r") << ".ppm";
          if (!renderFrame(myInstance, outputClip, t, view, 2, renderScale, ss.str())) {
            ret = 1;
          }
        }
#else
        std::ostringstream ss;
        ss << outputPrefix << "." << t << ".ppm";
        if (!renderFrame(myInstance, outputClip, t, 0, 1, renderScale, ss.str())) {
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
