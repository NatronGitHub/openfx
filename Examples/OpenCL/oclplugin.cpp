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
   OpenCL render (OFX 1.5, ofxGPURender.h), with the raw C API.

   A gain on the R, G and B channels of float RGBA images. The host decides, for each render
   action, whether the images are OpenCL buffers or images (kOfxImageEffectPropOpenCLEnabled=1),
   or CPU memory. The plug-in renders on the CPU when OpenCL is not enabled, also on an OFX 1.4
   host, which has none of the OpenCL render properties.
 */
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <limits.h>
#include <stddef.h>
#include <map>
#include <utility>

#ifdef __APPLE__
// OpenCL is deprecated on macOS (since 10.14), but still there
#ifndef CL_SILENCE_DEPRECATION
#define CL_SILENCE_DEPRECATION
#endif
#include <OpenCL/cl.h>
#else
// ofxGPURender.h: "Effects should target OpenCL 1.1 API and OpenCL C kernel language support."
#ifndef CL_TARGET_OPENCL_VERSION
#define CL_TARGET_OPENCL_VERSION 110
#endif
#include <CL/cl.h>
#endif

#include "ofxImageEffect.h"
#include "ofxGPURender.h" // OFX 1.5 GPU render: kOfxImageEffectPropOpenCL*, kOfxStatGPU*
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

////////////////////////////////////////////////////////////////////////////////
// the OpenCL kernels

// OpenCL buffers: one work item per pixel of the render window (width x height). Each image is
// given by its buffer, the offset of the first pixel of the window and the row length, in floats
// (see deviceLayout): float RGBA pixels, rows of increasing y.
static const char *kKernelSourceBuffers =
"__kernel void GainBuffers(int width, int height, float rGain, float gGain, float bGain,\n"
"                          __global const float *input, int inOffset, int inRow,\n"
"                          __global float *output, int outOffset, int outRow)\n"
"{\n"
"  const int x = get_global_id(0);\n"
"  const int y = get_global_id(1);\n"
"  if ((x < width) && (y < height)) {\n"
"    const int i = inOffset + y * inRow + x * 4;\n"
"    const int o = outOffset + y * outRow + x * 4;\n"
"    output[o + 0] = input[i + 0] * rGain;\n"
"    output[o + 1] = input[i + 1] * gGain;\n"
"    output[o + 2] = input[i + 2] * bGain;\n"
"    output[o + 3] = input[i + 3];\n"
"  }\n"
"}\n";

// OpenCL images: the same, with the position of the render window in each image (an image has
// the size of its bounds, its first row is the bottom one, as for buffers). read_imagef and
// write_imagef convert the image format to and from float.
static const char *kKernelSourceImages =
"__constant sampler_t sampler = CLK_NORMALIZED_COORDS_FALSE | CLK_ADDRESS_CLAMP_TO_EDGE | CLK_FILTER_NEAREST;\n"
"__kernel void GainImages(int width, int height, float rGain, float gGain, float bGain,\n"
"                         __read_only image2d_t input, int inX, int inY,\n"
"                         __write_only image2d_t output, int outX, int outY)\n"
"{\n"
"  const int x = get_global_id(0);\n"
"  const int y = get_global_id(1);\n"
"  if ((x < width) && (y < height)) {\n"
"    float4 p = read_imagef(input, sampler, (int2)(inX + x, inY + y));\n"
"    p.x *= rGain;\n"
"    p.y *= gGain;\n"
"    p.z *= bGain;\n"
"    write_imagef(output, (int2)(outX + x, outY + y), p);\n"
"  }\n"
"}\n";

// The kernels, built on first use, for each OpenCL context and kernel source. ofxGPURender.h:
// "The host may support multiple OpenCL devices. Therefore the plug-in should keep a separate set
// of kernels per OpenCL content (e.g., using a map). The OpenCL context can be found from the
// command queue using clGetCommandQueueInfo with CL_QUEUE_CONTEXT."
// They are released in the unload action. A kernel keeps a reference to its program, which keeps
// one to its context, so a context in the map stays valid (and its address unique) until then.
// The spec's "Do not retain OpenCL objects without a matching release within the render action"
// is about the host's objects: these are the plug-in's own. Failed builds are not cached (they
// are retried at the next render).
// gKernelMutex (an OFX mutex) protects the map, and the kernels: a cl_kernel is not thread-safe
// between clSetKernelArg and clEnqueueNDRangeKernel, so the lock is held through both
// (releaseKernels is called at unload, when no render runs).
typedef std::map<std::pair<cl_context, const char *>, cl_kernel> KernelMap;
static KernelMap gKernels;
static OfxMutexHandle gKernelMutex = 0;

// call with gKernelMutex locked; NULL on failure
static cl_kernel
getKernel(cl_context context, const char *source, const char *name)
{
  const KernelMap::key_type key(context, source);
  KernelMap::const_iterator it = gKernels.find(key);
  if(it != gKernels.end())
    return it->second;

  cl_int error = CL_SUCCESS;
  cl_program program = clCreateProgramWithSource(context, 1, &source, NULL, &error);
  if(error != CL_SUCCESS || !program) {
    fprintf(stderr, "oclplugin: clCreateProgramWithSource failed [%d]\n", (int)error);
    return NULL;
  }
  cl_kernel kernel = NULL;
  error = clBuildProgram(program, 0, NULL, NULL, NULL, NULL);
  if(error != CL_SUCCESS) {
    fprintf(stderr, "oclplugin: clBuildProgram failed [%d]\n", (int)error);
  }
  else {
    kernel = clCreateKernel(program, name, &error);
    if(error != CL_SUCCESS) {
      fprintf(stderr, "oclplugin: clCreateKernel failed [%d]\n", (int)error);
      kernel = NULL;
    }
  }
  clReleaseProgram(program); // the kernel keeps a reference to it
  if(kernel)
    gKernels[key] = kernel;
  return kernel;
}

static void
releaseKernels(void)
{
  for(KernelMap::iterator it = gKernels.begin(); it != gKernels.end(); ++it)
    clReleaseKernel(it->second);
  gKernels.clear();
}

// the render status of an OpenCL error
static OfxStatus
openCLStatus(cl_int error)
{
  switch(error) {
  case CL_SUCCESS:
    return kOfxStatOK;
  case CL_OUT_OF_RESOURCES:
  case CL_OUT_OF_HOST_MEMORY:
  case CL_MEM_OBJECT_ALLOCATION_FAILURE:
    return kOfxStatGPUOutOfMemory;
  default:
    return kOfxStatGPURenderFailed;
  }
}

// a kernel argument
struct KernelArg {
  size_t size;
  const void *value;
};

// set the arguments and enqueue the kernel on the host's queue, over the render window
static OfxStatus
runKernel(cl_command_queue queue, const char *source, const char *name, int width, int height,
          const KernelArg *args, cl_uint nArgs)
{
  cl_context context = NULL;
  cl_int error = clGetCommandQueueInfo(queue, CL_QUEUE_CONTEXT, sizeof(cl_context), &context, NULL);
  if(error != CL_SUCCESS || !context)
    return kOfxStatGPURenderFailed;

  OfxStatus status = kOfxStatOK;
  gThreadHost->mutexLock(gKernelMutex);
  cl_kernel kernel = getKernel(context, source, name);
  if(!kernel)
    status = kOfxStatGPURenderFailed;
  for(cl_uint i = 0; status == kOfxStatOK && i < nArgs; ++i)
    status = openCLStatus(clSetKernelArg(kernel, i, args[i].size, args[i].value));
  if(status == kOfxStatOK) {
    // one work item per pixel; the implementation chooses the work-group size. No clFinish:
    // kOfxImageEffectPropOpenCLCommandQueue, "The plug-in SHOULD NOT wait for final
    // asynchronous operations to complete before returning from the render action."
    const size_t globalWorkSize[2] = { (size_t)width, (size_t)height };
    status = openCLStatus(clEnqueueNDRangeKernel(queue, kernel, 2, NULL, globalWorkSize, NULL, 0, NULL, NULL));
  }
  gThreadHost->mutexUnLock(gKernelMutex);
  if(status != kOfxStatOK)
    fprintf(stderr, "oclplugin: the OpenCL render failed\n");
  return status;
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
  releaseKernels();
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
  void *data;                 // kOfxImagePropData: CPU memory, or an OpenCL buffer (cl_mem)
  void *clImage;              // kOfxImageEffectPropOpenCLImage: an OpenCL image (cl_mem), or NULL
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
  // only set by an OFX 1.5 host that passes OpenCL images (else the image doesn't have it)
  if(gPropHost->propGetPointer(img.props, kOfxImageEffectPropOpenCLImage, 0, &img.clImage) != kOfxStatOK)
    img.clImage = NULL;
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

// Does an OpenCL image cover the width x height window at (x0, y0), with a format that the kernel
// reads or writes (read_imagef and write_imagef convert float, half float and normalized integer
// RGBA)? The host's image is checked, rather than trusted to have the size of its bounds.
static bool
clImageCovers(cl_mem image, int x0, int y0, int width, int height)
{
  size_t w = 0, h = 0;
  cl_image_format format;
  if(clGetImageInfo(image, CL_IMAGE_WIDTH, sizeof(w), &w, NULL) != CL_SUCCESS ||
     clGetImageInfo(image, CL_IMAGE_HEIGHT, sizeof(h), &h, NULL) != CL_SUCCESS ||
     clGetImageInfo(image, CL_IMAGE_FORMAT, sizeof(format), &format, NULL) != CL_SUCCESS)
    return false;
  if(x0 < 0 || y0 < 0 || (size_t)x0 + (size_t)width > w || (size_t)y0 + (size_t)height > h)
    return false;
  if(format.image_channel_order != CL_RGBA && format.image_channel_order != CL_BGRA)
    return false;
  switch(format.image_channel_data_type) {
  case CL_FLOAT:
  case CL_HALF_FLOAT:
  case CL_UNORM_INT8:
  case CL_UNORM_INT16:
    return true;
  default:
    return false;
  }
}

// Does an OpenCL buffer hold the float RGBA pixels of the width x height window that the kernel
// reads or writes (from deviceLayout's offset and row length, in floats)?
static bool
clBufferHolds(cl_mem buffer, int offset, int rowFloats, int width, int height)
{
  size_t size = 0;
  if(clGetMemObjectInfo(buffer, CL_MEM_SIZE, sizeof(size), &size, NULL) != CL_SUCCESS)
    return false;
  const size_t end = (size_t)offset + (size_t)(height - 1) * (size_t)rowFloats + (size_t)width * 4;
  return end * sizeof(float) <= size;
}

// OpenCL render, on the host's command queue, with the host's buffers or images: no context,
// queue, buffer or copy of the plug-in's own. Images it can't process (a missing source, other
// layouts) fail with kOfxStatGPURenderFailed, as OpenCL errors do (kOfxStatGPUOutOfMemory if
// OpenCL is out of memory). ofxGPURender.h: "If a call using OpenCL rendering fails, the host may
// re-attempt using CPU buffers instead".
static OfxStatus
renderOpenCL(void *queue, const OfxRectI &window, const Image &src, const Image &dst, const float gain[3])
{
  if(!queue)
    return kOfxStatGPURenderFailed;
  cl_command_queue cmdQ = (cl_command_queue) queue;
  int width = window.x2 - window.x1;
  int height = window.y2 - window.y1;

  // "If a plug-in supports both, it must use kOfxImageEffectPropOpenCLImage to determine if
  // Images or Buffers are being used for a given render action."
  if(src.clImage || dst.clImage) {
    // OpenCL images: "OpenCL Images do not have the concept of row bytes"
    const OfxRectI &sb = src.bounds;
    const OfxRectI &db = dst.bounds;
    if(!src.clImage || !dst.clImage || !src.isFloatRGBA || !dst.isFloatRGBA ||
       window.x1 < sb.x1 || window.x2 > sb.x2 || window.y1 < sb.y1 || window.y2 > sb.y2)
      return kOfxStatGPURenderFailed;
    cl_mem input = (cl_mem) src.clImage;
    cl_mem output = (cl_mem) dst.clImage;
    int inX = window.x1 - sb.x1, inY = window.y1 - sb.y1;
    int outX = window.x1 - db.x1, outY = window.y1 - db.y1;
    if(!clImageCovers(input, inX, inY, width, height) || !clImageCovers(output, outX, outY, width, height))
      return kOfxStatGPURenderFailed;
    const KernelArg args[] = {
      { sizeof(int), &width }, { sizeof(int), &height },
      { sizeof(float), &gain[0] }, { sizeof(float), &gain[1] }, { sizeof(float), &gain[2] },
      { sizeof(cl_mem), &input }, { sizeof(int), &inX }, { sizeof(int), &inY },
      { sizeof(cl_mem), &output }, { sizeof(int), &outX }, { sizeof(int), &outY },
    };
    return runKernel(cmdQ, kKernelSourceImages, "GainImages", width, height, args, sizeof(args) / sizeof(args[0]));
  }

  // OpenCL buffers: "When rendering using OpenCL Buffers, the cl_mem of the buffers are retrieved
  // using kOfxImagePropData."
  int srcOffset, srcRow, dstOffset, dstRow;
  if(!deviceLayout(src, window, srcOffset, srcRow) || !deviceLayout(dst, window, dstOffset, dstRow))
    return kOfxStatGPURenderFailed;
  cl_mem input = (cl_mem) src.data;
  cl_mem output = (cl_mem) dst.data;
  if(!clBufferHolds(input, srcOffset, srcRow, width, height) || !clBufferHolds(output, dstOffset, dstRow, width, height))
    return kOfxStatGPURenderFailed;
  const KernelArg args[] = {
    { sizeof(int), &width }, { sizeof(int), &height },
    { sizeof(float), &gain[0] }, { sizeof(float), &gain[1] }, { sizeof(float), &gain[2] },
    { sizeof(cl_mem), &input }, { sizeof(int), &srcOffset }, { sizeof(int), &srcRow },
    { sizeof(cl_mem), &output }, { sizeof(int), &dstOffset }, { sizeof(int), &dstRow },
  };
  return runKernel(cmdQ, kKernelSourceBuffers, "GainBuffers", width, height, args, sizeof(args) / sizeof(args[0]));
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

  // ofxGPURender.h, kOfxImageEffectPropOpenCLEnabled: "If this property is set to 0, then it
  // must not attempt to use OpenCL while rendering. If this property is set to 1, then it must use
  // OpenCL buffers or images while rendering." It is read whatever the host descriptor says, so
  // that the images are never taken for CPU memory when the host passes OpenCL objects. An OFX
  // 1.4 host doesn't set it: CPU memory.
  int openCLEnabled = 0;
  if(gPropHost->propGetInt(inArgs, kOfxImageEffectPropOpenCLEnabled, 0, &openCLEnabled) != kOfxStatOK)
    openCLEnabled = 0;

  // kOfxImageEffectPropOpenCLCommandQueue: "the command queue to be used for OpenCL rendering
  // (cl_command_queue)"; "The plug-in SHOULD ensure that its render action enqueues any
  // asynchronous OpenCL operations onto the supplied queue."
  void *queue = NULL;
  if(openCLEnabled &&
     gPropHost->propGetPointer(inArgs, kOfxImageEffectPropOpenCLCommandQueue, 0, &queue) != kOfxStatOK)
    queue = NULL;

  DPRINT(("oclplugin: render window [%d, %d - %d, %d], OpenCL %s, queue %p\n",
          renderWindow.x1, renderWindow.y1, renderWindow.x2, renderWindow.y2,
          openCLEnabled ? "enabled" : "disabled", queue));

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
    // empty window or image: nothing to do (and an empty global work size is an error)
  }
  else if(openCLEnabled) {
    status = (src.props ? renderOpenCL(queue, window, src, dst, gain) : kOfxStatGPURenderFailed);
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

  // the CPU and OpenCL renders process float RGBA images
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedPixelDepths, 0, kOfxBitDepthFloat);

  // set some labels and the group it belongs to
  gPropHost->propSetString(effectProps, kOfxPropLabel, 0, "Resolve Simple Gain (OpenCL)");
  gPropHost->propSetString(effectProps, kOfxImageEffectPluginPropGrouping, 0, "OFX Example (GPU)");

  // define the contexts we can be used in
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 0, kOfxImageEffectContextFilter);
  gPropHost->propSetString(effectProps, kOfxImageEffectPropSupportedContexts, 1, kOfxImageEffectContextGeneral);

  // We support OpenCL render, and render on the CPU otherwise: "true" (ofxGPURender.h: Effects
  // that work "on CPUs but with optional OpenCL support [...] should set
  // kOfxImageEffectPropOpenCLSupported (Images) and/or kOfxImageEffectPropOpenCLRenderSupported
  // (Buffers) to be "true""). Both: "If both host and plug-in support both, it is up to the host
  // which it uses."
  // An OFX 1.4 host may not know these properties (kOfxStatErrUnknown): the status is ignored,
  // the plug-in renders on the CPU there.
  gPropHost->propSetString(effectProps, kOfxImageEffectPropOpenCLRenderSupported, 0, "true");
  gPropHost->propSetString(effectProps, kOfxImageEffectPropOpenCLSupported, 0, "true");

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
  "com.blackmagicdesign.ResolveSimpleGainOpenCLPlugin",
  1,
  1,
  setHostFunc,
  pluginMain
};

// the two mandated functions
OfxPlugin *
OfxGetPlugin(int nth)
{
  if(nth == 0)
    return &basicPlugin;
  return 0;
}

int
OfxGetNumberOfPlugins(void)
{
  return 1;
}
