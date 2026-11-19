#ifndef _ofxsInteract_H_
#define _ofxsInteract_H_

/** @brief Feature macro: defined when this Support library has the OFX 1.5 DrawSuite C++ API
    (OFX::DrawContext, OFX::DrawArgs::drawContext and
    OFX::ImageEffectDescriptor::setOverlayInteractDescriptorV2()).

    Test this rather than kOfxDrawSuite: kOfxDrawSuite only says that the C header
    ofxDrawSuite.h is available, and upstream's Support library defines it without this API.
*/
#define OFXS_SUPPORT_HAS_DRAWCONTEXT 1
/** @brief Feature macro: defined when this Support library has OFX::DrawArgs::getDrawContext(),
    the DrawContext that draws with the DrawSuite in V2 draws and with OpenGL in V1 draws
    (Support/Library/ofxsDrawContextOpenGL.cpp), and DrawContext::usesDrawSuite()/usesOpenGL(). */
#define OFXS_SUPPORT_HAS_DRAWCONTEXT_OPENGL 1
/** @brief Feature macro: defined when a descriptor class registered with
    ImageEffectDescriptor::setOverlayInteractDescriptorV2() has a distinct V1 entry point
    (InteractMainEntry<DESC>::overlayInteractMainEntryV1), DrawArgs::drawContext is NULL in the
    V1 draws, and DrawArgs::calledViaOverlayInteractV2() exists. Since
    OFXS_SUPPORT_HAS_OVERLAY_HAS_OPENGL_DRAW, the V1 entry is only registered if the plug-in asks
    for it (hasOpenGLDraw). */
#define OFXS_SUPPORT_HAS_OVERLAY_INTERACT_V1_ENTRY 1
/** @brief Feature macro: defined when getDrawContext() checks at run time that an OpenGL context
    is current before using OpenGL, and DrawArgs::isOpenGLContextCurrent() (that check) exists,
    for the plug-ins that draw with their own OpenGL code in V1 draws. */
#define OFXS_SUPPORT_HAS_OPENGL_CONTEXT_CHECK 1
/** @brief Feature macro: defined when the Support library applies the overlay drawing rules of
    the spec strictly (V1 draws use only OpenGL, V2 draws only the DrawSuite; see OFX::DrawContext)
    and ImageEffectDescriptor::setOverlayInteractDescriptorV2() has the hasOpenGLDraw argument:
    on a DrawSuite host, the V1 entry is registered next to the V2 one only if hasOpenGLDraw is
    true. In a V1 draw, drawContext is NULL even if the host passed a draw context, and
    getDrawContext() uses OpenGL if a context is current, else draws nothing (never the host's
    DrawSuite). */
#define OFXS_SUPPORT_HAS_OVERLAY_HAS_OPENGL_DRAW 1
/*
OFX Support Library, a library that skins the OFX plug-in API with C++ classes.
Copyright (C) 2004-2005 The Open Effects Association Ltd
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

/** @file This file contains core code that wraps OFX 'objects' with C++ classes.

This file only holds code that is visible to a plugin implementation, and so hides much
of the direct OFX objects and any library side only functions.
*/
#include "ofxsParam.h"
#include "ofxDrawSuite.h" // OFX 1.5 DrawSuite

#include <list>

/** @brief Nasty macro used to define empty protected copy ctors and assign ops */
#define mDeclareProtectedAssignAndCC(CLASS) \
  CLASS &operator=(const CLASS &) {assert(false); return *this;}      \
  CLASS(const CLASS &) {assert(false); } 

/** @brief The core 'OFX Support' namespace, used by plugin implementations. All code for these are defined in the common support libraries.
*/
namespace OFX {

  /** @brief forward declaration */
  class ImageEffect;

  namespace Private {
    class DrawContextBackend; // ofxsSupportPrivate.h
    struct DrawArgsAccess;    // ofxsSupportPrivate.h
  }

  /** @brief Draws an overlay with the OFX 1.5 DrawSuite (OfxDrawSuiteV1), or with an OpenGL
   emulation of the DrawSuite.

   The Support library applies the spec strictly (ofxInteract.h, kOfxInteractActionDraw: "If
   this is called via kOfxImageEffectPluginPropOverlayInteractV2, drawing MUST use DrawSuite. If
   this is called via kOfxImageEffectPluginPropOverlayInteractV1, drawing SHOULD use OpenGL."):
   - a V2 draw (the kOfxImageEffectPluginPropOverlayInteractV2 entry of an overlay for which
     ImageEffectDescriptor::setOverlayInteractDescriptorV2() returned true) uses only the
     DrawSuite, never OpenGL: the host may have no OpenGL context at all.
   - a V1 draw (an overlay registered with setOverlayInteractDescriptor(), with
     setOverlayInteractDescriptorV2() on a host without the DrawSuite, the V1 entry that
     setOverlayInteractDescriptorV2(desc, true) also registers on a DrawSuite host, and every
     parameter interact) uses only OpenGL, never the DrawSuite, even if the host passed a draw
     context (HostSupport does).

   DrawArgs::getDrawContext() (always valid) follows these rules:
   - V2 draw: the host's DrawSuite (usesDrawSuite()). If the host passed no
     kOfxInteractPropDrawContext, a context that draws nothing.
   - V1 draw: OpenGL (usesOpenGL()) if an OpenGL context is current, else a context that draws
     nothing.
   - DrawArgs not made by the Support library's dispatcher (e.g. built by the plug-in from a
     property set): the DrawSuite if the inArgs have a draw context, else OpenGL if an OpenGL
     context is current, else a context that draws nothing. This is the one exception to the
     rules above: such DrawArgs don't know whether the draw is V1 or V2, so they may use the
     DrawSuite in a V1 draw. Use the DrawArgs that draw() gets.
   DrawArgs::drawContext is the host's DrawSuite context in a V2 draw (NULL if it passed none),
   and NULL in every V1 draw.

   The only compatibility layer: a plug-in that draws only with the DrawSuite, on a host that
   only has V1 and OpenGL (no DrawSuite, e.g. an OFX 1.4 host). There setOverlayInteractDescriptorV2()
   registers V1, and getDrawContext() converts the DrawSuite calls to OpenGL. So a plug-in that
   registers with setOverlayInteractDescriptorV2(desc) and draws only through getDrawContext(),
   with no OpenGL call, works on every host. A plug-in that also has its own OpenGL drawing
   code keeps it, registers with setOverlayInteractDescriptorV2(desc, true), and in draw() uses
   OpenGL only if !calledViaOverlayInteractV2() (and isOpenGLContextCurrent()), not if
   drawContext == NULL.

   Plug-ins must not use OpenGL outside V1 draws. kOfxInteractPropBitDepth,
   kOfxInteractPropHasAlpha, kOfxInteractPropPenViewportPosition (Interact::getBitDepth(),
   hasAlpha(), PenArgs::penViewportPosition) and interactSwapBuffers are defined in OpenGL terms:
   don't rely on them when drawing with the DrawSuite.

   A DrawContext is only valid during the draw action it was passed to (the OpenGL backend
   draws into the current OpenGL context). The functions never throw: they return false where
   the DrawSuite would return an error (kOfxStatErrValue for an invalid enum, a point count
   that is not valid for the primitive, or no text; kOfxStatFailed).

   Coordinates are canonical, as for OpenGL overlays ("The GL_PROJECTION matrix will be set up
   so that it maps openGL coordinates to canonical image coordinates", ofxInteracts.rst). The
   spec doesn't give the DrawSuite's coordinate system; canonical is implied (the upstream
   DrawSuite example draws at canonical parameter positions, sized with the pixel scale), and
   this library assumes it:
   DrawArgs::pixelScale is the size of a screen pixel in canonical coordinates. The OpenGL
   backend uses the current OpenGL matrices, which the host sets up that way.

   The OpenGL backend (Support/Library/ofxsDrawContextOpenGL.cpp) emulates the DrawSuite with
   OpenGL 1.x compatibility-profile calls, and saves and restores all the OpenGL state it
   changes (glPushAttrib/glPopAttrib around each draw() and drawText()). Its drawing code is
   the OpenGL DrawSuite core, include/ofxDrawSuiteOpenGL.h (OFX::DrawSuiteOpenGL), which hosts
   can use too:
   - its state (colour, line width, stipple) starts as opaque white, width 0, solid, for each
     draw action. Set the colour before drawing.
   - setColour: "over" compositing (GL_BLEND with GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA).
   - setLineWidth: in screen pixels, multiplied by the host's screen pixel ratio
     (kOfxInteractPropScreenPixelRatio, Natron extension, 1 if absent), since "The host should
     adjust for screen density". Width 0 is an aliased single-pixel line, any other width a
     smooth (GL_LINE_SMOOTH) line.
   - setLineStipple: GL_LINE_STIPPLE, with the patterns (in screen pixels): dot 2 on 2 off,
     dash 8 on 8 off, alt dash 8 off 8 on, dot dash 2 on 3 off 8 on 3 off.
   - draw: Lines needs an even point count (as HostSupport's DrawSuite), Rectangle and Ellipse
     exactly 2 points, Polygon at least 3, the strips and loops at least 2.
   - Ellipse: a line loop, tessellated so that it is within 1/4 screen pixel of the ellipse
     (OFX::DrawSuiteOpenGL::ellipseSegmentCount(): 8 to 4096 vertices).
   - getColour: fixed colours (OFX::DrawSuiteOpenGL::standardColour()), close to what the
     Natron and SupportExt OpenGL overlays use.
   - drawText: a built-in 5x7 pixel font (printable ASCII; other characters are drawn as '?',
     control characters are skipped), one font pixel per screen pixel (the text has a constant
     size on screen), 6 pixels per character, 7 pixels above the baseline and 1 below.
   The OpenGL backend is not compiled if OFXS_DRAWCONTEXT_NO_OPENGL is defined when compiling
   ofxsDrawContextOpenGL.cpp (for plug-in binaries built without OpenGL): getDrawContext() on a
   V1 overlay then draws nothing (canDraw() is false). Plug-ins that call getDrawContext() must
   compile and link ofxsDrawContextOpenGL.cpp, which needs no OpenGL header (the core declares
   the OpenGL types and constants it uses) and doesn't link OpenGL: it loads the OpenGL 1.1 functions at run time, at the first getDrawContext() that may
   use OpenGL (a draw action called through a V1 entry, a parameter interact's draw action, or
   DrawArgs not made by the dispatcher without a draw context), with dlopen/dlsym of libGL.so.1,
   of the OpenGL framework on macOS, GetProcAddress on opengl32.dll on Windows (-ldl on Linux
   before glibc 2.34). So a plug-in that calls it still loads on a machine without OpenGL; if
   OpenGL can't be loaded, getDrawContext() behaves as without a current OpenGL context (above).
   From libOfxSupport.a (CMake's openfx::Support), plug-ins that don't call it don't link it;
   Support/Plugins/Makefile.master's SUPPORTOBJECTS are plain object files, so every plug-in built
   with it links ofxsDrawContextOpenGL.o.
   The legacy project files (Support/Support.xcodeproj, the .vcproj and .dsp files) are not
   maintained and don't list ofxsDrawContextOpenGL.cpp.
   */
  class DrawContext {
  public:
    /** @brief a context that draws nothing */
    DrawContext();
    /** @brief a context that draws with the host's DrawSuite (draws nothing if handle is NULL) */
    explicit DrawContext(OfxDrawContextHandle handle);

    /** @brief the C handle (kOfxInteractPropDrawContext), NULL with the OpenGL backend */
    OfxDrawContextHandle getHandle() const { return _handle; }

    /** @brief true if this context draws with the host's DrawSuite */
    bool usesDrawSuite() const { return _handle != 0; }

    /** @brief true if this context draws with OpenGL (overlay registered as V1) */
    bool usesOpenGL() const { return _handle == 0 && _openGL != 0; }

    /** @brief false if this context draws nothing (every function then returns false) */
    bool canDraw() const { return usesDrawSuite() || usesOpenGL(); }

    /** @brief get one of the host's standard overlay colours */
    bool getColour(OfxStandardColour stdColour, OfxRGBAColourF &colour) const;

    /** @brief set the colour of the next primitives ("over" compositing if not opaque) */
    bool setColour(const OfxRGBAColourF &colour) const;
    bool setColour(float r, float g, float b, float a = 1.f) const;

    /** @brief set the line width: 0 for a single pixel line, else smooth line of that width */
    bool setLineWidth(float width) const;

    /** @brief set the line stipple pattern */
    bool setLineStipple(OfxDrawLineStipplePattern pattern) const;

    /** @brief draw a primitive (lines, line strip/loop, rectangle, polygon, ellipse) */
    bool draw(OfxDrawPrimitive primitive, const OfxPointD *points, int pointCount) const;

    /** @brief draw UTF-8 text at pos, aligned with kOfxDrawTextAlignment* flags (font chosen by the host) */
    bool drawText(const std::string &text, const OfxPointD &pos, int alignment = kOfxDrawTextAlignmentLeft | kOfxDrawTextAlignmentBaseline) const;

  private:
    friend class Private::DrawContextBackend;
    friend struct Private::DrawArgsAccess;

    OfxDrawContextHandle _handle;
    const Private::DrawContextBackend *_openGL; // the OpenGL backend, NULL if none
    // for the OpenGL backend: the draw action's arguments, and the emulated DrawSuite state
    OfxPointD _pixelScale;
    double _screenPixelRatio;
    mutable OfxRGBAColourF _colour;
    mutable float _lineWidth;
    mutable OfxDrawLineStipplePattern _lineStipple;
  };

  /// all image effect interacts have these argumens
  struct InteractArgs {
    /// ctor
    InteractArgs(const PropertySet &props);
    double          time;              /**< @brief The current effect time to draw at */
    OfxPointD       renderScale;       /**< @brief The current render scale being applied to any image that would be fetched */
#ifdef OFX_EXTENSIONS_NUKE
    int             view;              /**< @brief The current effect view to draw onto */
#endif
#ifdef OFX_EXTENSIONS_NATRON
    bool            hasPickerColour;   /**< @brief If true, the pickerColour is valid and may be used */
    OfxRGBAColourD  pickerColour;      /**< @brief The picked colour */

    /**@brief pixel ratio of the screen, typically 2 for Retina screen. Should be used for functions using screen pixels such as glPointSize, glLineWidth, etc...*/
    double          screenPixelRatio;

    /** @brief The keyboard modifiers, a bitmask of kNatronOfxKeyModifier* (ofxNatron.h, extension 13):
        the host's kNatronOfxInteractPropKeyModifiers (or kOfxInteractPropKeyModifiers) if it passed
        it in the inArgs of this action, else the value of ImageEffect::getKeyModifiers() for the
        effect of the interact (the state tracked from the modifier key events of its overlays).
        Only valid in the args built by the Support dispatch for an interact action (the
        constructor sets it to 0) */
    int             keyModifiers;
    /** @brief If true, the host passed the keyboard modifiers in the inArgs of this action
        (otherwise keyModifiers is the tracked state) */
    bool            hasKeyModifiers;
#endif
  };


  /** @brief struct to pass arguments into OFX::Interact::draw */
  struct DrawArgs : public InteractArgs {
    DrawArgs(const PropertySet &props);
    DrawArgs(const DrawArgs &other);
    DrawArgs &operator=(const DrawArgs &other);

    /** @brief The host's OFX 1.5 DrawSuite context (kOfxInteractPropDrawContext) in a V2 draw,
        which MUST draw with it ("If this is called via kOfxImageEffectPluginPropOverlayInteractV2,
        drawing MUST use DrawSuite"); NULL if the host passed none. NULL in every V1 draw ("If
        this is called via kOfxImageEffectPluginPropOverlayInteractV1, drawing SHOULD use
        OpenGL"), even if the host passed a draw context. NULL does not mean that OpenGL can be
        used: see calledViaOverlayInteractV2() and isOpenGLContextCurrent(). See OFX::DrawContext. */
    const DrawContext *drawContext;

    /** @brief The context to draw this overlay with: the DrawSuite in a V2 draw, OpenGL in a V1
        draw if an OpenGL context is current, else a context that draws nothing (see
        OFX::DrawContext). Never NULL. Defined in ofxsDrawContextOpenGL.cpp. */
    const DrawContext &getDrawContext() const;

    /** @brief true if the Support library's dispatcher made these args for a V2 draw: a draw
        action called through the kOfxImageEffectPluginPropOverlayInteractV2 entry of an overlay
        registered with ImageEffectDescriptor::setOverlayInteractDescriptorV2() (drawing MUST use
        the DrawSuite); false in V1 draws (drawing uses only OpenGL), parameter interacts, and
        DrawArgs not made by the dispatcher. An interact registered with
        setOverlayInteractDescriptorV2(desc, true) may be created through either entry: test this
        in draw(), not the host's DrawSuite support. Defined in ofxsInteract.cpp
        (OFXS_SUPPORT_HAS_OVERLAY_INTERACT_V1_ENTRY). */
    bool calledViaOverlayInteractV2() const;

    /** @brief true if an OpenGL context is current on the calling thread: the run-time check
        that getDrawContext() makes before using OpenGL (wglGetCurrentContext,
        CGLGetCurrentContext, glXGetCurrentContext or eglGetCurrentContext (and, on ELF systems,
        OSMesaGetCurrentContext from the library of the gl* functions), loaded at run time
        like the OpenGL functions; false if none can be loaded). Evaluated at each call. Legacy
        OpenGL calls without a current context usually crash (macOS): a plug-in that draws with
        its own OpenGL code in a V1 draw should test this first, and draw nothing if it is
        false. It may load OpenGL (see DrawContext): only call it in draw actions. Defined in
        ofxsDrawContextOpenGL.cpp (false if it was compiled with OFXS_DRAWCONTEXT_NO_OPENGL);
        OFXS_SUPPORT_HAS_OPENGL_CONTEXT_CHECK. */
    bool isOpenGLContextCurrent() const;

#ifdef kOfxInteractPropViewportSize // removed in OFX 1.4
    OfxPointD       viewportSize;      /**< @brief The openGL viewport size for the instance */
#endif
    OfxPointD       pixelScale;        /**< @brief The size of a real screen pixel under the interact's canonical projection */
    OfxRGBColourD   backGroundColour;  /**< @brief The current background colour, ignore the A */

  private:
    friend struct Private::DrawArgsAccess;
    DrawContext     _drawContextStorage; // drawContext points here when valid
    mutable DrawContext _openGLDrawContext; // getDrawContext() for V1 overlays (it sets the backend)
    int             _overlayRegistration; // Private::eOverlayRegistration*, set by the dispatcher
  };

  /** @brief POD  to pass arguments into OFX::Interact pen actions */
  struct PenArgs : public InteractArgs {
    PenArgs(const PropertySet &props);

#ifdef kOfxInteractPropViewportSize // removed in OFX 1.4
    OfxPointD       viewportSize;      /**< @brief The openGL viewport size for the instance */
#endif
    OfxPointD       pixelScale;        /**< @brief The size of a real screen pixel under the interact's canonical projection */
    OfxRGBColourD   backGroundColour;  /**< @brief The current background colour, ignore the A */
    OfxPointD       penPosition;       /**< @brief The current pen position */
    OfxPointD       penViewportPosition;/**< @brief The current pen position in viewport coordinates */
    double          penPressure;       /**< @brief The normalised pressure on the pen */
  };

  /** @brief  struct to pass arguments into OFX::Interact key actions 

  Note 
  - some keys cannot be represented as UTF8 strings (eg: the key pad page down key kOfxKey_KP_Page_Up), in which case the key string will be set to "".
  - some UTF8 symbols (generally non-English language ones) cannot be represented by one of the keySymbols, in which case the UTF8 string will be set to some non empty value, but the keySymbol will be set to kOfxKey_Unknown.
  - in no case will keyString be set to "" and keySymbol be set to kOfxKey_Unknown.
  */
  struct KeyArgs : public InteractArgs {
    KeyArgs(const PropertySet &props);

    int             keySymbol;         /**< @brief The key represented as one of the entries in ofxKeySyms.h, see note in OFX::KeyArgs */
    std::string     keyString;         /**< @brief That key as a UTF8 string, see note in OFX::KeyArgs */
  };

  /** @brief struct to pass arguments into OFX::Interact focus actions */
  struct FocusArgs : public InteractArgs {
    FocusArgs(const PropertySet &props);

#ifdef kOfxInteractPropViewportSize // removed in OFX 1.4
    OfxPointD       viewportSize;      /**< @brief The openGL viewport size for the instance */
#endif
    OfxPointD       pixelScale;        /**< @brief The size of a real screen pixel under the interact's canonical projection */
    OfxRGBColourD   backGroundColour;  /**< @brief The current background colour, ignore the A */
  };

  /** @brief Wraps up an OFX interact object for an Image Effect. It won't work for any other plug-in type at present (it would need to be broken into a hierarchy of classes).
  */
  class InteractAbstract {
  public:
    /** @brief virtual destructor */
    virtual ~InteractAbstract() {}

    ////////////////////////////////////////////////////////////////////////////////
    // override the below in derived classes to do something useful

    /** @brief the function called to draw in the interact */
    virtual bool draw(const DrawArgs &args) = 0;

    /** @brief the function called to handle pen motion in the interact

    returns true if the interact trapped the action in some sense. This will block the action being passed to 
    any other interact that may share the viewer.
    */
    virtual bool penMotion(const PenArgs &args) = 0;

    /** @brief the function called to handle pen down events in the interact 

    returns true if the interact trapped the action in some sense. This will block the action being passed to 
    any other interact that may share the viewer.
    */
    virtual bool penDown(const PenArgs &args) = 0;

    /** @brief the function called to handle pen up events in the interact 

    returns true if the interact trapped the action in some sense. This will block the action being passed to 
    any other interact that may share the viewer.
    */
    virtual bool penUp(const PenArgs &args) = 0;

    /** @brief the function called to handle key down events in the interact 

    returns true if the interact trapped the action in some sense. This will block the action being passed to 
    any other interact that may share the viewer.
    */
    virtual bool keyDown(const KeyArgs &args) = 0;

    /** @brief the function called to handle key up events in the interact 

    returns true if the interact trapped the action in some sense. This will block the action being passed to 
    any other interact that may share the viewer.
    */
    virtual bool keyUp(const KeyArgs &args) = 0;

    /** @brief the function called to handle key down repeat events in the interact 

    returns true if the interact trapped the action in some sense. This will block the action being passed to 
    any other interact that may share the viewer.
    */
    virtual bool keyRepeat(const KeyArgs &args) = 0;

    /** @brief Called when the interact is given input focus */
    virtual void gainFocus(const FocusArgs &args) = 0;

    /** @brief Called when the interact is loses input focus */
    virtual void loseFocus(const FocusArgs &args) = 0;
  };

  class Interact : public InteractAbstract {
  protected :
    OfxInteractHandle  _interactHandle;     /**< @brief The handle for this interact */
    PropertySet        _interactProperties; /**< @brief The property set on this interact */
    std::list<Param *> _slaveParams;        /**< @brief List of params we are currently slaved to */
    ImageEffect *_effect;                   /**< @brief The instance we are associated with */

  public : 
    /** @brief ctor */
    Interact(OfxInteractHandle handle);

    /** @brief The effect instance this interact belongs to */
    ImageEffect *getEffect() const { return _effect; }

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief The keyboard modifiers of the effect of this interact, see ImageEffect::getKeyModifiers() */
    int getKeyModifiers() const;
#endif

    /** @brief virtual destructor */
    virtual ~Interact();

    PropertySet &getProperties() { return _interactProperties; }

    /** @brief The bitdepth of each component in the openGL frame buffer */
    int getBitDepth(void) const;

    /** @brief Does the openGL frame buffer have an alpha */
    bool hasAlpha(void) const;

    /** @brief Returns the size of a real screen pixel under the interact's cannonical projection */
    OfxPointD getPixelScale(void) const;

    /** @brief The suggested colour to draw a widget in an interact. Returns false if there is no suggestion. */
    bool getSuggestedColour(OfxRGBColourD &c) const;

    /** @brief the background colour */
    OfxRGBColourD getBackgroundColour(void) const;

    /** @brief Set a param that the interact should be redrawn on if its value changes */
    void addParamToSlaveTo(Param *p);

    /** @brief Remova a param that the interact should be redrawn on if its value changes */
    void removeParamToSlaveTo(Param *p);

    /** @brief Request a redraw */
    void requestRedraw(void) const;

    /** @brief Swap a buffer in the case of a double bufferred interact, this is possibly a silly one */
    void swapBuffers(void) const;

#ifdef OFX_EXTENSIONS_NATRON
    /** @brief Sets wether the interact uses an additional colour picking value in the inArgs of its actions. */
    void setColourPicking(bool useColourPicking);

    /** @brief Request a window-system cursor for this interact's view (proposed extension
        NatronGitHub/openfx#4), e.g. kOfxInteractCursorPointer, or kOfxInteractCursorNone to hide
        the cursor when the interact draws its own.

        Call it during a pen or key action, and return true (kOfxStatOK) from that action: the
        host displays the cursor of the interact that last trapped a pen or key event. Set
        kOfxInteractCursorDefault to give the host's cursor back.

        @returns false, and does nothing, if the host does not support interact cursors or does not
        list that cursor in kOfxInteractHostPropSupportedCursors. Never throws.
    */
    bool setCursor(const std::string &name);

    /** @brief Request a custom cursor image (kOfxInteractCursorCustom): a PNG file, with an alpha
        channel, in the Contents/Resources directory of the plug-in bundle (an optional "@2x" variant
        may be used on high-density displays), with its hot spot in image pixels from the top-left.

        @returns false, and does nothing, if the host does not support custom cursors. Never throws.
    */
    bool setCustomCursor(const std::string &pngFileName, int hotSpotX, int hotSpotY);
#endif

    ////////////////////////////////////////////////////////////////////////////////
    // override the below in derived classes to do something useful

    /** @brief the function called to draw in the interact */
    virtual bool draw(const DrawArgs &args);

    /** @brief the function called to handle pen motion in the interact

    returns true if the interact trapped the action in some sense. This will block the action being passed to 
    any other interact that may share the viewer.
    */
    virtual bool penMotion(const PenArgs &args);

    /** @brief the function called to handle pen down events in the interact 

    returns true if the interact trapped the action in some sense. This will block the action being passed to 
    any other interact that may share the viewer.
    */
    virtual bool penDown(const PenArgs &args);

    /** @brief the function called to handle pen up events in the interact 

    returns true if the interact trapped the action in some sense. This will block the action being passed to 
    any other interact that may share the viewer.
    */
    virtual bool penUp(const PenArgs &args);

    /** @brief the function called to handle key down events in the interact 

    returns true if the interact trapped the action in some sense. This will block the action being passed to 
    any other interact that may share the viewer.
    */
    virtual bool keyDown(const KeyArgs &args);

    /** @brief the function called to handle key up events in the interact 

    returns true if the interact trapped the action in some sense. This will block the action being passed to 
    any other interact that may share the viewer.
    */
    virtual bool keyUp(const KeyArgs &args);

    /** @brief the function called to handle key down repeat events in the interact 

    returns true if the interact trapped the action in some sense. This will block the action being passed to 
    any other interact that may share the viewer.
    */
    virtual bool keyRepeat(const KeyArgs &args);

    /** @brief Called when the interact is given input focus */
    virtual void gainFocus(const FocusArgs &args);

    /** @brief Called when the interact is loses input focus */
    virtual void loseFocus(const FocusArgs &args);
  };

  /** @brief an interact for an image effect overlay */
  class OverlayInteract : public Interact {
  public :
    /** @brief ctor */
    OverlayInteract(OfxInteractHandle handle);

    /** @brief dtor */
    virtual ~OverlayInteract();
  };

  /** @brief an interact for an image effect overlay made from an InteractAbstract helper class */
  template<class InteractHelper1>
  class OverlayInteractFromHelper : public OFX::OverlayInteract, private InteractHelper1
  {
  public:
    OverlayInteractFromHelper(OfxInteractHandle handle, OFX::ImageEffect* effect)
     : OFX::OverlayInteract(handle)
        , InteractHelper1(effect, this)
    {
    }

  private:
    virtual bool draw(const DrawArgs &args) {
      return InteractHelper1::draw(args);
    }

    virtual bool penMotion(const PenArgs &args) {
      return InteractHelper1::penMotion(args);
    }

    virtual bool penDown(const PenArgs &args) {
      return InteractHelper1::penDown(args);
    }

    virtual bool penUp(const PenArgs &args) {
      return InteractHelper1::penUp(args);
    }

    virtual bool keyDown(const OFX::KeyArgs &args) {
      return InteractHelper1::keyDown(args);
    }

    virtual bool keyUp(const OFX::KeyArgs &args) {
      return InteractHelper1::keyUp(args);
    }

    virtual bool keyRepeat(const KeyArgs &args) {
      return InteractHelper1::keyRepeat(args);
    }

    virtual void gainFocus(const FocusArgs &args) {
      InteractHelper1::gainFocus(args);
    }

    virtual void loseFocus(const FocusArgs &args) {
      InteractHelper1::loseFocus(args);
    }
  };

  /** @brief an interact for an image effect overlay made from two InteractAbstract helper classes */
  template<class InteractHelper1, class InteractHelper2>
  class OverlayInteractFromHelpers2 : public OFX::OverlayInteract, private InteractHelper1, InteractHelper2
  {
  public:
    OverlayInteractFromHelpers2(OfxInteractHandle handle, OFX::ImageEffect* effect)
     : OFX::OverlayInteract(handle)
     , InteractHelper1(effect, this)
        , InteractHelper2(effect, this)
    {
    }

  private:
    virtual bool draw(const DrawArgs &args) {
      // draw all interacts, in reverse order
      bool ret2 = InteractHelper2::draw(args);
      bool ret1 = InteractHelper1::draw(args);
      return ret1 || ret2;
    }

    virtual bool penMotion(const PenArgs &args) {
      return InteractHelper1::penMotion(args) || InteractHelper2::penMotion(args);
    }

    virtual bool penDown(const PenArgs &args) {
      return InteractHelper1::penDown(args) || InteractHelper2::penDown(args);
    }

    virtual bool penUp(const PenArgs &args) {
      return InteractHelper1::penUp(args) || InteractHelper2::penUp(args);
    }

    virtual bool keyDown(const OFX::KeyArgs &args) {
      return InteractHelper1::keyDown(args) || InteractHelper2::keyDown(args);
    }

    virtual bool keyUp(const OFX::KeyArgs &args) {
      return InteractHelper1::keyUp(args) || InteractHelper2::keyUp(args);
    }

    virtual bool keyRepeat(const KeyArgs &args) {
      return InteractHelper1::keyRepeat(args) || InteractHelper2::keyRepeat(args);
    }

    virtual void gainFocus(const FocusArgs &args) {
      InteractHelper1::gainFocus(args); InteractHelper2::gainFocus(args);
    }

    virtual void loseFocus(const FocusArgs &args) {
      InteractHelper1::loseFocus(args); InteractHelper2::loseFocus(args);
    }
  };

  /** @brief an interact for an image effect overlay made from three InteractAbstract helper classes */
  template<class InteractHelper1, class InteractHelper2, class InteractHelper3>
  class OverlayInteractFromHelpers3 : public OFX::OverlayInteract, private InteractHelper1, InteractHelper2, InteractHelper3
  {
  public:
    OverlayInteractFromHelpers3(OfxInteractHandle handle, OFX::ImageEffect* effect)
     : OFX::OverlayInteract(handle)
     , InteractHelper1(effect, this)
     , InteractHelper2(effect, this)
     , InteractHelper3(effect, this)
    {
    }

  private:
    virtual bool draw(const DrawArgs &args) {
      // draw all interacts, in reverse order
      bool ret3 = InteractHelper3::draw(args);
      bool ret2 = InteractHelper2::draw(args);
      bool ret1 = InteractHelper1::draw(args);
      return ret1 || ret2 || ret3;
    }

    virtual bool penMotion(const PenArgs &args) {
      return InteractHelper1::penMotion(args) || InteractHelper2::penMotion(args) || InteractHelper3::penMotion(args);
    }

    virtual bool penDown(const PenArgs &args) {
      return InteractHelper1::penDown(args) || InteractHelper2::penDown(args) || InteractHelper3::penDown(args);
    }

    virtual bool penUp(const PenArgs &args) {
      return InteractHelper1::penUp(args) || InteractHelper2::penUp(args) || InteractHelper3::penUp(args);
    }

    virtual bool keyDown(const OFX::KeyArgs &args) {
      return InteractHelper1::keyDown(args) || InteractHelper2::keyDown(args) || InteractHelper3::keyDown(args);
    }

    virtual bool keyUp(const OFX::KeyArgs &args) {
      return InteractHelper1::keyUp(args) || InteractHelper2::keyUp(args) || InteractHelper3::keyUp(args);
    }

    virtual bool keyRepeat(const KeyArgs &args) {
      return InteractHelper1::keyRepeat(args) || InteractHelper2::keyRepeat(args) || InteractHelper3::keyRepeat(args);
    }

    virtual void gainFocus(const FocusArgs &args) {
      InteractHelper1::gainFocus(args); InteractHelper2::gainFocus(args); InteractHelper3::gainFocus(args);
    }

    virtual void loseFocus(const FocusArgs &args) {
      InteractHelper1::loseFocus(args); InteractHelper2::loseFocus(args);; InteractHelper3::loseFocus(args);
    }
  };

  class InteractDescriptor
  {
  public:
    InteractDescriptor():_props(NULL) {}
    virtual ~InteractDescriptor() {}
    void setPropertySet(OFX::PropertySet* props){ _props = props; }
    virtual Interact* createInstance(OfxInteractHandle handle, ImageEffect *effect) = 0;
    bool getHasAlpha() const;
    int getBitDepth() const;
#ifdef OFX_EXTENSIONS_NATRON
    /** @brief Sets wether the interact uses an additional colour picking value in the inArgs of its actions. */
    void setColourPicking(bool useColourPicking);
#endif
    virtual OfxPluginEntryPoint* getMainEntry() = 0;
    virtual void describe() {}
  protected:
    OFX::PropertySet* _props;
  };

  typedef InteractDescriptor EffectOverlayDescriptor;

  class ParamInteractDescriptor : public InteractDescriptor
  {
  public:
    ParamInteractDescriptor():InteractDescriptor(){}
    virtual ~ParamInteractDescriptor() {}
    void setInteractSizeAspect(double asp);
    void setInteractMinimumSize(int x, int y);
    void setInteractPreferredSize(int x, int y);
    virtual void setParamName(const std::string& pName) { _paramName = pName; }
  protected:
    std::string _paramName;
  };

  class ParamInteract : public Interact 
  {
  public:
    ParamInteract(OfxInteractHandle handle, ImageEffect* effect);
    virtual ~ParamInteract() {}
    double getInteractSizeAspect() const;
    OfxPointI getInteractMinimumSize() const;
    OfxPointI getInteractPreferredSize() const;
    OfxPointI getInteractSize() const;
  protected:
    ImageEffect* _effect;
  };

  namespace Private
  {
    OfxStatus interactMainEntry(const char		*actionRaw,
      const void		*handleRaw,
      OfxPropertySetHandle	 inArgsRaw,
      OfxPropertySetHandle	 outArgsRaw,
      InteractDescriptor& desc);

    /** @brief The main entry of the OpenGL kOfxImageEffectPluginPropOverlayInteractV1 entry
        point that ImageEffectDescriptor::setOverlayInteractDescriptorV2(desc, true) registers
        next to the V2 one on DrawSuite hosts: as interactMainEntry(), but the draw actions are
        V1 ones (DrawArgs::drawContext is NULL; getDrawContext() uses OpenGL if an OpenGL
        context is current, else draws nothing) */
    OfxStatus interactMainEntryOpenGL(const char		*actionRaw,
      const void		*handleRaw,
      OfxPropertySetHandle	 inArgsRaw,
      OfxPropertySetHandle	 outArgsRaw,
      InteractDescriptor& desc);

    /** @brief Record that mainEntry has an OpenGL V1 sibling, mainEntryV1 (ofxsImageEffect.cpp).
        Called by the InteractMainEntry<DESC> constructor, i.e. when a descriptor is made,
        before ImageEffectDescriptor::setOverlayInteractDescriptorV2() looks it up. */
    void setOverlayInteractV1Sibling(OfxPluginEntryPoint *mainEntry, OfxPluginEntryPoint *mainEntryV1);
  }

  /** @brief The main entry points of the interact descriptor class DESC

      overlayInteractMainEntry is the entry point that DESC::getMainEntry() returns (and that
      setOverlayInteractDescriptor() and setOverlayInteractDescriptorV2() register).
      overlayInteractMainEntryV1 is a second entry point for the same DESC, which
      setOverlayInteractDescriptorV2(desc, true) registers as the OpenGL
      kOfxImageEffectPluginPropOverlayInteractV1 entry on DrawSuite hosts: the two entry points
      are distinct, so the dispatcher knows which one the host called. The constructor records
      the pair (no data member: the layout of the descriptor classes is unchanged). */
  template<class DESC>
  class InteractMainEntry
  {
  protected:
    InteractMainEntry()
    {
      OFX::Private::setOverlayInteractV1Sibling(&InteractMainEntry<DESC>::overlayInteractMainEntry,
                                                &InteractMainEntry<DESC>::overlayInteractMainEntryV1);
    }

    static OfxStatus overlayInteractMainEntry(const char *action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out)
    {
      return OFX::Private::interactMainEntry(action, handle, in, out, descriptor());
    }

    static OfxStatus overlayInteractMainEntryV1(const char *action, const void* handle, OfxPropertySetHandle in, OfxPropertySetHandle out)
    {
      return OFX::Private::interactMainEntryOpenGL(action, handle, in, out, descriptor());
    }

  private:
    // The descriptor that both entry points use (describe, createInstance). As before, nothing
    // locks it: kOfxActionDescribe sets its property set during describe(), so a host that
    // describes the interact through both entries (or twice) at the same time races on it.
    static DESC &descriptor()
    {
      static DESC desc;
      return desc;
    }
  };

  template<class DESC, class INSTANCE>
  class DefaultEffectOverlayDescriptor : public EffectOverlayDescriptor, public InteractMainEntry<DESC>
  {
  public:
    Interact* createInstance(OfxInteractHandle handle, ImageEffect *effect) { return new INSTANCE(handle, effect); }
    virtual OfxPluginEntryPoint* getMainEntry() { return InteractMainEntry<DESC>::overlayInteractMainEntry; }
  };

  template<class DESC, class INSTANCE>
  class DefaultParamInteractDescriptor : public ParamInteractDescriptor, public InteractMainEntry<DESC>
  {
  public:
    Interact* createInstance(OfxInteractHandle handle, ImageEffect *effect) { return new INSTANCE(handle, effect, _paramNameStatic); }
    virtual OfxPluginEntryPoint* getMainEntry() { return InteractMainEntry<DESC>::overlayInteractMainEntry; }
    virtual void setParamName(const std::string& pName) { _paramNameStatic = pName; }
  protected:
    static std::string _paramNameStatic;
  };

  template<class DESC, class INSTANCE> std::string OFX::DefaultParamInteractDescriptor<DESC, INSTANCE>::_paramNameStatic;
};


#undef mDeclareProtectedAssignAndCC

#endif

