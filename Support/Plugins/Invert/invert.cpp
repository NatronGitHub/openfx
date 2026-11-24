/*
OFX Invert Example plugin, a plugin that illustrates the use of the OFX Support library.

Copyright (C) 2007 The Open Effects Association Ltd
Author Bruno Nicoletti bruno@thefoundry.co.uk

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

The Open Effects Association Ltd
1 Wardour St
London W1D 6PA
England


*/

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <windows.h>
#endif

#include <stdio.h>
#include <limits> // numeric_limits (DBL_MAX is missing with some C++20 toolchains)
#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"

#include "ofxsProcessing.H"


// Base class for the RGBA and the Alpha processor
class InvertBase : public OFX::ImageProcessor {
protected :
  OFX::Image *_srcImg;
public :
  /** @brief no arg ctor */
  InvertBase(OFX::ImageEffect &instance)
    : OFX::ImageProcessor(instance)
    , _srcImg(NULL)
  {        
  }

  /** @brief set the src image */
  void setSrcImg(OFX::Image *v) {_srcImg = v;}
};

// template to do the RGBA processing
template <class PIX, int nComponents, int max>
class ImageInverter : public InvertBase {
public :
  // ctor
  ImageInverter(OFX::ImageEffect &instance) 
    : InvertBase(instance)
  {}

  // and do some processing
  void multiThreadProcessImages(const OfxRectI& procWindow, const OfxPointD& renderScale)
  {
    for(int y = procWindow.y1; y < procWindow.y2; y++) {
      if(_effect.abort()) break;

      PIX *dstPix = (PIX *) _dstImg->getPixelAddress(procWindow.x1, y);

      for(int x = procWindow.x1; x < procWindow.x2; x++) {

        PIX *srcPix = (PIX *)  (_srcImg ? _srcImg->getPixelAddress(x, y) : 0);

        // do we have a source image to scale up
        if(srcPix) {
          for(int c = 0; c < nComponents; c++) {
            dstPix[c] = max - srcPix[c];
          }
        }
        else {
          // no src pixel here, be black and transparent
          for(int c = 0; c < nComponents; c++) {
            dstPix[c] = 0;
          }
        }

        // increment the dst pixel
        dstPix += nComponents;
      }
    }
  }
};

////////////////////////////////////////////////////////////////////////////////
// Two parameters to test hosts (a real plug-in wouldn't have them):
// - supportsTiles, unchecked: the instance doesn't support tiles (kOfxImageEffectPropSupportsTiles=0);
// - testSmallRoI, checked: the plug-in asks for the centre of the region it needs only, so the
//   output is right only if the host gives it whole-RoD images (as it must without tiles).
#define kParamSupportsTiles "supportsTiles"
#define kParamTestSmallRoI "testSmallRoI"

/** @brief The plugin that does our work */
class InvertPlugin : public OFX::ImageEffect {
protected :
  // do not need to delete these, the ImageEffect is managing them for us
  OFX::Clip *dstClip_;
  OFX::Clip *srcClip_;
  OFX::BooleanParam *supportsTiles_;
  OFX::BooleanParam *testSmallRoI_;
  // an error message is shown: clear it once a render works again. Render calls on one instance
  // aren't concurrent (the default render thread safety, instance safe), so it needs no lock.
  bool messagePosted_;

public :
  /** @brief ctor */
  InvertPlugin(OfxImageEffectHandle handle)
    : ImageEffect(handle)
    , dstClip_(NULL)
    , srcClip_(NULL)
    , supportsTiles_(NULL)
    , testSmallRoI_(NULL)
    , messagePosted_(false)
  {
    dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
    srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
    supportsTiles_ = fetchBooleanParam(kParamSupportsTiles);
    testSmallRoI_ = fetchBooleanParam(kParamTestSmallRoI);
    // the instance's value of kOfxImageEffectPropSupportsTiles follows the parameter. OFX 1.4
    // defines changing it in instanceChanged; setting it here, in createInstance, works with
    // HostSupport and Natron
    setSupportsTiles(supportsTiles_->getValue());
  }

  /* testSmallRoI: the centre half of the region needed only */
  virtual void getRegionsOfInterest(const OFX::RegionsOfInterestArguments &args, OFX::RegionOfInterestSetter &rois)
  {
    if (!testSmallRoI_->getValue()) {
      return; // the default: the region needed
    }
    const OfxRectD& r = args.regionOfInterest;
    const double dblMax = std::numeric_limits<double>::max();
    // the bounds first (bit tests, which -ffast-math doesn't remove): a NaN or infinite bound
    // (if x1 == x2 == inf, x2 - x1 is NaN); then the sizes, which may overflow
    const double bounds[4] = { r.x1, r.y1, r.x2, r.y2 };
    for (int i = 0; i < 4; ++i) {
      if (OFX::IsNaN(bounds[i]) || OFX::IsInfinite(bounds[i])) {
        return; // no centre half: the default
      }
    }
    if (r.x2 - r.x1 >= dblMax || r.y2 - r.y1 >= dblMax) {
      return; // an infinite RoI has no centre half (and inf - inf is NaN): the default
    }
    const double dx = (r.x2 - r.x1) / 4, dy = (r.y2 - r.y1) / 4;
    OfxRectD roi = { r.x1 + dx, r.y1 + dy, r.x2 - dx, r.y2 - dy };
    rois.setRegionOfInterest(*srcClip_, roi);
  }

  /* the instance supports tiles if the parameter is checked */
  virtual void changedParam(const OFX::InstanceChangedArgs &/*args*/, const std::string &paramName)
  {
    if (paramName == kParamSupportsTiles) {
      setSupportsTiles(supportsTiles_->getValue());
      // also a test of the host's keyframe functions on a parameter without keys (a real plug-in
      // wouldn't do this): ofxParam.h allows only kOfxStatOK or kOfxStatErrBadHandle for these
      // (Support throws on any other status)
      if (supportsTiles_->getNumKeys() != 0) {
        OFX::throwSuiteStatusException(kOfxStatFailed);
      }
      supportsTiles_->deleteAllKeys();
    }
  }

  /* Override the render */
  virtual void render(const OFX::RenderArguments &args);

  /* tell the user why the render fails, then fail with that status */
  void renderError(const char *message, OfxStatus status)
  {
    setPersistentMessage(OFX::Message::eMessageError, "", message);
    messagePosted_ = true;
    OFX::throwSuiteStatusException(status);
  }

  /* set up and run a processor */
  void setupAndProcess(InvertBase &, const OFX::RenderArguments &args);
};


////////////////////////////////////////////////////////////////////////////////
/** @brief render for the filter */

////////////////////////////////////////////////////////////////////////////////
// basic plugin render function, just a skelington to instantiate templates from


// set up the images, then run the processor on the render window
void
InvertPlugin::setupAndProcess(InvertBase &processor, const OFX::RenderArguments &args)
{
  // get a dst image: NULL means the host has none, an error for the output
  OFX::auto_ptr<OFX::Image> dst(dstClip_->fetchImage(args.time));
  if (!dst.get()) {
    renderError("The host gave no output image", kOfxStatFailed);
  }
  OFX::BitDepthEnum dstBitDepth       = dst->getPixelDepth();
  OFX::PixelComponentEnum dstComponents  = dst->getPixelComponents();
  // render() chose the processor from the clip: the image must match it
  if (dstBitDepth != dstClip_->getPixelDepth() || dstComponents != dstClip_->getPixelComponents()) {
    renderError("The host gave an output image with the wrong depth or components", kOfxStatFailed);
  }
  checkBadRenderScaleOrField(dst, args); // debug builds only

  // fetch main input image: NULL (no input there) renders black and transparent
  OFX::auto_ptr<OFX::Image> src(srcClip_->fetchImage(args.time));

  // make sure bit depths are sane
  if(src.get()) {
    OFX::BitDepthEnum    srcBitDepth      = src->getPixelDepth();
    OFX::PixelComponentEnum srcComponents = src->getPixelComponents();

    // see if they have the same depths and bytes and all
    if(srcBitDepth != dstBitDepth || srcComponents != dstComponents) {
      renderError("The source and output images differ in depth or components", kOfxStatErrImageFormat);
    }
    checkBadRenderScaleOrField(src, args); // debug builds only
  }

  // the images are right: remove the message of an earlier failed render
  if (messagePosted_) {
    clearPersistentMessage();
    messagePosted_ = false;
  }

  // set the images
  processor.setDstImg(dst.get());
  processor.setSrcImg(src.get());

  // set the render window
  processor.setRenderWindow(args.renderWindow, args.renderScale);

  // Call the base class process member, this will call the derived templated process code
  processor.process();
}

// the overridden render function
void
InvertPlugin::render(const OFX::RenderArguments &args)
{
  // instantiate the render code based on the pixel depth of the dst clip
  OFX::BitDepthEnum       dstBitDepth    = dstClip_->getPixelDepth();
  OFX::PixelComponentEnum dstComponents  = dstClip_->getPixelComponents();

  // do the rendering
  if(dstComponents == OFX::ePixelComponentRGBA) {
    switch(dstBitDepth) {
case OFX::eBitDepthUByte : {      
  ImageInverter<unsigned char, 4, 255> fred(*this);
  setupAndProcess(fred, args);
                           }
                           break;

case OFX::eBitDepthUShort : {
  ImageInverter<unsigned short, 4, 65535> fred(*this);
  setupAndProcess(fred, args);
                            }                          
                            break;

case OFX::eBitDepthFloat : {
  ImageInverter<float, 4, 1> fred(*this);
  setupAndProcess(fred, args);
                           }
                           break;
default :
  OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
  }
  else {
    switch(dstBitDepth) {
case OFX::eBitDepthUByte : {
  ImageInverter<unsigned char, 1, 255> fred(*this);
  setupAndProcess(fred, args);
                           }
                           break;

case OFX::eBitDepthUShort : {
  ImageInverter<unsigned short, 1, 65535> fred(*this);
  setupAndProcess(fred, args);
                            }                          
                            break;

case OFX::eBitDepthFloat : {
  ImageInverter<float, 1, 1> fred(*this);
  setupAndProcess(fred, args);
                           }                          
                           break;
default :
  OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
  } 
}

mDeclarePluginFactory(InvertExamplePluginFactory, {}, {});

using namespace OFX;
void InvertExamplePluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
  // basic labels
  desc.setLabels("Invert", "Invert", "Invert");
  desc.setPluginGrouping("OFX");

  // add the supported contexts, only filter at the moment
  desc.addSupportedContext(eContextFilter);

  // add supported pixel depths
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);

  // set a few flags
  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(true);
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);

}

void InvertExamplePluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum /*context*/)
{
  // Source clip only in the filter context
  // create the mandated source clip
  ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  srcClip->addSupportedComponent(ePixelComponentRGBA);
  srcClip->addSupportedComponent(ePixelComponentAlpha);
  srcClip->setTemporalClipAccess(false);
  srcClip->setSupportsTiles(true);
  srcClip->setIsMask(false);

  // create the mandated output clip
  ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
  dstClip->addSupportedComponent(ePixelComponentRGBA);
  dstClip->addSupportedComponent(ePixelComponentAlpha);
  dstClip->setSupportsTiles(true);

  BooleanParamDescriptor *tiles = desc.defineBooleanParam(kParamSupportsTiles);
  tiles->setLabel("Supports Tiles");
  tiles->setHint("Unchecked, the instance doesn't support tiles (kOfxImageEffectPropSupportsTiles is 0), so the host "
                 "should render the whole region of definition and give it whole-RoD input images. "
                 "A test of hosts: the result is the same.");
  tiles->setDefault(true);
  tiles->setAnimates(false);

  BooleanParamDescriptor *smallRoI = desc.defineBooleanParam(kParamTestSmallRoI);
  smallRoI->setLabel("Test: Small RoI");
  smallRoI->setHint("Checked, the plug-in asks for the centre of the region it needs only: the output is right "
                    "only where the host gives it more, e.g. everywhere if Supports Tiles is unchecked (the host "
                    "must then give whole-RoD images). A test of hosts.");
  smallRoI->setDefault(false);
  smallRoI->setAnimates(false);
}

OFX::ImageEffect* InvertExamplePluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/)
{
  return new InvertPlugin(handle);
}

static InvertExamplePluginFactory p("net.sf.openfx.invertPlugin", 1, 0);
mRegisterPluginFactoryInstance(p)
