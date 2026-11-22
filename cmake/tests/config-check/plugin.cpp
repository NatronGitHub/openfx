// SPDX-License-Identifier: BSD-3-Clause
// A minimal plug-in on the Support library, for the link-time configuration check
// (NatronGitHub/openfx#226, OFXS_CONFIGURATION_CHECK in ofxsImageEffect.h): it links with the
// definitions of openfx::Support, and fails to link with any other set (see CMakeLists.txt).
#include "ofxsImageEffect.h"

namespace ofxConfigCheckTest {

class TestPlugin : public OFX::ImageEffect {
public:
  explicit TestPlugin(OfxImageEffectHandle handle) : OFX::ImageEffect(handle) {}
  virtual void render(const OFX::RenderArguments &) {}
};

mDeclarePluginFactory(TestPluginFactory, {}, {});

void TestPluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
  desc.setLabel("ConfigCheckTest");
  desc.addSupportedContext(OFX::eContextFilter);
  desc.addSupportedBitDepth(OFX::eBitDepthFloat);
}

void TestPluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum)
{
  OFX::ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(OFX::ePixelComponentRGBA);
  OFX::ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(OFX::ePixelComponentRGBA);
}

OFX::ImageEffect *TestPluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum)
{
  return new TestPlugin(handle);
}

} // namespace ofxConfigCheckTest

static ofxConfigCheckTest::TestPluginFactory p("net.sf.openfx.ConfigCheckTest", 1, 0);
mRegisterPluginFactoryInstance(p)
