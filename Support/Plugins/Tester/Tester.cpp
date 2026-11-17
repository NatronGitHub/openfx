/*
OFX GenericTest Example plugin, a plugin that illustrates the use of the OFX Support library.

Copyright (C) 2007 The Open Effects Association Ltd
Author Bruno Nicoletti bruno@thefoundry.co.uk

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

* Redistributions of source code must retain the above copyright notice,
this list of conditions and the following disclaimer.
* Redistributions in binary form must reproduce the above copyright notice,
this list of conditions and the following disclaimer in the documentation
and/or other materials provided with the distribution.
* Neither the name The Open Effects Association Ltd, nor the names of its 
contributors may be used to endorse or promote products derived from this
software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

The Open Effects Association Ltd
1 Wardour St
London W1D 6PA
England


*/

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#include <windows.h>
#endif

#ifdef __APPLE__
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <stdio.h>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <string>
#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsInteract.h"

#include "../include/ofxsProcessing.H"

static const OfxPointD kBoxSize = {5, 5};

template <class T> 
inline T Minimum(T a, T b) {    return (a < b) ? a : b;}

template <class T> 
inline T Absolute(T a) { return (a < 0) ? -a : a;}

class PositionInteract : public OFX::OverlayInteract 
{
protected :
  enum StateEnum {
    eInActive,
    ePoised,
    ePicked
  };

  StateEnum _state;
  OFX::Double2DParam* _position;
public :
  PositionInteract(OfxInteractHandle handle, OFX::ImageEffect* effect) 
    : OFX::OverlayInteract(handle)
    , _state(eInActive)
  {
    _position = effect->fetchDouble2DParam("widgetPos");
  }

  // overridden functions from OFX::Interact to do things
  virtual bool draw(const OFX::DrawArgs &args);
  virtual bool penMotion(const OFX::PenArgs &args);
  virtual bool penDown(const OFX::PenArgs &args);
  virtual bool penUp(const OFX::PenArgs &args);
  OfxPointD getCanonicalPosition(double time) const
  {
    OfxPointD retVal;
    _position->getValueAtTime(time, retVal.x, retVal.y);
    return retVal; 
  }
  void setCanonicalPosition(double x, double y, double time)
  {
    _position->setValueAtTime(time, x, y);
  }
};

bool PositionInteract::draw(const OFX::DrawArgs &args)
{
  OfxRGBColourF col;
  switch(_state) 
  {
  case eInActive : col.r = col.g = col.b = 0.0f; break;
  case ePoised   : col.r = col.g = col.b = 0.5f; break;
  case ePicked   : col.r = col.g = col.b = 1.0f; break;
  }

  // make the box a constant size on screen by scaling by the pixel scale
  float dx = (float)(kBoxSize.x / args.pixelScale.x);
  float dy = (float)(kBoxSize.y / args.pixelScale.y);

  // Draw a cross hair, the current coordinate system aligns with the image plane.
  glPushMatrix();

  // draw the bo
  OfxPointD pos = getCanonicalPosition(args.time);
  glColor3f(col.r, col.g, col.b);
  glTranslated(pos.x, pos.y, 0);
  glBegin(GL_POLYGON);
  glVertex2f(-dx, -dy);
  glVertex2f(-dx,  dy);
  glVertex2f( dx,  dy);
  glVertex2f( dx, -dy);
  glEnd();
  glPopMatrix();

  glPushMatrix();
  // draw a complementary outline
  glColor3f(1.0f - col.r, 1.0f - col.g, 1.0f - col.b);
  glTranslated(pos.x, pos.y, 0);
  glBegin(GL_LINE_LOOP);
  glVertex2f(-dx, -dy);
  glVertex2f(-dx,  dy);
  glVertex2f( dx,  dy);
  glVertex2f( dx, -dy);
  glEnd();
  glPopMatrix();

  return true;
}

// overridden functions from OFX::Interact to do things
bool PositionInteract::penMotion(const OFX::PenArgs &args)
{
  // figure the size of the box in cannonical coords
  float dx = (float)(kBoxSize.x / args.pixelScale.x);
  float dy = (float)(kBoxSize.y / args.pixelScale.y);

  OfxPointD pos = getCanonicalPosition(args.time);

  // pen position is in cannonical coords
  OfxPointD penPos = args.penPosition;

  switch(_state) 
  {
  case eInActive : 
  case ePoised   : 
    {
      // are we in the box, become 'poised'
      StateEnum newState;
      penPos.x -= pos.x;
      penPos.y -= pos.y;
      if(Absolute(penPos.x) < dx &&
        Absolute(penPos.y) < dy) {
          newState = ePoised;
      }
      else {
        newState = eInActive;
      }

      if(_state != newState) {
        _state = newState;
        _effect->redrawOverlays();
      }
    }
    break;

  case ePicked   : 
    {
      setCanonicalPosition(penPos.x, penPos.y, args.time);
      _effect->redrawOverlays();
    }
    break;
  }
  return _state != eInActive;
}

bool PositionInteract::penDown(const OFX::PenArgs &args)
{
  penMotion(args);
  if(_state == ePoised) {
    _state = ePicked;
    setCanonicalPosition(args.penPosition.x, args.penPosition.y, args.time);
    _effect->redrawOverlays();
  }

  return _state == ePicked;
}

bool PositionInteract::penUp(const OFX::PenArgs &args)
{
  if(_state == ePicked) 
  {
    _state = ePoised;
    penMotion(args);
    _effect->redrawOverlays();
    return true;
  }
  return false;
}

class GenericTestBase : public OFX::ImageProcessor {
protected :
  OFX::Image *_srcImg;
public :
  GenericTestBase(OFX::ImageEffect &instance): OFX::ImageProcessor(instance), _srcImg(NULL)
  {        
  }
  void setSrcImg(OFX::Image *v) {_srcImg = v;}
};

template <class PIX, int nComponents, int max>
class ImageGenericTester : public GenericTestBase 
{
public :
  ImageGenericTester(OFX::ImageEffect &instance) : GenericTestBase(instance){}
  void multiThreadProcessImages(const OfxRectI& procWindow, const OfxPointD& renderScale)
  {
    for(int y = procWindow.y1; y < procWindow.y2; y++) 
    {
      if(_effect.abort()) 
        break;
      PIX *dstPix = (PIX *) _dstImg->getPixelAddress(procWindow.x1, y);
      for(int x = procWindow.x1; x < procWindow.x2; x++) 
      {
        PIX *srcPix = (PIX *)  (_srcImg ? _srcImg->getPixelAddress(x, y) : 0);
        if(srcPix) 
        {
          for(int c = 0; c < nComponents; c++)
            dstPix[c] = max - srcPix[c];
        }
        else 
        {
          for(int c = 0; c < nComponents; c++)
            dstPix[c] = 0;
        }
        dstPix += nComponents;
      }
    }
  }
};



template<class TypeCarrier, int kComponents, int kMax>
class Analyser
{
public:
  Analyser(OFX::Clip* srcClip, OFX::DoubleParam* dbl)
  {
    OfxRangeD range = srcClip->getFrameRange();
    for(double d = range.min; d< range.max; ++d)
    {
      OFX::auto_ptr<OFX::Image> src(srcClip->fetchImage(d));
      dbl->setValueAtTime(d, d);
    }
  }
};

////////////////////////////////////////////////////////////////////////////////
/** @brief The plugin that does our work */
class GenericTestPlugin : public OFX::ImageEffect 
{
protected :
  OFX::Clip *dstClip_;
  OFX::Clip *srcClip_;

public :
  double nestedTime_; ///< the time of the last plug-in edit of keysDouble (keysTest)

  GenericTestPlugin(OfxImageEffectHandle handle) : ImageEffect(handle), dstClip_(NULL), srcClip_(NULL), nestedTime_(0.)
  {
    dstClip_ = fetchClip(kOfxImageEffectOutputClipName);
    srcClip_ = fetchClip(kOfxImageEffectSimpleSourceClipName);
  }

  virtual void render(const OFX::RenderArguments &args);
  void setupAndProcess(GenericTestBase &, const OFX::RenderArguments &args);

  // A test of the host's keyframe functions (paramSetValueAtTime, paramGetNumKeys, paramGetKeyTime,
  // paramGetKeyIndex, paramDeleteKey, paramDeleteAllKeys, paramGetValueAtTime, paramGetDerivative,
  // paramGetIntegral), for a host that interpolates linearly (hostDemo): returns "" if all is as
  // expected, else what isn't.
  std::string keysTest(double time)
  {
    std::ostringstream err;
#   define KEYS_CHECK(cond) do { if (!(cond)) { err << #cond << "; "; } } while (0)
#   define KEYS_NEAR(a, b) KEYS_CHECK(std::fabs((a) - (b)) < 1e-9)
    try {
      // the timeline is at the time of the action
      KEYS_NEAR(timeLineGetTime(), time);

      OFX::DoubleParam* d = fetchDoubleParam("keysDouble");
      d->deleteAllKeys();
      KEYS_CHECK(d->getNumKeys() == 0);
      KEYS_NEAR(d->differentiate(5.), 0.); // no key
      d->setValueAtTime(0., 1.);
      KEYS_NEAR(d->differentiate(5.), 0.); // one key
      KEYS_NEAR(d->integrate(2., 5.), 3.);
      d->setValueAtTime(10., 3.);
      d->setValueAtTime(4., 2.);
      KEYS_CHECK(d->getNumKeys() == 3);
      KEYS_NEAR(d->getKeyTime(0), 0.);
      KEYS_NEAR(d->getKeyTime(1), 4.);
      KEYS_NEAR(d->getKeyTime(2), 10.);
      bool outOfRange = false;
      try {
        d->getKeyTime(3);
      } catch (const std::out_of_range&) {
        outOfRange = true;
      }
      KEYS_CHECK(outOfRange);
      KEYS_CHECK(d->getKeyIndex(4., OFX::eKeySearchNear) == 1);
      KEYS_CHECK(d->getKeyIndex(4. + 5e-5, OFX::eKeySearchNear) == 1); // "some small delta"
      KEYS_CHECK(d->getKeyIndex(5., OFX::eKeySearchNear) == -1);
      KEYS_CHECK(d->getKeyIndex(5., OFX::eKeySearchForwards) == 2);
      KEYS_CHECK(d->getKeyIndex(5., OFX::eKeySearchBackwards) == 1);
      KEYS_CHECK(d->getKeyIndex(10., OFX::eKeySearchForwards) == -1);
      KEYS_CHECK(d->getKeyIndex(0., OFX::eKeySearchBackwards) == -1);
      KEYS_NEAR(d->getValueAtTime(2.), 1.5);
      KEYS_NEAR(d->getValueAtTime(7.), 2.5);
      KEYS_NEAR(d->getValueAtTime(-5.), 1.);
      KEYS_NEAR(d->getValueAtTime(20.), 3.);
      KEYS_NEAR(d->differentiate(2.), 0.25);
      KEYS_NEAR(d->differentiate(7.), 1. / 6.);
      KEYS_NEAR(d->differentiate(20.), 0.);
      KEYS_NEAR(d->integrate(0., 4.), 6.);
      KEYS_NEAR(d->integrate(-2., 0.), 2.);
      KEYS_NEAR(d->integrate(4., 0.), -6.);
      d->setValueAtTime(4. + 5e-5, 2.5); // the same key
      KEYS_CHECK(d->getNumKeys() == 3);
      KEYS_NEAR(d->getValueAtTime(4.), 2.5);
      d->deleteKeyAtTime(4.);
      KEYS_CHECK(d->getNumKeys() == 2);
      KEYS_NEAR(d->getValueAtTime(5.), 2.);
      d->deleteKeyAtTime(5.); // no key there: fails quietly
      KEYS_CHECK(d->getNumKeys() == 2);
      // paramSetValue on a parameter with keys: the key at the current time, and the plug-in's
      // instanceChanged for its own change is at that time too
      nestedTime_ = -1e9;
      d->setValue(7.);
      KEYS_NEAR(d->getValueAtTime(time), 7.);
      KEYS_CHECK(d->getKeyIndex(time, OFX::eKeySearchNear) >= 0);
      KEYS_NEAR(nestedTime_, time);
      // deleteAllKeys: the value at the current time stays
      d->deleteAllKeys();
      d->setValueAtTime(0., 1.);
      d->setValueAtTime(10., 3.);
      const double atTime = d->getValueAtTime(time);
      d->deleteAllKeys();
      KEYS_CHECK(d->getNumKeys() == 0);
      KEYS_NEAR(d->getValueAtTime(-100.), atTime);
      KEYS_NEAR(d->getValueAtTime(100.), atTime);
      // deleting the last key: its value stays
      d->setValueAtTime(2., 5.);
      d->deleteKeyAtTime(2.);
      KEYS_CHECK(d->getNumKeys() == 0);
      KEYS_NEAR(d->getValueAtTime(100.), 5.);

      OFX::IntParam* i = fetchIntParam("keysInt");
      i->deleteAllKeys();
      i->setValueAtTime(0., 0);
      i->setValueAtTime(10., 5);
      KEYS_CHECK(i->getValueAtTime(3.) == 2); // 1.5, rounded
      KEYS_CHECK(i->getValueAtTime(10.) == 5);

      OFX::BooleanParam* b = fetchBooleanParam("keysBool");
      b->deleteAllKeys();
      b->setValueAtTime(0., false);
      b->setValueAtTime(10., true);
      KEYS_CHECK(!b->getValueAtTime(9.)); // held
      KEYS_CHECK(b->getValueAtTime(10.));

      OFX::ChoiceParam* c = fetchChoiceParam("keysChoice");
      c->deleteAllKeys();
      c->setValueAtTime(0., 0);
      c->setValueAtTime(10., 2);
      int ci = -1;
      c->getValueAtTime(9., ci);
      KEYS_CHECK(ci == 0); // held
      c->getValueAtTime(10., ci);
      KEYS_CHECK(ci == 2);

      OFX::RGBParam* rgb = fetchRGBParam("keysRGB");
      rgb->deleteAllKeys();
      rgb->setValueAtTime(0., 0., 0., 0.);
      rgb->setValueAtTime(10., 1., 2., 3.);
      double r, g, bl;
      rgb->getValueAtTime(5., r, g, bl);
      KEYS_NEAR(r, 0.5); KEYS_NEAR(g, 1.); KEYS_NEAR(bl, 1.5);

      // the derivative and integral of a 2D double (Support has them for 1D and 2D doubles)
      OFX::Double2DParam* xy = fetchDouble2DParam("keys2D");
      xy->deleteAllKeys();
      xy->setValueAtTime(0., 0., 0.);
      xy->setValueAtTime(10., 1., 2.);
      double x, y;
      xy->differentiate(5., x, y);
      KEYS_NEAR(x, 0.1); KEYS_NEAR(y, 0.2);
      xy->integrate(0., 10., x, y);
      KEYS_NEAR(x, 5.); KEYS_NEAR(y, 10.);
    } catch (const std::exception& e) {
      err << "exception: " << e.what() << "; ";
    }
#   undef KEYS_NEAR
#   undef KEYS_CHECK
    return err.str();
  }
  void changedParam(const OFX::InstanceChangedArgs &args, const std::string &paramName)
  {
    if(paramName=="enableTest")
    {
      OFX::ChoiceParam* choice  = fetchChoiceParam("enableTest");
      OFX::DoubleParam* dbl = fetchDoubleParam("enableDbl");
      int value = 0;
      choice->getValueAtTime(args.time, value);
      dbl->setEnabled(value ==0 );
    }
    else if(paramName=="pbButton")
    {
      sendMessage(OFX::Message::eMessageMessage, "", "Push Button Pressed - TestPassed!");
    }
    else if(paramName=="messageText")
    {
      // the text is sent as a message, as is, even with printf conversions in it (Support passes
      // it as a "%s" argument of the message suite's format)
      std::string text;
      fetchStringParam("messageText")->getValue(text);
      sendMessage(OFX::Message::eMessageMessage, "", text);
      setPersistentMessage(OFX::Message::eMessageWarning, "", text);
    }
    else if(paramName=="keysDouble" && args.reason == OFX::eChangePluginEdit)
    {
      nestedTime_ = args.time; // (keysTest: the time of the instanceChanged of its own change)
    }
    else if(paramName=="keysTest")
    {
      const std::string err = keysTest(args.time);
      sendMessage(OFX::Message::eMessageMessage, "", err.empty() ? "Keys test passed" : "Keys test FAILED: " + err);
    }
    else if(paramName=="widgetPos")
    {
      redrawOverlays();
    }
    else if(paramName == "analyseButton")
    {
      OFX::BitDepthEnum       dstBitDepth    = srcClip_->getPixelDepth();
      OFX::PixelComponentEnum dstComponents  = srcClip_->getPixelComponents();
      OFX::DoubleParam* dbl = fetchDoubleParam("analysisParam");

      if(dstComponents == OFX::ePixelComponentRGBA) 
      {
        switch(dstBitDepth) 
        {
        case OFX::eBitDepthUByte : 
          {
            Analyser<unsigned char, 4, 255> analyse(srcClip_, dbl);
            break;
          }
        case OFX::eBitDepthUShort :
          {
            Analyser<unsigned short, 4, 65535> analyse(srcClip_, dbl);
            break;
          }
        case OFX::eBitDepthFloat :
          {
            Analyser<float, 4, 1> analyse(srcClip_, dbl);
            break;
          }
        default :
          OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }
      }
      else 
      {
        switch(dstBitDepth) 
        {
        case OFX::eBitDepthUByte :
          {
            Analyser<unsigned char, 1, 255> analyse(srcClip_, dbl);
            break;
          }
        case OFX::eBitDepthUShort :
          {
            Analyser<unsigned short, 1, 65535> analyse(srcClip_, dbl);
            break;
          }
        case OFX::eBitDepthFloat : 
          {
            Analyser<float, 1, 1> analyse(srcClip_, dbl);
            break;
          }
        default :
          OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        }
      }
    }
  }
};


void GenericTestPlugin::setupAndProcess(GenericTestBase &processor, const OFX::RenderArguments &args)
{
  OFX::auto_ptr<OFX::Image> dst(dstClip_->fetchImage(args.time));
  OFX::BitDepthEnum dstBitDepth       = dst->getPixelDepth();
  OFX::PixelComponentEnum dstComponents  = dst->getPixelComponents();
  OFX::auto_ptr<OFX::Image> src(srcClip_->fetchImage(args.time));

  if(src.get()) 
  {
    OFX::BitDepthEnum    srcBitDepth      = src->getPixelDepth();
    OFX::PixelComponentEnum srcComponents = src->getPixelComponents();

    // see if they have the same depths and bytes and all
    if(srcBitDepth != dstBitDepth || srcComponents != dstComponents)
      throw int(1); // HACK!! need to throw an sensible exception here!
  }

  processor.setDstImg(dst.get());
  processor.setSrcImg(src.get());
  processor.setRenderWindow(args.renderWindow, args.renderScale);
  processor.process();
}


void GenericTestPlugin::render(const OFX::RenderArguments &args)
{
  // a test of hosts: paramGetValue during a render is the value at the render's time
  {
    OFX::DoubleParam* keysDouble = fetchDoubleParam("keysDouble");
    if (std::fabs(keysDouble->getValue() - keysDouble->getValueAtTime(args.time)) > 1e-9) {
      setPersistentMessage(OFX::Message::eMessageError, "", "render: paramGetValue isn't the value at the render's time");
    }
  }
  OFX::BitDepthEnum       dstBitDepth    = dstClip_->getPixelDepth();
  OFX::PixelComponentEnum dstComponents  = dstClip_->getPixelComponents();

  if(dstComponents == OFX::ePixelComponentRGBA) 
  {
    switch(dstBitDepth) 
    {
    case OFX::eBitDepthUByte : 
      {      
        ImageGenericTester<unsigned char, 4, 255> fred(*this);
        setupAndProcess(fred, args);
      }
      break;

    case OFX::eBitDepthUShort : 
      {
        ImageGenericTester<unsigned short, 4, 65535> fred(*this);
        setupAndProcess(fred, args);
      }                          
      break;

    case OFX::eBitDepthFloat : 
      {
        ImageGenericTester<float, 4, 1> fred(*this);
        setupAndProcess(fred, args);
      }
      break;
    default :
      OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
  }
  else {
    switch(dstBitDepth) 
    {
    case OFX::eBitDepthUByte : 
      {
        ImageGenericTester<unsigned char, 1, 255> fred(*this);
        setupAndProcess(fred, args);
      }
      break;

    case OFX::eBitDepthUShort : 
      {
        ImageGenericTester<unsigned short, 1, 65535> fred(*this);
        setupAndProcess(fred, args);
      }                          
      break;

    case OFX::eBitDepthFloat : 
      {
        ImageGenericTester<float, 1, 1> fred(*this);
        setupAndProcess(fred, args);
      }                          
      break;
    default :
      OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
    }
  } 
}

class PositionOverlayDescriptor : public OFX::DefaultEffectOverlayDescriptor<PositionOverlayDescriptor, PositionInteract> {};

mDeclarePluginFactory(GenericTestExamplePluginFactory, {}, {});

using namespace OFX;
void GenericTestExamplePluginFactory::describe(OFX::ImageEffectDescriptor &desc)
{
  desc.setLabels("GenericTest", "GenericTest", "GenericTest");
  desc.setPluginGrouping("OFX");
  desc.addSupportedContext(eContextFilter);
  desc.addSupportedBitDepth(eBitDepthUByte);
  desc.addSupportedBitDepth(eBitDepthUShort);
  desc.addSupportedBitDepth(eBitDepthFloat);

  desc.setSingleInstance(false);
  desc.setHostFrameThreading(false);
  desc.setSupportsMultiResolution(true);
  desc.setSupportsTiles(true);
  desc.setTemporalClipAccess(false);
  desc.setRenderTwiceAlways(false);
  desc.setSupportsMultipleClipPARs(false);

  desc.setOverlayInteractDescriptor( new PositionOverlayDescriptor);
}

void GenericTestExamplePluginFactory::describeInContext(OFX::ImageEffectDescriptor &desc, OFX::ContextEnum /*context*/)
{
  ClipDescriptor *srcClip = desc.defineClip(kOfxImageEffectSimpleSourceClipName);
  srcClip->addSupportedComponent(ePixelComponentRGBA);
  srcClip->addSupportedComponent(ePixelComponentAlpha);
  srcClip->setTemporalClipAccess(false);
  srcClip->setSupportsTiles(true);
  srcClip->setIsMask(false);

  ClipDescriptor *dstClip = desc.defineClip(kOfxImageEffectOutputClipName);
  dstClip->addSupportedComponent(ePixelComponentRGBA);
  dstClip->addSupportedComponent(ePixelComponentAlpha);
  dstClip->setSupportsTiles(true);

  DoubleParamDescriptor *param1 = desc.defineDoubleParam("MinMaxTest");
  param1->setLabels("Min/Max Test", "Min/Max Test", "Min/Max Test");
  param1->setScriptName("minMaxTest");
  param1->setHint("A double parameter to illustrate visual min/max.");
  param1->setDefault(50.0);
  param1->setRange(-1000, 1000);
  param1->setDisplayRange(-1000, 1000);
  param1->setDoubleType(eDoubleTypePlain);

  ChoiceParamDescriptor* param2 = desc.defineChoiceParam("enableTest");
  param2->setLabels("Enabler", "Enabler", "Enabler");
  param2->appendOption("Enable parameter", "Enable parameter");
  param2->appendOption("Disable parameter", "Disable parameter");

  DoubleParamDescriptor *param3 = desc.defineDoubleParam("enableDbl");
  param3->setLabels("Enabled by Enabler", "Enabled by Enabler", "Enabled by Enabler");


  BooleanParamDescriptor* bparam = desc.defineBooleanParam("Insignificant");
  bparam->setLabels("Insignificant", "Insignificant", "Insignificant");
  bparam->setHint("Shouldn't cause a re-render.");
  bparam->setEvaluateOnChange(false);

  BooleanParamDescriptor* bparam2 = desc.defineBooleanParam("secretTest");
  bparam2->setLabels("SECRET!", "SECRET!", "SECRET!");
  bparam2->setIsSecret(true);
  bparam2->setHint("Shouldn't be shown in the user interface.");

  BooleanParamDescriptor* bparam3 = desc.defineBooleanParam("nonPersistent");
  bparam3->setLabels("Non-persistent", "Non-persistent", "Non-persistent");
  bparam3->setHint("Shouldn't be saved in the plugin description.");
  bparam3->setIsPersistent(false);
    
  DoubleParamDescriptor *param5 = desc.defineDoubleParam("animateDbl");
  param5->setLabels("No Animation", "No Animation", "No Animation");
  param5->setAnimates(false);

  DoubleParamDescriptor *param6 = desc.defineDoubleParam("angleTest");
  param6->setLabels("Angle?", "Angle?", "Angle?");
  param6->setRange(-180.0, 180.0);
  param6->setHint("An angle parameter.");
  param6->setDoubleType(eDoubleTypeAngle);

  PushButtonParamDescriptor* pb = desc.definePushButtonParam("pbButton");
  pb->setLabels("Push Me", "Push Me", "Push Me");

  // messageText: sent as a message when it changes (a test of the message suite's format)
  StringParamDescriptor* messageText = desc.defineStringParam("messageText");
  messageText->setLabels("Message Text", "Message Text", "Message Text");
  messageText->setAnimates(false);

  PushButtonParamDescriptor* pb2 = desc.definePushButtonParam("analyseButton");
  pb2->setLabels("Analyse", "Analyse", "Analyse");

  // keysTest: a test of the host's keyframe functions, on keysDouble, keysInt and keysBool (see
  // GenericTestPlugin::keysTest); it sends the message "Keys test passed", or what failed
  PushButtonParamDescriptor* pb3 = desc.definePushButtonParam("keysTest");
  pb3->setLabels("Keys Test", "Keys Test", "Keys Test");
  DoubleParamDescriptor* keysDouble = desc.defineDoubleParam("keysDouble");
  keysDouble->setLabels("Keys Double", "Keys Double", "Keys Double");
  IntParamDescriptor* keysInt = desc.defineIntParam("keysInt");
  keysInt->setLabels("Keys Int", "Keys Int", "Keys Int");
  BooleanParamDescriptor* keysBool = desc.defineBooleanParam("keysBool");
  keysBool->setLabels("Keys Bool", "Keys Bool", "Keys Bool");
  keysBool->setAnimates(true);
  ChoiceParamDescriptor* keysChoice = desc.defineChoiceParam("keysChoice");
  keysChoice->setLabels("Keys Choice", "Keys Choice", "Keys Choice");
  keysChoice->appendOption("a");
  keysChoice->appendOption("b");
  keysChoice->appendOption("c");
  keysChoice->setAnimates(true);
  RGBParamDescriptor* keysRGB = desc.defineRGBParam("keysRGB");
  keysRGB->setLabels("Keys RGB", "Keys RGB", "Keys RGB");
  Double2DParamDescriptor* keys2D = desc.defineDouble2DParam("keys2D");
  keys2D->setLabels("Keys 2D", "Keys 2D", "Keys 2D");

  DoubleParamDescriptor *param7 = desc.defineDoubleParam("analysisParam");
  param7->setLabels("Analysis Slave", "Analysis Slave", "Analysis Slave");

  Double2DParamDescriptor* widgetPos = desc.defineDouble2DParam("widgetPos");
  widgetPos->setLabels("Widget Position", "Widget Position", "Widget Position");
  widgetPos->setDoubleType(OFX::eDoubleTypeXYAbsolute);
  widgetPos->setDefaultCoordinateSystem(eCoordinatesNormalised);
  widgetPos->setDimensionLabels("X Position", "Y Position");
  widgetPos->setDefault(0.5, 0.5);
}

OFX::ImageEffect* GenericTestExamplePluginFactory::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum /*context*/)
{
  return new GenericTestPlugin(handle);
}

static GenericTestExamplePluginFactory p("net.sf.openfx.GenericTestPlugin", 1, 0);
mRegisterPluginFactoryInstance(p)
