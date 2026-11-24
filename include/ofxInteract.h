#ifndef _ofxInteract_h_
#define _ofxInteract_h_

#include "ofxCore.h"

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause


#ifdef __cplusplus
extern "C" {
#endif

/** @file ofxInteract.h
Contains the API for ofx plugin defined GUIs and interaction.
*/

#define kOfxInteractSuite "OfxInteractSuite"


/** @brief Blind declaration of an OFX interactive gui 
*/
typedef struct OfxInteract *OfxInteractHandle;

/**
   \addtogroup PropertiesAll
*/
/*@{*/
/**
   \defgroup PropertiesInteract Interact Property Definitions

These are the list of properties used by the Interact API documented in \ref CustomInteractionPage.
*/
/*@{*/
/** @brief The set of parameters on which a value change will trigger a redraw for an interact.

   - Type - string X N
   - Property Set - interact instance property (read/write)
   - Default - no values set
   - Valid Values - the name of any parameter associated with this interact.

If the interact is representing the state of some set of OFX parameters, then is will
need to be redrawn if any of those parameters' values change. This multi-dimensional property
links such parameters to the interact.

The interact can be slaved to multiple parameters (setting index 0, then index 1 etc...)

    @propdef
   type: string
   dimension: N
 */
#define kOfxInteractPropSlaveToParam "OfxInteractPropSlaveToParam"

/** @brief The size of a real screen pixel under the interact's canonical projection.

   - Type - double X 2
   - Property Set - interact instance and actions (read only)

    @propdef
   type: double
   dimension: 2
 */
#define kOfxInteractPropPixelScale "OfxInteractPropPixelScale"



/** @brief The background colour of the application behind an interact instance

    - Type - double X 3
    - Property Set - read only on the interact instance and in argument to the ::kOfxInteractActionDraw action
    - Valid Values - from 0 to 1

The components are in the order red, green then blue.

    @propdef
    type: double
    dimension: 3
 */
#define kOfxInteractPropBackgroundColour "OfxInteractPropBackgroundColour"

/** @brief The suggested colour to draw a widget in an interact, typically for overlays.
 
    - Type - double X 3 if set, else double X 0
    - Property Set - read only on the interact instance
    - Default - 1.0
    - Valid Values - greater than or equal to 0.0

Some applications allow the user to specify colours of any overlay via a colour picker, this
property represents the value of that colour. Plugins are at liberty to use this or not when
they draw an overlay.

If a host does not support such a colour, it should return kOfxStatReplyDefault

    @propdef
    type: double
    dimension: 3
*/
#define kOfxInteractPropSuggestedColour "OfxInteractPropSuggestedColour"

/** @brief The position of the pen in an interact.

   - Type - double X 2
   - Property Set - read only in argument to the ::kOfxInteractActionPenMotion, ::kOfxInteractActionPenDown and ::kOfxInteractActionPenUp actions

This value passes the position of the pen into an interact. This is in the interact's canonical coordinates.

    @propdef
   type: double
   dimension: 2
 */
#define kOfxInteractPropPenPosition "OfxInteractPropPenPosition"

/** @brief The position of the pen in an interact in viewport coordinates.

   - Type - int X 2
   - Property Set - read only in argument to the ::kOfxInteractActionPenMotion, ::kOfxInteractActionPenDown and ::kOfxInteractActionPenUp actions

This value passes the position of the pen into an interact. This is in the interact's openGL viewport coordinates, with 0,0 being at the bottom left.

    @propdef
   type: int
   dimension: 2
 */
#define kOfxInteractPropPenViewportPosition "OfxInteractPropPenViewportPosition"

/** @brief The pressure of the pen in an interact.

   - Type - double X 1
   - Property Set - read only in argument to the ::kOfxInteractActionPenMotion, ::kOfxInteractActionPenDown and ::kOfxInteractActionPenUp actions
   - Valid Values - from 0 (no pressure) to 1 (maximum pressure)

This is used to indicate the status of the 'pen' in an interact. If a pen has only two states (eg: a mouse button), these should map to 0.0 and 1.0.

    @propdef
   type: double
   dimension: 1
 */
#define kOfxInteractPropPenPressure "OfxInteractPropPenPressure"

#ifdef OFX_EXTENSIONS_NATRON
/** @defgroup InteractCursor Interact cursors (proposed extension)

PROPOSED standard extension (NatronGitHub/openfx#4,
Documentation/proposals/interact-cursor.md), not yet part of the OFX
standard. Implemented by the NatronGitHub HostSupport and Support libraries
under OFX_EXTENSIONS_NATRON. The names are the proposed standard names; they
may change during standardization.


An overlay interact may ask the host to change the shape of the window-system
mouse cursor while the pen is over its view, or to hide it (e.g. when the
interact draws its own cursor with OpenGL or the DrawSuite).

Negotiation: a host that supports this sets ::kOfxInteractHostPropSupportedCursors
on its host descriptor. A plug-in must check it before setting
::kOfxInteractPropCursor; on other hosts the property does not exist on interact
instances and setting it fails with kOfxStatErrUnknown.

Reporting: the cursor property is per interact instance (a host may use one
interact instance per view, or share one between views); it is
the plug-in's own report of the cursor it wants. After each of
kOfxInteractActionPenMotion, kOfxInteractActionPenDown, kOfxInteractActionPenUp,
kOfxInteractActionKeyDown, kOfxInteractActionKeyUp and
kOfxInteractActionKeyRepeat that does not fail (whether it returns kOfxStatOK,
i.e. the event was trapped, or kOfxStatReplyDefault), the host reads
::kOfxInteractPropCursor (and the related properties) on that interact instance.
Since kOfxInteractActionPenMotion is also issued while the pen is up, a plug-in
typically sets a cursor when the pen hovers over one of its handles, and sets
::kOfxInteractCursorDefault again when it moves off the handle. Setting the
cursor does not require trapping the event: the plug-in keeps returning
whatever it returned before. Trapping is optional, and only gives the request
priority in the arbitration below.

Arbitration: several interacts may share a view (overlays of several effects,
host overlays), so the host decides which request to display. The recommended
policy is: the cursor of the interact that trapped the event (returned
kOfxStatOK) wins, which matches the existing rule that "if the instance returns
kOfxStatOK the host should not pass the pen motion to any other interactive
object it may own that shares the same view"; otherwise the most recent
non-default request of an interact of that view; otherwise the host's own
cursor. An interact that loses focus (kOfxInteractActionLoseFocus) or is
destroyed no longer owns the cursor. When the displayed request is released this
way, or because its interact went back to ::kOfxInteractCursorDefault, the host
re-evaluates this policy from the current requests of all the interacts of the
view, and only shows its own cursor if none of them requests one. A plug-in
reports its request once, when it changes: an interact still hovering over its
handle does not report its unchanged cursor again.

Host strategies: a host may react to the change notifications, or, preferably,
read the current request of every interact of the view after dispatching each
pen/key event (in HostSupport: OFX::Host::Interact::Instance::getCursor()) and
recompute the displayed cursor from them. Polling is required when one interact
instance is shared by several views, since a change notification is only sent
once, while each view needs the request.
@{
*/

/** @brief Cursors supported by the host for ::kOfxInteractPropCursor

    - Type - C string X N
    - Property Set - host descriptor (read only)
    - Default - absent: the host does not support plug-in cursors
    - Valid Values - any of the standard cursor names below, plus the special
      value ::kOfxInteractCursorCustom if the host supports cursor images.

The standard names are the CSS cursor keywords of the W3C "CSS Basic User
Interface Module Level 4" (https://www.w3.org/TR/css-ui-4/#cursor), which have
well-known equivalents on all window systems. A host lists those it can display;
it must list at least ::kOfxInteractCursorDefault and should list
::kOfxInteractCursorNone.

    @propdef
    type: enum
    dimension: N
    values:
      - default
      - none
      - OfxInteractCursorCustom
      - pointer
      - crosshair
      - text
      - wait
      - progress
      - not-allowed
      - help
      - cell
      - copy
      - move
      - grab
      - grabbing
      - all-scroll
      - col-resize
      - row-resize
      - ew-resize
      - ns-resize
      - nesw-resize
      - nwse-resize
      - zoom-in
      - zoom-out
    extension: natron
*/
#define kOfxInteractHostPropSupportedCursors "OfxInteractHostPropSupportedCursors"

/** @brief The cursor requested by an interact instance

    - Type - C string X 1
    - Property Set - interact instance (read/write), only on hosts that set
      ::kOfxInteractHostPropSupportedCursors
    - Default - ::kOfxInteractCursorDefault
    - Valid Values - one of the values of ::kOfxInteractHostPropSupportedCursors.
      The host displays ::kOfxInteractCursorDefault for any other value.

May be set by the plug-in during any interact action. It is read by the host as
described in @ref InteractCursor.

    @propdef
    type: enum
    dimension: 1
    values:
      - default
      - none
      - OfxInteractCursorCustom
      - pointer
      - crosshair
      - text
      - wait
      - progress
      - not-allowed
      - help
      - cell
      - copy
      - move
      - grab
      - grabbing
      - all-scroll
      - col-resize
      - row-resize
      - ew-resize
      - ns-resize
      - nesw-resize
      - nwse-resize
      - zoom-in
      - zoom-out
    extension: natron
*/
#define kOfxInteractPropCursor "OfxInteractPropCursor"

/** @brief File name of a custom cursor image, used when ::kOfxInteractPropCursor is ::kOfxInteractCursorCustom

    - Type - UTF8 C string X 1
    - Property Set - interact instance (read/write), only on hosts that support
      ::kOfxInteractCursorCustom
    - Default - ""
    - Valid Values - the file name, without any directory, of a PNG image with an
      alpha channel in the Contents/Resources directory of the plug-in bundle.

If a file with the same name and an "@2x" suffix before the extension exists
(e.g. "Rotate@2x.png"), the host may use it on high-density displays. The host
may scale or refuse images larger than 32x32 points. If the image cannot be
loaded, the host displays ::kOfxInteractCursorDefault.

    @propdef
    type: string
    dimension: 1
    extension: natron
*/
#define kOfxInteractPropCursorImage "OfxInteractPropCursorImage"

/** @brief Hot spot of the custom cursor image

    - Type - int X 2
    - Property Set - interact instance (read/write), only on hosts that support
      ::kOfxInteractCursorCustom
    - Default - 0,0
    - Valid Values - x,y in pixels of the 1x image, from its top-left corner

When the host uses the "@2x" variant of ::kOfxInteractPropCursorImage on a
high-density display, the hot spot in that image's pixels is twice this value
(e.g. with Qt, load the @2x image in a QPixmap, call setDevicePixelRatio(2),
and pass this hot spot unchanged to QCursor).

    @propdef
    type: int
    dimension: 2
    extension: natron
*/
#define kOfxInteractPropCursorHotSpot "OfxInteractPropCursorHotSpot"

/** @brief The host's own cursor for this view (the cursor it would display without plug-in) */
#define kOfxInteractCursorDefault     "default"
/** @brief No cursor (e.g. the interact draws its own) */
#define kOfxInteractCursorNone        "none"
/** @brief Custom image, see ::kOfxInteractPropCursorImage */
#define kOfxInteractCursorCustom      "OfxInteractCursorCustom"
/* standard cursors (CSS names) */
#define kOfxInteractCursorPointer     "pointer"     /**< pointing hand, e.g. over a clickable handle */
#define kOfxInteractCursorCrosshair   "crosshair"
#define kOfxInteractCursorText        "text"
#define kOfxInteractCursorWait        "wait"
#define kOfxInteractCursorProgress    "progress"
#define kOfxInteractCursorNotAllowed  "not-allowed"
#define kOfxInteractCursorHelp        "help"
#define kOfxInteractCursorCell        "cell"
#define kOfxInteractCursorCopy        "copy"
#define kOfxInteractCursorMove        "move"
#define kOfxInteractCursorGrab        "grab"         /**< open hand */
#define kOfxInteractCursorGrabbing    "grabbing"     /**< closed hand */
#define kOfxInteractCursorAllScroll   "all-scroll"
#define kOfxInteractCursorColResize   "col-resize"
#define kOfxInteractCursorRowResize   "row-resize"
#define kOfxInteractCursorEWResize    "ew-resize"
#define kOfxInteractCursorNSResize    "ns-resize"
#define kOfxInteractCursorNESWResize  "nesw-resize"
#define kOfxInteractCursorNWSEResize  "nwse-resize"
#define kOfxInteractCursorZoomIn      "zoom-in"
#define kOfxInteractCursorZoomOut     "zoom-out"
/** @} */
#endif // OFX_EXTENSIONS_NATRON

/** @brief Indicates the bits per component in the interact's openGL frame buffer

   - Type - int X 1
   - Property Set - interact instance and descriptor (read only)

    @propdef
   type: int
   dimension: 1
 */
#define kOfxInteractPropBitDepth "OfxInteractPropBitDepth"

/** @brief Indicates whether the interact's frame buffer has an alpha component or not

   - Type - int X 1
   - Property Set - interact instance and descriptor (read only)
   - Valid Values - This must be one of
       - 0 indicates no alpha component
       - 1 indicates an alpha component

    @propdef
   type: bool
   dimension: 1
 */
#define kOfxInteractPropHasAlpha "OfxInteractPropHasAlpha"

/*@}*/
/*@}*/

/**
   \addtogroup ActionsAll
*/
/*@{*/
/**
   \defgroup InteractActions Interact Actions

These are the list of actions passed to an interact's entry point function. For more details on how to deal with actions, see \ref InteractActions.
*/
/*@{*/

/** @brief

 This action is the first action passed to an interact. It is
 where an interact defines how it behaves and the resources it needs to
 function.
 If not trapped, the default action is for the host to carry on as normal
 Note that the handle passed in acts as a descriptor for, rather than an
 instance of the interact.

 @param  handle handle to the interact descriptor, cast to an \ref OfxInteractHandle
 @param  inArgs is redundant and is set to NULL
 @param  outArgs is redundant and is set to NULL


 \pre
     - The plugin has been loaded and the effect described.

 @returns
     -  \ref kOfxStatOK the action was trapped and all was well
     -  \ref kOfxStatErrMemory in which case describe may be called again after a memory purge
     -  \ref kOfxStatFailed something was wrong, the host should ignore the interact
     -  \ref kOfxStatErrFatal
 */
#define kOfxActionDescribeInteract kOfxActionDescribe


/** @brief

 This action is the first action passed to an interact instance after its creation.
 It is there to allow a plugin to create any per-instance data structures it may need.

 @param  handle handle to the interact instance, cast to an \ref OfxInteractHandle
 @param  inArgs is redundant and is set to NULL
 @param  outArgs is redundant and is set to NULL


 \pre
     - \ref kOfxActionDescribe has been called on this interact

 \post
     -  the instance pointer will be valid until the \ref kOfxActionDestroyInstance
     action is passed to the plug-in with the same instance handle

 @returns
     -  \ref kOfxStatOK the action was trapped and all was well
     -  \ref kOfxStatReplyDefault the action was ignored, but all was well anyway
     -  \ref kOfxStatErrFatal
     -  \ref kOfxStatErrMemory in which case this may be called again after a memory purge
     -  \ref kOfxStatFailed in which case the host should ignore this interact
 */
#define kOfxActionCreateInstanceInteract kOfxActionCreateInstance

/**@brief

 This action is the last passed to an interact's instance before its
 destruction. It is there to allow a plugin to destroy any per-instance
 data structures it may have created.

 @param handle handle to the interact instance, cast to an \ref OfxInteractHandle
 @param inArgs is redundant and is set to NULL
 @param outArgs is redundant and is set to NULL

 \pre
     - \ref kOfxActionCreateInstance
     has been called on the handle,
     - the instance has not had any of its members destroyed yet

 \post
     -  the instance pointer is no longer valid and any operation on it will be undefined

 @returns
     To some extent, what is returned is moot, a bit like throwing an
     exception in a C++ destructor, so the host should continue destruction
     of the instance regardless
     - \ref kOfxStatOK the action was trapped and all was well
     - \ref kOfxStatReplyDefault the action was ignored as the effect had nothing to do
     - \ref kOfxStatErrFatal
     - \ref kOfxStatFailed something went wrong, but no error code appropriate.
 */
#define kOfxActionDestroyInstanceInteract kOfxActionDestroyInstance

/** @brief

 This action is issued to an interact whenever the host needs the plugin
 to redraw the given interact.

 The interact should either issue OpenGL calls to draw itself, or use DrawSuite calls.

 If this is called via kOfxImageEffectPluginPropOverlayInteractV2, drawing MUST use DrawSuite.
 
 If this is called via kOfxImageEffectPluginPropOverlayInteractV1, drawing SHOULD use OpenGL. Some existing plugins may use DrawSuite via kOfxImageEffectPluginPropOverlayInteractV1 if it's supported by the host, but this is discouraged.  

 Note that the interact may (in the case of custom parameter GUIS) or may
 not (in the case of image effect overlays) be required to swap buffers,
 that is up to the kind of interact.

 @param  handle handle to an interact instance, cast to an \ref OfxInteractHandle
 @param  inArgs has the following properties on an image effect plugin
     - \ref kOfxPropEffectInstance a handle to the effect for which the interact has been,
     - \ref kOfxInteractPropPixelScale the scale factor to convert canonical pixels to screen pixels
     - \ref kOfxInteractPropBackgroundColour the background colour of the application behind the current view
     - \ref kOfxPropTime the effect time at which the change occurred
     - \ref kOfxImageEffectPropRenderScale the render scale applied to any image fetched

 @param  outArgs is redundant and is set to NULL

\pre
     - \ref kOfxActionCreateInstance has been called on the instance handle
     - the openGL context for this interact has been set
     -  the projection matrix will correspond to the interact's canonical view

 @returns
     - \ref kOfxStatOK the action was trapped and all was well
     - \ref kOfxStatReplyDefault the action was ignored
     - \ref kOfxStatErrFatal
     - \ref kOfxStatFailed something went wrong, the host should ignore this interact in future

    @actiondef
    inArgs:
      - OfxPropEffectInstance
      - OfxInteractPropDrawContext
      - OfxInteractPropPixelScale
      - OfxInteractPropBackgroundColour
      - OfxPropTime
      - OfxImageEffectPropRenderScale
    outArgs:
 */
#define kOfxInteractActionDraw "OfxInteractActionDraw"

/** @brief

 This action is issued whenever the pen moves an the interact's has
 focus. It should be issued whether the pen is currently up or down.
 No openGL calls should be issued by the plug-in during this action.


 @param  handle handle to an interact instance, cast to an \ref OfxInteractHandle
 @param  inArgs has the following properties on an image effect plugin
     - \ref kOfxPropEffectInstance a handle to the effect for which the interact has been,
     - \ref kOfxInteractPropPixelScale the scale factor to convert canonical pixels to screen pixels
     - \ref kOfxInteractPropBackgroundColour the background colour of the application behind the current view
     - \ref kOfxPropTime the effect time at which the change occurred
     - \ref kOfxImageEffectPropRenderScale the render scale applied to any image fetched
     - \ref kOfxInteractPropPenPosition position of the pen in,
     - \ref kOfxInteractPropPenViewportPosition position of the pen in,
     - \ref kOfxInteractPropPenPressure the pressure of the pen,

 @param  outArgs is redundant and is set to NULL

\pre
     - \ref  kOfxActionCreateInstance
     has been called on the instance handle
     - the current instance handle has had
     \ref kOfxInteractActionGainFocus called on it

\post
     -  if the instance returns \ref kOfxStatOK the host should not
     pass the pen motion to any other interactive object it may own that
     shares the same view.

 @returns
     -  \ref kOfxStatOK the action was trapped and the host should not pass the event to other objects it may own
     -  \ref kOfxStatReplyDefault the action was not trapped and the host can deal with it if it wants

    @actiondef
    inArgs:
      - OfxPropEffectInstance
      - OfxInteractPropPixelScale
      - OfxInteractPropBackgroundColour
      - OfxPropTime
      - OfxImageEffectPropRenderScale
      - OfxInteractPropPenPosition
      - OfxInteractPropPenViewportPosition
      - OfxInteractPropPenPressure
    outArgs:
 */
#define kOfxInteractActionPenMotion "OfxInteractActionPenMotion"

/**@brief

 This action is issued when a pen transitions for the 'up' to the 'down'
 state.
 No openGL calls should be issued by the plug-in during this action.


 @param  handle handle to an interact instance, cast to an \ref OfxInteractHandle
 @param  inArgs has the following properties on an image effect plugin,
     - \ref kOfxPropEffectInstance a handle to the effect for which the interact has been,
     - \ref kOfxInteractPropPixelScale the scale factor to convert canonical pixels to screen pixels
     - \ref kOfxInteractPropBackgroundColour the background colour of the application behind the current view
     - \ref kOfxPropTime the effect time at which the change occurred
     - \ref kOfxImageEffectPropRenderScale the render scale applied to any image fetched
     - \ref kOfxInteractPropPenPosition position of the pen in
     - \ref kOfxInteractPropPenViewportPosition position of the pen in
     - \ref kOfxInteractPropPenPressure the pressure of the pen

 @param  outArgs is redundant and is set to NULL

 \pre
     -  \ref kOfxActionCreateInstance
     has been called on the instance handle,
     -  the current instance handle has had
     \ref kOfxInteractActionGainFocus
     called on it

\post
     -  if the instance returns \ref kOfxStatOK, the host should not
     pass the pen motion to any other interactive object it may own that
     shares the same view.

 @returns
     -  \ref kOfxStatOK, the action was trapped and the host should not pass the event to other objects it may own
     -  \ref kOfxStatReplyDefault , the action was not trapped and the host can deal with it if it wants

    @actiondef
    inArgs:
      - OfxPropEffectInstance
      - OfxInteractPropPixelScale
      - OfxInteractPropBackgroundColour
      - OfxPropTime
      - OfxImageEffectPropRenderScale
      - OfxInteractPropPenPosition
      - OfxInteractPropPenViewportPosition
      - OfxInteractPropPenPressure
    outArgs:
 */
#define kOfxInteractActionPenDown   "OfxInteractActionPenDown"

/**@brief

 This action is issued when a pen transitions for the 'down' to the 'up'
 state.
 No openGL calls should be issued by the plug-in during this action.

 @param  handle handle to an interact instance, cast to an \ref OfxInteractHandle
 @param  inArgs has the following properties on an image effect plugin,
 - \ref kOfxPropEffectInstance a handle to the effect for which the interact has been,
 - \ref kOfxInteractPropPixelScale the scale factor to convert canonical pixels to screen pixels
 - \ref kOfxInteractPropBackgroundColour the background colour of the application behind the current view
 - \ref kOfxPropTime the effect time at which the change occurred
 - \ref kOfxImageEffectPropRenderScale the render scale applied to any image fetched
 - \ref kOfxInteractPropPenPosition position of the pen in
 - \ref kOfxInteractPropPenViewportPosition position of the pen in
 - \ref kOfxInteractPropPenPressure the pressure of the pen

 @param  outArgs is redundant and is set to NULL

 \pre
     -  \ref kOfxActionCreateInstance
     has been called on the instance handle,
     -  the current instance handle has had
     \ref kOfxInteractActionGainFocus called on it

 \post
     -  if the instance returns \ref kOfxStatOK, the host should not
     pass the pen motion to any other interactive object it may own that
     shares the same view.

 @returns
     -  \ref kOfxStatOK, the action was trapped and the host should not pass the event to other objects it may own
     -  \ref kOfxStatReplyDefault , the action was not trapped and the host can deal with it if it wants

    @actiondef
    inArgs:
      - OfxPropEffectInstance
      - OfxInteractPropPixelScale
      - OfxInteractPropBackgroundColour
      - OfxPropTime
      - OfxImageEffectPropRenderScale
      - OfxInteractPropPenPosition
      - OfxInteractPropPenViewportPosition
      - OfxInteractPropPenPressure
    outArgs:
 */
#define kOfxInteractActionPenUp     "OfxInteractActionPenUp"

/**@brief

 This action is issued when a key on the keyboard is depressed.
 No openGL calls should be issued by the plug-in during this action.

 @param handle handle to an interact instance, cast to an \ref OfxInteractHandle
 @param inArgs has the following properties on an image effect plugin
     -  \ref kOfxPropEffectInstance a handle to the effect for which the interact has been,
     - \ref kOfxPropKeySym single integer value representing the key that was manipulated,
     this may not have a UTF8 representation (eg: a return key)
     -  \ref kOfxPropKeyString UTF8 string representing a character key that was pressed, some
     keys have no UTF8 encoding, in which case this is ""
     -  \ref kOfxPropTime the effect time at which the change occurred
     - \ref kOfxImageEffectPropRenderScale the render scale applied to any image fetched

 @param  outArgs is redundant and is set to NULL

\pre
     - \ref kOfxActionCreateInstance
     has been called on the instance handle,
     -  the current instance handle has had
     \ref kOfxInteractActionGainFocus called on it

 \post
     -  if the instance returns \ref kOfxStatOK, the host should not
     pass the pen motion to any other interactive object it may own that
     shares the same focus.

 @returns
     - \ref kOfxStatOK , the action was trapped and the host should not pass the event to other objects it may own
     - \ref kOfxStatReplyDefault , the action was not trapped and the host can deal with it if it wants

    @actiondef
    inArgs:
      - OfxPropEffectInstance
      - kOfxPropKeySym
      - kOfxPropKeyString
      - OfxPropTime
      - OfxImageEffectPropRenderScale
    outArgs:
 */
#define kOfxInteractActionKeyDown   "OfxInteractActionKeyDown"

/**@brief
 This action is issued when a key on the keyboard is released.
 No openGL calls should be issued by the plug-in during this action.

 @param handle handle to an interact instance, cast to an \ref OfxInteractHandle
 @param inArgs has the following properties on an image effect plugin
     -  \ref kOfxPropEffectInstance a handle to the effect for which the interact has been,
     - \ref kOfxPropKeySym single integer value representing the key that was manipulated,
     this may not have a UTF8 representation (eg: a return key)
     -  \ref kOfxPropKeyString UTF8 string representing a character key that was pressed, some
     keys have no UTF8 encoding, in which case this is ""
     -  \ref kOfxPropTime the effect time at which the change occurred
     - \ref kOfxImageEffectPropRenderScale the render scale applied to any image fetched

 @param  outArgs is redundant and is set to NULL

 \pre
     - \ref kOfxActionCreateInstance
     has been called on the instance handle,
     -  the current instance handle has had
     \ref kOfxInteractActionGainFocus called on it

 \post
     -  if the instance returns \ref kOfxStatOK, the host should not
     pass the pen motion to any other interactive object it may own that
     shares the same focus.

 @returns
     - \ref kOfxStatOK , the action was trapped and the host should not pass the event to other objects it may own
     - \ref kOfxStatReplyDefault , the action was not trapped and the host can deal with it if it wants

    @actiondef
    inArgs:
      - OfxPropEffectInstance
      - kOfxPropKeySym
      - kOfxPropKeyString
      - OfxPropTime
      - OfxImageEffectPropRenderScale
    outArgs:
 */
#define kOfxInteractActionKeyUp     "OfxInteractActionKeyUp"

/**@brief
 This action is issued when a key on the keyboard is repeated.
 No openGL calls should be issued by the plug-in during this action.

 @param handle handle to an interact instance, cast to an \ref OfxInteractHandle
 @param inArgs has the following properties on an image effect plugin
     -  \ref kOfxPropEffectInstance a handle to the effect for which the interact has been,
     - \ref kOfxPropKeySym single integer value representing the key that was manipulated,
     this may not have a UTF8 representation (eg: a return key)
     -  \ref kOfxPropKeyString UTF8 string representing a character key that was pressed, some
     keys have no UTF8 encoding, in which case this is ""
     -  \ref kOfxPropTime the effect time at which the change occurred
     - \ref kOfxImageEffectPropRenderScale the render scale applied to any image fetched

 @param  outArgs is redundant and is set to NULL

 \pre
     - \ref kOfxActionCreateInstance
     has been called on the instance handle,
     -  the current instance handle has had
     \ref kOfxInteractActionGainFocus called on it

 \post
     -  if the instance returns \ref kOfxStatOK, the host should not
     pass the pen motion to any other interactive object it may own that
     shares the same focus.

 @returns
     - \ref kOfxStatOK , the action was trapped and the host should not pass the event to other objects it may own
     - \ref kOfxStatReplyDefault , the action was not trapped and the host can deal with it if it wants

    @actiondef
    inArgs:
      - OfxPropEffectInstance
      - kOfxPropKeySym
      - kOfxPropKeyString
      - OfxPropTime
      - OfxImageEffectPropRenderScale
    outArgs:
 */
#define kOfxInteractActionKeyRepeat     "OfxInteractActionKeyRepeat"

/**@brief
 This action is issued when an interact gains input focus.
 No openGL calls should be issued by the plug-in during this action.

 @param handle handle to an interact instance, cast to an \ref OfxInteractHandle
 @param inArgs has the following properties on an image effect plugin
     - \ref kOfxPropEffectInstance a handle to the effect for which the interact is being used on,
     - \ref kOfxInteractPropPixelScale the scale factor to convert canonical pixels to screen pixels,
     - \ref kOfxInteractPropBackgroundColour the background colour of the application behind the current view
     - \ref kOfxPropTime the effect time at which the change occurred
     - \ref kOfxImageEffectPropRenderScale the render scale applied to any image fetched

 @param  outArgs is redundant and is set to NULL

 \pre
     - \ref kOfxActionCreateInstance
 has been called on the instance handle,


 @returns
     - \ref kOfxStatOK  the action was trapped
     - \ref kOfxStatReplyDefault  the action was not trapped

    @actiondef
    inArgs:
      - OfxPropEffectInstance
      - OfxInteractPropPixelScale
      - OfxInteractPropBackgroundColour
      - OfxPropTime
      - OfxImageEffectPropRenderScale
    outArgs:
 */
#define kOfxInteractActionGainFocus "OfxInteractActionGainFocus"

/**@brief
 This action is issued when an interact loses input focus.
 No openGL calls should be issued by the plug-in during this action.

 @param handle handle to an interact instance, cast to an \ref OfxInteractHandle
 @param inArgs has the following properties on an image effect plugin
     - \ref kOfxPropEffectInstance a handle to the effect for which the interact is being used on,
     - \ref kOfxInteractPropPixelScale the scale factor to convert canonical pixels to screen pixels,
     - \ref kOfxInteractPropBackgroundColour the background colour of the application behind the current view
     - \ref kOfxPropTime the effect time at which the change occurred
     - \ref kOfxImageEffectPropRenderScale the render scale applied to any image fetched

 @param  outArgs is redundant and is set to NULL

 \pre
     - \ref kOfxActionCreateInstance
     has been called on the instance handle,


 @returns
     - \ref kOfxStatOK  the action was trapped
     - \ref kOfxStatReplyDefault  the action was not trapped

    @actiondef
    inArgs:
      - OfxPropEffectInstance
      - OfxInteractPropPixelScale
      - OfxInteractPropBackgroundColour
      - OfxPropTime
      - OfxImageEffectPropRenderScale
    outArgs:
 */
#define kOfxInteractActionLoseFocus "OfxInteractActionLoseFocus"

/** @propset InteractInstance
    write: host
    props:
      - OfxPropEffectInstance
      - OfxPropInstanceData
      - OfxInteractPropPixelScale
      - OfxInteractPropBackgroundColour
      - OfxInteractPropHasAlpha
      - OfxInteractPropBitDepth
      - OfxInteractPropSlaveToParam
      - OfxInteractPropSuggestedColour
*/


/** @propset InteractDescriptor
    write: host
    props:
      - OfxInteractPropHasAlpha
      - OfxInteractPropBitDepth
*/


/*@}*/
/*@}*/

/** @brief OFX suite that allows an effect to interact with an openGL window so as to provide custom interfaces.

*/
typedef struct OfxInteractSuiteV1 {	
  /** @brief Requests an openGL buffer swap on the interact instance */
  OfxStatus (*interactSwapBuffers)(OfxInteractHandle interactInstance);

  /** @brief Requests a redraw of the interact instance */
  OfxStatus (*interactRedraw)(OfxInteractHandle interactInstance);

  /** @brief Gets the property set handle for this interact handle */
  OfxStatus (*interactGetPropertySet)(OfxInteractHandle interactInstance,
				      OfxPropertySetHandle *property);
} OfxInteractSuiteV1;

#ifdef __cplusplus
}
#endif

#endif
