// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @brief This file contains code that skins the ofx interact suite (for image effects) */


#include "ofxsSupportPrivate.h"
#include <algorithm> // for find
#include <new> // for std::bad_alloc

/** @brief The core 'OFX Support' namespace, used by plugin implementations. All code for these are defined in the common support libraries.
*/
namespace OFX {

  /** @brief fetch a pixel scale out of the property set */
  static OfxPointD getPixelScale(const PropertySet &props)
  {
    OfxPointD pixelScale = {1., 1.};
    props.propGetDoubleN(kOfxInteractPropPixelScale, &pixelScale.x, 2);

    return pixelScale;
  }

  /** @brief fetch a render scale out of the property set */
  static OfxPointD getRenderScale(const PropertySet &props)
  {
    OfxPointD v = {1., 1.};
    props.propGetDoubleN(kOfxImageEffectPropRenderScale, &v.x, 2);

    return v;
  }

  /** @brief fetch a background colour out of the property set */
  static OfxRGBColourD getBackgroundColour(const PropertySet &props)
  {
    OfxRGBColourD backGroundColour = {0., 0., 0.};
    props.propGetDoubleN(kOfxInteractPropBackgroundColour, &backGroundColour.r, 3);

    return backGroundColour;
  }

  /** @brief retrieves the image effect pointer from the interact handle */
  static ImageEffect *retrieveEffectFromInteractHandle(OfxInteractHandle handle)
  {
    // get the properties set on this handle
    OfxPropertySetHandle propHandle;
    OfxStatus stat = OFX::Private::gInteractSuite->interactGetPropertySet(handle, &propHandle);
    throwSuiteStatusException(stat);
    PropertySet interactProperties(propHandle);

    // get the effect handle from this handle
    OfxImageEffectHandle effectHandle = (OfxImageEffectHandle) interactProperties.propGetPointer(kOfxPropEffectInstance);

    // get the effect properties 
    return OFX::Private::retrieveImageEffectPointer(effectHandle);
  }

  /** @brief ctor */
  Interact::Interact(OfxInteractHandle handle)
    : _interactHandle(handle)
    , _effect(NULL)
  {
    // get the properties set on this handle
    OfxPropertySetHandle propHandle;
    OfxStatus stat = OFX::Private::gInteractSuite->interactGetPropertySet(handle, &propHandle);
    throwSuiteStatusException(stat);
    _interactProperties.propSetHandle(propHandle);

    // set othe instance data on the property handle to point to this interact
    _interactProperties.propSetPointer(kOfxPropInstanceData, (void *)this);

    // get the effect handle from this handle        
    _effect = retrieveEffectFromInteractHandle(handle);
  }

#ifdef OFX_EXTENSIONS_NATRON
  int Interact::getKeyModifiers() const
  {
    return _effect ? _effect->getKeyModifiers() : 0;
  }
#endif

  /** @brief virtual destructor */
  Interact::~Interact()
  {
    // fetched clips and params are owned and deleted by the ImageEffect and its ParamSet
  }

  /** @brief The bitdepth of each component in the openGL frame buffer */
  int 
    Interact::getBitDepth(void) const
  {
    return _interactProperties.propGetInt(kOfxInteractPropBitDepth);
  }

  /** @brief Does the openGL frame buffer have an alpha */
  bool
    Interact::hasAlpha(void) const
  {
    return _interactProperties.propGetInt(kOfxInteractPropHasAlpha) != 0;
  }

  /** @brief Returns the size of a real screen pixel under the interact's cannonical projection */
  OfxPointD 
    Interact::getPixelScale(void) const
  {
    return ::OFX::getPixelScale(_interactProperties);
  }

  /** @brief The suggested colour to draw a widget in an interact */
  bool
    Interact::getSuggestedColour(OfxRGBColourD &c) const
  {
    // OFX 1.2/1.3 specs say that the host should return kOfxStatReplyDefault if there is no suggested color
    int dim;
    OfxStatus stat = OFX::Private::gPropSuite->propGetDimension(_interactProperties.propSetHandle(), kOfxInteractPropSuggestedColour, &dim);
    // host may indicate that there is no suggested color either by returning kOfxStatReplyDefault or by returning dim=0
    if (stat == kOfxStatReplyDefault || (stat == kOfxStatOK && dim < 3) ) {
      return false;
    }
    if (stat == kOfxStatOK) {
      stat = OFX::Private::gPropSuite->propGetDoubleN(_interactProperties.propSetHandle(), kOfxInteractPropSuggestedColour, 3, &c.r);
    }
#ifdef OFX_EXTENSIONS_NUKE
    if (stat != kOfxStatOK && stat != kOfxStatReplyDefault) {
      stat = OFX::Private::gPropSuite->propGetDoubleN(_interactProperties.propSetHandle(), kOfxPropOverlayColour, 3, &c.r);
    }
#endif
    if (stat != kOfxStatOK) {
      return false; // host gave no suggestion (replied kOfxStatReplyDefault or property is unknown to host)
    }
    return true;
  }

#ifdef OFX_EXTENSIONS_NATRON
  /** @brief Sets wether the interact uses an additional colour picking value in the inArgs of its actions. */
  void
    Interact::setColourPicking(bool useColourPicking)
  {
    _interactProperties.propSetInt(kNatronOfxInteractColourPicking, (int)useColourPicking, 0, false);
  }

  static bool hostSupportsCursor(const std::string &name)
  {
    const std::vector<std::string> &cursors = OFX::getImageEffectHostDescription()->supportedCursors;
    return std::find(cursors.begin(), cursors.end(), name) != cursors.end();
  }

  bool
    Interact::setCursor(const std::string &name)
  {
    // kOfxInteractCursorCustom must be set through setCustomCursor()
    if (name == kOfxInteractCursorCustom || !hostSupportsCursor(name)) {
      return false;
    }
    OfxStatus stat = OFX::Private::gPropSuite->propSetString(_interactProperties.propSetHandle(), kOfxInteractPropCursor, 0, name.c_str());
    return stat == kOfxStatOK;
  }

  bool
    Interact::setCustomCursor(const std::string &pngFileName, int hotSpotX, int hotSpotY)
  {
    if (pngFileName.empty() || !hostSupportsCursor(kOfxInteractCursorCustom)) {
      return false;
    }
    OfxPropertySetHandle h = _interactProperties.propSetHandle();
    int hotSpot[2] = { hotSpotX, hotSpotY };
    return OFX::Private::gPropSuite->propSetString(h, kOfxInteractPropCursorImage, 0, pngFileName.c_str()) == kOfxStatOK &&
           OFX::Private::gPropSuite->propSetIntN(h, kOfxInteractPropCursorHotSpot, 2, hotSpot) == kOfxStatOK &&
           OFX::Private::gPropSuite->propSetString(h, kOfxInteractPropCursor, 0, kOfxInteractCursorCustom) == kOfxStatOK;
  }
#endif

  /** @brief Request a redraw */
  void 
    Interact::requestRedraw(void) const
  {
    OfxStatus stat = OFX::Private::gInteractSuite->interactRedraw(_interactHandle);
    throwSuiteStatusException(stat);
  }

  /** @brief Swap a buffer in the case of a double bufferred interact, this is possibly a silly one */
  void 
    Interact::swapBuffers(void) const
  {
    OfxStatus stat = OFX::Private::gInteractSuite->interactSwapBuffers(_interactHandle);
    throwSuiteStatusException(stat);
  }

  /** @brief Set a param that the interact should be redrawn on if its value changes */
  void 
    Interact::addParamToSlaveTo(Param *p)
  {
    // do we have it already ?
    std::list<Param *>::iterator i;
    i = std::find(_slaveParams.begin(), _slaveParams.end(), p);
    if(i == _slaveParams.end()) {
      // we have a new one to add in here
      _slaveParams.push_back(p);

      // and set the property
      int n = _interactProperties.propGetDimension(kOfxInteractPropSlaveToParam);
      _interactProperties.propSetString(kOfxInteractPropSlaveToParam, p->getName(), n);
    }

  }

  /** @brief Remova a param that the interact should be redrawn on if its value changes */
  void 
    Interact::removeParamToSlaveTo(Param *p)
  {
    // do we have it already ?
    std::list<Param *>::iterator i;
    i = std::find(_slaveParams.begin(), _slaveParams.end(), p);
    if(i != _slaveParams.end()) {
      // clobber it from the list
      _slaveParams.erase(i);

      // reset the property to remove our dead one
      _interactProperties.propReset(kOfxInteractPropSlaveToParam);

      // and add them all in again
      int n = 0;
      for(i = _slaveParams.begin(); i != _slaveParams.end(); ++i, ++n) {
        _interactProperties.propSetString(kOfxInteractPropSlaveToParam, (*i)->getName(), n);
      }
    }
  }

  /** @brief the background colour */
  OfxRGBColourD Interact::getBackgroundColour(void) const
  {
    return OFX::getBackgroundColour(_interactProperties);
  }

  /** @brief the function called to draw in the interact */
  bool 
    Interact::draw(const DrawArgs &/*args*/)
  {
    return false;
  }

  /** @brief the function called to handle pen motion in the interact

  returns true if the interact trapped the action in some sense. This will block the action being passed to 
  any other interact that may share the viewer.
  */
  bool 
    Interact::penMotion(const PenArgs &/*args*/)
  {
    return false;
  }

  /** @brief the function called to handle pen down events in the interact 

  returns true if the interact trapped the action in some sense. This will block the action being passed to 
  any other interact that may share the viewer.
  */
  bool 
    Interact::penDown(const PenArgs &/*args*/)
  {
    return false;
  }

  /** @brief the function called to handle pen up events in the interact 

  returns true if the interact trapped the action in some sense. This will block the action being passed to 
  any other interact that may share the viewer.
  */
  bool 
    Interact::penUp(const PenArgs &/*args*/)
  {
    return false;
  }

  /** @brief the function called to handle key down events in the interact 

  returns true if the interact trapped the action in some sense. This will block the action being passed to 
  any other interact that may share the viewer.
  */
  bool 
    Interact::keyDown(const KeyArgs &/*args*/)
  {
    return false;
  }

  /** @brief the function called to handle key up events in the interact 

  returns true if the interact trapped the action in some sense. This will block the action being passed to 
  any other interact that may share the viewer.
  */
  bool 
    Interact::keyUp(const KeyArgs &/*args*/)
  {
    return false;
  }

  /** @brief the function called to handle key down repeat events in the interact 

  returns true if the interact trapped the action in some sense. This will block the action being passed to 
  any other interact that may share the viewer.
  */
  bool 
    Interact::keyRepeat(const KeyArgs &/*args*/)
  {
    return false;
  }

  /** @brief Called when the interact is given input focus */
  void 
    Interact::gainFocus(const FocusArgs &/*args*/)
  {
  }

  /** @brief Called when the interact is loses input focus */
  void 
    Interact::loseFocus(const FocusArgs &/*args*/)
  {
  }

  ////////////////////////////////////////////////////////////////////////////////
  // overlay interact guff

  /** @brief ctor */
  OverlayInteract::OverlayInteract(OfxInteractHandle handle)
    : Interact(handle)
  {
    // add this interact into the list of overlays that the effect knows about
    if(_effect)
      _effect->addOverlayInteract(this);
  }

  /** @brief ctor */
  OverlayInteract::~OverlayInteract()
  {
    // add this interact into the list of overlays that the effect knows about
    if(_effect)
      _effect->removeOverlayInteract(this);
  }

  ////////////////////////////////////////////////////////////////////////////////
  /** @brief ctor */
  InteractArgs::InteractArgs(const PropertySet &props)
  {
    time          = props.propGetDouble(kOfxPropTime);
    renderScale   = getRenderScale(props);
#ifdef OFX_EXTENSIONS_NUKE
    view          = props.propGetInt(kFnOfxImageEffectPropView, 0, false);
#endif
#ifdef OFX_EXTENSIONS_NATRON
    pickerColour.r = pickerColour.g = pickerColour.b = pickerColour.a = -1.;
    if ( !props.propExists(kNatronOfxPropPickerColour) ) {
      hasPickerColour = false;
    } else {
      props.propGetDoubleN(kNatronOfxPropPickerColour, &pickerColour.r, 4, false);
      hasPickerColour = (pickerColour.r != -1. || pickerColour.g != -1. || pickerColour.b != -1. || pickerColour.a != -1.);
    }
    screenPixelRatio = 1.;
    screenPixelRatio = props.propGetDouble(kOfxInteractPropScreenPixelRatio, 0, false);
    // the dispatcher (interactMainEntry) updated the effect's state from these inArgs before
    // building the args, see Private::updateKeyModifiers()
    int hostModifiers = 0;
    hasKeyModifiers = OFX::Private::fetchKeyModifiers(props, &hostModifiers); // the value: see the dispatcher
    (void)hostModifiers;
    keyModifiers = 0;
#endif
  }

  ////////////////////////////////////////////////////////////////////////////////
  // DrawContext: wraps the OFX 1.5 DrawSuite, or the OpenGL backend (Private::DrawContextBackend,
  // ofxsDrawContextOpenGL.cpp, only called through virtual functions so that this file has no
  // reference to it)

  static void initDrawContextState(OfxRGBAColourF &colour, float &lineWidth, OfxDrawLineStipplePattern &lineStipple)
  {
    colour.r = colour.g = colour.b = colour.a = 1.f;
    lineWidth = 0.f;
    lineStipple = kOfxDrawLineStipplePatternSolid;
  }

  DrawContext::DrawContext()
    : _handle(0)
    , _openGL(0)
    , _screenPixelRatio(1.)
    , _drawSuiteShadows(false)
  {
    _pixelScale.x = _pixelScale.y = 1.;
    initDrawContextState(_colour, _lineWidth, _lineStipple);
  }

  DrawContext::DrawContext(OfxDrawContextHandle handle)
    : _handle(handle)
    , _openGL(0)
    , _screenPixelRatio(1.)
    , _drawSuiteShadows(false)
  {
    _pixelScale.x = _pixelScale.y = 1.;
    initDrawContextState(_colour, _lineWidth, _lineStipple);
  }

  bool DrawContext::getColour(OfxStandardColour stdColour, OfxRGBAColourF &colour) const
  {
    if (_handle) {
      return OFX::Private::gDrawSuite &&
             OFX::Private::gDrawSuite->getColour(_handle, stdColour, &colour) == kOfxStatOK;
    }
    return _openGL && _openGL->getColour(*this, stdColour, &colour) == kOfxStatOK;
  }

  bool DrawContext::setColour(const OfxRGBAColourF &colour) const
  {
    if (_handle) {
      return OFX::Private::gDrawSuite &&
             OFX::Private::gDrawSuite->setColour(_handle, &colour) == kOfxStatOK;
    }
    return _openGL && _openGL->setColour(*this, &colour) == kOfxStatOK;
  }

  bool DrawContext::setColour(float r, float g, float b, float a) const
  {
    OfxRGBAColourF colour;
    colour.r = r; colour.g = g; colour.b = b; colour.a = a;
    return setColour(colour);
  }

  bool DrawContext::setLineWidth(float width) const
  {
    if (_handle) {
      return OFX::Private::gDrawSuite &&
             OFX::Private::gDrawSuite->setLineWidth(_handle, width) == kOfxStatOK;
    }
    return _openGL && _openGL->setLineWidth(*this, width) == kOfxStatOK;
  }

  bool DrawContext::setLineStipple(OfxDrawLineStipplePattern pattern) const
  {
    if (_handle) {
      return OFX::Private::gDrawSuite &&
             OFX::Private::gDrawSuite->setLineStipple(_handle, pattern) == kOfxStatOK;
    }
    return _openGL && _openGL->setLineStipple(*this, pattern) == kOfxStatOK;
  }

  bool DrawContext::draw(OfxDrawPrimitive primitive, const OfxPointD *points, int pointCount) const
  {
    if (_handle) {
      return OFX::Private::gDrawSuite &&
             OFX::Private::gDrawSuite->draw(_handle, primitive, points, pointCount) == kOfxStatOK;
    }
    return _openGL && _openGL->draw(*this, primitive, points, pointCount) == kOfxStatOK;
  }

  bool DrawContext::drawText(const std::string &text, const OfxPointD &pos, int alignment) const
  {
    if (_handle) {
      return OFX::Private::gDrawSuite &&
             OFX::Private::gDrawSuite->drawText(_handle, text.c_str(), &pos, alignment) == kOfxStatOK;
    }
    return _openGL && _openGL->drawText(*this, text.c_str(), &pos, alignment) == kOfxStatOK;
  }

  /** @brief ctor */
  DrawArgs::DrawArgs(const PropertySet &props)
    : InteractArgs(props)
    , drawContext(NULL)
    , _overlayRegistration(Private::eOverlayRegistrationUnknown)
  {
    // OFX 1.5 DrawSuite: kOfxInteractPropDrawContext in the draw action inArgs
    OfxDrawContextHandle handle = NULL;
    if (OFX::Private::gDrawSuite) {
      handle = (OfxDrawContextHandle)props.propGetPointer(kOfxInteractPropDrawContext, false);
    }
    _drawContextStorage = DrawContext(handle);
    if (handle) {
      drawContext = &_drawContextStorage;
    }
#ifdef kOfxInteractPropViewportSize // removed in OFX 1.4
    viewportSize.x = viewportSize.y = 0.;
    props.propGetDoubleN(kOfxInteractPropViewportSize, &viewportSize.x, 2, false);
#endif
    backGroundColour = getBackgroundColour(props);
    pixelScale       = getPixelScale(props);
    // the OpenGL backend's context (its backend is set by getDrawContext())
    // (kOfxInteractPropScreenPixelRatio, Natron extension: 0 if the host doesn't set it, the
    // backend then uses 1)
#ifdef OFX_EXTENSIONS_NATRON
    const double ratio = screenPixelRatio;
    // Natron extension 14: drop shadows on the emulated DrawSuite elements (absent is 0; only
    // the OpenGL backend uses it: in V2 draws, the host's DrawSuite draws the shadows)
    const bool shadows = props.propGetInt(kNatronOfxInteractPropDrawSuiteShadows, 0, false) != 0;
#else
    const double ratio = 1.;
    const bool shadows = false;
#endif
    Private::DrawArgsAccess::initOpenGLContext(_openGLDrawContext, pixelScale, ratio, shadows);
  }

  DrawArgs::DrawArgs(const DrawArgs &other)
    : InteractArgs(other)
    , drawContext(NULL)
#ifdef kOfxInteractPropViewportSize // removed in OFX 1.4
    , viewportSize(other.viewportSize)
#endif
    , pixelScale(other.pixelScale)
    , backGroundColour(other.backGroundColour)
    , _drawContextStorage(other._drawContextStorage)
    , _openGLDrawContext(other._openGLDrawContext)
    , _overlayRegistration(other._overlayRegistration)
  {
    if (other.drawContext) {
      drawContext = &_drawContextStorage;
    }
  }

  DrawArgs &DrawArgs::operator=(const DrawArgs &other)
  {
    InteractArgs::operator=(other);
#ifdef kOfxInteractPropViewportSize // removed in OFX 1.4
    viewportSize = other.viewportSize;
#endif
    pixelScale = other.pixelScale;
    backGroundColour = other.backGroundColour;
    _drawContextStorage = other._drawContextStorage;
    _openGLDrawContext = other._openGLDrawContext;
    _overlayRegistration = other._overlayRegistration;
    drawContext = other.drawContext ? &_drawContextStorage : NULL;
    return *this;
  }


  bool DrawArgs::calledViaOverlayInteractV2() const
  {
    return _overlayRegistration == Private::eOverlayRegistrationV2;
  }

  /** @brief ctor */
  PenArgs::PenArgs(const PropertySet &props)
    : InteractArgs(props)
  {
#ifdef kOfxInteractPropViewportSize // removed in OFX 1.4
    viewportSize.x = viewportSize.y = 0.;
    props.propGetDoubleN(kOfxInteractPropViewportSize, &viewportSize.x, 2, false);
#endif
    pixelScale    = getPixelScale(props);
    backGroundColour = getBackgroundColour(props);
    penPosition.x = penPosition.y = 0;
    props.propGetDoubleN(kOfxInteractPropPenPosition, &penPosition.x, 2);
    // Introduced in OFX 1.2. Return (-1,-1) if not available
    OfxPointI pen = {-1, -1};
    props.propGetIntN(kOfxInteractPropPenViewportPosition, &pen.x, 2, false);
    penViewportPosition.x = pen.x;
    penViewportPosition.y = pen.y;
    penPressure   = props.propGetDouble(kOfxInteractPropPenPressure);
  }

  /** @brief ctor */
  KeyArgs::KeyArgs(const PropertySet &props)
    : InteractArgs(props)
  {
    time          = props.propGetDouble(kOfxPropTime);
    renderScale   = getRenderScale(props);
    keyString     = props.propGetString(kOfxPropKeyString);
    keySymbol     = props.propGetInt(kOfxPropKeySym);
  }

  /** @brief ctor */
  FocusArgs::FocusArgs(const PropertySet &props)
    : InteractArgs(props)
  {
#ifdef kOfxInteractPropViewportSize // removed in OFX 1.4
    viewportSize.x = viewportSize.y = 0.;
    props.propGetDoubleN(kOfxInteractPropViewportSize, &viewportSize.x, 2, false);
#endif
    pixelScale       = getPixelScale(props);
    backGroundColour = getBackgroundColour(props);
  }

  bool InteractDescriptor::getHasAlpha() const
  {
    return _props->propGetInt(kOfxInteractPropHasAlpha, 0, false);
  }

  int InteractDescriptor::getBitDepth() const
  {
    return _props->propGetInt(kOfxInteractPropBitDepth, 0, false);
  }

#ifdef OFX_EXTENSIONS_NATRON
  /** @brief Sets wether the interact uses an additional colour picking value in the inArgs of its actions. */
  void InteractDescriptor::setColourPicking(bool useColourPicking)
  {
    _props->propSetInt(kNatronOfxInteractColourPicking, (int)useColourPicking, 0, false);
  }
#endif

  void ParamInteractDescriptor::setInteractSizeAspect(double asp)
  {
    _props->propSetDouble(kOfxParamPropInteractSizeAspect , asp);
  }

  void ParamInteractDescriptor::setInteractMinimumSize(int x, int y)
  {
    double s[2] = {(double)x, (double)y};
    _props->propSetDoubleN(kOfxParamPropInteractMinimumSize, s, 2);
  }

  void ParamInteractDescriptor::setInteractPreferredSize(int x, int y)
  {
    int s[2] = {x, y};
    _props->propSetIntN(kOfxParamPropInteractPreferedSize, s, 2);
  }

  ParamInteract::ParamInteract(OfxInteractHandle handle, ImageEffect* effect):Interact(handle), _effect(effect)
  {}

  OfxPointI ParamInteract::getInteractSize() const
  {
    OfxPointI ret = {0, 0};
    _interactProperties.propGetIntN(kOfxParamPropInteractSize, &ret.x, 2);

    return ret;
  }

  namespace Private {

    /** @brief fetches our pointer out of the props on the handle */
    static
    Interact *retrieveInteractPointer(OfxInteractHandle handle)
    {
      Interact *instance;

      // get the prop set on the handle
      OfxPropertySetHandle propHandle;
      OfxStatus stat = OFX::Private::gInteractSuite->interactGetPropertySet(handle, &propHandle);
      throwSuiteStatusException(stat);

      // make our wrapper object
      PropertySet props(propHandle);

      // fetch the instance data out of the properties
      instance = (Interact *) props.propGetPointer(kOfxPropInstanceData);

      OFX::Log::error(instance == 0, "Instance data handle in effect instance properties is NULL!");

      // need to throw something here
      if (!instance) {
        throwSuiteStatusException(kOfxStatErrBadHandle);
      }

      // and dance to the music
      return instance;
    }

    /** @brief The common entry point used by all interacts */
    static
    OfxStatus
      interactMainEntry(const std::string     &action,
      OfxInteractHandle      handle,
      PropertySet              inArgs,
      PropertySet              /*outArgs*/,
      OverlayRegistrationEnum  registration)
    {
      OfxStatus stat = kOfxStatReplyDefault;

      // get the interact pointer
      Interact *interact = retrieveInteractPointer(handle);

      // if one was not made, return and do nothing
      if(!interact)
        return stat;

#ifdef OFX_EXTENSIONS_NATRON
      // extension 13 of ofxNatron.h: before the action, update the modifiers of the effect, only
      // from its image effect overlay interacts (the property and the key tracking): the key
      // events and loseFocus of a parameter interact (e.g. a parametric parameter's curve
      // editor) must not clear or overwrite the state tracked by the overlay
      // Only the nine interact actions touch the effect: not kOfxActionDestroyInstance, where the
      // host may already have destroyed the effect of a parameter interact.
      ImageEffect *effect = interact->getEffect();
      const bool isInteractAction =
           ( action == kOfxInteractActionDraw ||
             action == kOfxInteractActionPenMotion ||
             action == kOfxInteractActionPenDown ||
             action == kOfxInteractActionPenUp ||
             action == kOfxInteractActionKeyDown ||
             action == kOfxInteractActionKeyUp ||
             action == kOfxInteractActionKeyRepeat ||
             action == kOfxInteractActionGainFocus ||
             action == kOfxInteractActionLoseFocus );
      if ( isInteractAction && effect && inArgs.propSetHandle() && dynamic_cast<OverlayInteract *>(interact) ) {
        OFX::Private::updateKeyModifiers(*effect, action, inArgs);
      }
      // the host's value in this action's inArgs (also for parameter interacts, which don't update
      // the effect's state), else the effect's state
      int keyModifiers = (isInteractAction && effect) ? effect->getKeyModifiers() : 0;
      if ( isInteractAction && inArgs.propSetHandle() ) {
        OFX::Private::fetchKeyModifiers(inArgs, &keyModifiers);
      }
#endif

      if(action == kOfxActionDestroyInstance) {
        delete interact;
        stat = kOfxStatOK;
      }
      else if(action ==   kOfxInteractActionDraw) {
        // make the draw args
        DrawArgs drawArgs(inArgs);
#ifdef OFX_EXTENSIONS_NATRON
        drawArgs.keyModifiers = keyModifiers;
#endif
        // the backend of drawArgs.getDrawContext()
        DrawArgsAccess::setOverlayRegistration(drawArgs, registration);
        if (registration != eOverlayRegistrationV2) {
          // A V1 draw (a V1 overlay entry, or a parameter interact): "If this is called via
          // kOfxImageEffectPluginPropOverlayInteractV1, drawing SHOULD use OpenGL". V1 draws use
          // only OpenGL, so drop the host's draw context (HostSupport passes one to every
          // overlay): drawContext is NULL, and getDrawContext() never uses the DrawSuite.
          DrawArgsAccess::clearDrawSuiteContext(drawArgs);
        }
        if(interact->draw(drawArgs))
          stat = kOfxStatOK;
      }
      else if(action ==   kOfxInteractActionPenMotion) {

        // make the draw args
        PenArgs args(inArgs);
#ifdef OFX_EXTENSIONS_NATRON
        args.keyModifiers = keyModifiers;
#endif
        if(interact->penMotion(args))
          stat = kOfxStatOK;
      }
      else if(action ==   kOfxInteractActionPenDown) {
        // make the draw args
        PenArgs args(inArgs);
#ifdef OFX_EXTENSIONS_NATRON
        args.keyModifiers = keyModifiers;
#endif
        if(interact->penDown(args))
          stat = kOfxStatOK;
      }
      else if(action ==   kOfxInteractActionPenUp) {
        // make the draw args
        PenArgs args(inArgs);
#ifdef OFX_EXTENSIONS_NATRON
        args.keyModifiers = keyModifiers;
#endif
        if(interact->penUp(args))
          stat = kOfxStatOK;
      }
      else if(action ==   kOfxInteractActionKeyDown) {
        // make the draw args
        KeyArgs args(inArgs);
#ifdef OFX_EXTENSIONS_NATRON
        args.keyModifiers = keyModifiers;
#endif
        if(interact->keyDown(args))
          stat = kOfxStatOK;
      }
      else if(action ==   kOfxInteractActionKeyUp) {
        // make the draw args
        KeyArgs args(inArgs);
#ifdef OFX_EXTENSIONS_NATRON
        args.keyModifiers = keyModifiers;
#endif
        if(interact->keyUp(args))
          stat = kOfxStatOK;
      }
      else if(action ==   kOfxInteractActionKeyRepeat) {
        // make the draw args
        KeyArgs args(inArgs);
#ifdef OFX_EXTENSIONS_NATRON
        args.keyModifiers = keyModifiers;
#endif
        if(interact->keyRepeat(args))
          stat = kOfxStatOK;
      }
      else if(action ==   kOfxInteractActionGainFocus) {
        // make the draw args
        FocusArgs args(inArgs);
#ifdef OFX_EXTENSIONS_NATRON
        args.keyModifiers = keyModifiers;
#endif
        interact->gainFocus(args);
      }
      else if(action ==   kOfxInteractActionLoseFocus) {
        // make the draw args
        FocusArgs args(inArgs);
#ifdef OFX_EXTENSIONS_NATRON
        args.keyModifiers = keyModifiers;
#endif
        interact->loseFocus(args);
      }

      return stat;
    }

    /** @brief Which entry point of a descriptor class the host called */
    enum InteractEntryEnum {
      eInteractEntryMain,  // DESC::getMainEntry(): V1 or V2, as registered
      eInteractEntryOpenGL // InteractMainEntry<DESC>::overlayInteractMainEntryV1: always V1
    };

    static
    OfxStatus interactMainEntryImpl(const char             *actionRaw,
      const void             *handleRaw,
      OfxPropertySetHandle    inArgsRaw,
      OfxPropertySetHandle    outArgsRaw,
      InteractDescriptor& desc,
      InteractEntryEnum entry)
    {
      if (!actionRaw) {
        // not an action: a host error. Checked first: both Log::print("%s", NULL) and
        // std::string(NULL) are undefined behaviour.
        OFX::Log::error(true, "overlayInteractMainEntry: the requested action is a null pointer.");
        return kOfxStatFailed;
      }
      OFX::Log::print("********************************************************************************");
      OFX::Log::print("START overlayInteractMainEntry (%s)", actionRaw);
      OFX::Log::indent();
      OfxStatus stat = kOfxStatReplyDefault;

      try {
        // Cast the raw handle to be an image effect handle, because that is what it is
        OfxInteractHandle handle = (OfxInteractHandle) handleRaw;

        // Turn the arguments into wrapper objects to make our lives easier
        OFX::PropertySet inArgs(inArgsRaw);
        OFX::PropertySet outArgs(outArgsRaw);

        // turn the action into a std::string
        std::string action(actionRaw);

        // figure the actions
        if (action == kOfxActionDescribe) {
          OfxPropertySetHandle propHandle;
          // don't shadow the outer stat, which is returned to the host
          OfxStatus getStat = OFX::Private::gInteractSuite->interactGetPropertySet(handle, &propHandle);
          throwSuiteStatusException(getStat);
          PropertySet interactProperties(propHandle);
          desc.setPropertySet(&interactProperties);
          try {
            desc.describe();
          } catch (...) {
            desc.setPropertySet(NULL);
            throw;
          }
          // interactProperties is a local: don't leave the descriptor pointing to it
          desc.setPropertySet(NULL);
          // the action was trapped and all was well
          stat = kOfxStatOK;
        }
        else if (action == kOfxActionCreateInstance) 
        {
          // fetch the image effect we are being made for out of the interact's property handle
          ImageEffect *effect = retrieveEffectFromInteractHandle(handle);
          OFX::Interact* interact = desc.createInstance(handle, effect);
          (void)interact;
          // and all was well
          stat = kOfxStatOK;
        }
        else if (entry == eInteractEntryOpenGL) {
          // the OpenGL V1 entry that setOverlayInteractDescriptorV2(desc, true) registers next
          // to the V2 one: V1, and no DrawSuite context
          stat = interactMainEntry(action, handle, inArgs, outArgs, eOverlayRegistrationV1);
        }
        else {
          // V2 if this main entry was registered as kOfxImageEffectPluginPropOverlayInteractV2
          // (V1 for the other overlays and for parameter interacts)
          const OverlayRegistrationEnum registration =
            isOverlayInteractV2(desc.getMainEntry()) ? eOverlayRegistrationV2 : eOverlayRegistrationV1;
          stat = interactMainEntry(action, handle, inArgs, outArgs, registration);
        }

      }
      // the same statuses as mainEntryStr (ofxsImageEffect.cpp), except OFX_CLIENT_EXCEPTION_TYPE
      // (its handler needs the plug-in name, which is not known here)
      catch (const OFX::Exception::Suite &ex)
      {
        stat = ex.status();
      }
      catch (const OFX::Exception::HostInadequate&)
      {
        stat = kOfxStatErrMissingHostFeature;
      }
      catch (const OFX::Exception::PropertyUnknownToHost&)
      {
        stat = kOfxStatErrMissingHostFeature;
      }
      catch (const std::bad_alloc&)
      {
        stat = kOfxStatErrMemory;
      }
      catch(...)
      {
        stat = kOfxStatFailed;
      }

      OFX::Log::outdent();
      OFX::Log::print("STOP overlayInteractMainEntry (%s)", actionRaw);
      return stat;
    }

    /** @brief The main entry for image effect overlays */
    OfxStatus interactMainEntry(const char             *actionRaw,
      const void             *handleRaw,
      OfxPropertySetHandle    inArgsRaw,
      OfxPropertySetHandle    outArgsRaw,
      InteractDescriptor& desc)
    {
      return interactMainEntryImpl(actionRaw, handleRaw, inArgsRaw, outArgsRaw, desc, eInteractEntryMain);
    }

    /** @brief The OpenGL kOfxImageEffectPluginPropOverlayInteractV1 entry of an overlay
        registered with setOverlayInteractDescriptorV2() */
    OfxStatus interactMainEntryOpenGL(const char             *actionRaw,
      const void             *handleRaw,
      OfxPropertySetHandle    inArgsRaw,
      OfxPropertySetHandle    outArgsRaw,
      InteractDescriptor& desc)
    {
      return interactMainEntryImpl(actionRaw, handleRaw, inArgsRaw, outArgsRaw, desc, eInteractEntryOpenGL);
    }

  } // end namespace private

  // methods in ImageEffect which depends on ofxsInteract

  /** @brief force all overlays on this interact to be redrawn */
  void ImageEffect::redrawOverlays(void)
  {
    // find it
    std::list<OverlayInteract *>::iterator i;
    for(i = _overlayInteracts.begin(); i != _overlayInteracts.end(); ++i) {
      (*i)->requestRedraw();
    }
  }

} // end of namespace
