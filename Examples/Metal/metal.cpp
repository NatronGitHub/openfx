/*
Software License :

Copyright (c) 2012, The Open Effects Association Ltd. All rights reserved.

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


/*
   Metal render (OFX 1.5, ofxGPURender.h), with the raw C API. macOS only.

   A gain on the R, G and B channels of float RGBA images. The host decides, for each render
   action, whether the images are Metal buffers (kOfxImageEffectPropMetalEnabled=1) or CPU
   memory. The plug-in renders on the CPU when Metal is not enabled, also on an OFX 1.4 host,
   which has none of the Metal render properties.

   This file is plain C++: the Metal kernel and its encoding are in kernel.mm (Objective-C++).
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <stddef.h>

#include "ofxImageEffect.h"
#include "ofxGPURender.h" // OFX 1.5 GPU render: kOfxImageEffectPropMetal*, kOfxStatGPU*
#include "ofxMemory.h"
#include "ofxMultiThread.h"

#include "ofxUtilities.H" // example support utils

// pointers64 to various bits of the host
OfxHost               *gHost;
OfxImageEffectSuiteV1 *gEffectHost = 0;
OfxPropertySuiteV1    *gPropHost = 0;
OfxParameterSuiteV1   *gParamHost = 0;
OfxMemorySuiteV1      *gMemoryHost = 0;
OfxMultiThreadSuiteV1 *gThreadHost = 0;
OfxMessageSuiteV1     *gMessageSuite = 0;
OfxInteractSuiteV1    *gInteractHost = 0;

// kernel.mm: the Metal compute pipelines, one per MTLDevice, built on first use and released at
// unload. gKernelMutex (an OFX mutex) protects them: renders hold it when calling RunMetalKernel
// (ReleaseMetalKernels is called at unload, when no render runs).
// RunMetalKernel encodes the kernel on p_CmdQ (an id<MTLCommandQueue>) and commits it, without
// waiting; the images are id<MTLBuffer>. Returns 0 (success) or 1 (failed).
extern int RunMetalKernel(void *p_CmdQ, int p_Width, int p_Height, const float *p_Gain,
                          const void *p_Input, int p_InOffset, int p_InRow,
                          void *p_Output, int p_OutOffset, int p_OutRow);
extern void ReleaseMetalKernels(void);
static OfxMutexHandle gKernelMutex = 0;

#define DPRINT(args) print_dbg args
static void print_dbg(const char *fmt, ...)
{
#ifdef DEBUG
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fflush(stderr);
#else
  (void)fmt;
#endif
}

// private instance data type
struct MyInstanceData {
  // handles to the clips we deal with
  OfxImageClipHandle sourceClip;
  OfxImageClipHandle outputClip;

  // handles to our parameters
  OfxParamHandle rGainParam;
  OfxParamHandle gGainParam;
  OfxParamHandle bGainParam;
};

// Convenience wrapper to get private data
static MyInstanceData *
getMyInstanceData( OfxImageEffectHandle effect)
{
  return (MyInstanceData *) ofxuGetEffectInstanceData(effect);
}

/** @brief Called at load */
static OfxStatus
onLoad(void)
{
  // fetch the host suites, and make the mutex of the kernel cache
  OfxStatus stat = ofxuFetchHostSuites();
  if(stat != kOfxStatOK)
    return stat;
  return gThreadHost->mutexCreate(&gKernelMutex, 0);
}

/** @brief Called before unload */
static OfxStatus
onUnLoad(void)
{
  // no render action runs during the unload action: no lock
  ReleaseMetalKernels();
  if(gThreadHost && gKernelMutex)
    gThreadHost->mutexDestroy(gKernelMutex);
  gKernelMutex = 0;
  return kOfxStatOK;
}

//  instance construction
static OfxStatus
createInstance( OfxImageEffectHandle effect)
{
  // get a pointer to the effect's parameter set
  OfxParamSetHandle paramSet;
  gEffectHost->getParamSet(effect, &paramSet);

  // make my private instance data
  MyInstanceData *myData = new MyInstanceData;

  // cache away our param handles
  gParamHost->paramGetHandle(paramSet, "R Gain", &myData->rGainParam, 0);
  gParamHost->paramGetHandle(paramSet, "G Gain", &myData->gGainParam, 0);
  gParamHost->paramGetHandle(paramSet, "B Gain", &myData->bGainParam, 0);

  // cache away our clip handles
  gEffectHost->clipGetHandle(effect, kOfxImageEffectSimpleSourceClipName, &myData->sourceClip, 0);
  gEffectHost->clipGetHandle(effect, kOfxImageEffectOutputClipName, &myData->outputClip, 0);

  // set my private instance data
  ofxuSetEffectInstanceData(effect, (void *) myData);

  return kOfxStatOK;
}

// instance destruction
static OfxStatus
destroyInstance( OfxImageEffectHandle  effect)
{
  // get my instance data, and delete it
  delete getMyInstanceData(effect);
  return kOfxStatOK;
}

// tells the host what region we are capable of filling
static OfxStatus
getSpatialRoD( OfxImageEffectHandle  effect,  OfxPropertySetHandle inArgs,  OfxPropertySetHandle outArgs)
{
  // retrieve any instance data associated with this effect
  MyInstanceData *myData = getMyInstanceData(effect);

  OfxTime time;
  gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);

  // my RoD is the same as my input's
  OfxRectD rod;
  gEffectHost->clipGetRegionOfDefinition(myData->sourceClip, time, &rod);

  // set the rod in the out args
  gPropHost->propSetDoubleN(outArgs, kOfxImageEffectPropRegionOfDefinition, 4, &rod.x1);

  return kOfxStatOK;
}

// tells the host how much of the input we need to fill the given window
static OfxStatus
getSpatialRoI( OfxImageEffectHandle  /*effect*/,  OfxPropertySetHandle inArgs,  OfxPropertySetHandle outArgs)
{
  // get the RoI the effect is interested in from inArgs
  OfxRectD roi;
  gPropHost->propGetDoubleN(inArgs, kOfxImageEffectPropRegionOfInterest, 4, &roi.x1);

  // the input needed is the same as the output, so set that on the source clip
  gPropHost->propSetDoubleN(outArgs, "OfxImageClipPropRoI_Source", 4, &roi.x1);

  return kOfxStatOK;
}

// Tells the host how many frames we can fill, only called in the general context.
// This is actually redundant as this is the default behaviour, but for illustrative
// purposes.
static OfxStatus
getTemporalDomain( OfxImageEffectHandle  effect,  OfxPropertySetHandle /*inArgs*/,  OfxPropertySetHandle outArgs)
{
  MyInstanceData *myData = getMyInstanceData(effect);

  double sourceRange[2];

  // get the frame range of the source clip
  OfxPropertySetHandle props; gEffectHost->clipGetPropertySet(myData->sourceClip, &props);
  gPropHost->propGetDoubleN(props, kOfxImageEffectPropFrameRange, 2, sourceRange);

  // set it on the out args
  gPropHost->propSetDoubleN(outArgs, kOfxImageEffectPropFrameRange, 2, sourceRange);

  return kOfxStatOK;
}

// the gains at a time
static void
getGains(MyInstanceData *myData, OfxTime time, float gain[3])
{
  double rGain = 1, gGain = 1, bGain = 1;
  gParamHost->paramGetValueAtTime(myData->rGainParam, time, &rGain);
  gParamHost->paramGetValueAtTime(myData->gGainParam, time, &gGain);
  gParamHost->paramGetValueAtTime(myData->bGainParam, time, &bGain);
  gain[0] = (float)rGain;
  gain[1] = (float)gGain;
  gain[2] = (float)bGain;
}

// are the settings of the effect performing an identity operation
static OfxStatus
isIdentity( OfxImageEffectHandle  effect,
	    OfxPropertySetHandle inArgs,
	    OfxPropertySetHandle outArgs)
{
  OfxTime time = ofxuGetTime(inArgs);
  float gain[3];
  getGains(getMyInstanceData(effect), time, gain);
  if(gain[0] == 1.f && gain[1] == 1.f && gain[2] == 1.f) {
    // the output is the source image
    gPropHost->propSetString(outArgs, kOfxPropName, 0, kOfxImageEffectSimpleSourceClipName);
    return kOfxStatOK;
  }
  // render
  return kOfxStatReplyDefault;
}

////////////////////////////////////////////////////////////////////////////////
// function called when the instance has been changed by anything
static OfxStatus
instanceChanged( OfxImageEffectHandle  /*effect*/,
		 OfxPropertySetHandle /*inArgs*/,
		 OfxPropertySetHandle /*outArgs*/)
{
  // don't trap any others
  return kOfxStatReplyDefault;
}

////////////////////////////////////////////////////////////////////////////////
// rendering routines

// an image fetched from a clip
struct Image {
  OfxPropertySetHandle props; // NULL if there is no image
  void *data;                 // kOfxImagePropData: CPU memory, or an id<MTLBuffer> if Metal is enabled
  OfxRectI bounds;
  int rowBytes;
  bool isFloatRGBA;
};

// fetch the image of a clip at a time (props is NULL if there is none); release it with releaseImage
static void
fetchImage(OfxImageClipHandle clip, OfxTime time, Image &img)
{
  memset(&img, 0, sizeof(img));
  if(gEffectHost->clipGetImage(clip, time, NULL, &img.props) != kOfxStatOK) {
    img.props = NULL;
    return;
  }
  const char *components = NULL;
  gPropHost->propGetString(img.props, kOfxImageEffectPropComponents, 0, &components);
  img.data = ofxuGetImageData(img.props);
  img.bounds = ofxuGetImageBounds(img.props);
  img.rowBytes = ofxuGetImageRowBytes(img.props);
  img.isFloatRGBA = (ofxuGetImagePixelDepth(img.props) == 32 &&
                     components && strcmp(components, kOfxImageComponentRGBA) == 0);
}

static void
releaseImage(Image &img)
{
  if(img.props)
    gEffectHost->clipReleaseImage(img.props);
  img.props = NULL;
}

// the address of the float RGBA pixel (x, y) of a CPU image, NULL outside its bounds
static float *
pixelAddress(const Image &img, int x, int y)
{
  if(!img.data || x < img.bounds.x1 || x >= img.bounds.x2 || y < img.bounds.y1 || y >= img.bounds.y2)
    return NULL;
  return (float *) ((char *) img.data + (ptrdiff_t)(y - img.bounds.y1) * img.rowBytes
                    + (ptrdiff_t)(x - img.bounds.x1) * 4 * (ptrdiff_t)sizeof(float));
}

// CPU render: the render window of dst, black and transparent where there is no source pixel
static OfxStatus
renderCPU(OfxImageEffectHandle instance, const OfxRectI &window, const Image &src, const Image &dst,
          const float gain[3])
{
  for(int y = window.y1; y < window.y2; ++y) {
    if(gEffectHost->abort(instance))
      break;
    float *dstPix = pixelAddress(dst, window.x1, y);
    for(int x = window.x1; x < window.x2; ++x, dstPix += 4) {
      const float *srcPix = pixelAddress(src, x, y);
      if(srcPix) {
        dstPix[0] = srcPix[0] * gain[0];
        dstPix[1] = srcPix[1] * gain[1];
        dstPix[2] = srcPix[2] * gain[2];
        dstPix[3] = srcPix[3];
      }
      else {
        dstPix[0] = dstPix[1] = dstPix[2] = dstPix[3] = 0.f;
      }
    }
  }
  return kOfxStatOK;
}

// The layout of the render window in a device image, for the kernel: the offset of its first
// pixel and the row length, in floats. The kernel reads or writes the float RGBA pixels of the
// window from there: the image must cover the window, with positive row bytes (a multiple of
// sizeof(float)) of at least a row of its bounds, and the indices must fit in an int.
static bool
deviceLayout(const Image &img, const OfxRectI &window, int &offset, int &rowFloats)
{
  if(!img.props || !img.data || !img.isFloatRGBA)
    return false;
  const OfxRectI &b = img.bounds;
  if(window.x1 < b.x1 || window.x2 > b.x2 || window.y1 < b.y1 || window.y2 > b.y2)
    return false;
  if(img.rowBytes <= 0 || img.rowBytes % (int)sizeof(float) != 0 ||
     img.rowBytes / (int)sizeof(float) < (b.x2 - b.x1) * 4)
    return false;
  rowFloats = img.rowBytes / (int)sizeof(float);
  const long long first = (long long)(window.y1 - b.y1) * rowFloats + (long long)(window.x1 - b.x1) * 4;
  const long long end = first + (long long)(window.y2 - window.y1 - 1) * rowFloats + (long long)(window.x2 - window.x1) * 4;
  if(end > INT_MAX)
    return false;
  offset = (int)first;
  return true;
}

// Metal render: src and dst data are the host's Metal buffers (id<MTLBuffer>); the kernel is
// encoded on the host's command queue, and committed without waiting: no buffer or copy of the
// plug-in's own. Images it can't process (a missing source, other layouts) fail with
// kOfxStatGPURenderFailed, as Metal errors do, so that the host may render on the CPU instead.
// ofxGPURender.h: "If a plug-in returns kOfxStatGLRenderFailed [== kOfxStatGPURenderFailed], the
// host should retry the render with OpenGL rendering disabled".
static OfxStatus
renderMetal(void *queue, const OfxRectI &window, const Image &src, const Image &dst, const float gain[3])
{
  int srcOffset, srcRow, dstOffset, dstRow;
  if(!queue || !deviceLayout(src, window, srcOffset, srcRow) || !deviceLayout(dst, window, dstOffset, dstRow))
    return kOfxStatGPURenderFailed;
  gThreadHost->mutexLock(gKernelMutex);
  const int status = RunMetalKernel(queue, window.x2 - window.x1, window.y2 - window.y1, gain,
                                    src.data, srcOffset, srcRow, dst.data, dstOffset, dstRow);
  gThreadHost->mutexUnLock(gKernelMutex);
  return status == 0 ? kOfxStatOK : kOfxStatGPURenderFailed;
}

// the process code  that the host sees
static OfxStatus render( OfxImageEffectHandle  instance,
                         OfxPropertySetHandle inArgs,
                         OfxPropertySetHandle /*outArgs*/)
{
  // get the render window and the time from the inArgs
  OfxTime time;
  OfxRectI renderWindow;
  gPropHost->propGetDouble(inArgs, kOfxPropTime, 0, &time);
  gPropHost->propGetIntN(inArgs, kOfxImageEffectPropRenderWindow, 4, &renderWindow.x1);

  // Retrieve instance data associated with this effect
  MyInstanceData *myData = getMyInstanceData(instance);

  // ofxGPURender.h, kOfxImageEffectPropMetalEnabled: "1 indicates that the kOfxImagePropData of
  // each image of each clip is a Metal id<MTLBuffer>", 0 that it "is a CPU memory pointer, unless
  // kOfxImageEffectPropMetalTextureEnabled is 1" (Metal textures, which this plug-in doesn't
  // declare: kOfxImageEffectPropMetalTextureSupported). It is read whatever the host descriptor
  // says, so that the images are never taken for CPU memory when the host passes Metal buffers.
  // An OFX 1.4 host doesn't set it: CPU memory.
  int metalEnabled = 0;
  if(gPropHost->propGetInt(inArgs, kOfxImageEffectPropMetalEnabled, 0, &metalEnabled) != kOfxStatOK)
    metalEnabled = 0;

  // kOfxImageEffectPropMetalTextureEnabled: "1 indicates that kOfxImagePropData of each image of
  // each clip is a Metal id<MTLTexture>". The host "MUST set at most one of the two enabled
  // properties, and only for a path that both host and plug-in declared as supported", so it
  // shouldn't happen here; if it does, the plug-in can't read the images: let the host render on
  // the CPU.
  int metalTextureEnabled = 0;
  if(gPropHost->propGetInt(inArgs, kOfxImageEffectPropMetalTextureEnabled, 0, &metalTextureEnabled) == kOfxStatOK &&
     metalTextureEnabled)
    return kOfxStatGPURenderFailed;

  // kOfxImageEffectPropMetalCommandQueue: "the command queue to be used for Metal rendering
  // (id<MTLCommandQueue>)"; "The plug-in SHOULD ensure that its render action enqueues any
  // asynchronous Metal operations onto the supplied queue."
  void *queue = NULL;
  if(metalEnabled &&
     gPropHost->propGetPointer(inArgs, kOfxImageEffectPropMetalCommandQueue, 0, &queue) != kOfxStatOK)
    queue = NULL;

  DPRINT(("metal: render window [%d, %d - %d, %d], Metal %s, queue %p\n",
          renderWindow.x1, renderWindow.y1, renderWindow.x2, renderWindow.y2,
          metalEnabled ? "enabled" : "disabled", queue));

  float gain[3];
  getGains(myData, time, gain);

  // get the images (there is no source image if the clip is not connected)
  Image dst, src;
  fetchImage(myData->outputClip, time, dst);
  if(!dst.props)
    return kOfxStatFailed;
  fetchImage(myData->sourceClip, time, src);

  // render the part of the window that is in the output image
  OfxRectI window = renderWindow;
  if(window.x1 < dst.bounds.x1) window.x1 = dst.bounds.x1;
  if(window.x2 > dst.bounds.x2) window.x2 = dst.bounds.x2;
  if(window.y1 < dst.bounds.y1) window.y1 = dst.bounds.y1;
  if(window.y2 > dst.bounds.y2) window.y2 = dst.bounds.y2;

  OfxStatus status = kOfxStatOK;
  if(window.x1 >= window.x2 || window.y1 >= window.y2) {
    // empty window or image: nothing to do
  }
  else if(metalEnabled) {
    status = (src.props ? renderMetal(queue, window, src, dst, gain) : kOfxStatGPURenderFailed);
  }
  else if(!dst.data || !dst.isFloatRGBA || (src.props && (!src.data || !src.isFloatRGBA))) {
    status = kOfxStatErrImageFormat; // only float RGBA is declared in describe
  }
  else {
    status = renderCPU(instance, window, src, dst, gain);
  }

  releaseImage(src);
  releaseImage(dst);
  return status;
}

// convience function to define parameters
static void
defineParam( OfxParamSetHandle effectParams,
	     const char *name,
	     const char *label,
	     const char *scriptName,
	     const char *hint,
	     const char *parent)
{
  OfxPropertySetHandle props;
  gParamHost->paramDefine(effectParams, kOfxParamTypeDouble, name, &props);

  // say we are a scaling parameter
  gPropHost->propSetString(props, kOfxParamPropDoubleType, 0, kOfxParamDoubleTypeScale);
  gPropHost->propSetDouble(props, kOfxParamPropDefault, 0, 1.0);
  gPropHost->propSetDouble(props, kOfxParamPropMin, 0, 0.01);
  gPropHost->propSetDouble(props, kOfxParamPropDisplayMin, 0, 0.01);
  gPropHost->propSetDouble(props, kOfxParamPropDisplayMax, 0, 2.0);
  gPropHost->propSetDouble(props, kOfxParamPropIncrement, 0, 0.01);
  gPropHost->propSetString(props, kOfxParamPropHint, 0, hint);
  gPropHost->propSetString(props, kOfxParamPropScriptName, 0, scriptName);
  gPropHost->propSetString(props, kOfxPropLabel, 0, label);
  if(parent)
    gPropHost->propSetString(props, kOfxParamPropParent, 0, parent);
}

//  describe the plugin in context
static OfxStatus
describeInContext( OfxImageEffectHandle  effect,  OfxPropertySetHandle /*inArgs*/)
{
  OfxPropertySetHandle props;
  // define the single output clip in both contexts
  gEffectHost->clipDefine(effect, kOfxImageEffectOutputClipName, &props);

  // set the component types we can handle on out output
  gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);

  // define the single source clip in both contexts
  gEffectHost->clipDefine(effect, kOfxImageEffectSimpleSourceClipName, &props);

  // set the component types we can handle on our main input
  gPropHost->propSetString(props, kOfxImageEffectPropSupportedComponents, 0, kOfxImageComponentRGBA);

  ////////////////////////////////////////////////////////////////////////////////
  // define the parameters for this context
  // fetch the parameter set from the effect
  OfxParamSetHandle paramSet;
  gEffectHost->getParamSet(effect, &paramSet);

  defineParam(paramSet, "R Gain", "R Gain", "R Gain",
          "Red Gain", 0);
  defineParam(paramSet, "G Gain", "G Gain", "G Gain",
          "Green Gain", 0);
  defineParam(paramSet, "B Gain", "B Gain", "B Gain",
          "Blue Gain", 0);

  // make a page of controls and add my parameters to it
  gParamHost->paramDefine(paramSet, kOfxParamTypePage, "Main", &props);
  gPropHost->propSetString(props, kOfxParamPropPageChild, 0, "R Gain");
  gPropHost->propSetString(props, kOfxParamPropPageChild, 1, "G Gain");
  gPropHost->propSetString(props, kOfxParamPropPageChild, 2, "B Gain");
  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// the plugin's description routine
static OfxStatus
describe(OfxImageEffectHandle  effect)
{
  // get the property handle for the plugin
  OfxPropertySetHandle effectProps;
  gEffectHost->getPropertySet(effect, &effectProps);

  // We can render both fields in a fielded images in one hit if there is no animation
  // So set the flag that allows us to do this
  gPropHost->propSetInt(effectProps, kOfxImageEffectPluginPropFieldRenderTwiceAlways, 0, 0);

  // the CPU and Metal renders process float RGBA images
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);

  // set some labels and the group it belongs to
  gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "Simple Gain (Metal)");
  gPropHost->propSetString(effectProps, kOfxImageEffectPluginPropGrouping, 0, "OFX Example (GPU)");

  // define the contexts we can be used in
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);

  // We support Metal buffer render, and render on the CPU otherwise: "true" (ofxGPURender.h: "the
  // host or plug-in can support Metal buffer render"; "needed" would say that it can't render
  // without Metal). Not Metal textures (kOfxImageEffectPropMetalTextureSupported, "false" by
  // default): the host passes buffers or CPU memory.
  // An OFX 1.4 host may not know this property (kOfxStatErrUnknown): the status is ignored, the
  // plug-in renders on the CPU there.
  gPropHost->propSetString(effectProps, kOfxImageEffectPropMetalRenderSupported, 0, "true");

  return kOfxStatOK;
}

////////////////////////////////////////////////////////////////////////////////
// The main function
static OfxStatus
pluginMain(const char *action,  const void *handle, OfxPropertySetHandle inArgs,  OfxPropertySetHandle outArgs)
{
  // cast to appropriate type
  OfxImageEffectHandle effect = (OfxImageEffectHandle) handle;

  if(strcmp(action, kOfxActionDescribe) == 0) {
    return describe(effect);
  }
  else if(strcmp(action, kOfxImageEffectActionDescribeInContext) == 0) {
    return describeInContext(effect, inArgs);
  }
  else if(strcmp(action, kOfxActionLoad) == 0) {
    return onLoad();
  }
  else if(strcmp(action, kOfxActionUnload) == 0) {
    return onUnLoad();
  }
  else if(strcmp(action, kOfxActionCreateInstance) == 0) {
    return createInstance(effect);
  }
  else if(strcmp(action, kOfxActionDestroyInstance) == 0) {
    return destroyInstance(effect);
  }
  else if(strcmp(action, kOfxImageEffectActionIsIdentity) == 0) {
    return isIdentity(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxImageEffectActionRender) == 0) {
    return render(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxImageEffectActionGetRegionOfDefinition) == 0) {
    return getSpatialRoD(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxImageEffectActionGetRegionsOfInterest) == 0) {
    return getSpatialRoI(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxActionInstanceChanged) == 0) {
    return instanceChanged(effect, inArgs, outArgs);
  }
  else if(strcmp(action, kOfxImageEffectActionGetTimeDomain) == 0) {
    return getTemporalDomain(effect, inArgs, outArgs);
  }


  // other actions to take the default value
  return kOfxStatReplyDefault;
}

// function to set the host structure
static void
setHostFunc(OfxHost *hostStruct)
{
  gHost         = hostStruct;
}

////////////////////////////////////////////////////////////////////////////////
// the plugin struct
static OfxPlugin basicPlugin =
{
  kOfxImageEffectPluginApi,
  1,
  "net.sf.openfx.SimpleGainMetalPlugin",
  1,
  0,
  setHostFunc,
  pluginMain
};

// the two mandated functions
OfxExport OfxPlugin *
OfxGetPlugin(int nth)
{
  if(nth == 0)
    return &basicPlugin;
  return 0;
}

OfxExport int
OfxGetNumberOfPlugins(void)
{
  return 1;
}
