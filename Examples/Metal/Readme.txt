Metal render example (raw C API, macOS only)
============================================

A gain on the R, G and B channels of float RGBA images, rendered with Metal when
the host enables it, else on the CPU. It uses the Metal buffer render
properties of OFX 1.5, in include/ofxGPURender.h.

metal.cpp is the plug-in (plain C++), kernel.mm the Metal kernel and its
encoding (Objective-C++ with manual reference counting, compiled by the C++
compiler: Makefile.master's %.mm rule, or CMake on macOS). It links with the
Metal, Foundation and QuartzCore frameworks. Built by default on macOS by
Examples/Makefile (HAVE_METAL=0 to skip it) and by CMake.

Describe
--------
The plug-in sets kOfxImageEffectPropMetalRenderSupported = "true" on its
descriptor: "the host or plug-in can support Metal buffer render". "true" means
that it also renders on the CPU ("needed" would mean that it can't work without
Metal). It doesn't declare Metal textures (kOfxImageEffectPropMetalTextureSupported,
"false" by default), so the host passes Metal buffers or CPU memory.
An OFX 1.4 host doesn't know the property: its status is ignored, and the
plug-in renders on the CPU there.

In the load action, it reads the same property on the host descriptor.

Render
------
If the host supports Metal buffer render, the plug-in reads
kOfxImageEffectPropMetalEnabled in the render inArgs: "1 indicates that the
kOfxImagePropData of each image of each clip is a Metal id<MTLBuffer>", 0 that
it "is a CPU memory pointer".

When it is 1, the kernel is encoded on the host's queue, from
kOfxImageEffectPropMetalCommandQueue (an id<MTLCommandQueue>: "The plug-in
SHOULD ensure that its render action enqueues any asynchronous Metal operations
onto the supplied queue"), with the host's buffers, and the command buffer is
committed without waiting ("The plug-in SHOULD NOT wait for final asynchronous
operations to complete before returning from the render action"). The plug-in
creates no buffer and copies nothing. The kernel processes the render window,
from the image bounds and row bytes, and checks that the buffers are large
enough.

The compute pipeline is built from the kernel source on first use for each
MTLDevice (queue.device) and kept until the unload action, under an OFX mutex.
kernel.mm uses manual retain/release (it doesn't build with -fobjc-arc), and an
@autoreleasepool for the command buffer and encoder.

If it can't process the images (no source image, not float RGBA, an image that
doesn't cover the render window, negative row bytes, a buffer too small), or
Metal fails, the render returns kOfxStatGPURenderFailed, so that the host can
render on the CPU (ofxGPURender.h: "the host should retry the render with
OpenGL rendering disabled").

When Metal is not enabled (or on an OFX 1.4 host), the render is on the CPU, in
metal.cpp.

hostDemo (HostSupport/examples) has no Metal queue: it only tests the CPU path.
