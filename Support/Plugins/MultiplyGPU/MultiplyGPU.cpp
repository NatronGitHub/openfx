/*
 * OFX MultiplyGPU plugin.
 */

#include "MultiplyGPU.h"

#include <cmath>
#include <cstring>
#include <cfloat> // DBL_MAX
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <windows.h>
#endif

#include "ofxsProcessing.H"

using namespace OFX;
using namespace OFX::Plugin::MultiplyGPU;

namespace { // use anonymous namespace

#define kPluginName "MultiplyGPU"
#define kPluginGrouping "Color/Math"
#define kPluginDescription \
"Multiply the selected channels by a constant.\n" \
"See also: http://opticalenquiry.com/nuke/index.php?title=Multiply"

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
#ifdef HAVE_CUDA
  virtual void processImagesCUDA();// OVERRIDE FINAL;
#endif
#ifdef HAVE_OPENCL
  virtual void processImagesOpenCL();// OVERRIDE FINAL;
#endif
};


#ifdef HAVE_CUDA
void MultiplyProcessorBase::processImagesCUDA()
{
  // - all inputs and outputs must have the same size
  // - only float RGBA is supported
  // - rowBytes must be equal to width*4*sizeof(float)
  const OfxRectI& srcBounds = _srcImg->getBounds();

#ifndef NDEBUG
  const BitDepthEnum srcDepth = _srcImg->getPixelDepth();
  const PixelComponentEnum srcComponents = _srcImg->getPixelComponents();
  const int srcRowBytes = _srcImg->getRowBytes();
  const OfxRectI& dstBounds = _dstImg->getBounds();
  const BitDepthEnum dstDepth = _dstImg->getPixelDepth();
  const PixelComponentEnum dstComponents = _dstImg->getPixelComponents();
  const int dstRowBytes = _dstImg->getRowBytes();
  if (dstDepth != eBitDepthFloat || dstComponents != ePixelComponentRGBA ||
      dstRowBytes != (dstBounds.x2 - dstBounds.x1) * _dstImg->getPixelBytes() ) {
    throwSuiteStatusException(kOfxStatErrFormat);
  }
  if (dstDepth != srcDepth || dstComponents != srcComponents ||
      dstRowBytes != srcRowBytes ||
      dstBounds.x1 != srcBounds.x1 || dstBounds.x2 != srcBounds.x2 ||
      dstBounds.y1 != srcBounds.y1 || dstBounds.y2 != srcBounds.y2) {
    throwSuiteStatusException(kOfxStatErrFormat);
  }
#endif

  const int width = srcBounds.x2 - srcBounds.x1;
  const int height = srcBounds.y2 - srcBounds.y1;

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
  // - all inputs and outputs must have the same size
  // - only float RGBA is supported
  // - rowBytes must be equal to width*4*sizeof(float)
  const OfxRectI& srcBounds = _srcImg->getBounds();

#ifndef NDEBUG
  const BitDepthEnum srcDepth = _srcImg->getPixelDepth();
  const PixelComponentEnum srcComponents = _srcImg->getPixelComponents();
  const int srcRowBytes = _srcImg->getRowBytes();
  const OfxRectI& dstBounds = _dstImg->getBounds();
  const BitDepthEnum dstDepth = _dstImg->getPixelDepth();
  const PixelComponentEnum dstComponents = _dstImg->getPixelComponents();
  const int dstRowBytes = _dstImg->getRowBytes();
  if (dstDepth != eBitDepthFloat || dstComponents != ePixelComponentRGBA ||
      dstRowBytes != (dstBounds.x2 - dstBounds.x1) * _dstImg->getPixelBytes() ) {
    throwSuiteStatusException(kOfxStatErrFormat);
  }
  if (dstDepth != srcDepth || dstComponents != srcComponents ||
      dstRowBytes != srcRowBytes ||
      dstBounds.x1 != srcBounds.x1 || dstBounds.x2 != srcBounds.x2 ||
      dstBounds.y1 != srcBounds.y1 || dstBounds.y2 != srcBounds.y2) {
    throwSuiteStatusException(kOfxStatErrFormat);
  }
#endif

  const int width = srcBounds.x2 - srcBounds.x1;
  const int height = srcBounds.y2 - srcBounds.y1;

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
  {
    _dstClip = fetchClip(kOfxImageEffectOutputClipName);
    assert( _dstClip && (!_dstClip->isConnected() || _dstClip->getPixelComponents() == ePixelComponentRGB ||
                         _dstClip->getPixelComponents() == ePixelComponentRGBA) );
    _srcClip = getContext() == eContextGenerator ? NULL : fetchClip(kOfxImageEffectSimpleSourceClipName);
    assert( (!_srcClip && getContext() == eContextGenerator) ||
           ( _srcClip && (!_srcClip->isConnected() || _srcClip->getPixelComponents() ==  ePixelComponentRGB ||
                          _srcClip->getPixelComponents() == ePixelComponentRGBA) ) );
    _value = fetchRGBAParam(kParamValueName);
    assert(_value);
  }

private:
  /* Override the render */
  virtual void render(const RenderArguments &args);// OVERRIDE FINAL;

  /* set up and run a processor */
  void setupAndProcess(MultiplyProcessorBase &, const RenderArguments &args);

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

  if ( !dst.get() ) {
    throwSuiteStatusException(kOfxStatFailed);
  }
#ifndef NDEBUG
  BitDepthEnum dstBitDepth    = dst->getPixelDepth();
  PixelComponentEnum dstComponents  = dst->getPixelComponents();
  if ( ( dstBitDepth != _dstClip->getPixelDepth() ) ||
      ( dstComponents != _dstClip->getPixelComponents() ) ) {
    setPersistentMessage(Message::eMessageError, "", "OFX Host gave image with wrong depth or components");
    throwSuiteStatusException(kOfxStatFailed);
  }
  checkBadRenderScale(dst, args);
  if ( ( dst->getField() != eFieldNone) /* for DaVinci Resolve */ && ( dst->getField() != args.fieldToRender) ) {
    setPersistentMessage(Message::eMessageError, "", "OFX Host gave image with wrong field properties");
    throwSuiteStatusException(kOfxStatFailed);
  }
#endif
  auto_ptr<const Image> src( ( _srcClip && _srcClip->isConnected() ) ?
                                 _srcClip->fetchImage(time) : 0 );
#ifndef NDEBUG
  if ( src.get() ) {
    checkBadRenderScale(src, args);
    if ( ( src->getField() != eFieldNone) /* for DaVinci Resolve */ && ( src->getField() != args.fieldToRender) ) {
      setPersistentMessage(Message::eMessageError, "", "OFX Host gave image with wrong field properties");
      throwSuiteStatusException(kOfxStatFailed);
    }
    BitDepthEnum srcBitDepth      = src->getPixelDepth();
    PixelComponentEnum srcComponents = src->getPixelComponents();
    if ( (srcBitDepth != dstBitDepth) || (srcComponents != dstComponents) ) {
      throwSuiteStatusException(kOfxStatErrImageFormat);
    }
  }
#endif

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
  assert(dstComponents == ePixelComponentAlpha || dstComponents == ePixelComponentRGB || dstComponents == ePixelComponentRGBA);
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
  } else {
    assert(dstComponents == ePixelComponentRGB);
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
