.. SPDX-License-Identifier: CC-BY-4.0
.. _supportLibraryGuide:

==========================================
Writing a Plug-in with the Support Library
==========================================

The examples before this chapter use the raw C API: each plug-in fetches
the suites, reads and writes properties one by one, and compares action
strings in its own main entry point. Most plug-ins are instead written
with the C++ *Support library*, in
`Support/ <https://github.com/NatronGitHub/openfx/tree/master/Support>`__
(the ``ofxs*`` files, in namespace ``OFX``). openfx-misc, openfx-io and
openfx-arena are built on it. It wraps the C API in classes, so that a
plug-in describes itself with typed calls and handles an action by
overriding a virtual function.

Start with the example: it walks through the
`Invert <https://github.com/NatronGitHub/openfx/blob/master/Support/Plugins/Invert/invert.cpp>`__
sample of ``Support/Plugins/``. The sections after it explain how to
build a plug-in, how it registers, the configuration macros, what the
library does for you and its main classes. This chapter is not a class
reference: Doxygen generates that from the headers, with
``cd Support && doxygen support.doxy`` (the output is in
``Support/doc/html``).

Example: Invert
===============

`invert.cpp <https://github.com/NatronGitHub/openfx/blob/master/Support/Plugins/Invert/invert.cpp>`__
is the Support version of the :ref:`invert example <invertExample>`. It
also has two boolean parameters, ``supportsTiles`` and ``testSmallRoI``.
They are used to test hosts; a real plug-in wouldn't have them.

It includes the image effect header, and the samples' processing helper:

.. literalinclude:: ../../../Support/Plugins/Invert/invert.cpp
   :language: c++
   :start-at: #include "ofxsImageEffect.h"
   :end-at: #include "ofxsProcessing.H"

``ofxsImageEffect.h`` also includes ``ofxsParam.h``, ``ofxsInteract.h``
and ``ofxsCore.h``. ``ofxsProcessing.H`` is in ``Support/Plugins/include``,
which ``openfx::Support`` and ``Makefile.master`` put on the include path.

Describing the Plug-in
----------------------

The factory is declared with ``mDeclarePluginFactory``. ``describe()``
sets the labels, the contexts, the pixel depths and a few flags; each
setter writes one property of the effect descriptor, such as
:c:macro:`kOfxImageEffectPropSupportedPixelDepths` or
:c:macro:`kOfxImageEffectPropSupportsTiles`:

.. literalinclude:: ../../../Support/Plugins/Invert/invert.cpp
   :language: c++
   :start-at: mDeclarePluginFactory(InvertExamplePluginFactory
   :end-before: void InvertExamplePluginFactory::describeInContext

``describeInContext()`` defines the clips and the parameters. The filter
context needs a clip named :c:macro:`kOfxImageEffectSimpleSourceClipName`
and the output clip, :c:macro:`kOfxImageEffectOutputClipName`:

.. literalinclude:: ../../../Support/Plugins/Invert/invert.cpp
   :language: c++
   :start-at: void InvertExamplePluginFactory::describeInContext
   :end-before: OFX::ImageEffect* InvertExamplePluginFactory::createInstance

``describe()`` also says how the plug-in may be called from threads:
``setRenderThreadSafety()`` (:c:macro:`kOfxImageEffectPluginRenderThreadSafety`;
fully safe means ``render()`` may run on several threads at once, so any
state it shares must be locked, with ``OFX::MultiThread::Mutex``) and
``setHostFrameThreading()``.

``createInstance()`` creates the instance, and a static factory object
registers the plug-in with its identifier and version (see
:ref:`supportRegistration`):

.. literalinclude:: ../../../Support/Plugins/Invert/invert.cpp
   :language: c++
   :start-at: OFX::ImageEffect* InvertExamplePluginFactory::createInstance

The Instance
------------

``InvertPlugin`` derives from ``OFX::ImageEffect``. Its constructor
fetches the clips and parameters by name; the effect owns them:

.. literalinclude:: ../../../Support/Plugins/Invert/invert.cpp
   :language: c++
   :start-at: class InvertPlugin : public OFX::ImageEffect
   :end-before: /* testSmallRoI

It overrides ``getRegionsOfInterest()`` and ``changedParam()`` for the
two test parameters, and ``render()``. Mark each overridden action
``override`` (C++11): the configuration macros change some signatures
(:ref:`supportConfiguration`), and without ``override`` a function with
the wrong arguments compiles and is never called.

Rendering
---------

``render()`` picks a template instantiation for the output clip's depth
and components, for example ``ImageInverter<unsigned char, 4, 255>`` for
8-bit RGBA. Then ``setupAndProcess()`` fetches the images at the render
time, checks them, and runs the processor on the render window:

- ``fetchImage()`` returns ``NULL`` if the host has no image, for
  example for an unconnected input. No source image renders black and
  transparent; no output image is an error.
- The output image must have the depth and components of the clip, from
  which ``render()`` chose the processor.
- ``checkBadRenderScaleOrField()`` checks, in debug builds, that the
  host gave the render scale and field of the render arguments.

The ``OFX::auto_ptr`` objects (``std::unique_ptr`` from C++11) release
the images to the host when they go out of scope:

.. literalinclude:: ../../../Support/Plugins/Invert/invert.cpp
   :language: c++
   :start-after: // set up the images, then run the processor on the render window
   :end-before: // the overridden render function

The processor, an ``OFX::ImageProcessor``, splits the render window
between threads and calls ``multiThreadProcessImages()`` on each part.
The per-pixel code is a template on the pixel type, the number of
components and the maximum value:

.. literalinclude:: ../../../Support/Plugins/Invert/invert.cpp
   :language: c++
   :start-at: template <class PIX, int nComponents, int max>
   :end-before: ////////////////////////////////////////////////////////////////////////////////

``_effect.abort()`` asks the host whether the render was cancelled (the
image effect suite's ``abort()``). The source pixel is looked up with
``getPixelAddress()``, which returns ``NULL`` outside the image's bounds:
the output there is black and transparent.

Errors
------

The plug-in reports an error by throwing
``OFX::throwSuiteStatusException(status)``; the dispatcher returns that
status to the host (see :ref:`supportWhatItDoes`), so no action needs a
``try`` block just to return a status. For the render action the
specification lists :c:macro:`kOfxStatFailed` as "something wrong, but no
error code appropriate, plugin to post message": tell the user why first,
with ``setPersistentMessage(OFX::Message::eMessageError, "", reason)``,
and call ``clearPersistentMessage()`` once the instance works again
(Invert's ``renderError()`` and ``setupAndProcess()`` do this, clearing
only a message they posted, so that a working render makes no message
suite call). ``render()`` throws
:c:macro:`kOfxStatErrUnsupported` for a depth it doesn't handle.

Catch the exceptions of the libraries the plug-in calls: they would reach
the dispatcher as :c:macro:`kOfxStatFailed`, with no message. A
library's warning should become a message
(``OFX::Message::eMessageWarning``), not a failure.

Testing
-------

``Support/Plugins/tests`` has pytest tests of each sample. Each test runs
on both backends of the Python ``ofxhost`` module: the demo host,
hostDemo, in a subprocess, and in-process on the HostSupport Python
bindings (skipped if they aren't built). Build the samples and hostDemo
with ``-DBUILD_EXAMPLE_PLUGINS=ON -DBUILD_EXAMPLE_HOSTS=ON``, and see
``Support/Plugins/tests/README.md`` for how to run them.

Beyond the Sample
-----------------

Invert keeps to the minimum. Real plug-ins also get these right:

- **Parameters at the right time.** In ``render()``, ``isIdentity()``,
  ``getRegionOfDefinition()`` and ``getRegionsOfInterest()``, read
  animated parameters with ``getValueAtTime(args.time)``, not
  ``getValue()``.
- **isIdentity.** Return ``true`` only where ``render()`` would copy the
  input exactly: test the same conditions as ``render()``, through the
  same helper.
- **Coordinates.** Regions of definition and of interest are in canonical
  coordinates; the render window, image bounds and ``getPixelAddress()``
  are in pixels (see :ref:`CoordinateSystems`). The render scale is not
  always 1: multiply sizes and positions given in canonical units by it.
- **Components.** ``Clip::getPixelComponents()`` is what the host mapped
  the clip to, one of the values the plug-in declared: choose the render
  code from it. ``getUnmappedPixelComponents()`` is what the input really
  has.
- **Host capabilities.** ``OFX::getImageEffectHostDescription()`` tells
  ``describe()`` what the host has (``supportsMultipleClipDepths``,
  ``supportsDrawSuite``, ...). An OFX 1.4 host lacks the 1.5 suites and
  properties, and the optional suites (message V2, progress, time line)
  may be missing: their calls then do nothing, return a failure or a
  default (``timeLineGetTime()`` returns 0). If the host lacks something
  the plug-in can't do without, throw
  ``OFX::Exception::HostInadequate`` in ``describe()``: the host gets
  :c:macro:`kOfxStatErrMissingHostFeature` and ignores the plug-in. Some
  hosts also ignore parts of the specification, for example
  :c:macro:`kOfxParamPropDefaultCoordinateSystem`, so a normalised default
  arrives as is (``supportsDefaultCoordinateSystem()`` on the parameter
  descriptor tells).

Building a Plug-in
==================

With CMake
----------

``cmake/OpenFX.cmake`` provides ``add_ofx_plugin()``, which makes a
loadable module named ``<name>.ofx``, assembles the bundle
(``<name>.ofx.bundle/Contents/Info.plist`` and
``Contents/<architecture>/<name>.ofx``) in the build tree, and installs
it. It also exports only ``OfxGetPlugin`` and ``OfxGetNumberOfPlugins``,
and sets the rpaths. A plug-in project uses openfx as an installed
package (``find_package(openfx)``) or as a subdirectory:

.. code:: cmake

   cmake_minimum_required(VERSION 3.21)
   set(CMAKE_OSX_DEPLOYMENT_TARGET 14.0 CACHE STRING "Minimum macOS version") # optional, before project()
   project(MyPlugins CXX)
   add_subdirectory(openfx EXCLUDE_FROM_ALL) # or: find_package(openfx REQUIRED)

   add_ofx_plugin(MyInvert
     IDENTIFIER com.example.MyInvert # the bundle identifier, in Info.plist
     VERSION 1.0.0
     SOURCES invert.cpp)
   target_link_libraries(MyInvert PRIVATE openfx::Support)
   # icons, named after the plug-in identifier given to the factory
   ofx_plugin_resources(MyInvert com.example.myInvert.png com.example.myInvert.svg)
   ofx_deploy_bundle(MyInvert)       # macOS, with OFX_DEPLOY_BUNDLES=ON

A plug-in that uses only the C API links ``openfx::Api`` instead.

- ``ofx_plugin_resources()`` copies files into ``Contents/Resources``,
  where hosts look for the icons (:doc:`../Reference/ofxPackaging`).
- ``ofx_deploy_bundle()`` makes the installed macOS bundle
  self-contained. ``CMAKE_OSX_DEPLOYMENT_TARGET``, if set, goes before
  ``project()``; ``ofx_deploy_bundle()`` uses it to warn about bundled
  libraries that need a newer macOS.
- A project with many plug-ins uses ``ofx_plugin_bundle_group()``, which
  builds one combined bundle or one bundle per plug-in.

Each function, and the other options of ``add_ofx_plugin()``
(``BUNDLE_NAME``, ``INFO_PLIST``, ``EXPORT_SYMBOLS``, ``RPATH``), is
documented in the comment above it in
`cmake/OpenFX.cmake <https://github.com/NatronGitHub/openfx/blob/master/cmake/OpenFX.cmake>`__.
``Support/Plugins/CMakeLists.txt`` builds the samples this way.

With Make
---------

Each sample has a short Makefile that names its objects and includes
``Support/Plugins/Makefile.master``:

.. literalinclude:: ../../../Support/Plugins/Invert/Makefile
   :language: make

``Makefile.master`` compiles the Support sources into the plug-in
(``SUPPORTOBJECTS``) and builds the bundle, with the directory's
``Info.plist`` and the files listed in ``RESOURCES``; ``CONFIG`` is
``debug``, ``release`` or ``relwithdebinfo``. For example,
``make -C Support/Plugins CONFIG=release`` builds all the samples.

.. _supportRegistration:

Registering the Plug-ins
========================

A plug-in factory is registered with a static object, by
``mRegisterPluginFactoryInstance``:

.. code:: c++

   static MyPluginFactory p("com.example.myPlugin", 1, 0);
   mRegisterPluginFactoryInstance(p)

The arguments are the plug-in identifier and its major and minor
versions. One binary may register several factories, from one or more
source files: the
`MultiBundle <https://github.com/NatronGitHub/openfx/tree/master/Support/Plugins/MultiBundle>`__
sample has two plug-ins, one per source file. Two factories with the
same identifier and version are an error. Put each file's classes in an
unnamed namespace (SupportExt, used by openfx-misc, openfx-io and
openfx-arena, has the ``OFXS_NAMESPACE_ANONYMOUS_ENTER`` and
``OFXS_NAMESPACE_ANONYMOUS_EXIT`` macros): two classes with the same name
in two files of one binary, such as a copied processor, are a
one-definition-rule violation that the linker doesn't report.

The registration happens during static initialization, before the host
calls ``OfxGetNumberOfPlugins``. Plug-ins that are only known at run time
(for example, one per filter of a library that must be loaded first)
register lazily instead: a function
``void registerMyPlugins(OFX::PluginFactoryArray &factories)`` appends
their factories, and one static object,
``static OFX::PluginFactories lazy(&registerMyPlugins);``, hands it to the
library. The Support library calls the function once, at the first
``OfxGetNumberOfPlugins`` or ``OfxGetPlugin``, after the static
registrations; the factories must live until the binary is unloaded (see
``OFX::PluginFactories`` in ``ofxsImageEffect.h``). The older way,
compiling the Support library with ``OFXS_MANUAL_REGISTRATION`` and
defining ``OFX::Plugin::getPluginIDs()``, still works. A binary has one
lazy registration function at most.

Versions
--------

Increment the major version when "a setup from an earlier version, when
loaded into this version, will not yield the same result", and the minor
version otherwise (:ref:`OfxPlugin`). A host creates new instances with
the newest version, and loads a project with "the plug-in with the major
version that matches the setup, but has the greatest minor number". So,
to change a plug-in's output, keep the old major version in the binary.

Each factory object must be of its own class: the identifier and version
that the dispatcher uses are a static member of the factory class, so two
objects of one class collide. ``mDeclarePluginFactoryVersioned`` declares
a class template on the major version; its constructor takes the
identifier and the minor version:

.. code:: c++

   mDeclarePluginFactoryVersioned(MyPluginFactory, {}, {});

   template <unsigned int majorVersion>
   void MyPluginFactory<majorVersion>::describe(OFX::ImageEffectDescriptor &desc)
   {
     // ...
     if (majorVersion < kPluginVersionMajor) {
       desc.setIsObsolete(true); // hidden from new instances, still loads old projects
     }
   }

   template <unsigned int majorVersion>
   OFX::ImageEffect* MyPluginFactory<majorVersion>::createInstance(OfxImageEffectHandle handle, OFX::ContextEnum)
   {
     return new MyPlugin(handle, majorVersion); // renders as that version did
   }

   static MyPluginFactory<kPluginVersionMajor> p(kPluginIdentifier, kPluginVersionMinor);
   static MyPluginFactory<1> p1(kPluginIdentifier, 2); // the last 1.x
   mRegisterPluginFactoryInstance(p)
   mRegisterPluginFactoryInstance(p1)

Inside the template, reach the base class members through ``this->``.

.. _supportConfiguration:

Configuration Macros
====================

These compile definitions select what the Support library supports:

- ``OFX_EXTENSIONS_NUKE``, ``OFX_EXTENSIONS_NATRON``,
  ``OFX_EXTENSIONS_VEGAS``, ``OFX_EXTENSIONS_TUTTLE`` and
  ``OFX_EXTENSIONS_RESOLVE``: the host extensions (``include/nuke/``,
  ``include/ofxNatron.h``, ...). ``OFX_EXTENSIONS_NATRON`` needs
  ``OFX_EXTENSIONS_NUKE``.
- ``OFX_SUPPORTS_OPENGLRENDER`` (OpenGL rendering) and
  ``OFX_SUPPORTS_DIALOG`` (the dialog suite; off by default, and also a
  HostSupport option, for a host that implements that suite).

They change class layouts and virtual function signatures. For example,
with ``OFX_EXTENSIONS_NUKE``, ``ImageEffect::isIdentity()`` has two more
arguments, a view and a plane. So **every source file of one plug-in
binary, including the Support sources compiled into it, must be compiled
with the same set**, and with the same C++ standard (``OFX::auto_ptr`` is
``std::unique_ptr`` or ``std::auto_ptr`` depending on it;
``OFX_CXX_STANDARD`` with CMake). A mismatch is not detected at build time: it is a
silent violation of the one-definition rule, and the plug-in may crash.
Different binaries may use different sets: a plug-in and the host that
loads it negotiate the extensions at run time, through properties and
suites.

- With CMake, set them only through openfx's options of the same names
  (and ``OFX_EXTRA_DEFINITIONS``): ``openfx::Support`` passes them on to
  the plug-ins that link it. Don't add or remove one on a single target
  or file. The defaults are in the top-level
  `README.md <https://github.com/NatronGitHub/openfx/blob/master/README.md>`__
  (Building).
- The Makefiles define none of them. Pass them in ``CXXFLAGS_EXTRA``,
  which ``Support/Plugins/Makefile.master`` uses for both the plug-in and
  the Support sources.

.. _supportWhatItDoes:

What the Support Library Does
=============================

The library is not a shared library: its sources (``Support/Library``)
are compiled into each plug-in binary, as the static library
``openfx::Support`` with CMake, or as objects of the plug-in with the
Makefiles. It provides:

- **The binary's entry points.** ``OfxGetNumberOfPlugins`` and
  ``OfxGetPlugin`` are defined in ``ofxsImageEffect.cpp``. They return
  one ``OfxPlugin`` struct per registered plug-in factory, with its
  identifier, version, ``setHost`` function and main entry point. A C
  entry point must not let an exception reach the host: both catch
  everything and log it.

- **The action dispatcher.** Each factory's main entry point calls
  ``OFX::Private::mainEntryStr``, which maps the handle to the
  ``OFX::ImageEffect`` object of that instance, wraps ``inArgs`` and
  ``outArgs``, and calls the matching member function (table below).
  It turns the result into an ``OfxStatus``:

  - a function that returns ``false`` (it did nothing) gives
    :c:macro:`kOfxStatReplyDefault`, so the host applies the default
    behaviour;
  - a thrown ``OFX::Exception::Suite`` gives its status;
  - ``OFX::Exception::HostInadequate`` and
    ``OFX::Exception::PropertyUnknownToHost`` give
    :c:macro:`kOfxStatErrMissingHostFeature`;
  - ``std::bad_alloc`` gives :c:macro:`kOfxStatErrMemory`;
  - a plug-in may map its own exception type to a status, by defining
    ``OFX_CLIENT_EXCEPTION_TYPE`` and ``OFX_CLIENT_EXCEPTION_HANDLER``
    (and ``OFX_CLIENT_EXCEPTION_HEADER``, the header that declares them),
    for the Support sources too (``OFX_EXTRA_DEFINITIONS`` with CMake);
  - any other exception gives :c:macro:`kOfxStatFailed`.

- **The suites.** The host's suites are fetched once, at
  :c:macro:`kOfxActionLoad`. Most suite calls that fail throw an
  exception, so plug-in code rarely checks statuses. The exceptions:
  ``Clip::fetchImage()`` returns ``NULL`` when the host has no image, and
  the message calls return a reply instead of throwing.

- **Property validation, in debug builds.** When the Support sources are
  compiled with ``DEBUG``, ``ofxsPropertyValidation.cpp`` checks the
  host's properties against the specification, and the library logs to
  ``ofxTestLog.txt`` in the current directory, or to the file named by
  the ``OFX_PLUGIN_LOGFILE`` environment variable. The Makefiles'
  ``CONFIG=debug`` does this. With CMake, in Debug builds,
  ``add_ofx_plugin()`` defines ``DEBUG`` for the plug-in; with
  ``add_subdirectory(openfx)``, openfx defines it for ``openfx::Support``
  too; an installed ``openfx::Support`` keeps the configuration it was
  built with.

The actions and the functions they call:

.. list-table::
   :header-rows: 1
   :widths: 50 50

   * - Action
     - Support function
   * - :c:macro:`kOfxActionLoad`, :c:macro:`kOfxActionUnload`
     - ``PluginFactory::load()``, ``unload()``
   * - :c:macro:`kOfxActionDescribe`
     - ``PluginFactory::describe()``
   * - :c:macro:`kOfxImageEffectActionDescribeInContext`
     - ``PluginFactory::describeInContext()``
   * - :c:macro:`kOfxActionCreateInstance`
     - ``PluginFactory::createInstance()``, which creates the
       ``ImageEffect`` subclass
   * - :c:macro:`kOfxActionDestroyInstance`
     - the ``ImageEffect`` destructor
   * - :c:macro:`kOfxImageEffectActionGetClipPreferences`
     - ``ImageEffect::getClipPreferences()``
   * - :c:macro:`kOfxImageEffectActionGetRegionOfDefinition`
     - ``ImageEffect::getRegionOfDefinition()``
   * - :c:macro:`kOfxImageEffectActionGetRegionsOfInterest`
     - ``ImageEffect::getRegionsOfInterest()``
   * - :c:macro:`kOfxImageEffectActionGetFramesNeeded`
     - ``ImageEffect::getFramesNeeded()``
   * - :c:macro:`kOfxImageEffectActionGetTimeDomain`
     - ``ImageEffect::getTimeDomain()``
   * - :c:macro:`kOfxImageEffectActionIsIdentity`
     - ``ImageEffect::isIdentity()``
   * - :c:macro:`kOfxImageEffectActionBeginSequenceRender`,
       :c:macro:`kOfxImageEffectActionRender`,
       :c:macro:`kOfxImageEffectActionEndSequenceRender`
     - ``ImageEffect::beginSequenceRender()``, ``render()``,
       ``endSequenceRender()``
   * - :c:macro:`kOfxActionInstanceChanged` (and its begin and end actions)
     - ``ImageEffect::changedParam()`` or ``changedClip()``
       (``beginChanged()``, ``endChanged()``)
   * - :c:macro:`kOfxActionBeginInstanceEdit`,
       :c:macro:`kOfxActionEndInstanceEdit`
     - ``ImageEffect::beginEdit()``, ``endEdit()``
   * - :c:macro:`kOfxActionPurgeCaches`, :c:macro:`kOfxActionSyncPrivateData`
     - ``ImageEffect::purgeCaches()``, ``syncPrivateData()``
   * - :c:macro:`kOfxImageEffectActionGetOutputColourspace`
     - ``ImageEffect::getOutputColourspace()``

``render()`` is the only pure virtual function of ``ImageEffect``. The
others have defaults that do nothing, or return ``false``, which gives
the host's default behaviour. The extension actions (Nuke, Natron, VEGAS,
...) and the OpenGL context actions have their own member
functions, compiled in with the macros of :ref:`supportConfiguration`.

The Main Classes
================

They are declared in ``Support/include``: ``ofxsImageEffect.h``,
``ofxsParam.h``, ``ofxsInteract.h``, ``ofxsCore.h`` and
``ofxsMultiThread.h``.

``OFX::PluginFactory``
   One per plug-in version. The
   ``mDeclarePluginFactory(CLASS, LOADFUNCDEF, UNLOADFUNCDEF)`` macro
   declares a factory class, and you define its ``describe()``,
   ``describeInContext()`` and ``createInstance()``. The last two macro
   arguments are the bodies of ``load()`` and ``unload()``, usually
   ``{}``. ``mDeclarePluginFactoryVersioned`` declares a class template on
   the major version (see :ref:`supportRegistration`). A factory class
   with other template parameters, or a project's own factory macro,
   derives from ``OFX::PluginFactoryHelper<itself>``.

``OFX::ImageEffectDescriptor``
   Passed to ``describe()`` and ``describeInContext()``. Its setters write
   the effect descriptor's properties (``setLabels()``,
   ``addSupportedContext()``, ``addSupportedBitDepth()``,
   ``setSupportsTiles()``, ...). It defines the clips (``defineClip()``,
   which returns an ``OFX::ClipDescriptor``) and the parameters
   (``defineDoubleParam()``, ``defineBooleanParam()``, ...), and registers
   an overlay (``setOverlayInteractDescriptorV2()``).

``OFX::ImageEffect``
   One object per instance, created by ``createInstance()`` and deleted at
   :c:macro:`kOfxActionDestroyInstance`. Its constructor usually fetches the
   clips and parameters it uses (``fetchClip()``, ``fetchDoubleParam()``,
   ...); the effect owns these objects. It overrides the actions it
   handles, and has the instance's own setters, such as
   ``setSupportsTiles()``.

``OFX::Clip`` and ``OFX::Image``
   ``Clip`` gives a clip's preferences (``getPixelDepth()``,
   ``getPixelComponents()``, ...) and fetches images with
   ``fetchImage(time)``. The caller owns the returned ``Image``: hold it
   in an ``OFX::auto_ptr`` (``std::unique_ptr`` from C++11), which releases
   it to the host. ``Image`` gives the bounds, depth, components, row
   bytes and ``getPixelAddress(x, y)``.

The parameter classes
   One class per parameter type, such as ``OFX::DoubleParam``,
   ``OFX::BooleanParam`` and ``OFX::ChoiceParam``, each with its
   ``*ParamDescriptor``. A parameter gives its value with ``getValue()``
   or ``getValueAtTime(time, ...)``. ``ofxsParam.h`` and the Doxygen
   reference list them all.

``OFX::OverlayInteract``
   An overlay drawn in the host's viewer. Derive from it and override
   ``draw()``, ``penDown()``, ``penMotion()``, ``keyDown()``, ...; register
   it in ``describe()`` with a descriptor made by
   ``OFX::DefaultEffectOverlayDescriptor`` (the effect descriptor takes
   ownership of it). There are three ways to register it:

   - ``setOverlayInteractDescriptorV2(desc)``: the overlay is called
     through the DrawSuite (OFX 1.5) on a host that has it, and through
     OpenGL otherwise. ``draw()`` draws only through
     ``args.getDrawContext()``, which turns the same calls into OpenGL
     on a host without the DrawSuite.
   - ``setOverlayInteractDescriptorV2(desc, true)``: for an overlay that
     keeps its own OpenGL code. On a DrawSuite host it is registered both
     ways, and the host may call either: draw through
     ``getDrawContext()`` when ``args.calledViaOverlayInteractV2()`` is
     true, and with OpenGL otherwise (after checking
     ``args.isOpenGLContextCurrent()``).
   - ``setOverlayInteractDescriptor(desc)``: OpenGL only.

   The
   `Basic <https://github.com/NatronGitHub/openfx/blob/master/Support/Plugins/Basic/basic.cpp>`__
   sample uses the first:

   .. literalinclude:: ../../../Support/Plugins/Basic/basic.cpp
      :language: c++
      :start-at: class BasicExampleOverlayDescriptor
      :end-at: class BasicExampleOverlayDescriptor

   .. literalinclude:: ../../../Support/Plugins/Basic/basic.cpp
      :language: c++
      :start-at: // OverlayInteractV2 (DrawSuite) if the host has the DrawSuite
      :end-at: desc.setOverlayInteractDescriptorV2(
      :dedent: 2

The library throws exceptions of namespace ``OFX::Exception``. A plug-in
may throw too: ``OFX::throwSuiteStatusException(status)`` makes the
action return ``status``.

The headers ``ofxsProcessing.H`` and ``ofxsImageBlender.H`` in
``Support/Plugins/include`` are not part of the library. They are helpers
of the samples: ``OFX::ImageProcessor`` splits a render window between
threads with ``OFX::MultiThread::Processor``. The
`openfx-supportext <https://github.com/NatronGitHub/openfx-supportext>`__
repository has many more helpers (pixel processors, overlays, macros),
used by openfx-misc, openfx-io and openfx-arena.
