OpenCL render example (raw C API)
=================================

A gain on the R, G and B channels of float RGBA images, rendered with OpenCL
when the host enables it, else on the CPU. It uses the OpenCL render properties
of OFX 1.5, in include/ofxGPURender.h (they no longer need
OFX_EXTENSIONS_RESOLVE). Build with Makefile (OPENCL_HOME; the OpenCL framework
on macOS), with "make HAVE_OPENCL=1" in Examples/, or with CMake
-DOFX_SUPPORTS_OPENCLRENDER=ON.

Describe
--------
OFX 1.5 has two ways of passing OpenCL images, and the plug-in supports both.
It sets on its descriptor:
- kOfxImageEffectPropOpenCLRenderSupported = "true": OpenCL buffers (cl_mem
  buffers, in kOfxImagePropData);
- kOfxImageEffectPropOpenCLSupported = "true": OpenCL images (cl_mem images, in
  kOfxImageEffectPropOpenCLImage).
ofxGPURender.h: effects that work "on CPUs but with optional OpenCL support
[...] should set kOfxImageEffectPropOpenCLSupported (Images) and/or
kOfxImageEffectPropOpenCLRenderSupported (Buffers) to be "true"", and "If both
host and plug-in support both, it is up to the host which it uses."
An OFX 1.4 host doesn't know these properties: their status is ignored, and the
plug-in renders on the CPU there.

In the load action, it reads the same two properties on the host descriptor.

Render
------
If the host supports OpenCL render, the plug-in reads kOfxImageEffectPropOpenCLEnabled
in the render inArgs: "If this property is set to 0, then it must not attempt to
use OpenCL while rendering. If this property is set to 1, then it must use
OpenCL buffers or images while rendering."

When it is 1, the kernel is enqueued on the host's queue, from
kOfxImageEffectPropOpenCLCommandQueue (a cl_command_queue), with the host's
buffers or images: the plug-in creates no context, queue or buffer, and copies
nothing. It doesn't wait for the kernel ("The plug-in SHOULD NOT wait for final
asynchronous operations to complete before returning from the render action").
"If a plug-in supports both, it must use kOfxImageEffectPropOpenCLImage to
determine if Images or Buffers are being used for a given render action": the
plug-in uses images if the source or output image has it, else buffers.
- Buffers: the kernel processes the render window, from the image bounds and
  row bytes.
- Images: "OpenCL Images do not have the concept of row bytes": the kernel uses
  the position of the render window in each image (an image has the size of its
  bounds, the first row being the bottom one, as for buffers).

The kernels are built on first use for each OpenCL context, found from the
queue with CL_QUEUE_CONTEXT: "The host may support multiple OpenCL devices.
Therefore the plug-in should keep a separate set of kernels per OpenCL content
(e.g., using a map)." They are kept until the unload action. A cl_kernel is not
thread-safe, so an OFX mutex is held from clSetKernelArg to
clEnqueueNDRangeKernel. The kernels use OpenCL 1.1 only ("Effects should target
OpenCL 1.1 API and OpenCL C kernel language support").

If it can't process the images (no source image, not float RGBA, an image that
doesn't cover the render window, negative row bytes), or OpenCL fails, the
render returns kOfxStatGPURenderFailed (kOfxStatGPUOutOfMemory if OpenCL is out
of resources): "If a call using OpenCL rendering fails, the host may re-attempt
using CPU buffers instead".

When OpenCL is not enabled (or on an OFX 1.4 host), the render is on the CPU,
in oclplugin.cpp (the example used to copy CPU images to its own OpenCL
context and back).

hostDemo (HostSupport/examples) has no OpenCL queue: it only tests the CPU path.
