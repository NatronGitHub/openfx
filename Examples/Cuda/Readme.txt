CUDA render example (raw C API)
===============================

A gain on the R, G and B channels of float RGBA images, rendered with CUDA when
the host enables it, else on the CPU. It uses the CUDA render properties of
OFX 1.5, in include/ofxGPURender.h (they no longer need OFX_EXTENSIONS_RESOLVE).

cuda.cpp is the plug-in (no CUDA dependency), kernel.cu the kernel and its
launch, compiled by nvcc. Build with Makefile (CUDAPATH, CUDA_ARCH), with
"make HAVE_CUDA=1" in Examples/, or with CMake -DOFX_SUPPORTS_CUDARENDER=ON
(not on macOS, which has no CUDA).

Describe
--------
The plug-in sets on its descriptor:
- kOfxImageEffectPropCudaRenderSupported = "true": "the host or plug-in can
  support CUDA render". "true" means that it also renders on the CPU ("needed"
  would mean that it can't work without CUDA).
- kOfxImageEffectPropCudaStreamSupported = "true": "the host or plug-in can
  support CUDA streams".
An OFX 1.4 host doesn't know these properties: their status is ignored, and the
plug-in renders on the CPU there.

In the load action, it reads the same two properties on the host descriptor.

Render
------
If the host supports CUDA render, the plug-in reads kOfxImageEffectPropCudaEnabled
in the render inArgs:
  "1 indicates that the kOfxImagePropData of each image of each clip is a CUDA
   memory pointer", 0 that it "is a CPU memory pointer".

When it is 1, the kernel runs directly on the host's device pointers, on the
host's current device: no cudaSetDevice, no cudaMalloc, no copy. It is launched
on the stream of kOfxImageEffectPropCudaStream, which is set only "if the host
and plug-in both support CUDA streams". With a stream, the plug-in returns
without waiting:
  "the plug-in SHOULD ensure that its render action enqueues any asynchronous
   CUDA operations onto the supplied queue."
  "the plug-in SHOULD NOT wait for final asynchronous operations to complete
   before returning from the render action, and SHOULD NOT call
   cudaDeviceSynchronize() at any time."
Without a stream, it launches on the default stream and waits for it
(cudaStreamSynchronize(0)):
  "If not set: the plug-in SHOULD ensure that any asynchronous operations it
   enqueues have completed before returning from the render action."

The kernel processes the render window, from the image bounds and row bytes. If
it can't process the images (no source image, not float RGBA, an image that
doesn't cover the render window, negative row bytes), or the launch fails, the
render returns kOfxStatGPURenderFailed (kOfxStatGPUOutOfMemory if CUDA ran out
of memory), so that the host can render on the CPU (ofxGPURender.h: "the host
should retry the render with OpenGL rendering disabled"; with
kOfxStatGLOutOfMemory, "the host may choose to free resources on the GPU and
retry").

When CUDA is not enabled (or on an OFX 1.4 host), the render is on the CPU, in
cuda.cpp.

The examples have no CUDA host to test with: hostDemo (HostSupport/examples)
only tests the CPU path.
