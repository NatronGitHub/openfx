
// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#ifndef OFX_INTERACT_H
#define OFX_INTERACT_H

#include "ofxCore.h"
#include "ofxInteract.h" // OfxInteractHandle, OfxInteractSuiteV1
#include "ofxhPropertySuite.h" // Property::Set, Property::GetHook
#include "ofxOld.h" // old plugins may rely on deprecated properties being present
#include "ofxDrawSuite.h" // OFX 1.5 DrawSuite
#ifdef OFX_EXTENSIONS_NATRON
#include "ofxPixels.h" // for OfxRGBAColourD
#endif

namespace OFX {

  namespace Host {

    namespace Interact {
      
      /// fetch a versioned suite for our interact
      const void *GetSuite(int version);

      /// fetch a versioned OfxDrawSuite (kOfxDrawSuite, OFX 1.5). Hosts only hand it out
      /// if they implement the Instance::draw*() hooks, see ImageEffect::Host::supportsDrawSuite()
      const void *GetDrawSuite(int version);
      
      class Base {
      public:
        virtual ~Base() {
        }

        /// grab a handle on the parameter for passing to the C API
        OfxInteractHandle getHandle() {return (OfxInteractHandle)this;}
        
        /// get the property handle for this instance/descriptor
        virtual OfxPropertySetHandle getPropHandle() = 0;
      };

      /// state the interact can be in
      enum State {
        eUninitialised,
        eDescribed,
        eCreated,
        eFailed
      };
  
      /// Descriptor for an interact. Interacts all share a single description
      class Descriptor : public Base {
      protected:
        Property::Set        _properties;  ///< its props
        State                _state;       ///< how is it feeling today
        OfxPluginEntryPoint *_entryPoint;  ///< the entry point for this overlay

      public:
        /// CTOR
        Descriptor();

        /// dtor
        virtual ~Descriptor();

        /// set the main entry points
        void setEntryPoint(OfxPluginEntryPoint *entryPoint) {_entryPoint = entryPoint;}

        /// call describe on this descriptor, returns true if all went well
        bool describe(int bitDepthPerComponent, bool hasAlpha);

        /// back to the state before describe(), without entry point: e.g. when the plug-in is
        /// unloaded (its entry point may be in an unloaded binary)
        void reset();

        /// grab a handle on the properties of this parameter for the C api
        OfxPropertySetHandle getPropHandle() {return _properties.getHandle();}

        /// get prop set
        const Property::Set &getProperties() const {return _properties;}

        /// get a non const prop set
        Property::Set &getProperties() {return _properties;}

        /// call the entry point with action and the given args
        OfxStatus callEntry(const char *action, 
                            void *handle,
                            OfxPropertySetHandle inArgs, 
                            OfxPropertySetHandle outArgs);      

        /// what is it's state?
        State getState() const {return _state;}
      };

      /// a generic interact, it doesn't belong to anything in particular
      /// we need to generify this slighty more and remove the renderscale args
      /// into a derived class, as they only belong to image effect plugins
      class Instance : public Base, protected Property::GetHook {
      protected:
        Descriptor   &_descriptor;  ///< who we are
        Property::Set _properties;  ///< its props
        State         _state;       ///< how is it feeling today
        void         *_effectInstance; ///< this is ugly, we need a base class to all plugin instances at some point.
        Property::Set _argProperties;
#ifdef OFX_EXTENSIONS_NATRON
        // last cursor reported through cursorChanged() (proposed extension, NatronGitHub/openfx#4)
        std::string   _lastCursor;
        std::string   _lastCursorImage;
        int           _lastCursorHotSpot[2];
#endif

        /// true while kOfxInteractActionDraw is being called: the DrawSuite may only be used then
        bool          _inDraw;

        /// initialise the argument properties
        void initArgProp(Property::Set& props,
                         OfxTime time,
                         const OfxPointD   &renderScale
#ifdef OFX_EXTENSIONS_NUKE
                         , int view
#endif
#ifdef OFX_EXTENSIONS_NATRON
                         , const OfxRGBAColourD* colourPicker
#endif
        );

        /// set pen props in the args
        void setPenArgProps(const OfxPointD &penPos,
                            const OfxPointI &penPosViewport,
                            double  pressure);
        
        /// set key args in the props
        void setKeyArgProps(int     key,
                            char*   keyString);

#ifdef OFX_EXTENSIONS_NATRON
        /// does the host support the proposed interact cursor extension (NatronGitHub/openfx#4)?
        /// true if the host descriptor lists at least one kOfxInteractHostPropSupportedCursors
        static bool hostSupportsCursors();

        /// called after a pen or key action: if it did not fail and the cursor requested
        /// by this interact changed since the last report, call cursorChanged()
        void notifyCursor(OfxStatus actionStatus);
#endif
        
      private:
        /// the pen actions (motion, up, down) differ only by their name, and so do the key actions
        /// (down, up, repeat): the arguments are set, the action is called, and the cursor change
        /// is notified (with OFX_EXTENSIONS_NATRON) here
        OfxStatus penAction(const char *action,
                            OfxTime time,
                            const OfxPointD &renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                            int view,
#endif
#ifdef OFX_EXTENSIONS_NATRON
                            const OfxRGBAColourD* colourPicker,
#endif
                            const OfxPointD &penPos,
                            const OfxPointI &penPosViewport,
                            double pressure);

        OfxStatus keyAction(const char *action,
                            OfxTime time,
                            const OfxPointD &renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                            int view,
#endif
#ifdef OFX_EXTENSIONS_NATRON
                            const OfxRGBAColourD* colourPicker,
#endif
                            int key,
                            char* keyString);

      public:
        Instance(Descriptor &desc, void *effectInstance);

        /// Sends kOfxActionDestroyInstance if createInstanceAction() succeeded. Interact instances
        /// have no destroyInstanceAction() (unlike ImageEffect::Instance): the action is sent from
        /// this destructor, when the derived classes are already destroyed. A plug-in that calls
        /// interactSwapBuffers() or interactRedraw() then, or reads a property with a get hook
        /// (kOfxInteractPropPixelScale...), reaches pure virtuals, which aborts.
        virtual ~Instance();

        /// what is it's state?
        State getState() const {return _state;}

        /// grab a handle on the properties of this parameter for the C api
        OfxPropertySetHandle getPropHandle() {return _properties.getHandle();}

        /// get prop set
        const Property::Set &getProperties() const {return _properties;}

        /// call the entry point in the descriptor with action and the given args
        virtual OfxStatus callEntry(const char *action,
                                    Property::Set *inArgs);
        
#ifdef kOfxInteractPropViewportSize // removed in OFX 1.4
        /// hooks to kOfxInteractPropViewportSize in the property set
        /// this is actually redundant and is to be deprecated
        virtual void getViewportSize(double &width, double &height) const = 0;
#endif

        // hooks to live kOfxInteractPropPixelScale in the property set
        virtual void getPixelScale(double& xScale, double& yScale) const = 0;

#ifdef OFX_EXTENSIONS_NATRON
        // hooks to live kOfxInteractPropScreenPixelRatio in the property set
        virtual double getScreenPixelRatio() const = 0;

        /// Keyboard modifiers (extension 13 of ofxNatron.h, the proposed OFX 1.6 kOfxInteractPropKeyModifiers).
        ///
        /// kNatronOfxInteractPropKeyModifiers is declared in the inArgs of the interact actions of
        /// this instance (overlay or parameter interact) only if the host application sets
        /// kNatronOfxInteractHostPropSupportsKeyModifiers to 1 on its image effect host descriptor
        /// AND supportsKeyModifiers() returns true. It is then set from getKeyModifiers() before
        /// each of these actions (with kOfxInteractPropKeyModifiers too, if ofxInteract.h defines
        /// it). Otherwise it is not declared, and plug-ins track the modifier keys themselves.
        /// Requiring the override means that an interact class that doesn't implement
        /// getKeyModifiers() makes plug-ins fall back to tracking, instead of silently reporting
        /// "no modifier".
        ///
        /// When ofxInteract.h defines the OFX 1.6 names, either host property enables it, and both
        /// kOfxInteractPropKeyModifiers and kNatronOfxInteractPropKeyModifiers are passed: the host
        /// application should then set both host properties to 1.
        ///
        /// A host application that sets kNatronOfxInteractHostPropSupportsKeyModifiers to 1 should
        /// override both functions in all its interacts. Not overriding them is safe: the plug-ins
        /// of that interact then get no property and fall back to tracking the modifier keys.
        virtual bool supportsKeyModifiers() const { return false; }

        /// The state of the keyboard modifiers, a bitmask of kNatronOfxKeyModifierShift,
        /// kNatronOfxKeyModifierControl, kNatronOfxKeyModifierAlt and kNatronOfxKeyModifierMeta,
        /// consistent with the key symbols the host passes to keyDownAction(), keyUpAction() and
        /// keyRepeatAction() (in the key actions of a modifier key, the state after the event; in
        /// the draw and focus actions, the current state). Only called if supportsKeyModifiers()
        /// returns true. The default returns 0.
        virtual int getKeyModifiers() const { return 0; }
#endif

        // hooks to kOfxInteractPropBackgroundColour in the property set
        virtual void getBackgroundColour(double &r, double &g, double &b) const = 0;

        // hooks to kOfxInteractPropSuggestedColour and kOfxPropOverlayColour in the property set
        // return false if there is no color suggestion by the host.
        virtual bool getSuggestedColour(double &r, double &g, double &b) const = 0;

        /// implement
        virtual OfxStatus swapBuffers() = 0;

        /// implement this
        virtual OfxStatus redraw() = 0;

        /// @name DrawSuite hooks (OFX 1.5, OfxDrawSuiteV1)
        ///
        /// The draw context passed to the plug-in in kOfxInteractPropDrawContext during
        /// kOfxInteractActionDraw identifies this instance. The OfxDrawSuiteV1 functions
        /// call these hooks only during that action (outside it they return kOfxStatFailed:
        /// "failure, e.g. if function is called outside kOfxInteractActionDraw"), after
        /// checking their arguments (kOfxStatErrValue for an invalid colour enum, stipple
        /// pattern, primitive, point count, or NULL text/pos). A host that returns true from
        /// ImageEffect::Host::supportsDrawSuite() must override them to draw in this
        /// interact's view. The defaults return kOfxStatErrMissingHostFeature.
        /// The semantics of each function are those of include/ofxDrawSuite.h.
        ///
        /// A host that draws its overlays with OpenGL can forward these hooks to an
        /// OFX::Host::Interact::OpenGLDrawSuite member (ofxhDrawSuiteOpenGL.h, optional and
        /// header-only), which draws with the OpenGL DrawSuite core shared with the Support
        /// library's DrawSuite-to-OpenGL emulation (include/ofxDrawSuiteOpenGL.h), with hooks
        /// for the host's colours and font, and the shadows of drawSuiteShadows().
        ///
        /// Graphics state: HostSupport makes no OpenGL (or other graphics API) call around
        /// these hooks, and does not require any graphics context to be current. They are
        /// called synchronously, on the thread that called drawAction(), from inside the
        /// plug-in's kOfxInteractActionDraw, in the order the plug-in issues them. The host
        /// decides how to render them (OpenGL, Metal, a QPainter, a display list replayed
        /// later, ...) and is responsible for any context/state it needs. Colour, line
        /// width and stipple are state of this draw context: they apply to the following
        /// drawPrimitive/drawText calls of the same draw action ("Sets the colour for
        /// future drawing operations", "Sets the line width for future line drawing
        /// operations").
        ///
        /// Coordinates and units. HostSupport passes all values to the hooks unchanged, so
        /// any conversion to the host's drawing space is the host's job.
        /// - drawPrimitive() points and drawText() pos: ofxDrawSuite.h does not state their
        ///   coordinate space explicitly. They are in the same space as everything else in
        ///   an overlay's draw action, i.e. the interact's canonical coordinates: the draw
        ///   action is called with "the projection matrix will correspond to the
        ///   interact's canonical view" (ofxInteract.h, kOfxInteractActionDraw), pen
        ///   positions are "in the interact's canonical coordinates"
        ///   (kOfxInteractPropPenPosition), and the primitives are defined by analogy with
        ///   OpenGL ("like GL_LINES", "like GL_LINE_STRIP", ...). A host whose view draws
        ///   in canonical coordinates (e.g. with a zoom/pan transform) can use them as is;
        ///   kOfxInteractPropPixelScale ("The size of a real screen pixel under the
        ///   interact's canonical projection") gives the canonical-to-screen scale.
        ///   kOfxDrawPrimitiveRectangle and kOfxDrawPrimitiveEllipse take "2 opposite corner
        ///   points" of an axis-aligned rectangle (not necessarily bottom-left/top-right).
        /// - drawSetLineWidth(width): in screen pixels, not canonical units: "Use width 0
        ///   for a single pixel line or non-zero for a smooth line of the desired width.
        ///   The host should adjust for screen density" (i.e. multiply by the device pixel
        ///   ratio on high-density displays). It does not scale with the view's zoom.
        /// - drawSetLineStipple(pattern): an OfxDrawLineStipplePattern (solid, dot, dash,
        ///   alt dash, dot dash); the spec does not define the dash lengths, which the host
        ///   chooses in screen space, independent of the zoom, like the line width.
        /// - drawText(text, pos, alignment): text is UTF-8; pos is aligned with the
        ///   kOfxDrawTextAlignment* flags (pos is a point in canonical coordinates, see
        ///   above); "The text font face and size are determined by the host", in screen
        ///   space (the text does not scale with the zoom).
        /// - drawSetColour/drawGetColour: OfxRGBAColourF in [0,1]; "The host should use
        ///   "over" compositing when using a non-opaque colour". drawGetColour returns the
        ///   host's standard overlay colours (OfxStandardColour).
        /// @{
        virtual OfxStatus drawGetColour(OfxStandardColour std_colour, OfxRGBAColourF *colour);
        virtual OfxStatus drawSetColour(const OfxRGBAColourF *colour);
        virtual OfxStatus drawSetLineWidth(float width);
        virtual OfxStatus drawSetLineStipple(OfxDrawLineStipplePattern pattern);
        virtual OfxStatus drawPrimitive(OfxDrawPrimitive primitive, const OfxPointD *points, int point_count);
        virtual OfxStatus drawText(const char *text, const OfxPointD *pos, int alignment);
        /// @}

        /// is kOfxInteractActionDraw currently being called on this instance?
        bool isInDraw() const { return _inDraw; }

        /// Does the host want drop shadows on the DrawSuite elements of this interact? With
        /// OFX_EXTENSIONS_NATRON, drawAction() sets kNatronOfxInteractPropDrawSuiteShadows
        /// (ofxNatron.h, extension 14) to it in the inArgs of kOfxInteractActionDraw. The host's
        /// DrawSuite hooks draw the shadows of V2 draws (e.g. OpenGLDrawSuite::beginDraw() with
        /// this value); the Support library's DrawSuite-to-OpenGL emulation draws those of V1
        /// draws. Plug-ins never draw shadows with DrawSuite calls, and their OpenGL code keeps
        /// its own. Default: false.
        virtual bool drawSuiteShadows() const { return false; }

        /// returns the params the interact uses
        virtual void getSlaveToParam(std::vector<std::string>& params) const;

        // do nothing
        virtual int  getDimension(const std::string &name) const OFX_EXCEPTION_SPEC;
        
        // don't know what to do
        virtual void reset(const std::string &name) OFX_EXCEPTION_SPEC;

        /// the gethook virutals for  pixel scale, background colour
        virtual double getDoubleProperty(const std::string &name, int index) const OFX_EXCEPTION_SPEC;

        /// for pixel scale and background colour
        virtual void getDoublePropertyN(const std::string &name, double *first, int n) const OFX_EXCEPTION_SPEC;

        /// call create instance
        virtual OfxStatus createInstanceAction();
        
        // interact action - kOfxInteractActionDraw 
        // 
        // Params -
        //
        //    time              - the effect time at which changed occured
        //    renderScale       - the render scale
        virtual OfxStatus drawAction(OfxTime time,
                                     const OfxPointD &renderScale
#ifdef OFX_EXTENSIONS_NUKE
                                     , int view
#endif
#ifdef OFX_EXTENSIONS_NATRON
                                     , const OfxRGBAColourD* colourPicker
#endif
                                     );

        // interact action - kOfxInteractActionPenMotion
        //
        // Params  -
        //
        //    time              - the effect time at which changed occured
        //    renderScale       - the render scale
        //    penX              - the X position
        //    penY              - the Y position
        //    pressure          - the pen pressue 0 to 1
        virtual OfxStatus penMotionAction(OfxTime time,
                                          const OfxPointD &renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                          int view,
#endif
#ifdef OFX_EXTENSIONS_NATRON
                                          const OfxRGBAColourD* colourPicker,
#endif
                                          const OfxPointD &penPos,
                                          const OfxPointI &penPosViewport,
                                          double pressure);

        // interact action - kOfxInteractActionPenUp
        //
        // Params  -
        //
        //    time              - the effect time at which changed occured
        //    renderScale       - the render scale
        //    penX              - the X position
        //    penY              - the Y position
        //    pressure          - the pen pressue 0 to 1
        virtual OfxStatus penUpAction(OfxTime time,
                                      const OfxPointD &renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                      int view,
#endif
#ifdef OFX_EXTENSIONS_NATRON
                                      const OfxRGBAColourD* colourPicker,
#endif
                                      const OfxPointD &penPos,
                                      const OfxPointI &penPosViewport,
                                      double  pressure);

        // interact action - kOfxInteractActionPenDown
        //
        // Params  -
        //
        //    time              - the effect time at which changed occured
        //    renderScale       - the render scale
        //    penX              - the X position
        //    penY              - the Y position
        //    pressure          - the pen pressue 0 to 1
        virtual OfxStatus penDownAction(OfxTime time,
                                        const OfxPointD &renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                        int view,
#endif
#ifdef OFX_EXTENSIONS_NATRON
                                        const OfxRGBAColourD* colourPicker,
#endif
                                        const OfxPointD &penPos,
                                        const OfxPointI &penPosViewport,
                                        double  pressure);

        // interact action - kOfxInteractActionkeyDown
        //
        // Params  -
        //
        //    time              - the effect time at which changed occured
        //    renderScale       - the render scale
        //    key               - the pressed key
        //    keyString         - the pressed key string
        virtual OfxStatus keyDownAction(OfxTime time,
                                        const OfxPointD &renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                        int view,
#endif
#ifdef OFX_EXTENSIONS_NATRON
                                        const OfxRGBAColourD* colourPicker,
#endif
                                        int     key,
                                        char*   keyString);

        // interact action - kOfxInteractActionkeyUp
        //
        // Params  -
        //
        //    time              - the effect time at which changed occured
        //    renderScale       - the render scale
        //    key               - the pressed key
        //    keyString         - the pressed key string
        virtual OfxStatus keyUpAction(OfxTime time,
                                      const OfxPointD &renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                      int view,
#endif
#ifdef OFX_EXTENSIONS_NATRON
                                      const OfxRGBAColourD* colourPicker,
#endif
                                      int     key,
                                      char*   keyString);

        // interact action - kOfxInteractActionkeyRepeat
        //
        // Params  -
        //
        //    time              - the effect time at which changed occured
        //    renderScale       - the render scale
        //    key               - the pressed key
        //    keyString         - the pressed key string
        virtual OfxStatus keyRepeatAction(OfxTime time,
                                          const OfxPointD &renderScale,
#ifdef OFX_EXTENSIONS_NUKE
                                          int view,
#endif
#ifdef OFX_EXTENSIONS_NATRON
                                          const OfxRGBAColourD* colourPicker,
#endif
                                          int     key,
                                          char*   keyString);

        // interact action - kOfxInteractActionLoseFocus
        // 
        // Params -
        //
        //    time              - the effect time at which changed occured
        //    renderScale       - the render scale
        virtual OfxStatus gainFocusAction(OfxTime time,
                                          const OfxPointD &renderScale
#ifdef OFX_EXTENSIONS_NUKE
                                          , int view
#endif
#ifdef OFX_EXTENSIONS_NATRON
                                          , const OfxRGBAColourD* colourPicker
#endif
                                          );

        // interact action - kOfxInteractActionLoseFocus
        // 
        // Params -
        //
        //    time              - the effect time at which changed occured
        //    renderScale       - the render scale
        virtual OfxStatus loseFocusAction(OfxTime  time,
                                          const OfxPointD &renderScale
#ifdef OFX_EXTENSIONS_NUKE
                                          , int view
#endif
#ifdef OFX_EXTENSIONS_NATRON
                                          , const OfxRGBAColourD* colourPicker
#endif
                                          );

#ifdef OFX_EXTENSIONS_NATRON
        /// Proposed interact cursor extension (NatronGitHub/openfx#4).
        ///
        /// Called by HostSupport, only if the host descriptor lists
        /// kOfxInteractHostPropSupportedCursors, after a pen or key action (PenMotion, PenDown,
        /// PenUp, KeyDown, KeyUp, KeyRepeat) that did not fail (any status other than
        /// kOfxStatFailed and kOfxStatErr*), when the values of kOfxInteractPropCursor,
        /// kOfxInteractPropCursorImage or kOfxInteractPropCursorHotSpot on this instance differ
        /// from the last ones reported; and with kOfxInteractCursorDefault after
        /// kOfxInteractActionLoseFocus.
        ///
        /// actionStatus is the status of the triggering action. Several interacts may share a
        /// view, so the host arbitrates between them, e.g.: the interact that trapped the event
        /// (actionStatus == kOfxStatOK) wins; otherwise the most recent non-default cursor;
        /// otherwise the host's own cursor. getCursor() gives the current request of any
        /// interact, e.g. to re-evaluate after dispatching an event to all interacts of a view.
        ///
        /// Polling getCursor() on every interact of the view after each dispatched pen/key event,
        /// and recomputing the displayed cursor, is a valid and preferred host strategy; it is
        /// required when one interact instance is shared by several views (Natron), because a
        /// change is notified only once while each view needs it.
        ///
        /// Important: notifications are per interact and only report changes of that
        /// interact's own request. When the displayed cursor is released (its owner goes back
        /// to default, loses focus or is destroyed), the host must re-evaluate the arbitration
        /// from the current requests of all interacts of the view (getCursor(), or the last
        /// value each one reported), not fall back to its own cursor: another interact may
        /// still be requesting a cursor that it will not report again, because it did not change.
        /// An unsupported value should be displayed as kOfxInteractCursorDefault.
        ///
        /// Destruction: this cannot be called from ~Instance (the derived class is already
        /// destroyed), so the host must itself restore the view's cursor when an interact that
        /// owns it is destroyed.
        ///
        /// The default implementation does nothing.
        virtual void cursorChanged(OfxStatus actionStatus,
                                   const std::string &cursor,
                                   const std::string &image,
                                   int hotSpotX,
                                   int hotSpotY);

        /// the cursor currently requested by this interact (kOfxInteractPropCursor,
        /// kOfxInteractPropCursorImage, kOfxInteractPropCursorHotSpot), or
        /// kOfxInteractCursorDefault, "", 0, 0 if the host does not support interact cursors.
        /// image and hot spot are "" and 0, 0 unless cursor is kOfxInteractCursorCustom.
        void getCursor(std::string &cursor, std::string &image, int &hotSpotX, int &hotSpotY) const;
#endif
      };

    } // Interact

  } // Host

} // OFX

#endif // OFX_INTERACT_H
