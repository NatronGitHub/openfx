GPU render examples: Cuda, OpenCL and Metal
===========================================

The Cuda and OpenCL examples were contributed by Blackmagic Design (as of 21
April 2015) for the GPU render extensions of DaVinci Resolve, which were
defined in ofxImageEffect.h under OFX_EXTENSIONS_RESOLVE. These extensions
became standard in OFX 1.5, in include/ofxGPURender.h, with CUDA streams, Metal,
and OpenCL images. The examples now use ofxGPURender.h (OFX_EXTENSIONS_RESOLVE
is no longer needed), and the Metal example was added.

The directories:

1. Cuda   - CUDA render
2. OpenCL - OpenCL render (buffers and images)
3. Metal  - Metal buffer render (macOS only)

The properties, in include/ofxGPURender.h:

- kOfxImageEffectPropCudaRenderSupported   - "Indicates whether a host or plug-in can support CUDA render"
- kOfxImageEffectPropCudaEnabled           - "Indicates that a plug-in SHOULD use CUDA render in the current action"
- kOfxImageEffectPropCudaStreamSupported   - "Indicates whether a host or plug-in can support CUDA streams"
- kOfxImageEffectPropCudaStream            - "The CUDA stream to be used for rendering"
- kOfxImageEffectPropOpenCLRenderSupported - "Indicates whether a host or plug-in can support OpenCL Buffers render"
- kOfxImageEffectPropOpenCLSupported       - "Indicates whether a host or plug-in can support OpenCL Images render"
- kOfxImageEffectPropOpenCLEnabled         - "Indicates that a plug-in SHOULD use OpenCL render in the current action"
- kOfxImageEffectPropOpenCLCommandQueue    - "Indicates the OpenCL command queue that should be used for rendering"
- kOfxImageEffectPropOpenCLImage           - "Indicates the image handle of an image supplied as an OpenCL Image by the host"
- kOfxImageEffectPropMetalRenderSupported  - "Indicates whether a host or plug-in can support Metal buffer render"
- kOfxImageEffectPropMetalEnabled          - "Indicates that a plug-in SHOULD use Metal buffer render in the current action"
- kOfxImageEffectPropMetalCommandQueue     - "The command queue of Metal render"

The examples follow the same scheme:

- describe sets the "...Supported" properties of the plug-in to "true": it
  can render on the GPU, and on the CPU ("needed" would mean that it can't
  work without the GPU). An OFX 1.4 host doesn't know these properties: the
  plug-in ignores the status, and renders on the CPU there.
- render reads the "...Enabled" property in its inArgs (0 if it is missing,
  as on an OFX 1.4 host; whatever the host descriptor says). When it is 1, the
  images are GPU memory (CUDA device pointers, OpenCL buffers or images, Metal
  buffers): the plug-in renders on them directly, on the host's stream or
  queue, and doesn't wait for the GPU before returning (except CUDA without a
  stream: "If not set: the plug-in SHOULD ensure that any asynchronous
  operations it enqueues have completed before returning from the render
  action"). It doesn't select a device, allocate GPU memory or copy images.
- if it can't render on the GPU, it returns kOfxStatGPURenderFailed
  (kOfxStatGPUOutOfMemory when the GPU is out of memory), so that the host can
  render on the CPU ("If a call using OpenCL rendering fails, the host may
  re-attempt using CPU buffers instead").
- otherwise, it renders on the CPU.

To keep them simple, there is one example per GPU API. A plug-in can support
several by setting several "...Supported" properties to "true", and checking
the "...Enabled" properties in render: the host enables the one it runs on.

More information is in the Readme.txt of each directory.
