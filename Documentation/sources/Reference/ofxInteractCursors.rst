.. SPDX-License-Identifier: CC-BY-4.0

Interact cursors (proposed extension)
=====================================

.. note::

   This is a **proposed** extension, not part of the OpenFX standard, and not
   yet submitted upstream. It was tracked in
   `NatronGitHub/openfx#4 <https://github.com/NatronGitHub/openfx/issues/4>`_,
   and is implemented in NatronGitHub/openfx since
   `#63 <https://github.com/NatronGitHub/openfx/pull/63>`_ (headers, HostSupport
   and Support, behind ``OFX_EXTENSIONS_NATRON``), with the host side in Natron's
   RB-2.6 branch (Natron 2.6, unreleased) and the plug-in side in openfx-supportext and openfx-misc. The full
   proposal is in ``Documentation/proposals/interact-cursor.md``.

An overlay interact can draw its own cursor (with OpenGL or the DrawSuite), but
OFX 1.5.1 has no way to change or hide the window-system cursor, which is
always drawn on top.

The proposal adds, in ``ofxInteract.h``:

- ``kOfxInteractHostPropSupportedCursors`` (host descriptor, string X N): the
  cursors the host can display, named with the CSS cursor keywords
  (``default``, ``none``, ``pointer``, ``crosshair``, ``move``, ``grab``,
  ``grabbing``, ``ew-resize``, ``nwse-resize``, ...), plus
  ``OfxInteractCursorCustom`` for PNG images from the plug-in bundle;
- ``kOfxInteractPropCursor``, ``kOfxInteractPropCursorImage`` and
  ``kOfxInteractPropCursorHotSpot`` on interact instances.

The plug-in sets the cursor it wants during pen and key actions, typically when
the pen hovers over one of its handles, and sets ``default`` again when it
leaves. Trapping the event is not required. Several interacts can share a view,
so the host decides which request to display; it should read the current
request of every interact of the view after each event (in HostSupport:
``OFX::Host::Interact::Instance::getCursor()``).

It replaces the parameter-based Natron extension
(``kNatronOfxParamCursorName``), which is deprecated.
