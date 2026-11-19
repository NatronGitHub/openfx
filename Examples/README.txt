Hi,

the examples enclosed in this directory are designed to show certain features
of the OFX Image Effect API, they are not meant to be great examples of the
art of image processing or programming.

They are written in C++ against the raw C API (include/), without the C++
Support library (see Support/Plugins for examples that use it). Build them with
Examples/Makefile (which uses Makefile.master) or with CMake
(-DBUILD_EXAMPLE_PLUGINS=ON), on Linux, macOS, FreeBSD and Windows (MinGW, or
CMake with MSVC).

The examples are...

     - Basic   - shows a simple luminance scaling plugin, which defines several
                 parameters, processes images and has actions to
	         respond to user interaction.
     - Overlay - shows how to draw a simple overlay widget using the OFX Interact
                 API.
     - Custom  - shows how custom parameters work within the API, it also uses the
	         OFX Interact API.
     - DepthConverter - shows how to deal with pixel preferences.
     - Rectangle - example generator and filter that shows how to deal with spatial
                 parameters.
     - Invert  - a very simple plugin that inverts an image: basic plugin
                 definition, properties, image access and rendering.
     - OpenGL  - renders with OpenGL textures, using the OpenGL render suite
                 (ofxGPURender.h).
     - Test    - a host testing plugin, which logs the properties it is given.
     - DrawSuite - (OFX 1.5) sets both overlay entry points: V1 draws with OpenGL
                 (orange, "V1 OpenGL"), V2 draws with the OfxDrawSuiteV1 (magenta,
                 "V2 DrawSuite") and calls every DrawSuite function.
     - ColourSpace - (OFX 1.5) colour management: three plug-ins (Basic, Core and
                 Full styles) that set their preferred input and output colourspaces,
                 and write the clip colourspaces into the output image.
     - ChoiceParams - (OFX 1.5) Choice params with kOfxParamPropChoiceOrder, and a
                 StrChoice (string-valued) param.

GPU render examples (OFX 1.5, ofxGPURender.h). Each one is a gain on the R, G
and B channels of float RGBA images. The host decides, for each render action,
whether it passes GPU or CPU images: each example renders on the GPU when the
host enables it, on the host's queue or stream, with the host's images, and on
the CPU otherwise, also on an OFX 1.4 host. If it can't render on the GPU, it
returns kOfxStatGPURenderFailed, so that the host can render on the CPU.
See the Readme.txt in each directory.

     - Cuda    - CUDA render (kOfxImageEffectPropCudaRenderSupported,
                 kOfxImageEffectPropCudaEnabled, kOfxImageEffectPropCudaStream).
                 Needs nvcc: CMake -DOFX_SUPPORTS_CUDARENDER=ON (Linux, Windows
                 with MSVC), or "make HAVE_CUDA=1" (Linux).
     - OpenCL  - OpenCL render, with buffers (kOfxImageEffectPropOpenCLRenderSupported)
                 or images (kOfxImageEffectPropOpenCLSupported,
                 kOfxImageEffectPropOpenCLImage), on
                 kOfxImageEffectPropOpenCLCommandQueue. "make HAVE_OPENCL=1", or
                 CMake -DOFX_SUPPORTS_OPENCLRENDER=ON.
     - Metal   - Metal buffer render (kOfxImageEffectPropMetalRenderSupported,
                 kOfxImageEffectPropMetalEnabled,
                 kOfxImageEffectPropMetalCommandQueue), with an Objective-C++
                 kernel. macOS only, built by default there.

DrawSuite, ColourSpace and ChoiceParams were ported from the upstream OpenFX
repository (AcademySoftwareFoundation/openfx).

The directory include contains the file ofxUtilities.H which contains some code
shared by all the examples, and ofxuFont5x7.H, a small bitmap font.

cheers

Bruno Nicoletti
