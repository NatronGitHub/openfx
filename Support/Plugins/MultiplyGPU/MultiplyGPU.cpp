/*
 * OFX MultiplyGPU plugin: CPU, CUDA and OpenCL render. For Metal render, see the GPUGain sample.
 */

#include "MultiplyGPU.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <cfloat> // DBL_MAX
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <windows.h>
#endif

#include "ofxsProcessing.H"
#include "ofxGPURender.h" // kOfxStatGPURenderFailed

using namespace OFX;
using namespace OFX::Plugin::MultiplyGPU;

namespace { // use anonymous namespace

#define kPluginName "MultiplyGPU"
#define kPluginGrouping "Color/Math"
#define kPluginDescription \
"Multiply the selected channels by a constant.\n" \
"For Metal render, see the GPUGain sample.\n" \
"See also: https://web.archive.org/web/20220627015945/http://www.opticalenquiry.com/nuke/index.php?title=Multiply"

#define kPluginIdentifier "net.sf.openfx.MultiplyGPU"
// History:
// version 1.0: initial version
#define kPluginVersionMajor 1 // Incrementing this number means that you have broken backwards compatibility of the plug-in.
#define kPluginVersionMinor 0 // Increment this when you have fixed a bug or made it faster.

#define kSupportsTiles 1
#define kSupportsMultiResolution 1
#define kSupportsRenderScale 1
#define kSupportsMultipleClipPARs false
#define kSupportsMultipleClipDepths false
#define kRenderThreadSafety eRenderFullySafe

#define kParamValueName  "value"
#define kParamValueLabel "Value"
#define kParamValueHint  "Constant to multiply with the selected channels."

class MultiplyProcessorBase
: public ImageProcessor
{
protected:
  const Image *_srcImg;
  double _values[4];

public:

  MultiplyProcessorBase(ImageEffect &instance)
  : ImageProcessor(instance)
  , _srcImg(NULL)
  {
    _values[0] = 1.;
    _values[1] = 1.;
    _values[2] = 1.;
    _values[3] = 1.;
  }

  void setSrcImg(const Image *v) {_srcImg = v; }

  void setValues(double r, double g, double b, double a)
  {
    _values[0] = r;
    _values[1] = g;
    _values[2] = b;
    _values[3] = a;
  }

private:
#if defined(HAVE_CUDA) || defined(HAVE_OPENCL)
  void checkGPUImages(int *width, int *height) const;
#endif
#ifdef HAVE_CUDA
  virtual void processImagesCUDA();// OVERRIDE FINAL;
#endif
#ifdef HAVE_OPENCL
  virtual void processImagesOpenCL();// OVERRIDE FINAL;
#endif
};


#if defined(HAVE_CUDA) || defined(HAVE_OPENCL)
// The kernels process the whole image, one packed float RGBA pixel per work item: the source and
// output must be packed float RGBA images of the same bounds. Other images, and a missing source,
// fail with kOfxStatGPURenderFailed and no message, so that the host renders on the CPU instead
// (ofxGPURender.h, for kOfxStatGLRenderFailed, the same value: "the host should retry the render
// with OpenGL rendering disabled").
void MultiplyProcessorBase::checkGPUImages(int *width, int *height) const
{
  if (!_srcImg || !_dstImg) {
    throwSuiteStatusException(kOfxStatGPURenderFailed);
  }
  const OfxRectI& srcBounds = _srcImg->getBounds();
  const OfxRectI& dstBounds = _dstImg->getBounds();
  const int w = dstBounds.x2 - dstBounds.x1;
  const int packedRowBytes = w * 4 * (int)sizeof(float);
  if (_dstImg->getPixelDepth() != eBitDepthFloat || _dstImg->getPixelComponents() != ePixelComponentRGBA ||
      _srcImg->getPixelDepth() != eBitDepthFloat || _srcImg->getPixelComponents() != ePixelComponentRGBA ||
      _dstImg->getRowBytes() != packedRowBytes || _srcImg->getRowBytes() != packedRowBytes ||
      dstBounds.x1 != srcBounds.x1 || dstBounds.x2 != srcBounds.x2 ||
      dstBounds.y1 != srcBounds.y1 || dstBounds.y2 != srcBounds.y2) {
    throwSuiteStatusException(kOfxStatGPURenderFailed);
  }
  *width = w;
  *height = dstBounds.y2 - dstBounds.y1;
}
#endif

#ifdef HAVE_CUDA
void MultiplyProcessorBase::processImagesCUDA()
{
  int width, height;
  checkGPUImages(&width, &height);

  const float* input = static_cast<const float*>(_srcImg->getPixelData());
  float* output = static_cast<float*>(_dstImg->getPixelData());
  const float values[4] = { (float)_values[0], (float)_values[1], (float)_values[2], (float)_values[3] };

  // we could also pass x1,y1 if the effect were pixel-dependent
  RunCUDAKernel(width, height, values, input, output);
}
#endif

#ifdef HAVE_OPENCL
void MultiplyProcessorBase::processImagesOpenCL()
{
  int width, height;
  checkGPUImages(&width, &height);

  const float* input = static_cast<const float*>(_srcImg->getPixelData());
  float* output = static_cast<float*>(_dstImg->getPixelData());
  const float values[4] = { (float)_values[0], (float)_values[1], (float)_values[2], (float)_values[3] };

  // we could also pass x1,y1 if the effect were pixel-dependent
  RunOpenCLKernel(_pOpenCLCmdQ, width, height, values, input, output);
}
#endif


template <class T>
inline
T
ofxsClamp(T v,
          int min,
          int max)
{
  if ( v < T(min) ) {
    return T(min);
  }
  if ( v > T(max) ) {
    return T(max);
  }

  return v;
}

template <typename PIX, int maxValue>
inline
PIX
ofxsClampIfInt(float v,
               int min,
               int max)
{
  if (maxValue == 1) {
    return v;
  }

  return ofxsClamp(v, min, max) * maxValue + 0.5;
}

template <class PIX, int nComponents, int maxValue>
class MultiplyProcessor
: public MultiplyProcessorBase
{
public:
  MultiplyProcessor(ImageEffect &instance)
  : MultiplyProcessorBase(instance)
  {
  }

private:

  virtual void multiThreadProcessImages(const OfxRectI& procWindow, const OfxPointD& renderScale);// OVERRIDE FINAL;
};

template <class PIX, int nComponents, int maxValue>
void
MultiplyProcessor<PIX,nComponents,maxValue>::multiThreadProcessImages(const OfxRectI& procWindow, const OfxPointD& /*renderScale*/)
{
  assert(nComponents == 1 || nComponents == 3 || nComponents == 4);
  assert(_dstImg);
  for (int y = procWindow.y1; y < procWindow.y2; y++) {
    if ( _effect.abort() ) {
      break;
    }

    PIX *dstPix = (PIX *) _dstImg->getPixelAddress(procWindow.x1, y);

    for (int x = procWindow.x1; x < procWindow.x2; x++) {
      const PIX *srcPix = (const PIX *)  (_srcImg ? _srcImg->getPixelAddress(x, y) : 0);

      for (int c = 0; c < nComponents; ++c) {
        if (srcPix) {
          // if nComponents == 1, take the alpha gain
          dstPix[c] = ofxsClampIfInt<PIX,maxValue>(srcPix[c] * _values[nComponents == 1 ? 3 : c] / maxValue, 0, 1);
        } else {
          // no source (unconnected, or outside its bounds): black and transparent
          dstPix[c] = PIX();
        }
      }
      // increment the dst pixel
      dstPix += nComponents;
    }
  }
} // process

////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class MultiplyPlugin
: public ImageEffect
{
public:
  /** @brief ctor */
  MultiplyPlugin(OfxImageEffectHandle handle)
  : ImageEffect(handle)
  , _dstClip(NULL)
  , _srcClip(NULL)
  , _value(NULL)
  , _messagePosted(false)
  {
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    assert(_dstClip);
    _srcClip = getContext() == eContextGenerator ? NULL : fetchClip(kOfxImageEffectSimpleSourceClipName);
    assert(_srcClip || getContext() == eContextGenerator);
    // (the components are checked in render() and setupAndProcess(), in all builds)
    _value = fetchRGBAParam(kParamValueName);
    assert(_value);
  }

private:
  /* Override the render */
  virtual void render(const RenderArguments &args);// OVERRIDE FINAL;

  /* set up and run a processor */
  void setupAndProcess(MultiplyProcessorBase &, const RenderArguments &args);

  /* tell the user why the render fails, then fail with that status */
  void renderError(const char *message, OfxStatus status)
  {
    setPersistentMessage(Message::eMessageError, "", message);
    _messagePosted = true;
    throwSuiteStatusException(status);
  }

  virtual bool isIdentity(const IsIdentityArguments &args, Clip * &identityClip, double &identityTime
#ifdef OFX_EXTENSIONS_NUKE
                          , int& view
                          , std::string& plane
#endif
                          );// OVERRIDE FINAL;

  /** @brief called when a clip has just been changed in some way (a rewire maybe) */
  virtual void changedClip(const InstanceChangedArgs &args, const std::string &clipName);// OVERRIDE FINAL;
  virtual void changedParam(const InstanceChangedArgs &args, const std::string &paramName);// OVERRIDE FINAL;

private:
  // do not need to delete these, the ImageEffect is managing them for us
  Clip *_dstClip;
  Clip *_srcClip;
  RGBAParam *_value;
  // an error message is shown: clear it once a render works again. Atomic, because the render is
  // fully safe (kRenderThreadSafety, eRenderFullySafe): renders of one instance may run concurrently.
  std::atomic<bool> _messagePosted;
};


////////////////////////////////////////////////////////////////////////////////
/** @brief render for the filter */

////////////////////////////////////////////////////////////////////////////////
// basic plugin render function, just a skelington to instantiate templates from

/* set up and run a processor */
void
MultiplyPlugin::setupAndProcess(MultiplyProcessorBase &processor,
                                const RenderArguments &args)
{
  const double time = args.time;

  auto_ptr<Image> dst( _dstClip->fetchImage(time) );

  // NULL means the host has no output image, an error
  if ( !dst.get() ) {
    renderError("The host gave no output image", kOfxStatFailed);
  }
  // in all builds: render() chose the processor from the clip, which reads and writes the images with
  // its number of components
  BitDepthEnum dstBitDepth    = dst->getPixelDepth();
  PixelComponentEnum dstComponents  = dst->getPixelComponents();
  if ( ( dstBitDepth != _dstClip->getPixelDepth() ) ||
      ( dstComponents != _dstClip->getPixelComponents() ) ) {
    renderError("The host gave an output image with the wrong depth or components", kOfxStatFailed);
  }
#ifndef NDEBUG
  // not checkBadRenderScaleOrField(), which rejects the eFieldNone images of DaVinci Resolve
  checkBadRenderScale(dst, args);
  if ( ( dst->getField() != eFieldNone) /* for DaVinci Resolve */ && ( dst->getField() != args.fieldToRender) ) {
    renderError("OFX Host gave image with wrong field properties", kOfxStatFailed);
  }
#endif
  auto_ptr<const Image> src( ( _srcClip && _srcClip->isConnected() ) ?
                                 _srcClip->fetchImage(time) : 0 );
  if ( src.get() ) {
#ifndef NDEBUG
    checkBadRenderScale(src, args);
    if ( ( src->getField() != eFieldNone) /* for DaVinci Resolve */ && ( src->getField() != args.fieldToRender) ) {
      renderError("OFX Host gave image with wrong field properties", kOfxStatFailed);
    }
#endif
    // in all builds: the processor reads the source with the output's number of components
    BitDepthEnum srcBitDepth      = src->getPixelDepth();
    PixelComponentEnum srcComponents = src->getPixelComponents();
    if ( (srcBitDepth != dstBitDepth) || (srcComponents != dstComponents) ) {
      renderError("The source and output images differ in depth or components", kOfxStatErrImageFormat);
    }
  }

  // the images are right: remove the message of an earlier failed render
  if ( _messagePosted.exchange(false) ) {
    clearPersistentMessage();
  }

  // set the images
  processor.setDstImg( dst.get() );
  processor.setSrcImg( src.get() );

  // Setup OpenCL and CUDA Render arguments
  processor.setGPURenderArgs(args);

  // set the render window
  processor.setRenderWindow(args.renderWindow, args.renderScale);

  double r, g, b, a;
  _value->getValueAtTime(time, r, g, b, a);
  processor.setValues(r, g, b, a);

  // Call the base class process member, this will call the derived templated process code
  processor.process();
} // MultiplyPlugin::setupAndProcess

// the overridden render function
void
MultiplyPlugin::render(const RenderArguments &args)
{
  // instantiate the render code based on the pixel depth of the dst clip
  BitDepthEnum dstBitDepth    = _dstClip->getPixelDepth();
  PixelComponentEnum dstComponents  = _dstClip->getPixelComponents();

  assert( kSupportsMultipleClipPARs   || !_srcClip || _srcClip->getPixelAspectRatio() == _dstClip->getPixelAspectRatio() );
  assert( kSupportsMultipleClipDepths || !_srcClip || _srcClip->getPixelDepth()       == _dstClip->getPixelDepth() );
  if (dstComponents == ePixelComponentRGBA) {
    switch (dstBitDepth) {
      case eBitDepthUByte: {
        MultiplyProcessor<unsigned char, 4, 255> fred(*this);
        setupAndProcess(fred, args);
        break;
      }
      case eBitDepthUShort: {
        MultiplyProcessor<unsigned short, 4, 65535> fred(*this);
        setupAndProcess(fred, args);
        break;
      }
      case eBitDepthFloat: {
        MultiplyProcessor<float, 4, 1> fred(*this);
        setupAndProcess(fred, args);
        break;
      }
      default:
        throwSuiteStatusException(kOfxStatErrUnsupported);
    }
  } else if (dstComponents == ePixelComponentAlpha) {
    switch (dstBitDepth) {
      case eBitDepthUByte: {
        MultiplyProcessor<unsigned char, 1, 255> fred(*this);
        setupAndProcess(fred, args);
        break;
      }
      case eBitDepthUShort: {
        MultiplyProcessor<unsigned short, 1, 65535> fred(*this);
        setupAndProcess(fred, args);
        break;
      }
      case eBitDepthFloat: {
        MultiplyProcessor<float, 1, 1> fred(*this);
        setupAndProcess(fred, args);
        break;
      }
      default:
        throwSuiteStatusException(kOfxStatErrUnsupported);
    }
  } else if (dstComponents == ePixelComponentRGB) {
    switch (dstBitDepth) {
      case eBitDepthUByte: {
        MultiplyProcessor<unsigned char, 3, 255> fred(*this);
        setupAndProcess(fred, args);
        break;
      }
      case eBitDepthUShort: {
        MultiplyProcessor<unsigned short, 3, 65535> fred(*this);
        setupAndProcess(fred, args);
        break;
      }
      case eBitDepthFloat: {
        MultiplyProcessor<float, 3, 1> fred(*this);
        setupAndProcess(fred, args);
        break;
      }
      default:
        throwSuiteStatusException(kOfxStatErrUnsupported);
    }
  } else {
    renderError("The output clip has unsupported components", kOfxStatErrUnsupported);
  }
} // MultiplyPlugin::render

bool
MultiplyPlugin::isIdentity(const IsIdentityArguments &args,
                           Clip * &identityClip,
                           double & /*identityTime*/
#ifdef OFX_EXTENSIONS_NUKE
                           , int& /*view*/
                           , std::string& /*plane*/
#endif
                           )
{
  const double time = args.time;

  double r, g, b, a;
  _value->getValueAtTime(time, r, g, b, a);
  if ( (r == 1.) && ( g == 1.) && ( b == 1.) && ( a == 1.) ) {
    identityClip = _srcClip;

    return true;
  }

  return false;
} // MultiplyPlugin::isIdentity

void
MultiplyPlugin::changedClip(const InstanceChangedArgs &args,
                            const std::string &clipName)
{
  // nothing to do
  unused(args);
  unused(clipName);
}

void
MultiplyPlugin::changedParam(const InstanceChangedArgs &args,
                             const std::string &paramName)
{
  // nothing to do
  unused(args);
  unused(paramName);
}

mDeclarePluginFactory(MultiplyPluginFactory, {}, {});
void
MultiplyPluginFactory::describe(ImageEffectDescriptor &desc)
{
  // basic labels
  desc.setLabel(kPluginName);
  desc.setPluginGrouping(kPluginGrouping);
  desc.setPluginDescription(kPluginDescription);

  desc.addSupportedContext(eContextFilter);
  desc.addSupportedContext(eContextGeneral);
  desc.addSupportedContext(eContextPaint);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);

  // set a few flags
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(kSupportsMultiResolution);
  desc.setSupportsTiles(kSupportsTiles);
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);
  desc.setSupportsMultipleClipDepths(kSupportsMultipleClipDepths);
  desc.setRenderThreadSafety(kRenderThreadSafety);

  // Setup OpenCL and CUDA render capability flags
#ifdef HAVE_CUDA
  desc.setSupportsCudaRender(true);
#endif
#ifdef HAVE_OPENCL
  desc.setSupportsOpenCLRender(true);
#endif
}

void
MultiplyPluginFactory::describeInContext(ImageEffectDescriptor &desc,
                                         ContextEnum context)
{
  unused(context);

  // Source clip only in the filter context
  // create the mandated source clip
  ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);

  srcClip->addSupportedComponent(ePixelComponentRGBA);
  srcClip->addSupportedComponent(ePixelComponentRGB);
  srcClip->addSupportedComponent(ePixelComponentAlpha);
  srcClip->setTemporalClipAccess(false);
  srcClip->setSupportsTiles(kSupportsTiles);
  srcClip->setIsMask(false);

  // create the mandated output clip
  ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
  dstClip->addSupportedComponent(ePixelComponentRGBA);
  dstClip->addSupportedComponent(ePixelComponentRGB);
  dstClip->addSupportedComponent(ePixelComponentAlpha);
  dstClip->setSupportsTiles(kSupportsTiles);

  // make some pages and to things in
  PageParamDescriptor *page = desc.definePageParam("Controls");

  {
    RGBAParamDescriptor *param = desc.defineRGBAParam(kParamValueName);
    param->setLabel(kParamValueLabel);
    param->setHint(kParamValueHint);
    param->setDefault(1.0, 1.0, 1.0, 1.0);
    param->setRange(-DBL_MAX, -DBL_MAX, -DBL_MAX, -DBL_MAX, DBL_MAX, DBL_MAX, DBL_MAX, DBL_MAX); // Resolve requires range and display range or values are clamped to (-1,1)
    param->setDisplayRange(0, 0, 0, 0, 4, 4, 4, 4);
    param->setAnimates(true); // can animate
    if (page) {
      page->addChild(*param);
    }
  }

} // MultiplyPluginFactory::describeInContext

ImageEffect*
MultiplyPluginFactory::createInstance(OfxImageEffectHandle handle,
                                      ContextEnum /*context*/)
{
  return new MultiplyPlugin(handle);
}

static MultiplyPluginFactory p(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor);
mRegisterPluginFactoryInstance(p)

}// namespace {
