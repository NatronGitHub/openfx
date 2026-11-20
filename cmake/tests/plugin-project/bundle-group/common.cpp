// SPDX-License-Identifier: BSD-3-Clause
// COMMON_SOURCES of the test groups: compiled once, in an OBJECT library
#include "tiny.h"

#ifdef TINY_C_ONLY
#error "a plug-in's COMPILE_DEFINITIONS leaked into COMMON_SOURCES"
#endif

namespace tiny {
void describe(OFX::ImageEffectDescriptor &desc, const char *label)
{
  desc.setLabel(label);
  desc.addSupportedContext(OFX::eContextFilter);
  desc.addSupportedBitDepth(OFX::eBitDepthFloat);
}

void describeInContext(OFX::ImageEffectDescriptor &desc)
{
  OFX::ClipDescriptor *src = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  src->addSupportedComponent(OFX::ePixelComponentRGBA);
  OFX::ClipDescriptor *dst = desc.defineClip(kOfxImageEffectOutputClipName);
  dst->addSupportedComponent(OFX::ePixelComponentRGBA);
}
} // namespace tiny

// exported by the TinyReg bundles only (COMMON_EXPORT_SYMBOLS)
extern "C" {
OfxExport int OfxTestGroupSymbol(void);
int OfxTestGroupSymbol(void) { return 1; }
}
