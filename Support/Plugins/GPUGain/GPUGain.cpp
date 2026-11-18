// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include "GPUGain.h"

#include <stdio.h>
#include <memory>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsProcessing.H"
#include "ofxsLog.h"

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
    virtual void multiThreadProcessImages(const OfxRectI& p_ProcWindow, const OfxPointD& p_RenderScale) override;

    void setSrcImg(OFX::Image* p_SrcImg);
    void setScales(float p_ScaleR, float p_ScaleG, float p_ScaleB, float p_ScaleA);

private:
    OFX::Image* _srcImg;
    float _scales[4];
};

GainExample::GainExample(OFX::ImageEffect& p_Instance)
    : OFX::ImageProcessor(p_Instance)
    , _srcImg(NULL)
{
    _scales[0] = _scales[1] = _scales[2] = _scales[3] = 1.f;
}

#ifdef HAVE_CUDA
extern void RunCudaKernel(void* p_Stream, int p_Width, int p_Height, float* p_Gain, const float* p_Input, float* p_Output);

void GainExample::processImagesCuda()
{
    const OfxRectI& bounds = _srcImg->getBounds();
    const int width = bounds.x2 - bounds.x1;
    const int height = bounds.y2 - bounds.y1;

    float* input = static_cast<float*>(_srcImg->getPixelData());
    float* output = static_cast<float*>(_dstImg->getPixelData());

    RunCudaKernel(_pCudaStream, width, height, _scales, input, output);
}
#endif

#ifdef HAVE_METAL
extern void RunMetalKernel(void* p_CmdQ, int p_Width, int p_Height, float* p_Gain, const float* p_Input, float* p_Output);

void GainExample::processImagesMetal()
{
    const OfxRectI& bounds = _srcImg->getBounds();
    const int width = bounds.x2 - bounds.x1;
    const int height = bounds.y2 - bounds.y1;

    float* input = static_cast<float*>(_srcImg->getPixelData());
    float* output = static_cast<float*>(_dstImg->getPixelData());

    RunMetalKernel(_pMetalCmdQ, width, height, _scales, input, output);
}
#endif

#ifdef HAVE_OPENCL
extern void RunOpenCLKernelBuffers(void* p_CmdQ, int p_Width, int p_Height, float* p_Gain, const float* p_Input, float* p_Output);
extern void RunOpenCLKernelImages(void* p_CmdQ, int p_Width, int p_Height, float* p_Gain, const float* p_Input, float* p_Output);

void GainExample::processImagesOpenCL()
{
    const OfxRectI& bounds = _srcImg->getBounds();
    const int width = bounds.x2 - bounds.x1;
    const int height = bounds.y2 - bounds.y1;

    float* input = static_cast<float*>(_srcImg->getOpenCLImage());
    float* output = static_cast<float*>(_dstImg->getOpenCLImage());

    // if a plugin supports both OpenCL Buffers and Images, the host decides which is used and
    // the plugin must determine which based on whether kOfxImageEffectPropOpenCLImage or kOfxImagePropData is set
    if (input || output)
    {
        RunOpenCLKernelImages(_pOpenCLCmdQ, width, height, _scales, input, output);
    }
    else
    {
        input = static_cast<float*>(_srcImg->getPixelData());
        output = static_cast<float*>(_dstImg->getPixelData());

        RunOpenCLKernelBuffers(_pOpenCLCmdQ, width, height, _scales, input, output);
    }
}
#endif

void GainExample::multiThreadProcessImages(const OfxRectI& p_ProcWindow, const OfxPointD& /*p_RenderScale*/)
{
    for (int y = p_ProcWindow.y1; y < p_ProcWindow.y2; ++y)
    {
        if (_effect.abort()) break;

        float* dstPix = static_cast<float*>(_dstImg->getPixelAddress(p_ProcWindow.x1, y));

        for (int x = p_ProcWindow.x1; x < p_ProcWindow.x2; ++x)
        {
            float* srcPix = static_cast<float*>(_srcImg ? _srcImg->getPixelAddress(x, y) : 0);

            // do we have a source image to scale up
            if (srcPix)
            {
                for(int c = 0; c < 4; ++c)
                {
                    dstPix[c] = srcPix[c] * _scales[c];
                }
            }
            else
            {
                // no src pixel here, be black and transparent
                for (int c = 0; c < 4; ++c)
                {
                    dstPix[c] = 0;
                }
            }

            // increment the dst pixel
            dstPix += 4;
        }
    }
}

void GainExample::setSrcImg(OFX::Image* p_SrcImg)
{
    _srcImg = p_SrcImg;
}

void GainExample::setScales(float p_ScaleR, float p_ScaleG, float p_ScaleB, float p_ScaleA)
{
    _scales[0] = p_ScaleR;
    _scales[1] = p_ScaleG;
    _scales[2] = p_ScaleB;
    _scales[3] = p_ScaleA;
}

////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class GPUGain : public OFX::ImageEffect
{
public:
    explicit GPUGain(OfxImageEffectHandle p_Handle);

    /* Override the render */
    virtual void render(const OFX::RenderArguments& p_Args);

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
    if ((m_DstClip->getPixelDepth() == OFX::eBitDepthFloat) && (m_DstClip->getPixelComponents() == OFX::ePixelComponentRGBA))
    {
        GainExample imageScaler(*this);
        setupAndProcess(imageScaler, p_Args);
    }
    else
    {
        OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
}

bool GPUGain::isIdentity(const OFX::IsIdentityArguments& p_Args, OFX::Clip*& p_IdentityClip, double& p_IdentityTime
#ifdef OFX_EXTENSIONS_NUKE
                         , int& /*p_View*/, std::string& /*p_Plane*/
#endif
                         )
{
    double rScale = 1.0, gScale = 1.0, bScale = 1.0, aScale = 1.0;

    if (m_ComponentScalesEnabled->getValueAtTime(p_Args.time))
    {
        rScale = m_ScaleR->getValueAtTime(p_Args.time);
        gScale = m_ScaleG->getValueAtTime(p_Args.time);
        bScale = m_ScaleB->getValueAtTime(p_Args.time);
        aScale = m_ScaleA->getValueAtTime(p_Args.time);
    }

    const double scale = m_Scale->getValueAtTime(p_Args.time);
    rScale *= scale;
    gScale *= scale;
    bScale *= scale;

    if ((rScale == 1.0) && (gScale == 1.0) && (bScale == 1.0) && (aScale == 1.0))
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
    // the component enabledness depends on the clip being RGBA and the param being true
    const bool enable = (m_ComponentScalesEnabled->getValue() && (m_SrcClip->getPixelComponents() == OFX::ePixelComponentRGBA));

    m_ScaleR->setEnabled(enable);
    m_ScaleG->setEnabled(enable);
    m_ScaleB->setEnabled(enable);
    m_ScaleA->setEnabled(enable);
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
        // the GPU kernels need a source image
        OFX::throwSuiteStatusException(kOfxStatFailed);
    }

    double rScale = 1.0, gScale = 1.0, bScale = 1.0, aScale = 1.0;

    if (m_ComponentScalesEnabled->getValueAtTime(p_Args.time))
    {
        rScale = m_ScaleR->getValueAtTime(p_Args.time);
        gScale = m_ScaleG->getValueAtTime(p_Args.time);
        bScale = m_ScaleB->getValueAtTime(p_Args.time);
        aScale = m_ScaleA->getValueAtTime(p_Args.time);
    }

    const double scale = m_Scale->getValueAtTime(p_Args.time);
    rScale *= scale;
    gScale *= scale;
    bScale *= scale;

    // Set the images
    p_GainExample.setDstImg(dst.get());
    p_GainExample.setSrcImg(src.get());

    // Setup OpenCL and CUDA Render arguments
    p_GainExample.setGPURenderArgs(p_Args);

    // Set the render window
    p_GainExample.setRenderWindow(p_Args.renderWindow, p_Args.renderScale);

    // Set the scales
    p_GainExample.setScales(rScale, gScale, bScale, aScale);

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

    // Add supported pixel depths
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
    srcClip->setTemporalClipAccess(false);
    srcClip->setSupportsTiles(kSupportsTiles);
    srcClip->setIsMask(false);

    // Create the mandated output clip
    ClipDescriptor* dstClip = p_Desc.defineClip(kOfxImageEffectOutputClipName);
    dstClip->addSupportedComponent(ePixelComponentRGBA); // render() only does float RGBA
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
