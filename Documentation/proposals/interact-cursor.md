# DRAFT: Open Effects Proposal for Standard Change: Interact cursors

Status: draft, written for NatronGitHub/openfx#4. Not yet submitted upstream.
Under the OpenFX AI policy, a human author must submit and defend it.

## Summary

Let an overlay interact choose the window-system cursor shown over its view, or hide it, through a property on the interact instance.

## Motivation

Overlays often need a cursor that shows what a click would do: move, rotate or scale a handle, pick a colour, or paint with a brush. Today a plug-in can draw a cursor with OpenGL or the DrawSuite, but the system cursor is always drawn on top of it, because no interact "owns" the cursor (NatronGitHub/openfx#4, Natron#704). OFX 1.5 does not address this: `ofxInteract.h`, `ofxDrawSuite.h` and the reference docs define nothing about the cursor.

## Problem

1. No API lets a plug-in hide or change the cursor.
2. Several interacts can share a view (overlays of several effects, host overlays), so the spec must say whose cursor is shown.
3. A host can show the same effect in several views at once. The cursor is per-view state, not per-effect state.

## Proposal

Defined in `include/ofxInteract.h` (see the `InteractCursor` group):

| Name | On | Type | Purpose |
|---|---|---|---|
| `kOfxInteractHostPropSupportedCursors` | host descriptor | string X N | Negotiation, and the list of cursors the host can show |
| `kOfxInteractPropCursor` | interact instance | string X 1 | Requested cursor (default `"default"`) |
| `kOfxInteractPropCursorImage` | interact instance | string X 1 | PNG file in the bundle's `Contents/Resources`, used with `kOfxInteractCursorCustom` (optional `@2x`) |
| `kOfxInteractPropCursorHotSpot` | interact instance | int X 2 | Hot spot of the custom image |

The cursor names are the CSS cursor keywords (`default`, `none`, `pointer`, `crosshair`, `move`, `grab`, `grabbing`, `ew-resize`, `nwse-resize`, `zoom-in`, ...). They are platform-neutral and documented by the W3C, and they map directly to Cocoa, Win32, X11/Wayland and Qt cursors.

**Reporting: the plug-in's own report decides.** The cursor properties are the plug-in's report of the cursor it wants in that view.
- After every pen or key action (PenMotion, PenDown, PenUp, KeyDown, KeyUp, KeyRepeat) that does not fail, the host reads that instance's cursor properties. "Does not fail" means it returned `kOfxStatOK` (the event was trapped) or `kOfxStatReplyDefault` (not trapped).
- PenMotion is "issued whether the pen is currently up or down". So a plug-in sets a cursor when the pen hovers over a handle, and sets `default` again when the pen moves off it. Setting the cursor doesn't require trapping the event: plug-ins keep their existing return values. Trapping is optional, and only gives priority in arbitration. Reading the properties only after trapped events would leave a resize or grab cursor stuck after the pen leaves the handle.

**Arbitration: the host decides between interacts.** Several interacts can share a view (overlays of several effects, host overlays). Recommended policy:
1. the cursor of the interact that trapped the event (`kOfxStatOK`) wins. This extends the existing PenMotion postcondition ("if the instance returns kOfxStatOK the host should not pass the pen motion to any other interactive object ... that shares the same view");
2. otherwise, the most recent non-default request of an interact of that view;
3. otherwise, the host's own cursor.

When the displayed request is released (its interact goes back to `default`, loses focus or is destroyed), the host re-evaluates this order from the current requests of all interacts of the view, and only then falls back to its own cursor. Interacts report their request once, when it changes, so an interact still over its handle won't report its unchanged cursor again. (Found in the first Natron GUI test: ContactSheet's pointer was lost after hovering another overlay's handle and coming back.)

**Host strategies.** A host can react to `cursorChanged()` notifications, or, preferably, poll every interact's current request (`getCursor()`) after dispatching each pen or key event to the overlays of a view, and recompute the displayed cursor. Polling is required when one interact instance is shared by several views: the change is only reported once, but each view needs it. Natron does this: the first non-default request in dispatch order wins, otherwise the viewer's own cursor is shown. Viewer tools such as the wipe, RoI and colour picker keep priority.

An interact that loses focus or is destroyed no longer owns the cursor, and the host restores its own cursor if that interact's request was displayed.

**Custom images.** `kOfxInteractPropCursorHotSpot` is in pixels of the 1x image. For an `@2x` file the hot spot in that image's pixels is twice that (with Qt: `QPixmap::setDevicePixelRatio(2)`, hot spot unchanged).

**Why a property on the interact instance, and not a parameter?** A secret String parameter holding the cursor name would also work, but parameters are persistent: they are saved in the project, can be undone, trigger `kOfxActionInstanceChanged`, and are shared by all views. The cursor is transient UI state of the view under the pen. The interact instance is the right place for it. Some hosts create one per view; others, like Natron, share one instance between views. Such hosts read the current request with `getCursor()` after each event in the view that received it (see "Host strategies").

## Impact

This is a new feature with no compatibility impact. It adds only new properties, and no suite changes.

- **Host implements it, plug-in doesn't:** nothing changes.
- **Plug-in implements it, host doesn't:** the plug-in sees no `kOfxInteractHostPropSupportedCursors` and does not set the property. If it sets the property anyway, the call fails with `kOfxStatErrUnknown`, which is harmless.
- **Qt-based hosts (e.g. Natron):** the CSS names map onto `Qt::CursorShape` as follows:

| CSS | Qt (`Qt::CursorShape`) |
|---|---|
| default | ArrowCursor |
| none | BlankCursor |
| crosshair | CrossCursor |
| text | IBeamCursor |
| wait | WaitCursor |
| progress | BusyCursor |
| not-allowed | ForbiddenCursor |
| pointer | PointingHandCursor |
| help | WhatsThisCursor |
| ns-resize / ew-resize | SizeVerCursor / SizeHorCursor |
| nesw-resize / nwse-resize | SizeBDiagCursor / SizeFDiagCursor |
| move | SizeAllCursor |
| row-resize / col-resize | SplitVCursor / SplitHCursor |
| grab / grabbing | OpenHandCursor / ClosedHandCursor |

## Documentation impact

- New "Interact cursors" section in `Documentation/sources/Reference/ofxInteracts.rst`.
- The new properties in the generated property sets (`@propdef` blocks once ported to the 1.5 header format).
- A release note.

## Implementation plan (needs one host and one plug-in, from different source trees)

1. **Support library:**
   - `OFX::Interact::setCursor(const std::string&)`;
   - `setCustomCursor(const std::string& file, int hotX, int hotY)`;
   - `ImageEffectHostDescription::supportedCursors`.
2. **HostSupport:**
   - declare the instance properties when the host descriptor lists cursors;
   - after each non-failing pen/key action, call `Interact::Instance::cursorChanged(actionStatus, cursor, image, hotX, hotY)` (overridden by hosts) when the instance's request changed since the last report, and `"default"` after LoseFocus;
   - `Interact::Instance::getCursor()` to poll an interact's current request when arbitrating;
   - the host restores its own cursor when an interact that owns it is destroyed (HostSupport can't call a virtual from the destructor).
3. **Natron:**
   - map the names to Qt cursors, and apply the rule above in `ViewerGL`.
4. **openfx-misc:**
   - use the cursor in `ofxsTransformInteract` (move, rotate, scale handles);
   - use a `none` cursor with a drawn brush or crosshair where an overlay already draws one.

## Open questions

- Should the host also read the cursor after `kOfxInteractActionDraw`, so that a cursor can change without a pen event (e.g. after a parameter change)?
- Should a minimal set of cursors be mandatory when the property is present, e.g. `default`, `none`, `pointer`, `crosshair`, `move`?
- Should non-PNG formats be allowed for custom images (SVG for resolution independence)?
