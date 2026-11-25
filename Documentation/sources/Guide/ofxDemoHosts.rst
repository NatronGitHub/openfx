.. SPDX-License-Identifier: CC-BY-4.0
.. _demoHostsGuide:

============================================
The Demo Hosts: hostDemo and the Python Host
============================================

openfx ships two sample hosts built on the HostSupport library
(``HostSupport/``, the ``ofxh*`` classes):

- **hostDemo** (``HostSupport/examples/hostDemo*``), a C++ command-line
  host. It loads one plug-in, creates one instance on generated or PFM
  inputs, renders a few frames and writes the output images, with an
  optional JSON report of every action. The plug-in repositories' CI
  runs their plug-ins under it.
- **the Python host**: the HostSupport Python bindings (the ``ofxh``
  module, ``HostSupport/python``), and the ``ofxhost`` package on top of
  them (``HostSupport/examples/python``). ``ofxhost`` renders plug-ins
  from Python tests, either through hostDemo or in-process, and has a
  sample host that runs a graph of plug-in instances, described in JSON,
  over a frame sequence.

Use hostDemo to check that a plug-in loads, describes itself and renders
in a simple, predictable host, and to compare its output with reference
images in CI. hostDemo is frozen: it gets bug fixes and test hooks only,
no new features. Use the Python host for what hostDemo can't do (for
example, inputs with RGB or Alpha components, plug-ins connected in a
graph, image sequences, reader and writer plug-ins), and as the starting
point of a new host written in Python.

Both are also example code for host authors: they show which HostSupport
classes a host derives from, and the order in which a host calls the
actions.

The reference for the file formats, the input specifications, the JSON
documents and the ``ofxhost`` API is
`HostSupport/examples/python/README.md
<https://github.com/NatronGitHub/openfx/blob/master/HostSupport/examples/python/README.md>`__.
This chapter is an overview, and points to the sections of that file.


Building the Demo Hosts
=======================

With CMake, two options of the top-level ``CMakeLists.txt`` build them
(both turn on ``OFX_BUILD_HOSTSUPPORT``):

- ``BUILD_EXAMPLE_HOSTS`` builds hostDemo and the other example programs
  of ``HostSupport/examples``: ``cacheDemo``, ``propsetDump`` and
  ``suiteHandleChecks``. They are skipped when ``OFX_SUPPORTS_MULTITHREAD``
  is on, because they don't implement the multi-thread suite functions
  that option makes pure virtual.
- ``OFX_PYTHON_BINDINGS`` builds the ``ofxh`` module. It needs Python 3.10
  or later and `nanobind <https://github.com/wjakob/nanobind>`__ (found
  through ``python -m nanobind --cmake_dir``, or ``nanobind_DIR``).
  ``ofxhost`` and ``ofxh`` also need NumPy, and the tests pytest.

.. code-block:: sh

   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
       -DBUILD_EXAMPLE_PLUGINS=ON -DBUILD_EXAMPLE_HOSTS=ON -DOFX_PYTHON_BINDINGS=ON
   cmake --build build -j4

hostDemo is then ``build/HostSupport/examples/hostDemo``, the module is in
``build/HostSupport/python`` (put that directory in ``PYTHONPATH``), and
the sample plug-ins are in ``build/Plugins``.

The Makefiles build hostDemo too, but not the ``ofxh`` module:
``make -C HostSupport`` builds the library, then
``make -C HostSupport/examples`` builds the example hosts into
``HostSupport/examples/<OS>-release`` (``<OS>-debug`` with
``DEBUG=true``). Both builds use the system's expat.

The ``ofxh`` module can also be installed on its own, as a Python package,
on macOS and Linux:

.. code-block:: sh

   pip install ./HostSupport/python

This runs CMake through scikit-build-core, and builds only HostSupport and
the module. It needs a C++17 compiler, CMake 3.21 or later and the
system's expat. ``HostSupport/python/pyproject.toml`` documents the
options (for example, ``--no-build-isolation`` to use the nanobind and
scikit-build-core already installed, and ``-C cmake.define.<option>=...``
to change an openfx option).


hostDemo
========

Running a Plug-in
-----------------

hostDemo looks for plug-ins in ``OFX_PLUGIN_PATH`` and in the standard
plug-in locations (``--no-std-paths`` skips those). By default it renders
the Support library's Invert example, ``net.sf.openfx.invertPlugin``, in
the filter context, at times 0 and 1, on a PAL SD project, and writes
``Output.0.ppm`` and ``Output.1.ppm``. Its options choose the plug-in and
context, the inputs, the parameter values and keys, the frames, the
render scale and window, and the output format. ``hostDemo -h`` prints
them:

`hostDemo.cpp <https://github.com/NatronGitHub/openfx/blob/master/HostSupport/examples/hostDemo.cpp>`__

.. literalinclude:: ../../../HostSupport/examples/hostDemo.cpp
   :language: c++
   :start-at: "Usage: "
   :end-at: Exit status: 0 on success

For example, to render the Basic example in the general context, with a
key on its ``scale`` parameter, as float PFM files, and to write a report:

.. code-block:: sh

   OFX_PLUGIN_PATH=build/Plugins build/HostSupport/examples/hostDemo \
       -p net.sf.openfx.basicPlugin -c general -i Source=ramp -i Mask=none \
       -s scale@0=1 -s scale@1=2 --format pfm --report report.json

Parameters given with ``-s`` are set after the create instance action, as
a user would set them, each followed by an instance changed action. A
host that loads a saved project would instead set them before the create
instance action, with no instance changed action: the graph host does
that (see below).

The exit status is 0 if every action succeeded. The output image is
filled with NaN (float) or ``0xCD`` bytes before each render, and a float
render that leaves NaN pixels fails, unless ``--allow-unwritten`` is
given.

The JSON Documents
------------------

- ``--report file.json`` writes the report of the run, even if it failed:
  the plug-in, the inputs, every action and its status, the clip
  preferences, the time domain, and for each time and view the region of
  definition, isIdentity, the frames needed, the regions of interest, the
  render, and statistics and a CRC of the output image. It ends with the
  parameter values, the messages and the progress calls of the plug-in.
- ``--describe file.json`` describes the plug-in in all its contexts,
  without creating an instance: its effect flags, clips and parameters.
  It can't be used with ``--report``.

Both have a ``schema`` and a ``schemaVersion``. Their contents are
documented in the
`hostDemo JSON <https://github.com/NatronGitHub/openfx/blob/master/HostSupport/examples/python/README.md#hostdemo-json>`__
section of the README, and the input specifications and output files in
`hostDemo file formats <https://github.com/NatronGitHub/openfx/blob/master/HostSupport/examples/python/README.md#hostdemo-file-formats>`__.

An OFX 1.4 Host: ``--ofx14``
----------------------------

Plug-ins should still work in hosts that only implement OFX 1.4. With
``--ofx14``, hostDemo behaves as such a host: the host descriptor has
only the OFX 1.4 host properties, ``fetchSuite`` returns only the OFX 1.4
suites, ``OfxSetHost`` is not called, StrChoice and Bytes parameters
can't be defined, and one view is rendered. It uses its own plug-in cache
file, ``hostDemoPluginCache-ofx14.xml``, because a plug-in may describe
itself differently to an OFX 1.4 host. The report's ``host.profile`` is
``"ofx14"``.

An OFX 1.1 Host: ``--ofx11``
----------------------------

Some hosts still implement OFX 1.1: VEGAS Pro, for one (openfx-misc's
``README-hosts.txt``). With ``--ofx11``, hostDemo behaves as such a host:
the host descriptor has only the OFX 1.1 host properties, and API version
1.1; it has no tiles, no multi-resolution, no overlays, byte and float
depths only (``-d short`` is an error), and a single clip pixel aspect
ratio. ``fetchSuite`` returns the OFX 1.1 suites but the interact suite,
which VEGAS lacks, plus the message suite V2, which VEGAS Pro 12 lists; it
refuses persistent messages, as DaVinci Resolve and Fusion 2026 do.
``OfxSetHost`` is not called. The double
parameters have no :c:macro:`kOfxParamPropDefaultCoordinateSystem` (OFX
1.2), and the plug-in descriptors and instances no
:c:macro:`kOfxImageEffectInstancePropSequentialRender`, which other hosts
lack on the descriptor. One view is rendered. It uses its own plug-in
cache file, ``hostDemoPluginCache-ofx11.xml``, and the report's
``host.profile`` is ``"ofx11"``. The Python host's in-process backend has
no such profile.

The Code, as an Example Host
----------------------------

hostDemo's classes are in the ``MyHost`` namespace, one pair of files
each:

- ``MyHost::Host`` (``hostDemoHostDescriptor.h``) derives from
  ``OFX::Host::ImageEffect::Host``. It describes the host, makes the
  effect instances (``newInstance``) and descriptors
  (``makeDescriptor``), and shows the plug-ins' messages.
  ``restrictToOFX14()`` gives ``--ofx14``, and ``restrictToOFX11()``
  ``--ofx11``.
- ``MyHost::MyEffectInstance`` (``hostDemoEffectInstance.h``) derives from
  ``OFX::Host::ImageEffect::Instance``: the project (size, pixel aspect
  ratio, frame rate), the timeline, the messages and progress, and the
  factories of the clips and parameters.
- ``MyHost::MyClipInstance`` and ``MyHost::MyImage``
  (``hostDemoClipInstance.h``) derive from
  ``OFX::Host::ImageEffect::ClipInstance`` and
  ``OFX::Host::ImageEffect::Image``: the inputs' regions of definition and
  images, and the output image of a render.
- ``MyHost::MyDoubleInstance``, ``MyHost::MyChoiceInstance`` and the other
  ``My*Instance`` classes (``hostDemoParamInstance.h``) derive from the
  ``OFX::Host::Param::*Instance`` classes, and store each parameter's
  value and keys.

The test programs ``propsetDump`` and ``suiteHandleChecks`` reuse these
classes, compiled from the same files (see
``HostSupport/examples/CMakeLists.txt``): a C++ test host that needs a
working host and effect instance can do the same, and only add its
checks.

``hostDemo.cpp`` has the main program: it sets up the plug-in cache,
creates one instance and calls the actions of a render in the order
described in :ref:`hostSupportGuide`, the chapter on writing a host with
HostSupport. Running hostDemo in a debugger, with a breakpoint in a
plug-in's action, is a quick way to see the whole sequence.


The Python Host
===============

The ``ofxh`` Module
-------------------

``ofxh`` exposes HostSupport to Python: a host is a set of subclasses of
``ofxh.Host``, ``ofxh.ImageEffectInstance``, ``ofxh.ClipInstance`` and,
optionally, ``ofxh.Param``, that override the callbacks the host needs.
Images are NumPy arrays. ``ofxh.init()`` loads the plug-ins (there is one
host per process), ``ofxh.get_plugin()`` finds one,
``ofxh.create_instance()`` makes an instance, and the instance's methods
call the actions.

``HostSupport/examples/python/minimal_host.py`` is the smallest complete
host on ``ofxh``: it renders one frame of a filter on one input image. A
host overrides the message callback of ``ofxh.Host``:

.. literalinclude:: ../../../HostSupport/examples/python/minimal_host.py
   :language: python
   :pyobject: Host

a clip gives its image and region of definition to the plug-in (the
output keeps the defaults):

.. literalinclude:: ../../../HostSupport/examples/python/minimal_host.py
   :language: python
   :pyobject: Clip

the instance holds the project and makes the clips:

.. literalinclude:: ../../../HostSupport/examples/python/minimal_host.py
   :language: python
   :pyobject: Instance

and ``render()`` calls the actions of one render, in order:

.. literalinclude:: ../../../HostSupport/examples/python/minimal_host.py
   :language: python
   :pyobject: render

Run it on a NumPy image (``.npy``, bottom-up rows, as OFX images):

.. code-block:: sh

   PYTHONPATH=build/HostSupport/python python HostSupport/examples/python/minimal_host.py \
       build/Plugins net.sf.openfx.invertPlugin in.npy out.npy

Rendering from Tests: ``ofxhost.render()``
------------------------------------------

``ofxhost.render()`` renders a plug-in on NumPy arrays or test patterns,
with parameter values and keys, and returns the output images and the
hostDemo report. ``ofxhost.describe()`` returns the ``--describe`` JSON.
They have two backends, with the same results: ``"subprocess"`` (the
default) runs hostDemo in a new process for each call, so that a crashing
plug-in doesn't stop the tests; ``"inprocess"`` runs a port of hostDemo
on ``ofxh`` (``ofxhost.inprocess``), without files, and adds what hostDemo
can't do, such as RGB and Alpha inputs. The README's first section
documents them and their arguments. For example, with the CMake build
above, which puts the sample plug-ins in ``build/Plugins``:

.. code-block:: python

   import numpy as np, ofxhost
   src = np.random.default_rng(0).random((48, 64, 3), dtype=np.float32)
   r = ofxhost.render("build/Plugins", "net.sf.openfx.invertPlugin",
                      inputs={"Source": src}, depth="float")
   assert np.allclose(r.outputs[0][..., :3], 1 - src, atol=1e-6)

``ofxhost.testing`` has pytest fixtures that run a test on each backend.
``Support/Plugins/tests`` tests the sample plug-ins this way, and is a
template for the plug-in repositories' tests.

The Graph Host: ``ofxhost.graphhost``
-------------------------------------

``ofxhost.graph`` describes a graph of plug-in instances: each ``Node``
has a plug-in, a context, parameter values or keys, and inputs, which are
other nodes, image sequences or test patterns. A ``Graph`` names its
nodes and outputs, and holds the project and the render settings. It is
saved and loaded as JSON (``"schema": "ofxhost.graph"``). The format is
in the README's
`Graphs <https://github.com/NatronGitHub/openfx/blob/master/HostSupport/examples/python/README.md#graphs>`__
section; ``python -m ofxhost params PLUGIN -j`` writes a one-node graph
with the plug-in's defaults, to start from.

``ofxhost.graphhost.run()`` runs a graph in this process, with the images
in memory between the nodes, and writes the outputs' files. It returns a
``graphhost.Result``: ``ok`` (whether the run succeeded), ``report`` (the
JSON report, as a dict), ``images`` (the rendered images of each output
node, by time) and ``files`` (the files written). From the command line:

.. code-block:: sh

   PYTHONPATH=build/HostSupport/python:HostSupport/examples/python \
       python -m ofxhost -b build/Plugins render graph.json --frames 1-10

``graphhost.py`` is the template for a real host on ``ofxh``. It calls the
actions in a host's order, as Natron calls them: the project load, the
time domains, the sequence renders, the frames needed and regions of
interest of each input before a render, identities, image sequences and
file parameters. For example, a node's instance is set up as a host loads
a saved project, with the parameter values set before the create instance
action:

.. literalinclude:: ../../../HostSupport/examples/python/ofxhost/graphhost.py
   :language: python
   :pyobject: _Run.setup

What it does at each step, its limits (one view, single-threaded renders,
no interacts) and its command line are in the README's
`The sample host and its command line <https://github.com/NatronGitHub/openfx/blob/master/HostSupport/examples/python/README.md#the-sample-host-and-its-command-line>`__
section. A graph is like a script: it chooses the plug-ins that run and
the files they read and write, so only run the graphs you trust.

Running the Tests
-----------------

With the build above, the tests of both hosts and of the sample plug-ins
run with pytest. The ``ofxh`` bindings have their own tests, in
``HostSupport/python/tests``, run separately as CI does:

.. code-block:: sh

   OFX_TEST_PLUGIN_PATH=build/Plugins PYTHONPATH=build/HostSupport/python \
   OFXHOST_HOSTDEMO=build/HostSupport/examples/hostDemo \
       python3 -m pytest -q -rs HostSupport/examples/python/tests Support/Plugins/tests
   PYTHONPATH=build/HostSupport/python python3 -m pytest -q -rs HostSupport/python/tests

The README's
`Tests <https://github.com/NatronGitHub/openfx/blob/master/HostSupport/examples/python/README.md#tests>`__
section says what each test file covers.
