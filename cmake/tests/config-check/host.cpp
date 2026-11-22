// SPDX-License-Identifier: BSD-3-Clause
// A minimal host on HostSupport, for the link-time configuration check (NatronGitHub/openfx#226,
// OFXH_CONFIGURATION_NAME in ofxhImageEffect.h): it links with the definitions of
// openfx::HostSupport, and fails to link with any other set (see CMakeLists.txt).
// main() constructs the host, as every host does: the reference is in code that runs (a
// constructor that nothing calls could be removed by the linker, e.g. with -dead_strip, and
// the undefined symbol with it).
#include <cstdarg>
#include "ofxhImageEffect.h"
#include "ofxhPluginCache.h"

namespace ofxConfigCheckTest {

using OFX::Host::ImageEffect::Descriptor;
using OFX::Host::ImageEffect::ImageEffectPlugin;

class TestHost : public OFX::Host::ImageEffect::Host {
public:
  virtual OFX::Host::ImageEffect::Instance *newInstance(void *, ImageEffectPlugin *, Descriptor &,
                                                        const std::string &) { return NULL; }
  virtual Descriptor *makeDescriptor(ImageEffectPlugin *) { return NULL; }
  virtual Descriptor *makeDescriptor(const Descriptor &, ImageEffectPlugin *) { return NULL; }
  virtual Descriptor *makeDescriptor(const std::string &, ImageEffectPlugin *) { return NULL; }
  virtual OfxStatus vmessage(const char *, const char *, const char *, va_list) { return kOfxStatOK; }
  virtual OfxStatus setPersistentMessage(const char *, const char *, const char *, va_list) { return kOfxStatOK; }
  virtual OfxStatus clearPersistentMessage() { return kOfxStatOK; }
#ifdef OFX_SUPPORTS_MULTITHREAD
  virtual OfxStatus multiThread(OfxThreadFunctionV1, unsigned int, void *) { return kOfxStatFailed; }
  virtual OfxStatus multiThreadNumCPUS(unsigned int *n) const { *n = 1; return kOfxStatOK; }
  virtual OfxStatus multiThreadIndex(unsigned int *i) const { *i = 0; return kOfxStatOK; }
  virtual int multiThreadIsSpawnedThread() const { return 0; }
  virtual OfxStatus mutexCreate(OfxMutexHandle *, int) { return kOfxStatFailed; }
  virtual OfxStatus mutexDestroy(const OfxMutexHandle) { return kOfxStatFailed; }
  virtual OfxStatus mutexLock(const OfxMutexHandle) { return kOfxStatFailed; }
  virtual OfxStatus mutexUnLock(const OfxMutexHandle) { return kOfxStatFailed; }
  virtual OfxStatus mutexTryLock(const OfxMutexHandle) { return kOfxStatFailed; }
#endif
#ifdef OFX_SUPPORTS_DIALOG
  virtual OfxStatus requestDialog(OfxImageEffectHandle, OfxPropertySetHandle, void *) { return kOfxStatFailed; }
  virtual OfxStatus notifyRedrawPending(OfxImageEffectHandle, OfxPropertySetHandle) { return kOfxStatFailed; }
#endif
#ifdef OFX_SUPPORTS_OPENGLRENDER
  virtual OfxStatus flushOpenGLResources() const { return kOfxStatFailed; }
#endif
};

} // namespace ofxConfigCheckTest

int main()
{
  ofxConfigCheckTest::TestHost host;
  OFX::Host::PluginCache::getPluginCache();
  return host.getHandle() != NULL ? 0 : 1;
}
