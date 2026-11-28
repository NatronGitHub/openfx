// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

////////////////////////////////////////////////////////////////////////////////
/// The regression checks of suiteHandleChecks: each one reproduces a bug that was found and fixed
/// in HostSupport, and names its issue or PR; it stays as long as the bug could come back. They
/// are linked into suiteHandleChecks and called by it where their state is ready (the param
/// descriptors of main(), the instance of Support/Plugins/Invert, the plug-in cache it scanned);
/// each one opens a RegressionSection, so its failures print "suiteHandleChecks: regression: ...".
/// The functional checks are in ../functional/suiteHandleChecks.cpp, the shared harness in
/// ../suiteHandleChecks.h.

#include <cfloat>
#include <climits>
#include <cstdlib>
#include <iterator> // std::next
#include <new>

#include "suiteHandleChecks.h"

#include "ofxMemory.h"
#include "ofxhClip.h"
#include "ofxhMemory.h"
#include "ofxhPluginAPICache.h"
#include "ofxhPluginCache.h"
#include "ofxhUtilities.h" // utf16_to_utf8 (Windows)
#ifdef OFX_EXTENSIONS_VEGAS
#include "ofxSonyVegas.h"
#endif
#if !defined(_WIN32)
#include <fcntl.h> // open (lowestFreeFd)
#include <unistd.h> // close
#endif

namespace SuiteHandleChecks {
namespace Regression {

  /// #200 (ecdce164; the NULL count from its review, 7cb7dc58): propGetDimension checks the
  /// magic number of the set, as the other property functions do: a handle that is not a property
  /// set (here zeroed memory), NULL, or a NULL count gives kOfxStatErrBadHandle, and the count is
  /// left alone
  void propGetDimension(const OfxPropertySuiteV1 *propSuite, OfxPropertySetHandle paramDescriptor)
  {
    RegressionSection section;
    std::vector<unsigned char> notASet(2 * sizeof(Property::Set), 0);
    int dim = -1;
    check("propGetDimension(NULL) = kOfxStatErrBadHandle", propSuite->propGetDimension(NULL, kOfxParamPropDefault, &dim) == kOfxStatErrBadHandle && dim == -1);
    check("propGetDimension(NULL count) = kOfxStatErrBadHandle", propSuite->propGetDimension(paramDescriptor, kOfxParamPropDefault, NULL) == kOfxStatErrBadHandle);
    check("propGetDimension(not a property set) = kOfxStatErrBadHandle",
          propSuite->propGetDimension((OfxPropertySetHandle)&notASet[0], kOfxParamPropDefault, &dim) == kOfxStatErrBadHandle && dim == -1);
    check("propGetDimension(param descriptor, kOfxParamPropDefault) = kOfxStatOK",
          propSuite->propGetDimension(paramDescriptor, kOfxParamPropDefault, &dim) == kOfxStatOK && dim == 1);
    check("propGetDimension(param descriptor, unknown property) = kOfxStatErrUnknown",
          propSuite->propGetDimension(paramDescriptor, "OfxNoSuchProperty", &dim) == kOfxStatErrUnknown);
  }

  ////////////////////////////////////////////////////////////////////////////////
  // Bytes param defaults

  /// a Bytes instance that records what setDefault() sees, as a host that overrides it (Natron)
  class TestBytes : public Param::BytesInstance {
  public:
    int setDefaultCalls;
    const OfxBytes *seenDefault;              ///< kOfxParamPropDefault in the last setDefault()
    std::vector<unsigned char> seenGetDefault; ///< getDefault() in the last setDefault()
    TestBytes(Param::Descriptor &d, Param::SetInstance *s) : Param::BytesInstance(d, s), setDefaultCalls(0), seenDefault(NULL) {}
    virtual void setDefault()
    {
      ++setDefaultCalls;
      seenDefault = (const OfxBytes *)getProperties().getPointerProperty(kOfxParamPropDefault);
      seenGetDefault = getDefault();
    }
    virtual OfxStatus get(std::vector<unsigned char> &v) { v.clear(); return kOfxStatOK; }
    virtual OfxStatus get(OfxTime, std::vector<unsigned char> &v) { v.clear(); return kOfxStatOK; }
    virtual OfxStatus set(const unsigned char *, size_t) { return kOfxStatOK; }
    virtual OfxStatus set(OfxTime, const unsigned char *, size_t) { return kOfxStatOK; }
  };

  /// the OfxBytes of kOfxParamPropDefault, through the property suite
  const OfxBytes gGetFailed = { NULL, 0 }; ///< what getBytesDefault() returns if propGetPointer fails
  const OfxBytes *getBytesDefault(const OfxPropertySuiteV1 *propSuite, OfxPropertySetHandle h)
  {
    void *p = NULL;
    if (propSuite->propGetPointer(h, kOfxParamPropDefault, 0, &p) != kOfxStatOK) {
      return &gGetFailed; // never a valid default
    }
    return (const OfxBytes *)p;
  }

  bool sameBytes(const OfxBytes *b, const unsigned char *data, size_t length)
  {
    return b && b->length == length && b->data && std::memcmp(b->data, data, length) == 0;
  }

  /// the Bytes default checks, on a descriptor (bytes == NULL) or on an instance
  void checkBytesDefault(const OfxPropertySuiteV1 *propSuite, OfxPropertySetHandle h, const char *what, TestBytes *bytes)
  {
    const unsigned char orig[4] = { 1, 2, 3, 4 };
    const OfxBytes *stackAddress = NULL;
    const unsigned char *bufAddress = NULL;
    int calls = bytes ? bytes->setDefaultCalls : 0;
    {
      unsigned char buf[4];
      std::memcpy(buf, orig, sizeof(buf));
      OfxBytes stackBytes = { buf, sizeof(buf) };
      stackAddress = &stackBytes;
      bufAddress = buf;
      check((std::string(what) + ": propSetPointer(default, stack OfxBytes) = kOfxStatOK").c_str(),
            propSuite->propSetPointer(h, kOfxParamPropDefault, 0, &stackBytes) == kOfxStatOK);
      // the plug-in reuses its buffer
      std::memset(buf, 0xff, sizeof(buf));
      stackBytes.data = NULL;
      stackBytes.length = 0;
    }
    const OfxBytes *def = getBytesDefault(propSuite, h);
    check((std::string(what) + ": the default is a copy, not the plug-in's OfxBytes").c_str(),
          def != NULL && def != stackAddress && def->data != bufAddress);
    check((std::string(what) + ": the copy has the original length and bytes").c_str(), sameBytes(def, orig, sizeof(orig)));
    if (bytes) {
      check((std::string(what) + ": getDefault() has the original bytes").c_str(),
            bytes->getDefault().size() == sizeof(orig) && std::memcmp(&bytes->getDefault()[0], orig, sizeof(orig)) == 0);
      check((std::string(what) + ": setDefault() called once, and sees the copy").c_str(),
            bytes->setDefaultCalls == calls + 1 && bytes->seenDefault == def && bytes->seenGetDefault == bytes->getDefault());
    }

    // the plug-in may pass a pointer into the host's copy (alias-safe copy)
    if (def && def->data && def->length == sizeof(orig)) {
      OfxBytes alias = { def->data + 1, 2 };
      check((std::string(what) + ": propSetPointer(default, into the host's copy) = kOfxStatOK").c_str(),
            propSuite->propSetPointer(h, kOfxParamPropDefault, 0, &alias) == kOfxStatOK);
      check((std::string(what) + ": a default into the host's copy is copied").c_str(),
            sameBytes(getBytesDefault(propSuite, h), orig + 1, 2) && (!bytes || bytes->getDefault().size() == 2));
    }

    check((std::string(what) + ": propSetPointer(default, NULL) = kOfxStatOK").c_str(),
          propSuite->propSetPointer(h, kOfxParamPropDefault, 0, NULL) == kOfxStatOK);
    check((std::string(what) + ": a NULL default stays NULL").c_str(), getBytesDefault(propSuite, h) == NULL && (!bytes || bytes->getDefault().empty()));

    unsigned char buf2[2] = { 5, 6 };
    OfxBytes b2 = { buf2, sizeof(buf2) };
    check((std::string(what) + ": propReset(default) = kOfxStatOK").c_str(),
          propSuite->propSetPointer(h, kOfxParamPropDefault, 0, &b2) == kOfxStatOK &&
          sameBytes(getBytesDefault(propSuite, h), buf2, sizeof(buf2)) &&
          propSuite->propReset(h, kOfxParamPropDefault) == kOfxStatOK);
    check((std::string(what) + ": propReset gives a NULL default").c_str(), getBytesDefault(propSuite, h) == NULL && (!bytes || bytes->getDefault().empty()));

    OfxBytes noData = { NULL, 4 };
    check((std::string(what) + ": propSetPointer(default, {NULL, 4}) = kOfxStatErrValue").c_str(),
          propSuite->propSetPointer(h, kOfxParamPropDefault, 0, &b2) == kOfxStatOK &&
          propSuite->propSetPointer(h, kOfxParamPropDefault, 0, &noData) == kOfxStatErrValue);
    check((std::string(what) + ": {NULL, 4} gives a NULL default").c_str(), getBytesDefault(propSuite, h) == NULL && (!bytes || bytes->getDefault().empty()));
    if (bytes) {
      check((std::string(what) + ": {NULL, 4}: setDefault() sees the NULL default").c_str(), bytes->seenDefault == NULL && bytes->seenGetDefault.empty());
    }
  }

  /// #191, for #188: Bytes params (OFX 1.5) copy their default, which the plug-in owns (the
  /// descriptor kept the plug-in's pointer, which dangled once the plug-in reused its buffer).
  /// The C++ API, as the param suite's paramDefine refuses Bytes if the host doesn't support them.
  void bytesDefault(const OfxPropertySuiteV1 *propSuite, TestSetDescriptor &setDesc, TestSetInstance &setInst)
  {
    RegressionSection section;
    Param::Descriptor *bytesDesc = setDesc.paramDefine(kOfxParamTypeBytes, "bytes");
    check("paramDefine(Bytes)", bytesDesc != NULL);
    if (!propSuite || !bytesDesc) {
      return;
    }
    checkBytesDefault(propSuite, bytesDesc->getPropHandle(), "Bytes descriptor", NULL);

    // an instance copies the descriptor's default, and points its property to its own copy
    const unsigned char descBytes[3] = { 7, 8, 9 };
    OfxBytes descDefault = { descBytes, sizeof(descBytes) };
    check("Bytes descriptor: propSetPointer(default) = kOfxStatOK",
          propSuite->propSetPointer(bytesDesc->getPropHandle(), kOfxParamPropDefault, 0, &descDefault) == kOfxStatOK);
    const OfxBytes *descCopy = getBytesDefault(propSuite, bytesDesc->getPropHandle());
    TestBytes bytesInst(*bytesDesc, &setInst);
    const OfxBytes *instDefault = getBytesDefault(propSuite, bytesInst.getPropHandle());
    check("Bytes instance: the default is the instance's copy",
          instDefault != NULL && instDefault != descCopy && instDefault != &descDefault && instDefault->data != descCopy->data &&
          sameBytes(instDefault, descBytes, sizeof(descBytes)) &&
          bytesInst.getDefault().size() == sizeof(descBytes) && std::memcmp(&bytesInst.getDefault()[0], descBytes, sizeof(descBytes)) == 0);
    check("Bytes instance: setDefault() not called by the constructor", bytesInst.setDefaultCalls == 0);
    checkBytesDefault(propSuite, bytesInst.getPropHandle(), "Bytes instance", &bytesInst);
    check("Bytes instance: the descriptor's default is unchanged", sameBytes(getBytesDefault(propSuite, bytesDesc->getPropHandle()), descBytes, sizeof(descBytes)));

    // an instance of a descriptor with a NULL default has a NULL default
    check("Bytes descriptor: propSetPointer(default, NULL) = kOfxStatOK",
          propSuite->propSetPointer(bytesDesc->getPropHandle(), kOfxParamPropDefault, 0, NULL) == kOfxStatOK);
    TestBytes nullInst(*bytesDesc, &setInst);
    check("Bytes instance of a NULL default: NULL", getBytesDefault(propSuite, nullInst.getPropHandle()) == NULL && nullInst.getDefault().empty());
  }

  /// #232 (1bca1a61; the instance rows moved by #356): OfxImageEffectSuiteV1::abort returns "0 if
  /// the effect should continue [...] 1 if the effect should abort" (ofxImageEffect.h), not a
  /// status: one table of handles and of what the host's Instance::abort() returns (-1: it
  /// throws). A handle that is not an effect instance (NULL, a descriptor) gives 1.
  void abort(MyHost::Host &host, OfxImageEffectHandle descriptor, TestEffectInstance *instance)
  {
    RegressionSection section;
    const OfxImageEffectSuiteV1 *ies = (const OfxImageEffectSuiteV1 *)host.fetchSuite(kOfxImageEffectSuite, 1);
    check("image effect suite (abort)", ies != NULL);
    if (!ies) {
      return;
    }
    enum Handle { kNull, kDescriptor, kInstance };
    static const struct { Handle handle; int value, expected; const char *what; } aborts[] = {
      { kNull, 0, 1, "abort(NULL) = 1" },
      { kDescriptor, 0, 1, "abort(descriptor) = 1" },
      { kInstance, 0, 0, "abort(instance), Instance::abort() = 0: 0" },
      { kInstance, 1, 1, "abort(instance), Instance::abort() = 1: 1" },
      { kInstance, 2, 1, "abort(instance), Instance::abort() = 2: 1" },
      { kInstance, -1, 1, "abort(instance), Instance::abort() throws: 1" },
    };
    for (const auto &a : aborts) {
      if (a.handle == kInstance && !instance) {
        continue; // (no plug-in instance: checkEffectInstance() reports why)
      }
      if (a.handle == kInstance) {
        instance->abortValue = a.value;
      }
      const OfxImageEffectHandle h = a.handle == kInstance ? instance->getHandle() : a.handle == kDescriptor ? descriptor : NULL;
      check(a.what, ies->abort(h) == a.expected);
    }
    if (instance) {
      instance->abortValue = 0;
    }
  }

  ////////////////////////////////////////////////////////////////////////////////
  // std::bad_alloc from the host

  /// a clip instance whose image and texture fetches fail with std::bad_alloc, as a host's do
  /// when it runs out of memory
  class OutOfMemoryClip : public ImageEffect::ClipInstance {
    std::string _s;
  public:
    OutOfMemoryClip(ImageEffect::ClipDescriptor &d) : ImageEffect::ClipInstance(NULL, d), _s(kOfxBitDepthFloat) {}
    virtual const std::string &getUnmappedBitDepth() const { return _s; }
    virtual const std::string &getUnmappedComponents() const { return _s; }
    virtual const std::string &getPremult() const { return _s; }
#ifdef OFX_EXTENSIONS_NATRON
    virtual OfxRectI getFormat() const { OfxRectI r = { 0, 0, 1, 1 }; return r; }
#endif
    virtual double getAspectRatio() const { return 1.; }
    virtual double getFrameRate() const { return 25.; }
    virtual void getFrameRange(double &s, double &e) const { s = 0.; e = 1.; }
    virtual const std::string &getFieldOrder() const { return _s; }
    virtual bool getConnected() const { return true; }
    virtual double getUnmappedFrameRate() const { return 25.; }
    virtual void getUnmappedFrameRange(double &s, double &e) const { s = 0.; e = 1.; }
    virtual bool getContinuousSamples() const { return false; }
    virtual ImageEffect::Image *getImage(OfxTime, const OfxRectD *) { throw std::bad_alloc(); }
#ifdef OFX_EXTENSIONS_NUKE
    virtual ImageEffect::Image *getImagePlane(OfxTime, int, const std::string &, const OfxRectD *) { throw std::bad_alloc(); }
    virtual OfxRectD getRegionOfDefinition(OfxTime, int) const { throw std::bad_alloc(); }
#endif
#ifdef OFX_SUPPORTS_OPENGLRENDER
    virtual ImageEffect::Texture *loadTexture(OfxTime, const char *, const OfxRectD *) { throw std::bad_alloc(); }
#endif
#ifdef OFX_EXTENSIONS_VEGAS
    virtual ImageEffect::Image *getStereoscopicImage(OfxTime, int, const OfxRectD *) { throw std::bad_alloc(); }
#endif
#if defined(OFX_EXTENSIONS_VEGAS) || defined(OFX_EXTENSIONS_NUKE)
    virtual void setView(int) {}
#endif
    virtual OfxRectD getRegionOfDefinition(OfxTime) const { throw std::bad_alloc(); }
  };

  /// image memory whose lock fails with std::bad_alloc (e.g. a host that pages it back in)
  class OutOfMemoryImageMemory : public Memory::Instance {
  public:
    virtual void lock() { throw std::bad_alloc(); }
  };

  /// #202 (fcc72d4d, 33bc9005): a std::bad_alloc from the host is kOfxStatErrMemory, and the
  /// returned handle or pointer NULL (it reached the plug-in as an exception, or as
  /// kOfxStatFailed with a dangling output): clipGetImage, clipGetRegionOfDefinition,
  /// imageMemoryLock, clipLoadTexture, the image plane suite V2 and the Vegas stereoscopic suite
  void badAlloc(MyHost::Host &host)
  {
    RegressionSection section;
    ImageEffect::ClipDescriptor desc(kOfxImageEffectSimpleSourceClipName);
    OutOfMemoryClip clip(desc);
    OfxImageClipHandle ch = clip.getHandle();
    const OfxRectD region = { 0., 0., 1., 1. };
    void *bad = &clip; // not NULL: the functions must set it to NULL
    const OfxImageEffectSuiteV1 *ies = (const OfxImageEffectSuiteV1 *)host.fetchSuite(kOfxImageEffectSuite, 1);
    check("image effect suite", ies != NULL);
    if (ies) {
      OfxPropertySetHandle img = (OfxPropertySetHandle)bad;
      check("clipGetImage(std::bad_alloc) = kOfxStatErrMemory", ies->clipGetImage(ch, 0., &region, &img) == kOfxStatErrMemory && img == NULL);
      OfxRectD rod = region;
      check("clipGetRegionOfDefinition(std::bad_alloc) = kOfxStatErrMemory", ies->clipGetRegionOfDefinition(ch, 0., &rod) == kOfxStatErrMemory);
      OutOfMemoryImageMemory mem;
      void *ptr = bad;
      check("imageMemoryLock(std::bad_alloc) = kOfxStatErrMemory", ies->imageMemoryLock(mem.getHandle(), &ptr) == kOfxStatErrMemory && ptr == NULL);
    }
#ifdef OFX_SUPPORTS_OPENGLRENDER
    const OfxImageEffectOpenGLRenderSuiteV1 *gl = (const OfxImageEffectOpenGLRenderSuiteV1 *)host.fetchSuite(kOfxOpenGLRenderSuite, 1);
    check("OpenGL render suite", gl != NULL);
    if (gl) {
      OfxPropertySetHandle tex = (OfxPropertySetHandle)bad;
      check("clipLoadTexture(std::bad_alloc) = kOfxStatErrMemory", gl->clipLoadTexture(ch, 0., NULL, &region, &tex) == kOfxStatErrMemory && tex == NULL);
    }
#endif
#ifdef OFX_EXTENSIONS_NUKE
    const FnOfxImageEffectPlaneSuiteV2 *planes = (const FnOfxImageEffectPlaneSuiteV2 *)host.fetchSuite(kFnOfxImageEffectPlaneSuite, 2);
    check("image plane suite V2", planes != NULL);
    if (planes) {
      OfxPropertySetHandle img = (OfxPropertySetHandle)bad;
      check("clipGetImagePlane(std::bad_alloc) = kOfxStatErrMemory",
            planes->clipGetImagePlane(ch, 0., 0, kFnOfxImagePlaneColour, &region, &img) == kOfxStatErrMemory && img == NULL);
      OfxRectD rod = region;
      check("clipGetRegionOfDefinition V2 (view)(std::bad_alloc) = kOfxStatErrMemory", planes->clipGetRegionOfDefinition(ch, 0., 0, &rod) == kOfxStatErrMemory);
    }
#endif
#ifdef OFX_EXTENSIONS_VEGAS
    const OfxVegasStereoscopicImageSuiteV1 *stereo =
      (const OfxVegasStereoscopicImageSuiteV1 *)host.fetchSuite(kOfxVegasStereoscopicImageEffectSuite, 1);
    check("Vegas stereoscopic image suite", stereo != NULL);
    if (stereo) {
      OfxRectD r = region;
      OfxPropertySetHandle img = (OfxPropertySetHandle)bad;
      check("clipGetStereoscopicImage(std::bad_alloc) = kOfxStatErrMemory",
            stereo->clipGetStereoscopicImage(ch, 0., 0, &r, &img) == kOfxStatErrMemory && img == NULL);
    }
#endif
  }

  /// #204 (a31bf5a8, 3ec41168): memoryFree of a pointer that memoryAlloc didn't return is
  /// kOfxStatErrBadHandle ("allocatedData was not a valid pointer returned by
  /// OfxMemorySuiteV1::memoryAlloc", ofxMemory.h), instead of free()ing it: a pointer inside a
  /// block, and one into a buffer of the plug-in's (the 64-byte "header" before each pointer is
  /// zeroed memory the test owns)
  void memoryFree(MyHost::Host &host)
  {
    RegressionSection section;
    const OfxMemorySuiteV1 *mem = (const OfxMemorySuiteV1 *)host.fetchSuite(kOfxMemorySuite, 1);
    check("memory suite", mem != NULL);
    if (!mem) {
      return;
    }
    void *p = NULL;
    check("memoryAlloc(100) = kOfxStatOK", mem->memoryAlloc(NULL, 100, &p) == kOfxStatOK && p != NULL);
    if (p) {
      std::memset(p, 0, 100);
      // 64 bytes into the block, not returned by memoryAlloc: its "header" is p[0..63], zeroed above
      check("memoryFree(inside a block) = kOfxStatErrBadHandle", mem->memoryFree((char *)p + 64) == kOfxStatErrBadHandle);
      check("memoryFree(memoryAlloc's pointer) = kOfxStatOK", mem->memoryFree(p) == kOfxStatOK);
    }
    // 64 bytes into a zeroed buffer: the 64-byte "header" before it is inside the buffer
    std::vector<double> notABlock(16, 0.);
    check("memoryFree(not from memoryAlloc) = kOfxStatErrBadHandle", mem->memoryFree(&notABlock[8]) == kOfxStatErrBadHandle);
  }

  /// the host descriptor lists the pixel depths, components and contexts that hostDemo supports
  void checkHostDescriptor(MyHost::Host &host, const std::string &profile)
  {
    Property::Set &p = host.getProperties();
    const std::string what = "host descriptor (" + profile + "): ";
    check((what + "kOfxImageEffectPropSupportedPixelDepths is not empty").c_str(),
          p.getDimension(kOfxImageEffectPropSupportedPixelDepths) > 0);
    const char *const depths[] = { kOfxBitDepthByte, kOfxBitDepthShort, kOfxBitDepthHalf, kOfxBitDepthFloat };
    for (size_t i = 0; i < sizeof(depths) / sizeof(depths[0]); ++i) {
      const bool listed = p.findStringPropValueIndex(kOfxImageEffectPropSupportedPixelDepths, depths[i]) != -1;
      check((what + depths[i] + " is listed if and only if MyImage makes it").c_str(),
            listed == MyHost::MyImage::isSupported(depths[i], kOfxImageComponentRGBA));
    }
    const int nComps = p.getDimension(kOfxImageEffectPropSupportedComponents);
    check((what + "kOfxImageEffectPropSupportedComponents is not empty").c_str(), nComps > 0);
    for (int i = 0; i < nComps; ++i) {
      const std::string c = p.getStringProperty(kOfxImageEffectPropSupportedComponents, i);
      check((what + "MyImage makes the listed components " + c).c_str(),
            MyHost::MyImage::isSupported(kOfxBitDepthFloat, c));
    }
    check((what + "RGBA and Alpha are listed").c_str(),
          p.findStringPropValueIndex(kOfxImageEffectPropSupportedComponents, kOfxImageComponentRGBA) != -1 &&
          p.findStringPropValueIndex(kOfxImageEffectPropSupportedComponents, kOfxImageComponentAlpha) != -1);
    check((what + "kOfxImageEffectPropSupportedContexts lists filter and general (-c)").c_str(),
          p.findStringPropValueIndex(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextFilter) != -1 &&
          p.findStringPropValueIndex(kOfxImageEffectPropSupportedContexts, kOfxImageEffectContextGeneral) != -1);
  }

  /// #213, for openfx-misc#222 (HostSupport left them empty): the host descriptor lists the pixel
  /// depths, components and contexts that hostDemo supports: the depths and components of
  /// MyImage, and the contexts of -c; in the default profile and with --ofx14 (they are OFX 1.4
  /// host properties)
  void hostDescriptor(MyHost::Host &host)
  {
    RegressionSection section;
    checkHostDescriptor(host, "default");
    MyHost::Host host14;
    host14.restrictToOFX14();
    checkHostDescriptor(host14, "ofx14");
  }

  ////////////////////////////////////////////////////////////////////////////////
  // properties and the plug-in cache

  /// the malformed properties of a plug-in cache: one table for the element reader
  /// (APICache::propertySetXMLRead, propertyValues()) and for readCache() of a whole cache
  /// (pluginCacheRescanAndRead()). Each case is an error, not an assert nor a huge allocation
  /// (#218); cacheIndex: the value index that replaces the first index="0" of the cache's
  /// kOfxImageEffectPropSupportedContexts (a variable-dimension property), or NULL; bogusElement:
  /// <bogus/> in the cache's <apiproperties>
  struct MalformedCase {
    const char *what;
    const char *dimension, *index, *element; ///< for the element reader
    const char *cacheIndex;
    bool bogusElement;
  };
  const MalformedCase kMalformed[] = {
    { "a variable-dimension property value at index 1000000000", "0", "1000000000", "value", "1000000000", false },
    { "a variable-dimension property value at index 2000000000", "0", "2000000000", "value", "2000000000", false },
    { "a variable-dimension property value at index 1 before index 0", "0", "1", "value", "1", false },
    { "a 1-dimension property value at index 1", "1", "1", "value", NULL, false },
    { "a property value at index 99999999999999999999", "0", "99999999999999999999", "value", "99999999999999999999", false },
    { "a property value at index \"x\"", "0", "x", "value", "x", false },
    { "an unknown element", "0", "0", "bogus", NULL, true },
  };

  /// #218 (bec213bb, a C++ Core Guidelines review): property values: a pointer property defaults
  /// to NULL, whatever the default string of its spec; doubles turn into strings that read back
  /// exactly (the plug-in cache, and the numeric param limits); the numeric params' limits are
  /// the type's; a malformed property in the cache is an error (kMalformed)
  void propertyValues()
  {
    RegressionSection section;
    static const Property::PropSpec pointerSpec[] = {
      { "TestPointerZero", Property::ePointer, 1, true, "0" },
      { "TestPointerNull", Property::ePointer, 1, false, NULL },
      Property::propSpecEnd
    };
    Property::Set pointers(pointerSpec);
    check("a pointer property with the default \"0\" is NULL", pointers.getPointerProperty("TestPointerZero") == NULL);
    check("a pointer property with no default is NULL", pointers.getPointerProperty("TestPointerNull") == NULL);

    const double values[] = { DBL_MAX, -DBL_MAX, DBL_MIN, -DBL_MIN, 0.1, 1. / 3., 5e-324 };
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
      const std::string s = Property::castToString(values[i]);
      check(("castToString(" + s + ") reads back exactly (stringToDouble)").c_str(), Property::stringToDouble(s) == values[i]);
      check(("castToString(" + s + ") reads back exactly (atof)").c_str(), std::atof(s.c_str()) == values[i]);
    }

    // a double param's limits are exactly -DBL_MAX and DBL_MAX, an int param's INT_MIN and INT_MAX
    Param::Descriptor dbl(kOfxParamTypeDouble, "dbl");
    Param::Descriptor dbl2(kOfxParamTypeDouble2D, "dbl2");
    Param::Descriptor integer(kOfxParamTypeInteger, "int");
    dbl.addStandardParamProps(kOfxParamTypeDouble);
    dbl2.addStandardParamProps(kOfxParamTypeDouble2D);
    integer.addStandardParamProps(kOfxParamTypeInteger);
    const Property::Set &dp = dbl.getProperties();
    const Property::Set &dp2 = dbl2.getProperties();
    const Property::Set &ip = integer.getProperties();
    check("double param: kOfxParamPropMax == DBL_MAX, kOfxParamPropMin == -DBL_MAX",
          dp.getDoubleProperty(kOfxParamPropMax) == DBL_MAX && dp.getDoubleProperty(kOfxParamPropMin) == -DBL_MAX);
    check("double param: kOfxParamPropDisplayMax == DBL_MAX, kOfxParamPropDisplayMin == -DBL_MAX",
          dp.getDoubleProperty(kOfxParamPropDisplayMax) == DBL_MAX && dp.getDoubleProperty(kOfxParamPropDisplayMin) == -DBL_MAX);
    check("double 2D param: kOfxParamPropMax[1] == DBL_MAX, kOfxParamPropMin[1] == -DBL_MAX",
          dp2.getDoubleProperty(kOfxParamPropMax, 1) == DBL_MAX && dp2.getDoubleProperty(kOfxParamPropMin, 1) == -DBL_MAX);
    check("int param: kOfxParamPropMax == INT_MAX, kOfxParamPropMin == INT_MIN",
          ip.getIntProperty(kOfxParamPropMax) == INT_MAX && ip.getIntProperty(kOfxParamPropMin) == INT_MIN);

    // the element reader: a value index beyond the values read so far (a variable dimension) or
    // the dimension (a fixed one), a bad index, or an unknown element, is an error
    for (const MalformedCase &c : kMalformed) {
      Property::Set set;
      Property::Property *current = NULL;
      std::map<std::string, std::string> prop, value;
      prop["name"] = "TestStrings";
      prop["type"] = "string";
      prop["dimension"] = c.dimension;
      value["index"] = c.index;
      value["value"] = "v";
      bool refused = false;
      try {
        APICache::propertySetXMLRead("property", prop, set, current);
        APICache::propertySetXMLRead(c.element, value, set, current);
      } catch (const std::runtime_error &) {
        refused = true;
      }
      check((std::string("plug-in cache: ") + c.what + " is an error").c_str(),
            refused && (!current || current->getDimension() <= 1));
    }
    {
      Property::Set set;
      Property::Property *current = NULL;
      std::map<std::string, std::string> prop, value;
      prop["name"] = "TestDoubles";
      prop["type"] = "double";
      prop["dimension"] = "0";
      bool ok = true;
      try {
        APICache::propertySetXMLRead("property", prop, set, current);
        for (int i = 0; i < 3; ++i) {
          value["index"] = Property::castToString(i);
          value["value"] = Property::castToString(values[i]); // as propertySetXMLWrite
          APICache::propertySetXMLRead("value", value, set, current);
        }
      } catch (...) {
        ok = false;
      }
      check("plug-in cache: DBL_MAX, -DBL_MAX and DBL_MIN read back exactly",
            ok && set.getDimension("TestDoubles") == 3 && set.getDoubleProperty("TestDoubles", 0) == DBL_MAX &&
            set.getDoubleProperty("TestDoubles", 1) == -DBL_MAX && set.getDoubleProperty("TestDoubles", 2) == DBL_MIN);
    }
  }

  /// an int property that counts its live copies, and whose deepCopy() can throw
  /// (inside the class, Property names the base class OFX::Host::Property::Property)
  typedef OFX::Host::Property::Int IntProperty;
  class CountedInt : public IntProperty {
  public:
    static int live;
    bool throwOnCopy;
    CountedInt(const std::string &name, bool throws)
      : IntProperty(name, 1, false, 0), throwOnCopy(throws) { ++live; }
    CountedInt(const CountedInt &other) : IntProperty(other), throwOnCopy(other.throwOnCopy) { ++live; }
    virtual ~CountedInt() { --live; }
    virtual CountedInt *deepCopy() const
    {
      if (throwOnCopy) {
        throw std::bad_alloc();
      }
      return new CountedInt(*this);
    }
  };
  int CountedInt::live = 0;

  /// #232: the copy of a property set that throws midway deletes the copies that it made (the
  /// properties are copied in the order of their names: C throws, after A and B)
  void setCopyThrows()
  {
    RegressionSection section;
    {
      Property::Set set;
      set.addProperty(new CountedInt("A", false));
      set.addProperty(new CountedInt("B", false));
      set.addProperty(new CountedInt("C", true));
      bool thrown = false;
      try {
        Property::Set copy(set);
      } catch (const std::bad_alloc &) {
        thrown = true;
      }
      check("Set(const Set&): a property copy that throws: the exception reaches the caller, and no copy leaks",
            thrown && CountedInt::live == 3);
      Property::Set noThrow;
      noThrow.addProperty(new CountedInt("A", false));
      {
        Property::Set copy(noThrow);
        check("Set(const Set&) copies the properties", CountedInt::live == 5 && copy.fetchProperty("A") != NULL &&
              copy.fetchProperty("A") != noThrow.fetchProperty("A"));
      }
    }
    check("the properties are deleted with their sets", CountedInt::live == 0);
  }

  /// #229 (97094174): the labels of an image effect, through the C++ getters, fall back to the
  /// label, then to the name (the short and long labels gave "" when only the label was set). The
  /// functional checks of the labels through the property suite are checkLabels()
  /// (functional/suiteHandleChecks.cpp).
  void effectLabels()
  {
    RegressionSection section;
    static const Property::PropSpec spec[] = {
      { kOfxPropName, Property::eString, 1, false, "name" },
      { kOfxPropLabel, Property::eString, 1, false, "" },
      { kOfxPropShortLabel, Property::eString, 1, false, "" },
      { kOfxPropLongLabel, Property::eString, 1, false, "" },
      Property::propSpecEnd
    };
    ImageEffect::Base effect(spec);
    check("effect labels: no label, the name",
          effect.getLabel() == "name" && effect.getShortLabel() == "name" && effect.getLongLabel() == "name");
    effect.getProps().setStringProperty(kOfxPropLabel, "label");
    check("effect labels: a label, no short or long label: the label",
          effect.getLabel() == "label" && effect.getShortLabel() == "label" && effect.getLongLabel() == "label");
    effect.getProps().setStringProperty(kOfxPropShortLabel, "short");
    effect.getProps().setStringProperty(kOfxPropLongLabel, "long");
    check("effect labels: the short and long labels",
          effect.getLabel() == "label" && effect.getShortLabel() == "short" && effect.getLongLabel() == "long");
  }

  /// #324 (#339): setPluginHostPath(hostId) adds the host's own directories, .../OFX/hostId, and
  /// not the standard one, .../OFX/Plugins, on every OS; on Windows, getStdOFXPluginPath(hostId)
  void pluginHostPath()
  {
    RegressionSection section;
    const auto endsWith = [](const std::string &s, const std::string &end) {
      return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
    };
    const std::string hostId = "SuiteHandleChecksHost";
#if defined(WINDOWS)
    const std::string suffix = "\\OFX\\" + hostId;
    // (the default hostId first, as in a host: the PluginCache constructor)
    const std::string stdPath = OFX::utf16_to_utf8(PluginCache::getStdOFXPluginPath());
    check("getStdOFXPluginPath() ends with \\OFX\\Plugins", endsWith(stdPath, "\\OFX\\Plugins"));
    check("getStdOFXPluginPath(hostId) is getStdOFXPluginPath() with hostId instead of Plugins",
          OFX::utf16_to_utf8(PluginCache::getStdOFXPluginPath(hostId)) == stdPath.substr(0, stdPath.size() - std::strlen("Plugins")) + hostId);
#else
    const std::string suffix = "/OFX/" + hostId;
#endif
    PluginCache pc; // (not the singleton)
    const std::list<std::string> before = pc.getPluginPath();
    pc.setPluginHostPath(hostId);
    const std::list<std::string> &after = pc.getPluginPath();
    check("setPluginHostPath(hostId) adds directories", after.size() > before.size());
    bool allHost = true;
    for (std::list<std::string>::const_iterator it = std::next(after.begin(), before.size()); it != after.end(); ++it) {
      if (!endsWith(*it, suffix)) {
        std::printf("suiteHandleChecks: setPluginHostPath(\"%s\") added %s\n", hostId.c_str(), it->c_str());
        allHost = false;
      }
    }
    check("setPluginHostPath(hostId) adds only .../OFX/hostId directories", allHost);
  }

  /// the number of image effect plug-ins of a plug-in cache with this identifier
  int countPlugins(const std::list<Plugin *> &plugins, const std::string &id)
  {
    int n = 0;
    for (std::list<Plugin *>::const_iterator it = plugins.begin(); it != plugins.end(); ++it) {
      n += ((*it)->getIdentifier() == id);
    }
    return n;
  }

  /// a PluginCache other than the singleton, with its image effect API cache (destroyed after the
  /// plug-ins of cache). registerInCache() sets ImageEffect::gImageEffectHost: the destructor
  /// restores it (the same host).
  struct ScratchCache {
    MyHost::Host &host;
    ImageEffect::PluginCache effects;
    PluginCache cache;
    explicit ScratchCache(MyHost::Host &h) : host(h), effects(&h) { effects.registerInCache(cache); }
    ~ScratchCache() { ImageEffect::gImageEffectHost = &host; }
    ScratchCache(const ScratchCache &) = delete;
    ScratchCache &operator=(const ScratchCache &) = delete;
    /// readCache(text): false if it threw std::runtime_error (a malformed cache), whose message is
    /// printed unless quiet
    bool read(const std::string &text, bool quiet = false)
    {
      std::istringstream is(text);
      try {
        cache.readCache(is);
      } catch (const std::runtime_error &e) {
        if (!quiet) {
          std::printf("suiteHandleChecks: readCache: %s\n", e.what());
        }
        return false;
      }
      return true;
    }
    /// writePluginCache()
    std::string written()
    {
      std::ostringstream os;
      cache.writePluginCache(os);
      return os.str();
    }
  };

  /// #218 (bec213bb, f53333b2): a binary changed since the cache (another mtime) is loaded by the
  /// first scan, and not again by the second one: the plug-in stays the same, and isn't added
  /// twice. Run when no other plug-in cache has the binary loaded (two caches loading one library
  /// would share its Load/Unload state).
  void changedBinary(MyHost::Host &host, const CacheSnapshot &snapshot, const std::string &pluginId)
  {
    RegressionSection section;
    const int nInvert = snapshot.nInvert;
    std::string changed = snapshot.xml;
    const size_t bundle = snapshot.bundle.empty() ? std::string::npos : changed.find(snapshot.bundle);
    const size_t mtime = bundle == std::string::npos ? bundle : changed.find("mtime=\"", bundle);
    const size_t end = mtime == std::string::npos ? mtime : changed.find('"', mtime + strlen("mtime=\""));
    check("the cache has the mtime of the Invert binary", end != std::string::npos);
    if (end != std::string::npos) {
      changed.replace(mtime, end + 1 - mtime, "mtime=\"1\"");
      ScratchCache other(host);
      const bool ok = other.read(changed);
      other.cache.scanPluginFiles();
      ImageEffect::ImageEffectPlugin *first = other.effects.getPluginById(pluginId);
      const size_t n = other.cache.getPlugins().size();
      check("a changed binary: the cache is read, the first scan loads its plug-in, and the cache is dirty",
            ok && first != NULL && other.cache.dirty() && countPlugins(other.cache.getPlugins(), pluginId) == nInvert);
      other.cache.scanPluginFiles();
      // other.effects.getPlugins().size() == n holds because image effects are the only API registered here
      check("a changed binary: the second scan doesn't load it again (same plug-in, same count)",
            other.effects.getPluginById(pluginId) == first && other.cache.getPlugins().size() == n &&
            countPlugins(other.cache.getPlugins(), pluginId) == nInvert && other.effects.getPlugins().size() == n);
    }
  }

  /// #235 (2a935eac, 463449ae): a binary of the plug-in cache that can't be stat()ed (e.g.
  /// removed) is invalid, and the plug-ins the cache has for it are not used, even if the cache
  /// recorded the mtime and size that a failed stat() gives (0); the cache converges
  void cachedInvalidBinary(MyHost::Host &host, const CacheSnapshot &snapshot)
  {
    const std::string &cache = snapshot.xml;
    RegressionSection section;
    {
      PluginBinary pb("/nonexistent/x.ofx.bundle/Contents/MacOS/x.ofx", "/nonexistent/x.ofx.bundle", 0, 0);
      check("a cached binary that doesn't exist (mtime 0, size 0) is invalid, and not changed", pb.isInvalid() && !pb.hasBinaryChanged());
      PluginBinary pb2("/nonexistent/x.ofx.bundle/Contents/MacOS/x.ofx", "/nonexistent/x.ofx.bundle", 1, 2);
      check("a cached binary that doesn't exist (another mtime and size) is invalid, and changed", pb2.isInvalid() && pb2.hasBinaryChanged());
      // a binary cached valid that can no longer be stat()ed: its scan records the mtime and size
      // of a failed stat(), so that the cache is rewritten once, and then stays clean
      pb2.loadPluginInfo(NULL);
      check("a cached binary that doesn't exist: loadPluginInfo() records mtime 0 and size 0",
            pb2.getFileModificationTime() == 0 && pb2.getFileSize() == 0 && pb2.getNPlugins() == 0);
    }
    std::string missing = cache;
    const size_t invert = snapshot.bundle.empty() ? std::string::npos : missing.find(snapshot.bundle);
    const size_t bundle = invert == std::string::npos ? invert : missing.rfind("<bundle>", invert);
    const size_t path = bundle == std::string::npos ? bundle : missing.find(" path=\"", bundle);
    const size_t pathEnd = path == std::string::npos ? path : missing.find('"', path + strlen(" path=\""));
    const size_t mtime = pathEnd == std::string::npos ? pathEnd : missing.find(" mtime=\"", pathEnd);
    const size_t end = mtime == std::string::npos ? mtime : missing.find("/>", mtime);
    const size_t plugin = end == std::string::npos ? end : missing.find("<plugin ", end);
    const bool hasPlugin = plugin != std::string::npos && plugin < missing.find("</bundle>", end);
    check("the Invert <bundle> of the cache has a <binary> with a path and an mtime, and a <plugin>", hasPlugin);
    if (!hasPlugin) {
      return;
    }
    missing.replace(mtime, end - mtime, " mtime=\"0\" size=\"0\"");
    missing.replace(path, pathEnd + 1 - path, " path=\"/nonexistent/x.ofx.bundle/Contents/MacOS/x.ofx\"");
    ScratchCache other(host);
    const bool ok = other.read(missing);
    const std::string w = other.written();
    const size_t wBundle = w.find("/nonexistent/x.ofx.bundle/Contents/MacOS/x.ofx");
    const size_t wEnd = wBundle == std::string::npos ? wBundle : w.find("</bundle>", wBundle);
    check("a cached binary that doesn't exist (mtime 0, size 0): its cached plug-ins are not read",
          ok && wEnd != std::string::npos && w.substr(wBundle, wEnd - wBundle).find("<plugin ") == std::string::npos);
  }

  /// #218 (bec213bb, f53333b2): plug-in cache rescans and reads: a rescan doesn't duplicate the
  /// plug-ins (getPluginById() stable, also by (id, major)); the cache reads into the PluginCache
  /// that reads it, not the singleton; and a malformed cache (kMalformed) is discarded entirely.
  /// Returns the cache the singleton writes, for changedBinary() and cachedInvalidBinary().
  CacheSnapshot pluginCacheRescanAndRead(MyHost::Host &host, ImageEffect::PluginCache &imageEffectPluginCache,
                                         ImageEffect::Instance &instance, const std::string &pluginId)
  {
    RegressionSection section;
    PluginCache *pc = PluginCache::getPluginCache();
    const size_t nPlugins = pc->getPlugins().size();
    const size_t nEffects = imageEffectPluginCache.getPlugins().size();
    const size_t nIds = imageEffectPluginCache.getPluginsByID().size();
    const int nInvert = countPlugins(pc->getPlugins(), pluginId);
    ImageEffect::ImageEffectPlugin *const before = imageEffectPluginCache.getPluginById(pluginId);
    ImageEffect::ImageEffectPlugin *const instancePlugin = instance.getPlugin();
    pc->scanPluginFiles();
    check("rescan: getPluginById() is the same plug-in", imageEffectPluginCache.getPluginById(pluginId) == before);
    check("rescan: the instance's plug-in is the one getPluginById() gives",
          instance.getPlugin() == instancePlugin && instancePlugin == before);
    check("rescan: as many plug-ins (PluginCache::getPlugins())", pc->getPlugins().size() == nPlugins && countPlugins(pc->getPlugins(), pluginId) == nInvert);
    check("rescan: as many image effect plug-ins (ImageEffect::PluginCache::getPlugins())",
          imageEffectPluginCache.getPlugins().size() == nEffects && imageEffectPluginCache.getPluginsByID().size() == nIds);
    check("rescan: getPluginById() finds the plug-in", imageEffectPluginCache.getPluginById(pluginId) != NULL);
    check("getPluginById(no such plug-in) = NULL", imageEffectPluginCache.getPluginById("no.such.plugin") == NULL);
    check("getPluginById(id, major) finds the plug-in", imageEffectPluginCache.getPluginById(pluginId, imageEffectPluginCache.getPluginById(pluginId)->getVersionMajor()) == imageEffectPluginCache.getPluginById(pluginId));

    std::ostringstream written;
    pc->writePluginCache(written);
    const std::string cache = written.str();
    {
      // read into another PluginCache: the singleton doesn't change, and the other one writes
      // the same cache
      ScratchCache other(host);
      const bool ok = other.read(cache);
      std::ostringstream singleton;
      pc->writePluginCache(singleton);
      check("readCache() on a PluginCache other than the singleton reads into it", ok && other.written() == cache);
      check("readCache() on a PluginCache other than the singleton leaves the singleton", singleton.str() == cache && pc->getPlugins().size() == nPlugins);
      other.cache.scanPluginFiles();
      ImageEffect::ImageEffectPlugin *a = imageEffectPluginCache.getPluginById(pluginId);
      ImageEffect::ImageEffectPlugin *b = other.effects.getPluginById(pluginId);
      check("the plug-in read from the cache, after the scan", b != NULL && b != a && countPlugins(other.cache.getPlugins(), pluginId) == nInvert);
    }
    {
      // a malformed cache is discarded entirely (readCache() throws, nothing read is kept): the
      // cases of kMalformed that a whole cache can have
      const size_t api = cache.find("<apiproperties>");
      const size_t contexts = cache.find("\"" kOfxImageEffectPropSupportedContexts "\"");
      const size_t value = contexts == std::string::npos ? contexts : cache.find("index=\"0\"", contexts);
      check("the cache has an <apiproperties> and a kOfxImageEffectPropSupportedContexts value", api != std::string::npos && value != std::string::npos);
      for (const MalformedCase &c : kMalformed) {
        std::string bad = cache;
        if (c.bogusElement && api != std::string::npos) {
          bad.insert(api + strlen("<apiproperties>"), "<bogus/>");
        } else if (c.cacheIndex && value != std::string::npos) {
          bad.replace(value, strlen("index=\"0\""), std::string("index=\"") + c.cacheIndex + "\"");
        } else {
          continue; // (the element reader only, in propertyValues())
        }
        ScratchCache other(host);
        const bool refused = !other.read(bad, true);
        check((std::string("readCache() of a cache with ") + c.what + " fails, and keeps nothing").c_str(),
              refused && other.written().find("<bundle>") == std::string::npos);
      }
    }

    CacheSnapshot snapshot;
    snapshot.xml = cache;
    snapshot.nInvert = nInvert;
    // (the plug-in after the rescan: if a rescan replaced it, the checks above fail, and this reads
    // no freed pointer)
    ImageEffect::ImageEffectPlugin *const after = imageEffectPluginCache.getPluginById(pluginId);
    if (after && after->getBinary()) {
      const std::string &path = after->getBinary()->getBundlePath();
      const size_t slash = path.find_last_of("/\\");
      // (with its separator: a bundle "...invert.ofx.bundle" of another name doesn't match)
      snapshot.bundle = slash == std::string::npos ? path : path.substr(slash);
    }
    check("the bundle of the plug-in's binary", snapshot.bundle.size() > 1);
    return snapshot;
  }

  /// the clips of an instance: getNthClip() is NULL out of range (#218, bec213bb); and, with the
  /// Nuke extensions, the base class's kFnOfxImageEffectPropComponentsPresent: the colour plane
  /// whatever the components, none on a disconnected input (#342, #347), and the string a plug-in
  /// got stays valid across a disconnect and a reconnect (#355)
  void clips(ImageEffect::Instance *instance)
  {
    RegressionSection section;
    // getNthClip(): NULL out of range
    const int nClips = (int)instance->getDescriptor().getClipsByOrder().size();
    check("getNthClip(0) != NULL", nClips > 0 && instance->getNthClip(0) != NULL);
    check("getNthClip(-1) = NULL", instance->getNthClip(-1) == NULL);
    check("getNthClip(number of clips) = NULL", instance->getNthClip(nClips) == NULL);
#ifdef OFX_EXTENSIONS_NUKE
    ImageEffect::ClipInstance *source = instance->getClip(kOfxImageEffectSimpleSourceClipName);
    ImageEffect::ClipInstance *output = instance->getClip(kOfxImageEffectOutputClipName);
    const OfxPropertySuiteV1 *props = (const OfxPropertySuiteV1 *)Property::GetSuite(1);
    check("Source and Output clips", source && output && props);
    if (source && output && props) {
      const std::string sourceComponents = source->getComponents(), outputComponents = output->getComponents();
      source->setComponents(kOfxImageComponentRGBA);
      output->setComponents(kOfxImageComponentAlpha);
      const char *s = NULL, *o = NULL;
      const OfxStatus ss = props->propGetString(source->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, 0, &s);
      const OfxStatus os = props->propGetString(output->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, 0, &o);
      int sn = -1, on = -1;
      props->propGetDimension(source->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, &sn);
      props->propGetDimension(output->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, &on);
      check("kFnOfxImageEffectPropComponentsPresent of Source and Output", ss == kOfxStatOK && os == kOfxStatOK && s && o);
      check("kFnOfxImageEffectPropComponentsPresent of Source and Output: dimension 1", sn == 1 && on == 1);
      if (s && o) {
        check("kFnOfxImageEffectPropComponentsPresent of Source and Output: the colour plane, whatever the components (RGBA, Alpha)",
              std::strcmp(s, kFnOfxImagePlaneColour) == 0 && std::strcmp(o, kFnOfxImagePlaneColour) == 0);
      }
      source->setComponents(sourceComponents);
      output->setComponents(outputComponents);
      // a disconnected input: "the planes [...] that exist on the clip", none
      MyHost::MyClipInstance *mySource = dynamic_cast<MyHost::MyClipInstance *>(source);
      check("Source is a MyClipInstance", mySource != NULL);
      if (mySource) {
        const MyHost::InputSpec input = mySource->getInput();
        MyHost::InputSpec none;
        none.type = MyHost::InputSpec::eNone;
        mySource->setInput(none);
        int dn = -1;
        const OfxStatus ds = props->propGetDimension(source->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, &dn);
        check("kFnOfxImageEffectPropComponentsPresent of a disconnected Source: dimension 0", ds == kOfxStatOK && dn == 0);
        mySource->setInput(input);
        dn = -1;
        props->propGetDimension(source->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, &dn);
        check("kFnOfxImageEffectPropComponentsPresent of Source connected again: dimension 1", dn == 1);
        // the const char* a plug-in kept stays valid: the same string after a disconnect and a reconnect
        const char *again = NULL;
        props->propGetString(source->getPropHandle(), kFnOfxImageEffectPropComponentsPresent, 0, &again);
        check("kFnOfxImageEffectPropComponentsPresent of Source connected again: the same string", s && again == s);
      }
    }
#endif
  }

  /// an image effect API cache whose newPlugin() throws, as when it runs out of memory
  class ThrowingEffectsCache : public ImageEffect::PluginCache {
  public:
    std::string thrownFor; ///< the file of the binary whose plug-in made it throw
    explicit ThrowingEffectsCache(ImageEffect::Host *host) : ImageEffect::PluginCache(host) {}
    using ImageEffect::PluginCache::newPlugin;
    virtual Plugin *newPlugin(PluginBinary *pb, int /*pi*/, OfxPlugin * /*plug*/)
    {
      thrownFor = pb->getFilePath();
      throw std::bad_alloc();
    }
  };

#if !defined(_WIN32)
  /// the lowest free file descriptor: it is higher if one leaked
  int lowestFreeFd()
  {
    const int fd = open("/dev/null", O_RDONLY);
    if (fd >= 0) {
      close(fd);
    }
    return fd;
  }
#endif

  /// #232: a scan that throws (here from the API cache, while the first binary is scanned) passes
  /// the exception to the caller, and closes the directories it opened (a file descriptor leak)
  void scanThrows(MyHost::Host &host)
  {
    RegressionSection section;
#if !defined(_WIN32)
    const int fd = lowestFreeFd();
#endif
    bool thrown = false;
    {
      ThrowingEffectsCache throwingEffects(&host); // (destroyed after the plug-ins of other)
      PluginCache other;
      throwingEffects.registerInCache(other);
      try {
        other.scanPluginFiles();
      } catch (const std::bad_alloc &) {
        thrown = true;
      }
      check("a scan that throws: the exception reaches the caller", thrown && !throwingEffects.thrownFor.empty());
    }
#if !defined(_WIN32)
    check("a scan that throws closes the directories it opened", lowestFreeFd() == fd);
#endif
    ImageEffect::gImageEffectHost = &host;
  }

} // Regression
} // SuiteHandleChecks
