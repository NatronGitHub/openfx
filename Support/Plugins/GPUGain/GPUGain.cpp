// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include "GPUGain.h"

#include <stdio.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.H"
#include "ofxsLog.h"
#include "ofxGPURender.h" // kOfxStatGPURenderFailed, kOfxStatGPUOutOfMemory
#ifdef OFX_SUPPORTS_OPENGLRENDER
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h> // must precede GL/gl.h on Windows
#endif
#ifdef __APPLE__
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION
#endif
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
#endif

// GPU render paths, each optional (the CPU path always works, also on OFX 1.4 hosts, which
// have no GPU render properties): HAVE_CUDA (CudaKernel.cu), HAVE_OPENCL (OpenCLKernel.cpp),
// HAVE_METAL (MetalKernel.mm, macOS). The plug-in only declares the paths it was built with.

#define kPluginName "GPU Gain"
#define kPluginGrouping "OFX Example (Support)"
#define kPluginDescription "Apply separate RGB gain adjustments to each channels; CUDA/OpenCL Buffers/OpenCL Images/Metal"
#define kPluginIdentifier "com.OpenFXSample.GPUGain"
#define kPluginVersionMajor 1
#define kPluginVersionMinor 0

#define kSupportsTiles false
#define kSupportsMultiResolution false
#define kSupportsMultipleClipPARs false

////////////////////////////////////////////////////////////////////////////////

// The statuses of a failed GPU render (ofxGPURender.h). "If a plug-in returns
// kOfxStatGLRenderFailed [== kOfxStatGPURenderFailed], the host should retry the render with
// OpenGL rendering disabled", and "If a call using OpenCL rendering fails, the host may re-attempt
// using CPU buffers": every GPU path failure, including images the kernels can't process, returns
// it, so that the host can render on the CPU. kOfxStatGPUOutOfMemory: the host may free GPU
// memory and retry.
#define kGPUGainStatGPUFailed kOfxStatGPURenderFailed
#define kGPUGainStatGPUOutOfMemory kOfxStatGPUOutOfMemory

// The status codes of the Run*Kernel launchers (CudaKernel.cu, MetalKernel.mm, OpenCLKernel.cpp)
enum { kGPUGainKernelOK = 0, kGPUGainKernelFailed = 1, kGPUGainKernelOutOfMemory = 2 };

#if defined(HAVE_CUDA) || defined(HAVE_OPENCL) || defined(HAVE_METAL)
static void throwKernelStatus(int p_Status)
{
    if (p_Status == kGPUGainKernelOutOfMemory)
    {
        OFX::throwSuiteStatusException(kGPUGainStatGPUOutOfMemory);
    }
    if (p_Status != kGPUGainKernelOK)
    {
        OFX::throwSuiteStatusException(kGPUGainStatGPUFailed);
    }
}
#endif

// The scale (channelScales) and gain (channelGains, as float) of each channel of an image with
// p_Components, from the r, g, b and a scales: RGBA (r, g, b, a: the scales as they are, as in
// upstream's sample, also for unknown components); RGB (r, g, b, and 1 for the alpha of a GPU image
// that has one); Alpha (a in every channel: the OFX image of an Alpha component has one, the alpha,
// but an OpenCL Image or an OpenGL texture of it may hold it in any channel, e.g. CL_A or CL_R).
typedef std::array<double, 4> Scales;
typedef std::array<float, 4> Gains;

static Scales channelScales(OFX::PixelComponentEnum p_Components, const Scales& p_Scales)
{
    switch (p_Components)
    {
    case OFX::ePixelComponentAlpha:
        return Scales{ { p_Scales[3], p_Scales[3], p_Scales[3], p_Scales[3] } };
    case OFX::ePixelComponentRGB:
        return Scales{ { p_Scales[0], p_Scales[1], p_Scales[2], 1. } };
    default:
        return p_Scales;
    }
}

static Gains channelGains(OFX::PixelComponentEnum p_Components, const Scales& p_Scales)
{
    const Scales scales = channelScales(p_Components, p_Scales);
    Gains gains;
    for (int c = 0; c < 4; ++c)
    {
        gains[c] = static_cast<float>(scales[c]);
    }
    return gains;
}

// The processor: the GPU paths (one for all depths and components), and the CPU path of
// GainProcessor<PIX, nComponents, maxValue>.
class GainExample : public OFX::ImageProcessor
{
public:
    explicit GainExample(OFX::ImageEffect& p_Instance);

#ifdef HAVE_CUDA
    virtual void processImagesCuda() override; // called by ImageProcessor::processImagesCUDA()
#endif
#ifdef HAVE_OPENCL
    virtual void processImagesOpenCL() override;
#endif
#ifdef HAVE_METAL
    virtual void processImagesMetal() override;
#endif

    void setSrcImg(OFX::Image* p_SrcImg);
    // the scales of r, g, b and a, for images with p_Components (see channelGains)
    void setScales(const Scales& p_Scales, OFX::PixelComponentEnum p_Components);

protected:
    OFX::Image* _srcImg;
    Gains _gains; // the gain of each channel (channelGains)

private:
#if defined(HAVE_CUDA) || defined(HAVE_OPENCL) || defined(HAVE_METAL)
    // the image size, and the layout of the pixels of buffers
    struct GPUImages
    {
        int width;
        int height;
        int components;     // per pixel: 1, 3 or 4
        int componentBytes; // 1 (bytes), 2 (shorts) or 4 (floats)
    };
    GPUImages checkGPUImages(bool p_Buffers) const;
#endif
};

GainExample::GainExample(OFX::ImageEffect& p_Instance)
    : OFX::ImageProcessor(p_Instance)
    , _srcImg(NULL)
{
    _gains.fill(1.f);
}

#if defined(HAVE_CUDA) || defined(HAVE_OPENCL) || defined(HAVE_METAL)
// The GPU kernels process the whole image, one pixel per work item: the source and output must
// have the same bounds (and the same depth and components, which setupAndProcess checks), and
// buffers (CUDA, Metal, OpenCL buffers) must be packed rows of pixels of 1, 3 or 4 components, of
// bytes, shorts or floats: the depths and components the plug-in declares. OFX 1.5.1 has no
// property limiting the depths or components of GPU images, so the kernels take all of them.
// Other images (and missing ones) fail with kOfxStatGPURenderFailed, so that the host may render
// on the CPU instead.
GainExample::GPUImages GainExample::checkGPUImages(bool p_Buffers) const
{
    if (!_srcImg || !_dstImg)
    {
        OFX::throwSuiteStatusException(kGPUGainStatGPUFailed);
    }
    const OfxRectI& srcBounds = _srcImg->getBounds();
    const OfxRectI& dstBounds = _dstImg->getBounds();
    if ((srcBounds.x1 != dstBounds.x1) || (srcBounds.x2 != dstBounds.x2) ||
        (srcBounds.y1 != dstBounds.y1) || (srcBounds.y2 != dstBounds.y2))
    {
        OFX::throwSuiteStatusException(kGPUGainStatGPUFailed);
    }
    GPUImages images;
    images.width = dstBounds.x2 - dstBounds.x1;
    images.height = dstBounds.y2 - dstBounds.y1;
    images.components = _dstImg->getPixelComponentCount();
    switch (_dstImg->getPixelDepth())
    {
    case OFX::eBitDepthUByte:
        images.componentBytes = 1;
        break;
    case OFX::eBitDepthUShort:
        images.componentBytes = 2;
        break;
    case OFX::eBitDepthFloat:
        images.componentBytes = 4;
        break;
    default:
        images.componentBytes = 0;
        break;
    }
    if (p_Buffers)
    {
        const int packedRowBytes = images.width * images.components * images.componentBytes;
        if ((images.componentBytes == 0) ||
            ((images.components != 1) && (images.components != 3) && (images.components != 4)) ||
            (_srcImg->getRowBytes() != packedRowBytes) || (_dstImg->getRowBytes() != packedRowBytes))
        {
            OFX::throwSuiteStatusException(kGPUGainStatGPUFailed);
        }
    }
    return images;
}
#endif

#ifdef HAVE_CUDA
// returns a kGPUGainKernel* status
extern int RunCudaKernel(void* p_Stream, int p_Width, int p_Height, int p_Components, int p_ComponentBytes,
                         const float* p_Gain, const void* p_Input, void* p_Output);

void GainExample::processImagesCuda()
{
    const GPUImages images = checkGPUImages(true);
    throwKernelStatus(RunCudaKernel(_pCudaStream, images.width, images.height, images.components, images.componentBytes,
                                    _gains.data(), _srcImg->getPixelData(), _dstImg->getPixelData()));
}
#endif

#ifdef HAVE_METAL
// returns a kGPUGainKernel* status
extern int RunMetalKernel(void* p_CmdQ, int p_Width, int p_Height, int p_Components, int p_ComponentBytes,
                          const float* p_Gain, const void* p_Input, void* p_Output);

void GainExample::processImagesMetal()
{
    const GPUImages images = checkGPUImages(true);
    throwKernelStatus(RunMetalKernel(_pMetalCmdQ, images.width, images.height, images.components, images.componentBytes,
                                     _gains.data(), _srcImg->getPixelData(), _dstImg->getPixelData()));
}
#endif

#ifdef HAVE_OPENCL
// return a kGPUGainKernel* status
extern int RunOpenCLKernelBuffers(void* p_CmdQ, int p_Width, int p_Height, int p_Components, int p_ComponentBytes,
                                  const float* p_Gain, const void* p_Input, void* p_Output);
extern int RunOpenCLKernelImages(void* p_CmdQ, int p_Width, int p_Height, const float* p_Gain, const void* p_Input, void* p_Output);

void GainExample::processImagesOpenCL()
{
    // if a plugin supports both OpenCL Buffers and Images, the host decides which is used and
    // the plugin must determine which based on whether kOfxImageEffectPropOpenCLImage or kOfxImagePropData is set
    const void* inputImage = _srcImg ? _srcImg->getOpenCLImage() : NULL;
    void* outputImage = _dstImg ? _dstImg->getOpenCLImage() : NULL;
    const bool clImages = inputImage || outputImage;

    const GPUImages images = checkGPUImages(!clImages);

    int status;
    if (clImages)
    {
        if (!inputImage || !outputImage)
        {
            OFX::throwSuiteStatusException(kGPUGainStatGPUFailed);
        }
        status = RunOpenCLKernelImages(_pOpenCLCmdQ, images.width, images.height, _gains.data(), inputImage, outputImage);
    }
    else
    {
        status = RunOpenCLKernelBuffers(_pOpenCLCmdQ, images.width, images.height, images.components, images.componentBytes,
                                        _gains.data(), _srcImg->getPixelData(), _dstImg->getPixelData());
    }
    throwKernelStatus(status);
}
#endif

void GainExample::setSrcImg(OFX::Image* p_SrcImg)
{
    _srcImg = p_SrcImg;
}

void GainExample::setScales(const Scales& p_Scales, OFX::PixelComponentEnum p_Components)
{
    _gains = channelGains(p_Components, p_Scales);
}

// A component times its gain. Integer components (maxValue 255 or 65535) are clamped to
// [0, maxValue] and rounded to the nearest value, as in the GPU kernels; floats are not clamped.
template <class PIX, int maxValue>
static PIX scaledComponent(PIX p_Value, float p_Gain)
{
    const float v = p_Value * p_Gain;
    if (maxValue == 1)
    {
        return static_cast<PIX>(v);
    }
    return static_cast<PIX>((v > 0.f ? (v < maxValue ? v : static_cast<float>(maxValue)) : 0.f) + 0.5f);
}

// The CPU render of images of nComponents components of type PIX (maxValue: 1 for floats)
template <class PIX, int nComponents, int maxValue>
class GainProcessor final : public GainExample
{
public:
    explicit GainProcessor(OFX::ImageEffect& p_Instance)
        : GainExample(p_Instance)
    {
    }

private:
    virtual void multiThreadProcessImages(const OfxRectI& p_ProcWindow, const OfxPointD& p_RenderScale) override;
};

template <class PIX, int nComponents, int maxValue>
void GainProcessor<PIX, nComponents, maxValue>::multiThreadProcessImages(const OfxRectI& p_ProcWindow, const OfxPointD& /*p_RenderScale*/)
{
    for (int y = p_ProcWindow.y1; y < p_ProcWindow.y2; ++y)
    {
        if (_effect.abort()) break;

        PIX* dstPix = static_cast<PIX*>(_dstImg->getPixelAddress(p_ProcWindow.x1, y));

        for (int x = p_ProcWindow.x1; x < p_ProcWindow.x2; ++x)
        {
            const PIX* srcPix = static_cast<const PIX*>(_srcImg ? _srcImg->getPixelAddress(x, y) : 0);

            for (int c = 0; c < nComponents; ++c)
            {
                // no src pixel here: be black and transparent
                dstPix[c] = srcPix ? scaledComponent<PIX, maxValue>(srcPix[c], _gains[c]) : PIX();
            }

            // increment the dst pixel
            dstPix += nComponents;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class GPUGain : public OFX::ImageEffect
{
public:
    explicit GPUGain(OfxImageEffectHandle p_Handle);

    /* Override the render */
    virtual void render(const OFX::RenderArguments& p_Args);

#ifdef OFX_SUPPORTS_OPENGLRENDER
    /* the OpenGL render (kOfxImageEffectPropOpenGLEnabled) */
    void renderOpenGL(const OFX::RenderArguments& p_Args);
#endif

    /* Override is identity */
    virtual bool isIdentity(const OFX::IsIdentityArguments& p_Args, OFX::Clip*& p_IdentityClip, double& p_IdentityTime
#ifdef OFX_EXTENSIONS_NUKE
                            , int& p_View, std::string& p_Plane
#endif
                            );

    /* Override changedParam */
    virtual void changedParam(const OFX::InstanceChangedArgs& p_Args, const std::string& p_ParamName);

    /* Override changed clip */
    virtual void changedClip(const OFX::InstanceChangedArgs& p_Args, const std::string& p_ClipName);

    /* Set the enabledness of the component scale params depending on the type of input image and the state of the scaleComponents param */
    void setEnabledness();

    /* Set up and run a processor */
    void setupAndProcess(GainExample &p_GainExample, const OFX::RenderArguments& p_Args);

private:
    /* The CPU or GPU render of images with nComponents components, at the depth p_Depth */
    template <int nComponents>
    void renderDepth(OFX::BitDepthEnum p_Depth, const OFX::RenderArguments& p_Args);

    /* The r, g, b and a scales at p_Time */
    Scales getScales(double p_Time) const;

    // Does not own the following pointers
    OFX::Clip* m_DstClip;
    OFX::Clip* m_SrcClip;

    OFX::DoubleParam* m_Scale;
    OFX::DoubleParam* m_ScaleR;
    OFX::DoubleParam* m_ScaleG;
    OFX::DoubleParam* m_ScaleB;
    OFX::DoubleParam* m_ScaleA;
    OFX::BooleanParam* m_ComponentScalesEnabled;
};

GPUGain::GPUGain(OfxImageEffectHandle p_Handle)
    : ImageEffect(p_Handle)
{
    m_DstClip = fetchClip(kOfxImageEffectOutputClipName);
    m_SrcClip = fetchClip(kOfxImageEffectSimpleSourceClipName);

    m_Scale = fetchDoubleParam("scale");
    m_ScaleR = fetchDoubleParam("scaleR");
    m_ScaleG = fetchDoubleParam("scaleG");
    m_ScaleB = fetchDoubleParam("scaleB");
    m_ScaleA = fetchDoubleParam("scaleA");
    m_ComponentScalesEnabled = fetchBooleanParam("scaleComponents");

    // Set the enabledness of our RGBA sliders
    setEnabledness();
}

void GPUGain::render(const OFX::RenderArguments& p_Args)
{
#ifdef OFX_SUPPORTS_OPENGLRENDER
    if (p_Args.openGLEnabled)
    {
        renderOpenGL(p_Args);
        return;
    }
#endif
    // the processor of the output's depth and components (the source's are the same: setupAndProcess)
    const OFX::BitDepthEnum depth = m_DstClip->getPixelDepth();
    switch (m_DstClip->getPixelComponents())
    {
    case OFX::ePixelComponentRGBA:
        renderDepth<4>(depth, p_Args);
        break;
    case OFX::ePixelComponentRGB:
        renderDepth<3>(depth, p_Args);
        break;
    case OFX::ePixelComponentAlpha:
        renderDepth<1>(depth, p_Args);
        break;
    default:
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

template <int nComponents>
void GPUGain::renderDepth(OFX::BitDepthEnum p_Depth, const OFX::RenderArguments& p_Args)
{
    switch (p_Depth)
    {
    case OFX::eBitDepthUByte:
    {
        GainProcessor<unsigned char, nComponents, 255> processor(*this);
        setupAndProcess(processor, p_Args);
        break;
    }
    case OFX::eBitDepthUShort:
    {
        GainProcessor<unsigned short, nComponents, 65535> processor(*this);
        setupAndProcess(processor, p_Args);
        break;
    }
    case OFX::eBitDepthFloat:
    {
        GainProcessor<float, nComponents, 1> processor(*this);
        setupAndProcess(processor, p_Args);
        break;
    }
    default:
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

Scales GPUGain::getScales(double p_Time) const
{
    Scales scales = { { 1., 1., 1., 1. } };
    if (m_ComponentScalesEnabled->getValueAtTime(p_Time))
    {
        scales[0] = m_ScaleR->getValueAtTime(p_Time);
        scales[1] = m_ScaleG->getValueAtTime(p_Time);
        scales[2] = m_ScaleB->getValueAtTime(p_Time);
        scales[3] = m_ScaleA->getValueAtTime(p_Time);
    }
    // the overall scale is on r, g and b only
    const double scale = m_Scale->getValueAtTime(p_Time);
    for (int c = 0; c < 3; ++c)
    {
        scales[c] *= scale;
    }
    return scales;
}

#ifdef OFX_SUPPORTS_OPENGLRENDER
// The OpenGL render: the source texture drawn over the render window, modulated by the scales
// (GL_MODULATE with a colour: the scales of channelScales, so that an Alpha texture gets the alpha
// scale whichever channel holds it), with the fixed-function pipeline. A vertex colour is clamped to
// [0, 1] there, so other scales are a "GPU render failed in a non-memory-related way"
// (kOfxStatGPURenderFailed, ofxGPURender.h), and "the host should retry the render with OpenGL
// rendering disabled". Values above 1 (in the source, or after the modulation) stay: the textures
// are float (kOfxOpenGLPropPixelDepth float, whatever the clip depth: the host converts byte and
// short images), and GL_CLAMP_FRAGMENT_COLOR's default, FIXED_ONLY,
// clamps only fixed-point render targets. That needs OpenGL 3.0 or GL_ARB_color_buffer_float (the
// macOS legacy profile has it), and the clamping left at FIXED_ONLY or FALSE: else the render
// fails the same way, and the host renders on the CPU.
void GPUGain::renderOpenGL(const OFX::RenderArguments& p_Args)
{
    const Scales scales = channelScales(m_DstClip->getPixelComponents(), getScales(p_Args.time));
    for (int c = 0; c < 4; ++c)
    {
        if (!(scales[c] >= 0. && scales[c] <= 1.))
        {
            OFX::throwSuiteStatusException(kGPUGainStatGPUFailed);
        }
    }
    {
        const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        int major = 0;
        if (version)
        {
            std::sscanf(version, "%d", &major);
        }
        const bool colorBufferFloat = major >= 3 || (extensions && std::strstr(extensions, "GL_ARB_color_buffer_float"));
        GLint clamp = 1;
        if (colorBufferFloat)
        {
            glGetIntegerv(0x891B /* GL_CLAMP_FRAGMENT_COLOR */, &clamp);
        }
        if (!colorBufferFloat || clamp == 1 /* GL_TRUE */)
        {
            OFX::throwSuiteStatusException(kGPUGainStatGPUFailed);
        }
    }
    // the output texture is the render target, with the viewport on the render window
    std::unique_ptr<OFX::Texture> dst(m_DstClip->loadTexture(p_Args.time));
    if (!dst.get())
    {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    std::unique_ptr<OFX::Texture> src(m_SrcClip->isConnected() ? m_SrcClip->loadTexture(p_Args.time) : NULL);
    const OfxRectI& rw = p_Args.renderWindow;
    glPushAttrib(GL_ALL_ATTRIB_BITS);
    // "it is completely up to the effect to choose what OpenGL operations to render with, including
    // projections and so on" (ofxGPURender.h): pixels from the bottom left of the render window
    glMatrixMode(GL_PROJECTION);
    glPushMatrix();
    glLoadIdentity();
    glOrtho(0., rw.x2 - rw.x1, 0., rw.y2 - rw.y1, -1., 1.);
    glMatrixMode(GL_MODELVIEW);
    glPushMatrix();
    glLoadIdentity();
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    // black where there is no source, as the CPU render
    glClearColor(0.f, 0.f, 0.f, 0.f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (src.get())
    {
        const OfxRectI& b = src->getBounds();
        const GLenum target = static_cast<GLenum>(src->getTarget());
        glEnable(target);
        glBindTexture(target, static_cast<GLuint>(src->getIndex()));
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glColor4d(scales[0], scales[1], scales[2], scales[3]);
        const float x1 = float(b.x1 - rw.x1), y1 = float(b.y1 - rw.y1), x2 = float(b.x2 - rw.x1), y2 = float(b.y2 - rw.y1);
        glBegin(GL_QUADS);
        glTexCoord2f(0.f, 0.f); glVertex2f(x1, y1);
        glTexCoord2f(1.f, 0.f); glVertex2f(x2, y1);
        glTexCoord2f(1.f, 1.f); glVertex2f(x2, y2);
        glTexCoord2f(0.f, 1.f); glVertex2f(x1, y2);
        glEnd();
        glBindTexture(target, 0);
        glDisable(target);
    }
    glMatrixMode(GL_PROJECTION);
    glPopMatrix();
    glMatrixMode(GL_MODELVIEW);
    glPopMatrix();
    glPopAttrib();
    if (glGetError() == GL_OUT_OF_MEMORY)
    {
        OFX::throwSuiteStatusException(kGPUGainStatGPUOutOfMemory);
    }
}
#endif

bool GPUGain::isIdentity(const OFX::IsIdentityArguments& p_Args, OFX::Clip*& p_IdentityClip, double& p_IdentityTime
#ifdef OFX_EXTENSIONS_NUKE
                         , int& /*p_View*/, std::string& /*p_Plane*/
#endif
                         )
{
    // only the scales of the source's channels matter (e.g. scaleA for an Alpha source; all four for
    // RGBA, as in upstream's sample)
    const Scales scales = channelScales(m_SrcClip->getPixelComponents(), getScales(p_Args.time));
    if ((scales[0] == 1.0) && (scales[1] == 1.0) && (scales[2] == 1.0) && (scales[3] == 1.0))
    {
        p_IdentityClip = m_SrcClip;
        p_IdentityTime = p_Args.time;
        return true;
    }

    return false;
}

void GPUGain::changedParam(const OFX::InstanceChangedArgs& /*p_Args*/, const std::string& p_ParamName)
{
    if (p_ParamName == "scaleComponents")
    {
        setEnabledness();
    }
}

void GPUGain::changedClip(const OFX::InstanceChangedArgs& /*p_Args*/, const std::string& p_ClipName)
{
    if (p_ClipName == kOfxImageEffectSimpleSourceClipName)
    {
        setEnabledness();
    }
}

void GPUGain::setEnabledness()
{
    // the scales that change the render (channelScales): scale and scaleR/G/B for the r, g and b of
    // RGBA and RGB sources, scaleA for the alpha of RGBA and Alpha sources; the per-component ones
    // only with scaleComponents. As in upstream's sample, the per-component ones are disabled for
    // other components (e.g. a disconnected source), and scale is enabled.
    const OFX::PixelComponentEnum components = m_SrcClip->getPixelComponents();
    const bool rgba = components == OFX::ePixelComponentRGBA;
    const bool rgb = rgba || (components == OFX::ePixelComponentRGB);
    const bool alpha = rgba || (components == OFX::ePixelComponentAlpha);
    const bool perComponent = m_ComponentScalesEnabled->getValue();

    // (scale is only set when it changes, so that for RGBA sources the host gets upstream's calls)
    const bool scaleEnabled = components != OFX::ePixelComponentAlpha;
    if (m_Scale->getIsEnable() != scaleEnabled)
    {
        m_Scale->setEnabled(scaleEnabled);
    }
    m_ScaleR->setEnabled(perComponent && rgb);
    m_ScaleG->setEnabled(perComponent && rgb);
    m_ScaleB->setEnabled(perComponent && rgb);
    m_ScaleA->setEnabled(perComponent && alpha);
}

void GPUGain::setupAndProcess(GainExample& p_GainExample, const OFX::RenderArguments& p_Args)
{
    // Get the dst image
    std::unique_ptr<OFX::Image> dst(m_DstClip->fetchImage(p_Args.time));
    if (!dst.get())
    {
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }
    OFX::BitDepthEnum dstBitDepth = dst->getPixelDepth();
    OFX::PixelComponentEnum dstComponents = dst->getPixelComponents();

    // Get the src image (none if the input is disconnected: the CPU path then renders black)
    std::unique_ptr<OFX::Image> src(m_SrcClip->isConnected() ? m_SrcClip->fetchImage(p_Args.time) : NULL);
    if (src.get())
    {
        OFX::BitDepthEnum srcBitDepth = src->getPixelDepth();
        OFX::PixelComponentEnum srcComponents = src->getPixelComponents();

        // Check to see if the bit depth and number of components are the same
        if ((srcBitDepth != dstBitDepth) || (srcComponents != dstComponents))
        {
            OFX::throwSuiteStatusException(kOfxStatErrValue);
        }
    }
    else if (p_Args.isEnabledOpenCLRender || p_Args.isEnabledCudaRender || p_Args.isEnabledMetalRender)
    {
        // the GPU kernels need a source image: let the host render on the CPU
        OFX::throwSuiteStatusException(kGPUGainStatGPUFailed);
    }

    const Scales scales = getScales(p_Args.time);

    // Set the images
    p_GainExample.setDstImg(dst.get());
    p_GainExample.setSrcImg(src.get());

    // Setup OpenCL and CUDA Render arguments
    p_GainExample.setGPURenderArgs(p_Args);

    // Set the render window
    p_GainExample.setRenderWindow(p_Args.renderWindow, p_Args.renderScale);

    // Set the scales
    p_GainExample.setScales(scales, dstComponents);

    // Call the base class process member, this will call the derived templated process code
    p_GainExample.process();
}

////////////////////////////////////////////////////////////////////////////////

using namespace OFX;

GPUGainFactory::GPUGainFactory()
    : OFX::PluginFactoryHelper<GPUGainFactory>(kPluginIdentifier, kPluginVersionMajor, kPluginVersionMinor)
{
}

void GPUGainFactory::describe(OFX::ImageEffectDescriptor& p_Desc)
{
    // Basic labels
    p_Desc.setLabels(kPluginName, kPluginName, kPluginName);
    p_Desc.setPluginGrouping(kPluginGrouping);
    p_Desc.setPluginDescription(kPluginDescription);

    // Add the supported contexts, only filter at the moment
    p_Desc.addSupportedContext(eContextFilter);
    p_Desc.addSupportedContext(eContextGeneral);

    // Add supported pixel depths (the CPU and GPU paths take all three)
    p_Desc.addSupportedBitDepth(eBitDepthUByte);
    p_Desc.addSupportedBitDepth(eBitDepthUShort);
    p_Desc.addSupportedBitDepth(eBitDepthFloat);

    // Set a few flags
    p_Desc.setSingleInstance(false);
    p_Desc.setHostFrameThreading(false);
    p_Desc.setSupportsMultiResolution(kSupportsMultiResolution);
    p_Desc.setSupportsTiles(kSupportsTiles);
    p_Desc.setTemporalClipAccess(false);
    p_Desc.setRenderTwiceAlways(false);
    p_Desc.setSupportsMultipleClipPARs(kSupportsMultipleClipPARs);

    // GPU render capability flags (OFX 1.5, ignored by older hosts), only for the paths this
    // build has: the host may then pass GPU images instead of CPU ones, so declaring a path
    // without its code would break rendering. CPU rendering is always supported.
#ifdef HAVE_OPENCL
    // OpenCL Buffers and Images: the host chooses (see processImagesOpenCL)
    p_Desc.setSupportsOpenCLBuffersRender(true);
    p_Desc.setSupportsOpenCLImagesRender(true);
#endif
#ifdef HAVE_CUDA
    p_Desc.setSupportsCudaRender(true);
    p_Desc.setSupportsCudaStream(true);
#endif
#ifdef HAVE_METAL
    p_Desc.setSupportsMetalRender(true);
#endif
#ifdef OFX_SUPPORTS_OPENGLRENDER
    // OpenGL, with a CPU path: "true" - "a host or plug-in can support OpenGL accelerated rendering,
    // in the case of plug-ins this also means that it is capable of CPU based rendering in the
    // absence of a GPU" (kOfxImageEffectPropOpenGLRenderSupported). Float textures only, whatever
    // the clip depth (see renderOpenGL): "When a plug-in sets this property, the host will try to
    // provide buffers/textures in one of the supported formats. Additionally, the target buffers
    // where the plug-in renders to will be set to one of the supported formats."
    // (kOfxOpenGLPropPixelDepth)
    p_Desc.setSupportsOpenGLRender(true);
    p_Desc.addOpenGLBitDepth(OFX::eBitDepthFloat);
#endif
}

static DoubleParamDescriptor* defineScaleParam(OFX::ImageEffectDescriptor& p_Desc, const std::string& p_Name, const std::string& p_Label,
                                               const std::string& p_Hint, GroupParamDescriptor* p_Parent)
{
    DoubleParamDescriptor* param = p_Desc.defineDoubleParam(p_Name);
    param->setLabels(p_Label, p_Label, p_Label);
    param->setScriptName(p_Name);
    param->setHint(p_Hint);
    param->setDefault(1);
    param->setRange(0, 10);
    param->setIncrement(0.1);
    param->setDisplayRange(0, 10);
    param->setDoubleType(eDoubleTypeScale);

    if (p_Parent)
    {
        param->setParent(*p_Parent);
    }

    return param;
}


void GPUGainFactory::describeInContext(OFX::ImageEffectDescriptor& p_Desc, OFX::ContextEnum /*p_Context*/)
{
    // Source clip only in the filter context
    // Create the mandated source clip
    ClipDescriptor* srcClip = p_Desc.defineClip(kOfxImageEffectSimpleSourceClipName);
    srcClip->addSupportedComponent(ePixelComponentRGBA);
    srcClip->addSupportedComponent(ePixelComponentRGB);
    srcClip->addSupportedComponent(ePixelComponentAlpha);
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    // Create the mandated output clip
    ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA);
    dstClip->addSupportedComponent(ePixelComponentRGB);
    dstClip->addSupportedComponent(ePixelComponentAlpha);
    dstClip->setSupportsTiles(kSupportsTiles);

    // Make some pages and to things in
    PageParamDescriptor* page = p_Desc.definePageParam("Controls");

    // Group param to group the scales
    GroupParamDescriptor* componentScalesGroup = p_Desc.defineGroupParam("componentScales");
    componentScalesGroup->setHint("Scales on the individual component");
    componentScalesGroup->setLabels("Components", "Components", "Components");

    // Make overall scale params
    DoubleParamDescriptor* param = defineScaleParam(p_Desc, "scale", "scale", "Scales all component in the image", 0);
    page->addChild(*param);

    // Add a boolean to enable the component scale
    BooleanParamDescriptor* boolParam = p_Desc.defineBooleanParam("scaleComponents");
    boolParam->setDefault(true);
    boolParam->setHint("Enables scales on individual components");
    boolParam->setLabels("Scale Components", "Scale Components", "Scale Components");
    boolParam->setParent(*componentScalesGroup);
    page->addChild(*boolParam);

    // Make the four component scale params
    param = defineScaleParam(p_Desc, "scaleR", "red", "Scales the red component of the image", componentScalesGroup);
    page->addChild(*param);

    param = defineScaleParam(p_Desc, "scaleG", "green", "Scales the green component of the image", componentScalesGroup);
    page->addChild(*param);

    param = defineScaleParam(p_Desc, "scaleB", "blue", "Scales the blue component of the image", componentScalesGroup);
    page->addChild(*param);

    param = defineScaleParam(p_Desc, "scaleA", "alpha", "Scales the alpha component of the image", componentScalesGroup);
    page->addChild(*param);
}

ImageEffect* GPUGainFactory::createInstance(OfxImageEffectHandle p_Handle, ContextEnum /*p_Context*/)
{
    return new GPUGain(p_Handle);
}

// the Support library's automatic registration (it has no OFX::Plugin::getPluginIDs)
static GPUGainFactory gainPlugin;
mRegisterPluginFactoryInstance(gainPlugin)
