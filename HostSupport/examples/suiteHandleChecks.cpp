// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

////////////////////////////////////////////////////////////////////////////////
/// suiteHandleChecks: checks that the HostSupport param, progress and timeline suites, and the
/// host's fetchSuite(), reject NULL and describe-time (descriptor) handles, NULL names and NULL
/// return pointers with the statuses of the spec, instead of crashing; and that Bytes params copy
/// their default (descriptor and instance). Run by .github/scripts/hostdemo-checks.sh; exits with 1
/// if a check fails.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "ofxCore.h"
#include "ofxParam.h"
#include "ofxProgress.h"
#include "ofxTimeLine.h"

#include "ofxhPropertySuite.h"
#include "ofxhParam.h"
#include "ofxhImageEffect.h"
#include "hostDemoHostDescriptor.h"

using namespace OFX::Host;

namespace {
  int gFailures = 0;
  int gChecks = 0;

  void check(const char *what, bool ok)
  {
    ++gChecks;
    if (!ok) {
      ++gFailures;
      std::printf("suiteHandleChecks: %s: FAILED\n", what);
    }
  }

  class TestSetDescriptor : public Param::SetDescriptor {
    Property::Set _props;
  public:
    virtual Property::Set &getParamSetProps() { return _props; }
  };

  class TestInteger : public Param::IntegerInstance {
  public:
    TestInteger(Param::Descriptor &d, Param::SetInstance *s) : Param::IntegerInstance(d, s) {}
    virtual OfxStatus get(int &v) { v = 1; return kOfxStatOK; }
    virtual OfxStatus get(OfxTime, int &v) { v = 1; return kOfxStatOK; }
    virtual OfxStatus set(int) { return kOfxStatOK; }
    virtual OfxStatus set(OfxTime, int) { return kOfxStatOK; }
  };

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

  class TestSetInstance : public Param::SetInstance {
    Property::Set _props;
  public:
    virtual Property::Set &getParamSetProps() { return _props; }
    virtual void paramChangedByPlugin(Param::Instance *) {}
    virtual Param::Instance *newParam(const std::string &, Param::Descriptor &d) { return new TestInteger(d, this); }
    virtual OfxStatus editBegin(const std::string &) { return kOfxStatOK; }
    virtual OfxStatus editEnd() { return kOfxStatOK; }
  };
}

int main()
{
  MyHost::Host host;
  ImageEffect::gImageEffectHost = &host; // (set by ImageEffectPluginCache in a real host)

  const OfxParameterSuiteV1 *ps = (const OfxParameterSuiteV1 *)Param::GetSuite(1);
  check("param suite", ps != NULL);
  if (!ps) {
    return 1;
  }

  // paramDefine
  TestSetDescriptor setDesc;
  OfxParamSetHandle descSet = setDesc.getParamSetHandle();
  OfxPropertySetHandle props = (OfxPropertySetHandle)&setDesc;
  check("paramDefine(NULL type) = kOfxStatErrUnknown", ps->paramDefine(descSet, NULL, "a", &props) == kOfxStatErrUnknown);
  check("paramDefine(NULL type): *propertySet = NULL", props == NULL);
  check("paramDefine(NULL name) = kOfxStatErrValue", ps->paramDefine(descSet, kOfxParamTypeInteger, NULL, &props) == kOfxStatErrValue);
  check("paramDefine(unknown type) = kOfxStatErrUnknown", ps->paramDefine(descSet, "OfxParamTypeNoSuchType", "a", &props) == kOfxStatErrUnknown);
  check("paramDefine(integer) = kOfxStatOK", ps->paramDefine(descSet, kOfxParamTypeInteger, "i", &props) == kOfxStatOK && props != NULL);
  check("paramDefine(existing name) = kOfxStatErrExists", ps->paramDefine(descSet, kOfxParamTypeInteger, "i", &props) == kOfxStatErrExists && props != NULL);
  check("paramDefine(NULL set) = kOfxStatErrBadHandle", ps->paramDefine(NULL, kOfxParamTypeInteger, "j", &props) == kOfxStatErrBadHandle);

  TestSetInstance setInst;
  Param::Descriptor *intDesc = setDesc.getParams().find("i")->second;
  setInst.addParam("i", setInst.newParam("i", *intDesc));
  OfxParamSetHandle instSet = setInst.getParamSetHandle();
  check("paramDefine(instance's set) = kOfxStatErrBadHandle", ps->paramDefine(instSet, kOfxParamTypeInteger, "j", &props) == kOfxStatErrBadHandle);

  // paramGetHandle
  OfxParamHandle param = (OfxParamHandle)&setInst;
  props = (OfxPropertySetHandle)&setInst;
  check("paramGetHandle(NULL name) = kOfxStatErrUnknown", ps->paramGetHandle(instSet, NULL, &param, &props) == kOfxStatErrUnknown);
  check("paramGetHandle(NULL name): *param and *propertySet = NULL", param == NULL && props == NULL);
  OfxParamHandle instParam = NULL;
  check("paramGetHandle(instance) = kOfxStatOK", ps->paramGetHandle(instSet, "i", &instParam, NULL) == kOfxStatOK && instParam != NULL);
  OfxParamHandle descParam = NULL;
  check("paramGetHandle(descriptor) = kOfxStatOK", ps->paramGetHandle(descSet, "i", &descParam, NULL) == kOfxStatOK && descParam != NULL);

  // the value and key functions need an instance; paramGetPropertySet works on both
  int v = 0;
  unsigned int n = 0;
  check("paramGetValue(instance) = kOfxStatOK", ps->paramGetValue(instParam, &v) == kOfxStatOK && v == 1);
  check("paramGetValue(descriptor) = kOfxStatErrBadHandle", ps->paramGetValue(descParam, &v) == kOfxStatErrBadHandle);
  check("paramGetValue(NULL) = kOfxStatErrBadHandle", ps->paramGetValue(NULL, &v) == kOfxStatErrBadHandle);
  check("paramSetValue(descriptor) = kOfxStatErrBadHandle", ps->paramSetValue(descParam, 2) == kOfxStatErrBadHandle);
  check("paramGetNumKeys(descriptor) = kOfxStatErrBadHandle", ps->paramGetNumKeys(descParam, &n) == kOfxStatErrBadHandle);
  check("paramGetNumKeys(NULL output) = kOfxStatErrBadHandle", ps->paramGetNumKeys(instParam, NULL) == kOfxStatErrBadHandle);
  check("paramGetKeyTime(NULL output) = kOfxStatErrBadHandle", ps->paramGetKeyTime(instParam, 0, NULL) == kOfxStatErrBadHandle);
  check("paramGetKeyIndex(NULL output) = kOfxStatErrBadHandle", ps->paramGetKeyIndex(instParam, 0., 0, NULL) == kOfxStatErrBadHandle);
  check("paramCopy(instance, descriptor) = kOfxStatErrBadHandle", ps->paramCopy(instParam, descParam, 0., NULL) == kOfxStatErrBadHandle);
  props = NULL;
  check("paramGetPropertySet(descriptor) = kOfxStatOK", ps->paramGetPropertySet(descParam, &props) == kOfxStatOK && props == intDesc->getPropHandle());

  // Bytes params (OFX 1.5) copy their default, which the plug-in owns (#188 for the descriptor).
  // The C++ API, as the param suite's paramDefine refuses Bytes if the host doesn't support them.
  const OfxPropertySuiteV1 *propSuite = (const OfxPropertySuiteV1 *)Property::GetSuite(1);
  check("property suite", propSuite != NULL);
  Param::Descriptor *bytesDesc = setDesc.paramDefine(kOfxParamTypeBytes, "bytes");
  check("paramDefine(Bytes)", bytesDesc != NULL);
  if (propSuite && bytesDesc) {
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

  // progress and timeline suites: NULL and an effect descriptor
  ImageEffect::Descriptor effectDesc("/nonexistent", NULL);
  void *effectDescHandle = (void *)effectDesc.getHandle();
  const OfxProgressSuiteV1 *progress1 = (const OfxProgressSuiteV1 *)host.fetchSuite(kOfxProgressSuite, 1);
  const OfxProgressSuiteV2 *progress2 = (const OfxProgressSuiteV2 *)host.fetchSuite(kOfxProgressSuite, 2);
  const OfxTimeLineSuiteV1 *timeline = (const OfxTimeLineSuiteV1 *)host.fetchSuite(kOfxTimeLineSuite, 1);
  check("progress and timeline suites", progress1 && progress2 && timeline);
  if (progress1 && progress2 && timeline) {
    void *handles[2] = { NULL, effectDescHandle };
    for (int i = 0; i < 2; ++i) {
      void *h = handles[i];
      double t = 0., t1 = 0., t2 = 0.;
      const bool bad =
        progress1->progressStart(h, "label") == kOfxStatErrBadHandle &&
        progress2->progressStart(h, "message", NULL) == kOfxStatErrBadHandle &&
        progress2->progressUpdate(h, 0.5) == kOfxStatErrBadHandle &&
        progress2->progressEnd(h) == kOfxStatErrBadHandle &&
        timeline->getTime(h, &t) == kOfxStatErrBadHandle &&
        timeline->gotoTime(h, 1.) == kOfxStatErrBadHandle &&
        timeline->getTimeBounds(h, &t1, &t2) == kOfxStatErrBadHandle;
      check(i == 0 ? "progress and timeline(NULL) = kOfxStatErrBadHandle" :
                     "progress and timeline(descriptor) = kOfxStatErrBadHandle", bad);
    }
  }

  // the C fetchSuite of the host
  OfxHost *ofxHost = host.getHandle();
  Property::Set notHostProps;
  check("fetchSuite(host) != NULL", ofxHost->fetchSuite(ofxHost->host, kOfxPropertySuite, 1) != NULL);
  check("fetchSuite(NULL host) = NULL", ofxHost->fetchSuite(NULL, kOfxPropertySuite, 1) == NULL);
  check("fetchSuite(not the host property set) = NULL", ofxHost->fetchSuite(notHostProps.getHandle(), kOfxPropertySuite, 1) == NULL);
  check("fetchSuite(NULL name) = NULL", ofxHost->fetchSuite(ofxHost->host, NULL, 1) == NULL);

  ImageEffect::gImageEffectHost = NULL;
  std::printf("suiteHandleChecks: %d of %d checks OK\n", gChecks - gFailures, gChecks);
  return gFailures ? 1 : 0;
}
