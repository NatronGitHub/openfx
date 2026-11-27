.. SPDX-License-Identifier: CC-BY-4.0
.. _hostSupportGuide:

===========================================
Writing a Host with the HostSupport Library
===========================================

A host can implement OFX directly on the C API, but most of the work is
the same in every host: finding and describing plug-ins, keeping the
property sets of descriptors and instances, answering the suite calls,
and running the actions in the right order with the right arguments.
The C++ HostSupport library (``HostSupport/``, the ``ofxh*`` files,
namespace ``OFX::Host``) does that work. Natron is built on it.

A host using HostSupport:

-  derives a few classes and implements their pure virtual functions,
   which connect OFX to the host's own data (images, parameter values,
   the project, the user interface);
-  sets the host properties that describe what it supports;
-  calls the action methods of the effect instances, in the order the
   OFX specification gives.

This chapter describes these steps. The example host ``hostDemo``
(``HostSupport/examples/hostDemo*.cpp``) is a complete, small
implementation, and the excerpts below come from it (see
:ref:`demoHostsGuide`). The headers in ``HostSupport/include`` document
each class and function.

You still need to know what the API does: HostSupport implements its
mechanics, not the host's policy. The :ref:`reference guide<ImageEffectActions>`
describes each action.

Discovering and Loading Plug-ins
================================

Plug-ins are found and described by two caches:

-  ``OFX::Host::PluginCache`` (``ofxhPluginCache.h``), a singleton
   returned by ``PluginCache::getPluginCache()``, knows the plug-in
   binaries. It scans the plug-in path for bundles
   (``NAME.ofx.bundle/Contents/ARCHITECTURE/NAME.ofx``; the architecture
   directories are in ``ofxhArchDirs.h``), and keeps one
   ``OFX::Host::PluginBinary`` per binary file and one
   ``OFX::Host::Plugin`` per plug-in in it.
-  ``OFX::Host::ImageEffect::PluginCache`` (``ofxhImageEffectAPI.h``)
   handles the plug-ins of the image effect API. It makes an
   ``ImageEffect::ImageEffectPlugin`` for each one, which holds its
   descriptor and its contexts. Its constructor takes the host object
   (see below), which becomes the host of all image effects.

The Plug-in Path
----------------

The ``PluginCache`` constructor reads the ``OFX_PLUGIN_PATH``
environment variable (a list of directories, separated by ``;`` on
Windows, and by ``:`` or ``;`` on Linux, FreeBSD and macOS) and then
appends the standard location of the system, as given in
:ref:`ArchitectureInstallingLocation`:
``/usr/OFX/Plugins`` on Linux and FreeBSD, ``/Library/OFX/Plugins`` on
macOS, and ``Common Files\OFX\Plugins`` on Windows.

The host can change the path:

-  ``PluginCache::useStdOFXPluginsLocation(false)``, before the first
   call to ``getPluginCache()``, leaves out the standard location;
-  ``addFileToPath()`` and ``prependFileToPath()`` add a directory
   (``recurse=false`` to scan it without its sub-directories);
-  ``setPluginHostPath(hostId)`` adds the host's own directory, for
   example ``/usr/OFX/MyHost`` (also after
   ``useStdOFXPluginsLocation(false)``).

The XML Plug-in Cache
---------------------

Describing a plug-in means loading its binary and running its load and
describe actions, which is slow for a large plug-in set. The
``PluginCache`` can save what it learnt in an XML file, and read it back
at the next launch. It then loads a binary only when it is new or has
changed (it compares the file's modification time and size), and the
other plug-ins only when they are used. The XML is parsed with the
expat library.

The usual sequence is: set the cache version, register the image
effect cache, read the old cache, scan, and write the cache back. In
``hostDemo.cpp``:

.. literalinclude:: ../../../HostSupport/examples/hostDemo.cpp
   :language: c++
   :start-at: OFX::Host::PluginCache::useStdOFXPluginsLocation(useStdPaths);
   :end-before: // get the plugin (by default

-  ``setCacheVersion()`` labels the cache. A cache with another label is
   ignored, so change it when what the plug-ins see changes (hostDemo
   uses another version when it presents itself as an OFX 1.4 host,
   because plug-ins may describe themselves differently).
-  ``registerInCache()`` tells the ``PluginCache`` which API cache
   handles plug-ins of the ``OfxImageEffectPluginAPI`` API.
-  ``scanPluginFiles()`` walks the plug-in path. ``dirty()`` says
   whether the cache changed and should be written.
-  ``PluginCache::clearPluginCache()``, before the host exits, unloads
   the plug-ins and deletes the cache. Call it after every instance is
   deleted, while the ``Host`` object and the image effect cache still
   exist.

The plug-in descriptions are cached, so the host properties they depend
on must be set before the scan, and must not change between launches
without a new cache version.

The plug-ins are then available from the image effect cache, by
identifier (``getPluginById()``, which by default picks the highest
version) or as a list (``getPlugins()``).

``OFX::Host::Host::supportsOfxSetHost()`` and
``keepPluginsLoadedAfterScan()`` change two details of the scan: whether
the optional ``OfxSetHost`` entry point of OFX 1.5 is called, and whether
the scanned plug-ins stay loaded. Their documentation in ``ofxhHost.h``
explains the effects on the cache.

The Classes a Host Derives From
===============================

HostSupport implements the OFX objects as C++ classes whose pure
virtual functions the host fills in. Some functions are pure virtual
only when a configuration macro is set (for example the multi-thread
functions with ``OFX_SUPPORTS_MULTITHREAD``, the view functions with
``OFX_EXTENSIONS_NUKE``); the headers mark them with ``#ifdef``.
The sections below give the purpose of each class. The headers list
the functions to implement, and hostDemo implements those of the
default configuration (except the overlay classes: it draws no
overlays).

The Host: ``ImageEffect::Host``
-------------------------------

``OFX::Host::ImageEffect::Host`` (``ofxhImageEffect.h``, derived from
``OFX::Host::Host`` in ``ofxhHost.h``) describes the host application to
the plug-ins and is the factory of the objects HostSupport creates. One
object of it exists in the host. Its pure virtual functions make the
host's own objects (the effect instance, the descriptors) and answer the
message suite calls made with the host as handle. The multi-thread and
dialog suite functions are added with their configuration macros, and so
is ``flushOpenGLResources()``, with ``OFX_SUPPORTS_OPENGLRENDER`` (on by
default with CMake).

Its constructor sets the host properties: name, version, supported
contexts, pixel depths and components, and the capabilities of the
host (tiles, multi-resolution, overlays, parameter features). The
HostSupport base class gives them default values; set each one to what
the host really does, since plug-ins read them to decide what they
describe. ``hostDemoHostDescriptor.cpp`` shows a complete list.
HostSupport's defaults claim support for tiles
(:c:macro:`kOfxImageEffectPropSupportsTiles`), multi-resolution
(:c:macro:`kOfxImageEffectPropSupportsMultiResolution`), temporal clip
access (:c:macro:`kOfxImageEffectPropTemporalClipAccess`) and overlays
(:c:macro:`kOfxImageEffectPropSupportsOverlays`): set them to 0 if the
host doesn't do them. The factory functions are as short as they can
be:

.. literalinclude:: ../../../HostSupport/examples/hostDemoHostDescriptor.cpp
   :language: c++
   :start-at: OFX::Host::ImageEffect::Instance* Host::newInstance
   :end-before: /// message

Other virtual functions have defaults the host may override, for
example ``pluginSupported()`` to reject plug-ins, ``loadingStatus()`` to
show progress during the scan, and ``fetchSuite()`` to add suites (call
the base class for the others).

The Effect Instance: ``ImageEffect::Instance``
----------------------------------------------

``OFX::Host::ImageEffect::Instance`` (``ofxhImageEffect.h``) is one
instance of a plug-in in a context. It owns the instance's clips and
parameters and implements the action methods (``createInstanceAction()``,
``renderAction()``, ...). Its pure virtual functions make the host's
clip and parameter objects (``newClipInstance()``, ``newParam()``), give
the project and time information plug-ins read from the instance (project
size, frame rate, effect duration, ...), and implement the message,
progress and time line suites for the instance, and the parameter edit
calls (``editBegin()`` and ``editEnd()``, which group the plug-in's
changes, for example for undo).

Two virtual functions have defaults that a real host usually overrides:

-  ``paramChangedByPlugin()``, called when the plug-in sets a parameter,
   sends the begin, instance changed and end actions at once, on the
   calling thread, which may be a render thread;
-  ``abort()`` never aborts: override it so that a cancelled render
   stops.

The lifetime of an instance is: ``ImageEffectPlugin::createInstance()``
(which calls the host's ``newInstance()`` and makes the clips and
parameters), ``createInstanceAction()``, the other actions, then
``destroyInstanceAction()``, and only then delete the instance. Call
``destroyInstanceAction()`` yourself: a plug-in may call the host during
that action, and when the destructor of ``Instance`` sends it as a
fallback the host's derived classes are already destroyed.

Before the create instance action, ``createInstanceAction()`` sets
default clip preferences from the connected inputs
(``setDefaultClipPreferences()``), since many plug-ins read the clip
depth and components there. A host that overrides
``createInstanceAction()`` must do the same.

Clips and Images: ``ClipInstance`` and ``Image``
------------------------------------------------

``OFX::Host::ImageEffect::ClipInstance`` (``ofxhClip.h``) connects a
clip of the effect to the host's image source. Its pure virtual
functions report the unmapped clip's properties (depth, components,
premultiplication, frame rate and range, ...), its region of definition,
and return images (``getImage()``). HostSupport answers the plug-in's
property reads on a clip by calling these functions, so the values are
always current.

``getImage()`` returns an ``OFX::Host::ImageEffect::Image``, usually of
a derived class that owns the pixels. Its constructor from the clip
instance takes the render scale, the pixel data pointer, the bounds,
the region of definition, the row bytes, the field and a unique
identifier, and sets the image properties from them. ``getImage()``
returns a new heap object with one reference; the plug-in releases it
with ``clipReleaseImage``, which calls ``releaseReference()``, and the
image deletes itself when no reference is left. The derived image must
keep its pixels alive until it is destroyed.

When the host renders, each input image must cover the region of
interest the plug-in asked for (see below), clipped to the region of
definition of the input. ``clipGetImage`` in ``ofxImageEffect.h`` says:

    If the region parameter is not set, then the region fetched will be
    at least the Region of Interest the effect has previously
    specified, clipped the clip's Region of Definition.

HostSupport converts no pixels: an image must already have the pixel
depth and components that the clip preferences set on its clip.

Parameters: the ``Param::*Instance`` Classes
--------------------------------------------

``ofxhParam.h`` has one class per parameter type
(``Param::DoubleInstance``, ``ChoiceInstance``, ``StringInstance``,
...). The host derives the ones it supports and makes them in
``newParam()``, from the descriptor's type:

.. literalinclude:: ../../../HostSupport/examples/hostDemoEffectInstance.cpp
   :language: c++
   :start-at: OFX::Host::Param::Instance* MyEffectInstance::newParam
   :end-before: // undo/redo blocks

The valued classes read and write the host's value, and its animation
curve if any, through pure virtual ``get()`` and ``set()`` functions.
The key frame functions (``getNumKeys()``, ``getKeyTime()``, ...) have
defaults that report no keys; override them if the host animates the
parameter. A host that supports the string
choice or the bytes parameters says so with
``kOfxParamHostPropSupportsStrChoice`` and
``kOfxHostSupportHostPropSupportsBytes``.

Overlays
--------

A host that draws plug-in overlays derives
``OFX::Host::ImageEffect::OverlayInteract`` (``ofxhImageEffect.h``) and
implements the viewport functions of ``Interact::Instance``
(``ofxhInteract.h``): ``getViewportSize()``, ``getPixelScale()``,
``getScreenPixelRatio()``, ``getBackgroundColour()``,
``getSuggestedColour()``, ``swapBuffers()`` and ``redraw()``. It then
sends the interact actions (draw, pen and key events) through the
methods of that class.

The Property Suite and the Other Suites
=======================================

HostSupport implements the property suite on its ``Property::Set``
class (``ofxhPropertySuite.h``). Every descriptor, instance, clip,
image and parameter is a property set, and the host reads and writes
its properties with ``getIntProperty()``, ``setStringProperty()`` and
the like. A property can also be computed on demand by a
``Property::GetHook``, or report its changes to a
``Property::NotifyHook``: that is how clip and parameter properties
reach the host's functions above.

The suites are returned by ``fetchSuite()``. HostSupport supplies:

-  in ``OFX::Host::Host``: the property suite and the memory suite;
-  in ``ImageEffect::Host``: the image effect suite, the parameter
   suite, the message suite (versions 1 and 2), the interact suite, the
   progress suite (versions 1 and 2), the time line suite, the
   multi-thread suite, and, depending on the configuration and on the
   host, the dialog suite, the OpenGL render suite, the OpenCL program
   suite, the draw suite, and the extension suites (Nuke image planes,
   Vegas stereoscopic images).

The parametric parameter suite is the exception: with
``OFX_SUPPORTS_PARAMETRIC``, ``fetchSuite()`` returns the host's own
implementation, from ``ofxhParametricParam.h`` and its source file, which
the host provides (Natron has them in ``libs/OpenFX_extensions``).

Most suite functions are implemented in HostSupport; those that need the
host's data call the virtual functions of the previous section. Without
``OFX_SUPPORTS_MULTITHREAD``, the multi-thread suite runs the plug-in's
function on the calling thread only, and its mutexes do nothing. The
suites that need host features are only returned when the host says it
has them (for example ``supportsDrawSuite()`` and
``supportsOpenCLProgramSuite()``).

What HostSupport does not supply is the host itself: the pixels and
their conversions, the parameter values and their animation, the
project and time line, the user interface, threads, and the order in
which actions are called.

The Render Action Sequence
==========================

The specification describes the order of the actions for each frame
rendered in :doc:`../Reference/ofxRendering`, and when the clip
preferences are run in :ref:`ImageEffectClipPreferences`. In
particular:

    All calls to the :c:macro:`kOfxImageEffectActionRender` are
    bracketed by a pair of
    :c:macro:`kOfxImageEffectActionBeginSequenceRender` and
    :c:macro:`kOfxImageEffectActionEndSequenceRender` actions.

With HostSupport, a host that renders a sequence of frames calls:

#. ``ImageEffectPlugin::createInstance(context, clientData)``, then set
   the parameters from a saved project if there is one, then
   ``createInstanceAction()``;
#. ``getClipPreferences()`` once the clips are connected. HostSupport
   doesn't run it again by itself: ``clipInstanceChangedAction()``, and
   ``paramInstanceChangedAction()`` for a parameter listed in
   ``kOfxImageEffectPropClipPreferencesSlaveParam``, only mark the clip
   preferences out of date. Before the next render, call
   ``runGetClipPrefsConditionally()``, which runs the action if they are
   (hostDemo tests ``areClipPrefsDirty()`` and calls
   ``getClipPreferences()``);
#. optionally ``getTimeDomainAction()``, to know the frame range of the
   effect;
#. ``beginRenderAction()`` with the frame range;
#. for each frame:

   #. ``getRegionOfDefinitionAction()``, and clip the render window to
      the result;
   #. optionally ``isIdentityAction()``: if the effect is an identity,
      use the named input at the returned time instead of rendering;
   #. ``getFrameNeededAction()`` for effects that read other frames;
   #. ``getRegionOfInterestAction()`` for the clipped window, which
      gives a region per input clip (HostSupport fills in the default
      when the plug-in doesn't trap the action, or doesn't support
      tiles);
   #. produce the input images for those regions (rendering the
      upstream effects), so that ``ClipInstance::getImage()`` can return
      them;
   #. ``renderAction()`` with the render window;

#. ``endRenderAction()`` with the same arguments as
   ``beginRenderAction()``;
#. finally ``destroyInstanceAction()`` and delete the instance.

The host's duties during a render:

-  without tiles (:c:macro:`kOfxImageEffectPropSupportsTiles` 0), render
   the whole region of definition if the effect or its output clip lacks
   them, and give full-RoD images from any input clip that lacks them:
   "the host should supply full RoD images to the effect whenever it
   fetches one";
-  an effect that needs its frames in order
   (:c:macro:`kOfxImageEffectInstancePropSequentialRender`, read with
   ``requiresSequentialRender()`` and ``prefersSequentialRender()``)
   gets them from first to last for a final render. The
   ``sequentialRender`` argument of ``beginRenderAction()`` and
   ``renderAction()`` says whether the render is sequential
   (:c:macro:`kOfxImageEffectPropSequentialRenderStatus`).

When the user changes a parameter, call ``beginInstanceChangedAction()``,
``paramInstanceChangedAction()`` and ``endInstanceChangedAction()``,
then check the clip preferences as above.

HostSupport doesn't know the host's node graph. When an effect's output
preferences change (its depth, components, premultiplication or frame
rate), run the clip preferences of the effects downstream too, in
order. Don't run an instance's clip preferences while another action of
that instance is running.

hostDemo follows this sequence: ``renderFrame()`` in ``hostDemo.cpp``
calls the region of definition, identity, frames needed, regions of
interest and render actions for each frame, between the begin and end
sequence render calls of ``run()``.

Threads
-------

hostDemo renders on one thread. A host that calls plug-ins from several
threads must:

-  build HostSupport with ``OFX_SUPPORTS_MULTITHREAD`` and implement the
   multi-thread suite functions it adds to ``ImageEffect::Host``;
-  honour the effect's render thread safety
   (:c:macro:`kOfxImageEffectPluginRenderThreadSafety`, read with
   ``getRenderThreadSafety()``): an unsafe effect gets one render call at
   a time in all its instances, an instance-safe effect one render call
   at a time per instance;
-  render one frame as several render windows at once only if the effect
   allows it (:c:macro:`kOfxImageEffectPluginPropHostFrameThreading`,
   ``getHostFrameThreading()``, 1 by default); with 0, the plug-in
   threads each frame itself;
-  keep the render state of each thread apart: ``ClipInstance::getImage()``
   gets no render scale, and the view of ``getImage()`` and
   ``getRegionOfDefinition(time)`` is the one ``setView()`` stored on the
   clip, so they must use the scale and view of the render on the
   calling thread (Natron uses thread-local storage);
-  not call ``ImageEffectPlugin::getPluginHandle()`` or
   ``createInstance()`` from several threads at once: they aren't
   thread-safe.

OpenGL Rendering
----------------

HostSupport implements the OpenGL render suite
(``OfxImageEffectOpenGLRenderSuiteV1``) and sends
:c:macro:`kOfxActionOpenGLContextAttached` and
:c:macro:`kOfxActionOpenGLContextDetached`, but the contexts and the
textures are the host's: ``ClipInstance::loadTexture()`` is pure
virtual. The optional library ``openfx::HostSupportOpenGL`` (CMake option
``OFX_HOSTSUPPORT_OPENGL``, off by default) provides them for hosts that
don't have their own:

-  ``ofxhOpenGLContext.h``: offscreen contexts, with backends chosen at
   run time (EGL surfaceless, EGL devices and GLX on Linux and BSD; CGL
   on macOS; WGL on Windows), the renderers they offer, each selectable
   by its id (EGL devices, CGL renderers, and GPUs through
   ``WGL_NV_gpu_affinity`` or ``WGL_AMD_gpu_association``), a context
   pool (one context per render tree) and ``CurrentContext``, which
   makes a context current on the calling thread. The host may give its
   own OpenGL loader, a logger, and a wrapper for driver calls (for
   example to mask floating-point exceptions). With GLX, a host that uses
   Xlib from several threads with libX11 older than 1.8 (for example
   EL9's 1.7) calls ``XInitThreads()`` first, before any other Xlib call:
   the library serializes its own Xlib calls but doesn't call it;
-  ``ofxhOpenGLTexture.h``: textures of OFX image rectangles (RGBA, RGB
   and Alpha, in each depth), with upload, readback and the render
   target;
-  ``ofxhOpenGLRender.h``: ``chooseRenderMode()`` (OpenGL, CPU, or
   neither, from :c:macro:`kOfxImageEffectPropOpenGLRenderSupported` and
   :c:macro:`kOfxImageEffectPropCPURenderSupported`), ``Attachment``
   (the context an instance is attached to, and its context data), and
   ``RenderSession``, which attaches the instance, loads the textures of
   the clips of a render, frees the textures the plug-in did not free,
   and reads the output back. The host still overrides
   ``ClipInstance::loadTexture()``: its override forwards to
   ``RenderSession::loadTexture()`` of the render running on the
   calling thread. ``clipLoadTexture`` with an explicit format gives a
   texture of that depth ("When a specific format is requested, the host
   ensures it gives the requested format"), and a format that isn't
   :c:macro:`kOfxBitDepthByte`, ``Short``, ``Half`` or ``Float`` fails
   the input instead of falling back to another depth.

``flushResources`` ("request the host to flush its GPU resources if it
holds any") calls the host's ``flushOpenGLResources()``. A host with a
``ContextPool`` returns ``OpenGL::flushResources(pool)``, which destroys
the idle contexts no instance is attached to and returns
:c:macro:`kOfxStatOK` if it destroyed one, :c:macro:`kOfxStatReplyDefault`
otherwise.

The host still decides when to render with OpenGL: it sets
:c:macro:`kOfxImageEffectPropOpenGLRenderSupported` to ``"true"`` on its
descriptor, passes ``openGLRender`` to the render actions inside a
``RenderSession``, and renders again on the CPU when the plug-in returns
:c:macro:`kOfxStatGPURenderFailed`, or when ``RenderSession::inputFailed()``
says an input couldn't be given as a texture, if the plug-in can render
on the CPU.

A context serves one render action at a time: effects "can use the
graphics context as they see fit", with render passes "interleaved with
passes performed on the CPU" (``ofxGPURender.h``), so its OpenGL state must
last for the whole action of the plug-in. While ``RenderSession::loadTexture()`` calls
``ClipInstance::getImage()``, the context is suspended (current nowhere) but
reserved to the render. A render that the fetch starts on another thread
(for example the tiles of an input rendered on demand) gets its context
with ``ContextPool::tryAcquire()`` and a key of its own, which never waits:
with no context available, it renders on the CPU if the plug-in can, and
fails otherwise. A render nested on the same thread may use the reserved
context. A host whose own ``ClipInstance::getImage()`` may render during an
OpenGL render (a plug-in fetching a CPU image in OpenGL mode) does the
same: ``suspend()`` the session's ``CurrentContext`` before rendering, and
``resume()`` it after. The header
of ``ofxhOpenGLRender.h`` shows the sequence, and
``HostSupport/examples/openglRenderChecks.cpp`` runs it.

OpenCL Rendering
----------------

With OpenCL ("OpenCL Acceleration of Rendering", ``ofxGPURender.h``), the
host passes the images of a render as OpenCL buffers
(:c:macro:`kOfxImageEffectPropOpenCLRenderSupported`, the ``cl_mem`` in
:c:macro:`kOfxImagePropData`) or OpenCL images
(:c:macro:`kOfxImageEffectPropOpenCLSupported`, the ``cl_mem`` in
:c:macro:`kOfxImageEffectPropOpenCLImage`), and sets
:c:macro:`kOfxImageEffectPropOpenCLEnabled` and
:c:macro:`kOfxImageEffectPropOpenCLCommandQueue` in the arguments of the
BeginSequenceRender, Render and EndSequenceRender actions. HostSupport sets
those two while an ``ImageEffect::GPURenderScope`` is alive on the calling
thread for the instance (``ofxhImageEffect.h``); without one, they are 0 and
NULL.

The optional library ``openfx::HostSupportOpenCL`` (CMake option
``OFX_HOSTSUPPORT_OPENCL``, off by default) does the rest for hosts that
don't have their own OpenCL code. It needs no OpenCL SDK: the OpenCL ICD
loader (the OpenCL framework on macOS) is loaded at run time, unless the
host gives its own loader.

-  ``ofxhOpenCLDevice.h``: the devices of the OpenCL platforms, a context
   on one of them (``createDevice()``), and ``Queue``, which lends a
   render one of the device's in-order command queues;
-  ``ofxhOpenCLRender.h``: ``chooseRenderMode()`` (OpenCL buffers, OpenCL
   images, the CPU, or none of them) and ``RenderSession``, which opens the
   ``GPURenderScope`` (only if both the plug-in and the host descriptor
   support the mode, as the spec requires before
   :c:macro:`kOfxImageEffectPropOpenCLEnabled` is set) and copies the images
   of a render: the host's
   ``ClipInstance::getImage()`` override returns
   ``RenderSession::getImage()`` during the render, with the CPU image it
   would return, and ``readOutput()`` copies the output back to the host's
   image.

The images keep the bounds, depth and components of the CPU images, with
no conversion: buffers are packed, and OpenCL images are ``CL_RGBA``. OFX
does not say how other components map to OpenCL images, so
``chooseRenderMode()`` uses OpenCL images only when the clips are RGBA, and
OpenCL buffers or the CPU otherwise. After a failure, the host
may render on the CPU ("If a call using OpenCL rendering fails, the host may
re-attempt using CPU buffers instead"): ``cpuRetryAllowed()`` says when the
plug-in can. The rule is the OpenGL one, ``ImageEffect::gpuRenderFailed()``
and ``ImageEffect::canRenderOnCPU()`` in ``ofxhImageEffect.h``; OpenGL adds
that a plug-in whose :c:macro:`kOfxImageEffectPropOpenGLRenderSupported` is
``"needed"`` has no CPU render. ``HostSupport/examples/openclRenderChecks.cpp`` runs a render
with the GPUGain sample.

``Device::compileProgram()`` implements the OpenCL program suite
(``OfxOpenCLProgramSuiteV1``): it builds the program with the host's
options (``Hooks::buildOptions``, for example ``-cl-denorms-are-zero``)
and keeps the binaries, in memory and in ``Hooks::programCacheDirectory``,
keyed on the source, the options and the platform, device and driver
("if the source of the program or the OpenCL environment changes, the host
must recompile"). A cache file is checked whole before its binary reaches the
driver. The directory must be private to the user, since a binary may carry
native code: on POSIX, a directory not owned by the user or writable by
others is not used (memory cache only), and the files are created with mode
0600. The host owns the directory (nothing evicts its files). ``compileProgram`` has no context argument: the program
is built for the context of the render's command queue, which
``OpenCL::currentQueue()`` finds from the ``GPURenderScope`` of the calling
thread. A host that runs the plug-in's multi-thread functions on other
threads passes the scope on to them (``ofxhImageEffect.h``). Outside an
OpenCL render, a call with ``fOptional`` set returns
:c:macro:`kOfxStatReplyDefault` and no program ("host may skip compiling on
this call"), and other calls fail. The host returns ``true`` from
``Host::supportsOpenCLProgramSuite()`` and forwards
``Host::compileOpenCLProgram()``; hostDemo (``--gpu=opencl``) and the
Python host (``ofxh.init(opencl=True)``) do.

Metal Rendering
---------------

With Metal ("Apple Metal Rendering", ``ofxGPURender.h``), the host passes
the images of a render as Metal buffers
(:c:macro:`kOfxImageEffectPropMetalRenderSupported`, the ``id<MTLBuffer>`` in
:c:macro:`kOfxImagePropData`) or, after OFX 1.5.1, Metal textures
(:c:macro:`kOfxImageEffectPropMetalTextureSupported`, the ``id<MTLTexture>``
in :c:macro:`kOfxImagePropData`), and sets
:c:macro:`kOfxImageEffectPropMetalEnabled` (buffers) or
:c:macro:`kOfxImageEffectPropMetalTextureEnabled` (textures), never both, and
:c:macro:`kOfxImageEffectPropMetalCommandQueue` in the arguments of the
BeginSequenceRender, Render and EndSequenceRender actions, while an
``ImageEffect::GPURenderScope`` of API ``Metal`` or ``MetalTextures`` is
alive, as for OpenCL.

The optional library ``openfx::HostSupportMetal`` (CMake option
``OFX_HOSTSUPPORT_METAL``, off by default, macOS only) is the Metal
counterpart of ``openfx::HostSupportOpenCL``. It is written in
Objective-C++ with ARC, but its headers are plain C++: the Metal objects
are given as ``void *``.

-  ``ofxhMetalDevice.h``: the Metal devices, one of them (``createDevice()``,
   by default the system default device, or ``createDeviceWithRegistryID()``),
   ``Queue``, which lends a render one of the device's command queues, and
   ``AutoreleasePool``, for the host's render threads;
-  ``ofxhMetalRender.h``: ``chooseRenderMode()`` (Metal buffers, Metal
   textures, the CPU, or none; buffers when the plug-in has both, unless the
   host prefers textures; textures only for RGBA and Alpha clips, as the spec
   forbids RGB textures) and ``RenderSession``, which opens the
   ``GPURenderScope`` only if both the plug-in and the host descriptor
   support its mode, and copies the images of a render to packed Metal
   buffers in shared storage, or to private textures of the spec's pixel
   formats, through blits, and the render window of the output back. A
   texture's texel (i, j) is the pixel (x1 + i, y1 + j): the spec's bottom
   left origin. The session keeps every buffer and texture until it ends, and
   waits for the plug-in's work on them with a blit that reads and writes
   each one: Metal's hazard tracking runs it after the plug-in's commands that
   use them. A failed command buffer of the plug-in goes undetected (Metal has
   no queue-wide error).

The CPU retry is the OpenCL one (``cpuRetryAllowed()``).
``HostSupport/examples/metalRenderChecks.cpp`` runs a render with the
GPUGain sample (buffers), and checks the textures with its own kernels
(GPUGain has no texture path).

CUDA Rendering
--------------

With CUDA ("CUDA Rendering", ``ofxGPURender.h``), the host passes the images
of a render in CUDA device memory
(:c:macro:`kOfxImageEffectPropCudaRenderSupported`, a CUDA device pointer in
:c:macro:`kOfxImagePropData`), and sets
:c:macro:`kOfxImageEffectPropCudaEnabled` in the arguments of the
BeginSequenceRender, Render and EndSequenceRender actions, while an
``ImageEffect::GPURenderScope`` of API ``CUDA`` is alive.
:c:macro:`kOfxImageEffectPropCudaStream` "will only be set if the host and
plug-in both support CUDA streams": HostSupport adds it to those arguments
only in a scope of API ``CUDA`` with a stream, when the host descriptor and
the plug-in both support streams (``ImageEffect::cudaStreamsSupported()``);
otherwise the arguments don't have it.

The optional library ``openfx::HostSupportCUDA`` (CMake option
``OFX_HOSTSUPPORT_CUDA``, off by default) is the CUDA counterpart of
``openfx::HostSupportOpenCL``. It needs no CUDA toolkit: the CUDA driver API
(``libcuda.so.1``, ``nvcuda.dll``; there is none on macOS) is loaded at run
time, unless the host gives its own loader.

-  ``ofxhCUDADevice.h``: the CUDA devices (with their UUID, the identity a
   host saves: the indices change with ``CUDA_VISIBLE_DEVICES``), one of
   them (``createDevice()``, by default the first, or
   ``createDeviceWithUUID()``), its primary context (the CUDA runtime API's,
   so that a plug-in built with the runtime API works on the host's
   memory), ``ContextScope``, which makes it current on a thread, and
   ``Stream``, which lends a render one of the device's non-blocking
   streams (a host may keep one for a whole frame, for the sessions of all
   its nodes). After a sticky CUDA error the device is no longer
   ``usable()``, and the host renders on the CPU;
-  ``ofxhCUDARender.h``: ``chooseRenderMode()`` (CUDA, the CPU, or neither)
   and ``RenderSession``, which opens the ``GPURenderScope`` only if both
   the plug-in and the host descriptor support CUDA, makes the primary
   context current during the actions, passes a stream if both support
   streams, and copies the images of a render to packed device memory, and
   the render window of the output back. With a stream, the plug-in "SHOULD
   NOT wait for final asynchronous operations to complete before returning
   from the render action", so the session waits for the stream before it
   reads the output back and before it frees the memory, which it keeps
   until it ends (the plug-in releases its images before its work is done).

The CPU retry is the OpenCL one (``cpuRetryAllowed()``), also after a stream
or a copy failed; ``RenderSession::status()`` gives the failure as
:c:macro:`kOfxStatGPUOutOfMemory` or :c:macro:`kOfxStatGPURenderFailed`. ``HostSupport/examples/cudaRenderChecks.cpp`` checks the
library on a fake CUDA driver, on any machine, and on a CUDA device with the
GPUGain sample built with CUDA, if there is one.

Building a Host with HostSupport
================================

HostSupport is a static library. It needs the OFX headers in
``include/`` and the expat XML library, used as a system library (the
repository doesn't contain a copy of it). Install expat with your
system's package manager (for example ``libexpat1-dev``, MacPorts
``expat``, MSYS2 ``mingw-w64-ucrt-x86_64-expat``, or vcpkg or conan
``expat``).

With CMake, the ``OFX_BUILD_HOSTSUPPORT`` option builds it. It is on by
default when openfx is the top-level project, and off when a project
includes openfx with ``add_subdirectory()`` (most of those are plug-ins,
which don't need it). The library target is ``openfx::HostSupport``,
which brings the include directories, expat and the configuration
definitions with it:

.. code-block:: cmake

    set(OFX_BUILD_HOSTSUPPORT ON)
    add_subdirectory(openfx)
    target_link_libraries(myhost PRIVATE openfx::HostSupport)

An openfx installed with ``cmake --install``, after a build with
HostSupport, provides the same target to ``find_package(openfx)``. If
CMake does not find expat, set ``EXPAT_INCLUDE_DIR`` and
``EXPAT_LIBRARY``.

The Makefile in ``HostSupport/`` builds ``libofxHost.a`` with
``make -C HostSupport``. It takes the expat flags from ``pkg-config``,
or from ``EXPAT_INCLUDE=<dir>``; ``HostSupport/BUILDING`` gives the
details. Unlike the CMake defaults, it defines none of the
``OFX_EXTENSIONS_*`` and ``OFX_SUPPORTS_*`` macros: pass them in
``OPTIMISE`` (which replaces the default optimisation flags), the same
as for the host's own sources (see below).

Configuration Macros
--------------------

The ``OFX_EXTENSIONS_*`` and ``OFX_SUPPORTS_*`` macros select the
extensions and optional features compiled into HostSupport. They change
class layouts and add pure virtual functions, so every source file of
the host, and the HostSupport sources compiled into it, must use the
same set. With CMake, choose them only through openfx's options, which
``openfx::HostSupport`` passes on to the targets that link it; a build
that compiles the HostSupport sources itself must give them the same
definitions. A mismatch is not detected at build time. The plug-ins are
separate binaries and need not use the same set as the host: the
extensions are negotiated at run time through properties and suites.
The `README <https://github.com/NatronGitHub/openfx/blob/master/README.md>`_
lists the options and their defaults.

``OFX_SUPPORTS_MULTITHREAD``, ``OFX_SUPPORTS_DIALOG`` and the
parametric parameter options are off by default: turn them on only if
the host implements the functions they make pure virtual (and, for the
parametric options, provides ``ofxhParametricParam.h`` and its
implementation).
