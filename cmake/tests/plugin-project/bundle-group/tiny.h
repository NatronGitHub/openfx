// SPDX-License-Identifier: BSD-3-Clause
// Tiny Support plug-ins for the ofx_plugin_bundle_group() test (they are loaded, not rendered)
#ifndef OFX_TEST_TINY_H
#define OFX_TEST_TINY_H
#include "ofxsImageEffect.h"

#ifndef TINY_COMMON
#error "COMMON_COMPILE_DEFINITIONS was not applied"
#endif

namespace tiny {
// common.cpp (COMMON_SOURCES)
void describe(OFX::ImageEffectDescriptor &desc, const char *label);
void describeInContext(OFX::ImageEffectDescriptor &desc);

class Effect : public OFX::ImageEffect {
public:
  explicit Effect(OfxImageEffectHandle handle) : OFX::ImageEffect(handle) {}
  void render(const OFX::RenderArguments &) override {}
};
} // namespace tiny

#define TINY_FACTORY(CLASS, LABEL) \
  mDeclarePluginFactory(CLASS, {}, {}) \
  void CLASS::describe(OFX::ImageEffectDescriptor &desc) { tiny::describe(desc, LABEL); } \
  void CLASS::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum) { tiny::describeInContext(desc); } \
  OFX::ImageEffect *CLASS::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum) { return new tiny::Effect(handle); }

#endif
