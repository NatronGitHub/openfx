// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxsDrawContextOpenGL.cpp
  OFX::DrawArgs::getDrawContext(), and the OpenGL backend of OFX::DrawContext: an emulation of the
  OFX 1.5 DrawSuite (include/ofxDrawSuite.h, OfxDrawSuiteV1) with OpenGL 1.x compatibility-profile
  calls, for the overlays registered as kOfxImageEffectPluginPropOverlayInteractV1. See the
  documentation of OFX::DrawContext in ofxsInteract.h.

  This is the only file of the Support library that calls OpenGL. ofxsInteract.cpp only calls this
  backend through virtual functions, so a plug-in that never calls getDrawContext() doesn't link
  this file from a static library (libOfxSupport.a, CMake's openfx::Support). With
  Support/Plugins/Makefile.master, whose SUPPORTOBJECTS are object files, every plug-in links it
  (and needs the OpenGL headers). The legacy project files (Support.xcodeproj, .vcproj, .dsp) are
  not maintained and don't list this file.

  It doesn't link OpenGL: it has no undefined OpenGL symbol, and loads the OpenGL 1.1 functions it
  uses at run time, the first time getDrawContext() needs OpenGL: in a draw action called through
  a kOfxImageEffectPluginPropOverlayInteractV1 entry, in the draw action of a parameter interact
  (always V1: there is no DrawSuite registration for them), or for DrawArgs not made by the
  dispatcher whose inArgs have no draw context. So a plug-in that uses it still loads on a
  machine or in a host without OpenGL (a render node, or a DrawSuite host without OpenGL:
  kOfxImageEffectPropSupportsOverlays is not tied to OpenGL).
  - Linux, FreeBSD and the other ELF systems: dlsym(RTLD_DEFAULT) if it finds all the functions
    (OpenGL already loaded globally), else dlopen("libGL.so.1"), "libGL.so", then GLVND's
    "libOpenGL.so.0" (the library the host loaded is shared, dlopen doesn't load a second copy).
    Linux before glibc 2.34 needs -ldl (Makefile.master links it; openfx::Support links
    CMAKE_DL_LIBS).
  - macOS: dlsym(RTLD_DEFAULT), else dlopen of /System/Library/Frameworks/OpenGL.framework/OpenGL.
  - Windows: GetProcAddress on opengl32.dll (already loaded by the host, else LoadLibraryEx from
    System32 only, against DLL planting): the OpenGL 1.1 functions are exported by opengl32.dll,
    wglGetProcAddress doesn't return them.
  If they can't all be loaded, getDrawContext() doesn't use OpenGL. The result is computed once
  (thread-safe in C++11 and later).

  It also checks that an OpenGL context is current before using OpenGL, since legacy OpenGL calls
  without one usually crash (macOS), and a host may have the DrawSuite and no OpenGL. The spec's
  precondition of kOfxInteractActionDraw, "the openGL context for this interact has been set", is
  unconditional, but hosts that call V2 overlays with the DrawSuite don't necessarily honour it.
  The check uses, loaded at run time like the rest: wglGetCurrentContext (Windows),
  CGLGetCurrentContext (macOS), glXGetCurrentContext and eglGetCurrentContext (ELF systems, from
  the libraries that are already loaded: GLX, EGL or GLVND), and on ELF systems
  OSMesaGetCurrentContext, only if it is in the same library as the gl* functions (not a
  plug-in-private OSMesa). OpenGL is current if any of them returns a context; if none can be
  loaded, it isn't. The list of these functions is looked up again as long as it is empty (the
  host may load libEGL or libGLX later).
  Without a current OpenGL context, getDrawContext() uses the host's draw context if it passed one
  (DrawSuite through V1 is allowed: "Some existing plugins may use DrawSuite via
  kOfxImageEffectPluginPropOverlayInteractV1 if it's supported by the host, but this is
  discouraged"), else it returns a context that draws nothing (canDraw() is false, every function
  returns false).

  Define OFXS_DRAWCONTEXT_NO_OPENGL to build without OpenGL (no OpenGL header, no OpenGL call):
  getDrawContext() then returns a context that draws nothing for the V1 overlays.

  Spec references (upstream include/ofxDrawSuite.h, include/ofxInteract.h and
  Documentation/sources/Reference/ofxInteracts.rst, OFX 1.5):
  - coordinates: "The GL_PROJECTION matrix will be set up so that it maps openGL coordinates to
    canonical image coordinates. The GL_MODELVIEW matrix will be the identity matrix."
    (ofxInteracts.rst, OpenGL overlays). The DrawSuite doesn't say it, but its points are in the
    same canonical coordinates (the upstream DrawSuite example draws its cross hair at the point
    parameter's canonical position, sized with kOfxInteractPropPixelScale). This backend uses the
    current OpenGL matrices, so the points are canonical coordinates.
  - kOfxInteractPropPixelScale: "The size of a real screen pixel under the interact's canonical
    projection." It is used for the sizes that are in screen pixels (text, ellipse tessellation).
  - setColour: "Sets the colour for future drawing operations (lines, filled shapes and text)
    [...] The host should use "over" compositing when using a non-opaque colour."
  - setLineWidth: "Use width 0 for a single pixel line or non-zero for a smooth line of the
    desired width. The host should adjust for screen density."
  - draw: "kOfxDrawPrimitiveLines - like GL_LINES, n points draws n/2 separated lines;
    kOfxDrawPrimitiveLineStrip - like GL_LINE_STRIP, n points draws n-1 connected lines;
    kOfxDrawPrimitiveLineLoop - like GL_LINE_LOOP, n points draws n connected lines;
    kOfxDrawPrimitiveRectangle - draws an axis-aligned filled rectangle defined by 2 opposite
    corner points; kOfxDrawPrimitivePolygon - like GL_POLYGON, draws a filled n-sided polygon;
    kOfxDrawPrimitiveEllipse - draws a axis-aligned elliptical line (not filled) within the
    rectangle defined by 2 opposite corner points". Returns "kOfxStatErrValue - invalid
    primitive, or point_count not valid for primitive".
  - drawText: "The text font face and size are determined by the host." Returns
    "kOfxStatErrValue - text or pos were not defined".
  - OfxDrawTextAlignment: Left 0x1, Right 0x2, Top 0x4, Bottom 0x8, Baseline 0x10,
    CenterH = (Left | Right), CenterV = (Top | Baseline). CenterV is Top|Baseline, not
    Top|Bottom: this backend centres the text vertically when Top is set with Bottom or Baseline.
  - getColour: "Retrieves the host's desired draw colour for" an OfxStandardColour; the spec
    gives no values. This backend returns the fixed colours of kStandardColours below.
 */

#include "ofxsSupportPrivate.h"

#ifndef OFXS_DRAWCONTEXT_NO_OPENGL

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h> // for WINGDIAPI and APIENTRY, used by GL/gl.h
#endif

#ifdef __APPLE__
#ifndef GL_SILENCE_DEPRECATION
#define GL_SILENCE_DEPRECATION // OpenGL 1.x compatibility profile, as every OpenGL overlay
#endif
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif
// only for the types and constants: the functions are loaded at run time (see above)

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#define OFXS_GL_APIENTRY APIENTRY // the calling convention of opengl32.dll (__stdcall on x86)
#else
#include <dlfcn.h>
#define OFXS_GL_APIENTRY
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <string>
#if (__cplusplus >= 201103L || (defined(_MSVC_LANG) && _MSVC_LANG >= 201103L))
#define OFXS_DRAWCONTEXT_CXX11 1
#include <mutex>
#endif

#include "ofxsFont5x7.h"

#endif // !OFXS_DRAWCONTEXT_NO_OPENGL

namespace OFX {

  namespace Private {

#ifndef OFXS_DRAWCONTEXT_NO_OPENGL

    namespace {

      // The OpenGL 1.1 functions that the backend calls, loaded at run time
      struct GLFunctions {
        void (OFXS_GL_APIENTRY *Begin)(GLenum mode);
        void (OFXS_GL_APIENTRY *End)(void);
        void (OFXS_GL_APIENTRY *Vertex2d)(GLdouble x, GLdouble y);
        void (OFXS_GL_APIENTRY *Color4f)(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
        void (OFXS_GL_APIENTRY *Enable)(GLenum cap);
        void (OFXS_GL_APIENTRY *Disable)(GLenum cap);
        void (OFXS_GL_APIENTRY *Hint)(GLenum target, GLenum mode);
        void (OFXS_GL_APIENTRY *LineWidth)(GLfloat width);
        void (OFXS_GL_APIENTRY *LineStipple)(GLint factor, GLushort pattern);
        void (OFXS_GL_APIENTRY *PolygonMode)(GLenum face, GLenum mode);
        void (OFXS_GL_APIENTRY *BlendFunc)(GLenum sfactor, GLenum dfactor);
        void (OFXS_GL_APIENTRY *PushAttrib)(GLbitfield mask);
        void (OFXS_GL_APIENTRY *PopAttrib)(void);
      };

      // A function pointer can't be converted from a void* in standard C++: copy the bits. The
      // sizes are checked at compile time (C++98-compatible static assertion).
      template<class F>
      F functionFromPointer(void *p)
      {
        typedef char function_and_object_pointers_have_the_same_size[(sizeof(F) == sizeof(void *)) ? 1 : -1];
        (void)sizeof(function_and_object_pointers_have_the_same_size);
        F f;
        std::memcpy(&f, &p, sizeof(f));
        return f;
      }

#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
#ifndef LOAD_LIBRARY_SEARCH_SYSTEM32
#define LOAD_LIBRARY_SEARCH_SYSTEM32 0x00000800 // (old MinGW headers)
#endif
      // opengl32.dll: the host's (already loaded), else the one in System32 only (no search of
      // the current or the application directory, against DLL planting). Never freed.
      void *openGLLibraryWindows()
      {
        HMODULE lib = GetModuleHandleA("opengl32.dll");
        if (!lib) {
          lib = LoadLibraryExA("opengl32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        return (void *)lib;
      }

      void *openGLSymbol(void *lib, const char *name)
      {
        if (!lib) {
          return NULL;
        }
        FARPROC f = GetProcAddress((HMODULE)lib, name);
        typedef char farproc_and_object_pointers_have_the_same_size[(sizeof(f) == sizeof(void *)) ? 1 : -1];
        (void)sizeof(farproc_and_object_pointers_have_the_same_size);
        void *p = NULL;
        std::memcpy(&p, &f, sizeof(p));
        return p;
      }
#else
      void *openGLSymbol(void *lib, const char *name)
      {
        return dlsym(lib, name);
      }

      // the address of a function, as an object pointer (for dladdr)
      template<class F>
      void *pointerFromFunction(F f)
      {
        typedef char function_and_object_pointers_have_the_same_size[(sizeof(F) == sizeof(void *)) ? 1 : -1];
        (void)sizeof(function_and_object_pointers_have_the_same_size);
        void *p = NULL;
        std::memcpy(&p, &f, sizeof(p));
        return p;
      }

#ifndef __APPLE__
      // the base address of the library that contains the address p, NULL if unknown
      void *libraryBase(void *p)
      {
        Dl_info info;
        if ( p && dladdr(p, &info) ) {
          return info.dli_fbase;
        }
        return NULL;
      }
#endif

#ifdef RTLD_NOLOAD
      // keep the library that contains the address p loaded (a reference that is never released)
      void pinLibraryOf(void *p)
      {
        Dl_info info;
        if ( p && dladdr(p, &info) && info.dli_fname && *info.dli_fname ) {
          (void)dlopen(info.dli_fname, RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
        }
      }
#endif

#if defined(RTLD_NOLOAD) && !defined(__APPLE__)
      // a library, only if it is already loaded (its functions can only report a current
      // context if the host loaded it). Never closed.
      void *loadedLibrary(const char *name)
      {
        return dlopen(name, RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
      }
#endif
#endif

      template<class F>
      bool loadGLFunction(void *lib, const char *name, F &f)
      {
        void *p = openGLSymbol(lib, name);
        f = p ? functionFromPointer<F>(p) : NULL;
        return f != NULL;
      }

      // all the functions from lib (fns is filled in, even if some are missing)
      bool loadGLFunctions(void *lib, GLFunctions &fns)
      {
        bool ok = true;
        ok = loadGLFunction(lib, "glBegin", fns.Begin) && ok;
        ok = loadGLFunction(lib, "glEnd", fns.End) && ok;
        ok = loadGLFunction(lib, "glVertex2d", fns.Vertex2d) && ok;
        ok = loadGLFunction(lib, "glColor4f", fns.Color4f) && ok;
        ok = loadGLFunction(lib, "glEnable", fns.Enable) && ok;
        ok = loadGLFunction(lib, "glDisable", fns.Disable) && ok;
        ok = loadGLFunction(lib, "glHint", fns.Hint) && ok;
        ok = loadGLFunction(lib, "glLineWidth", fns.LineWidth) && ok;
        ok = loadGLFunction(lib, "glLineStipple", fns.LineStipple) && ok;
        ok = loadGLFunction(lib, "glPolygonMode", fns.PolygonMode) && ok;
        ok = loadGLFunction(lib, "glBlendFunc", fns.BlendFunc) && ok;
        ok = loadGLFunction(lib, "glPushAttrib", fns.PushAttrib) && ok;
        ok = loadGLFunction(lib, "glPopAttrib", fns.PopAttrib) && ok;
        return ok;
      }

      // The functions that return the current OpenGL context (NULL if none), to check that one
      // is current before any OpenGL call: legacy OpenGL calls without a current context
      // usually crash (macOS). An OpenGL context is current if any of them returns non-NULL.
      typedef void *(OFXS_GL_APIENTRY *CurrentContextFunction)(void);
      enum { kMaxCurrentContextFunctions = 8 };
      struct CurrentContextFunctions {
        CurrentContextFunction f[kMaxCurrentContextFunctions];
        int count;
      };

      void addCurrentContextFunctionPointer(CurrentContextFunctions &c, void *p)
      {
        if (!p || c.count >= kMaxCurrentContextFunctions) {
          return;
        }
        const CurrentContextFunction f = functionFromPointer<CurrentContextFunction>(p);
        for (int i = 0; i < c.count; ++i) {
          if (c.f[i] == f) {
            return;
          }
        }
        c.f[c.count++] = f;
      }
      // lib may be RTLD_DEFAULT, which is NULL on glibc and musl: the callers check that the
      // other libraries are not NULL
      void addCurrentContextFunction(CurrentContextFunctions &c, void *lib, const char *name)
      {
        addCurrentContextFunctionPointer( c, openGLSymbol(lib, name) );
      }

      // the OpenGL functions and the library they come from (NULL if they can't all be loaded)
      struct OpenGLLoad {
        const GLFunctions *fns;
        void *lib;
      };

      OpenGLLoad loadOpenGL()
      {
        static GLFunctions fns;
        OpenGLLoad r = { NULL, NULL };
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
        void *lib = openGLLibraryWindows();
        if ( lib && loadGLFunctions(lib, fns) ) {
          r.fns = &fns;
          r.lib = lib;
        }
#else
        // already loaded globally (e.g. the host links it): no dlopen. If only some of the
        // functions are found there (another library exports them), try the OpenGL libraries.
        if ( loadGLFunctions(RTLD_DEFAULT, fns) ) {
          r.fns = &fns;
          r.lib = RTLD_DEFAULT; // NULL on glibc and musl: test r.fns, not r.lib
#ifdef RTLD_NOLOAD
          // dlsym(RTLD_DEFAULT) takes no reference: keep the library that has them loaded,
          // so that the pointers stay valid if the host unloads it
          pinLibraryOf( pointerFromFunction(fns.Begin) );
#endif
          return r;
        }
#ifdef __APPLE__
        static const char *const names[] = { "/System/Library/Frameworks/OpenGL.framework/OpenGL", NULL };
#else
        static const char *const names[] = { "libGL.so.1", "libGL.so", "libOpenGL.so.0", NULL };
#endif
        for (int i = 0; names[i]; ++i) {
          // (the library the host loaded is shared: dlopen doesn't load a second copy)
          void *lib = dlopen(names[i], RTLD_LAZY | RTLD_LOCAL); // never closed if used
          if (!lib) {
            continue;
          }
          if ( loadGLFunctions(lib, fns) ) {
            r.fns = &fns;
            r.lib = lib;
            return r;
          }
          dlclose(lib);
        }
#endif
        return r;
      }

      // loaded once, at the first call (thread-safe in C++11 and later)
      const OpenGLLoad &openGLLoad()
      {
        static const OpenGLLoad load = loadOpenGL();
        return load;
      }

      const GLFunctions *openGLFunctions()
      {
        return openGLLoad().fns;
      }

      CurrentContextFunctions loadCurrentContextFunctions()
      {
        CurrentContextFunctions c;
        c.count = 0;
        const OpenGLLoad &load = openGLLoad();
        if (!load.fns) { // (not load.lib: RTLD_DEFAULT is NULL on glibc and musl)
          return c;
        }
        void *gl = load.lib;
#if defined(_WIN32) || defined(__WIN32__) || defined(WIN32)
        addCurrentContextFunction(c, gl, "wglGetCurrentContext");
#elif defined(__APPLE__)
        // (no OSMesa: a plug-in-private OSMesa, e.g. openfx-misc's Shadertoy with
        // libMangledOSMesa32, exports an unmangled OSMesaGetCurrentContext, and its context
        // being current says nothing about the OpenGL framework that the gl* functions use)
        addCurrentContextFunction(c, gl, "CGLGetCurrentContext");
        addCurrentContextFunction(c, RTLD_DEFAULT, "CGLGetCurrentContext");
#else
        // GLX (libGL, or GLVND's libGLX), EGL (e.g. Wayland, or GLVND's libOpenGL with EGL):
        // a context of either may be current. Only the libraries that are already loaded can
        // have one.
        addCurrentContextFunction(c, gl, "glXGetCurrentContext");
        addCurrentContextFunction(c, RTLD_DEFAULT, "glXGetCurrentContext");
        addCurrentContextFunction(c, RTLD_DEFAULT, "eglGetCurrentContext");
#ifdef RTLD_NOLOAD
        void *glx = loadedLibrary("libGLX.so.0");
        if (glx) {
          addCurrentContextFunction(c, glx, "glXGetCurrentContext");
        }
        void *egl = loadedLibrary("libEGL.so.1");
        if (egl) {
          addCurrentContextFunction(c, egl, "eglGetCurrentContext");
        }
#endif
        // OSMesa: only if the gl* functions come from the same library as
        // OSMesaGetCurrentContext (a host that renders its viewer with OSMesa). A plug-in-private
        // OSMesa (e.g. Shadertoy's libMangledOSMesa32, which exports an unmangled
        // OSMesaGetCurrentContext) being current says nothing about the libGL context.
        void *glBase = libraryBase( pointerFromFunction(load.fns->Begin) );
        void *osmesa = openGLSymbol(RTLD_DEFAULT, "OSMesaGetCurrentContext");
#ifdef RTLD_NOLOAD
        if (!osmesa) {
          void *lib = loadedLibrary("libOSMesa.so.8");
          if (lib) {
            osmesa = openGLSymbol(lib, "OSMesaGetCurrentContext");
          }
        }
#endif
        if ( osmesa && glBase && libraryBase(osmesa) == glBase ) {
          addCurrentContextFunctionPointer(c, osmesa);
        }
#endif
        return c;
      }

#ifdef OFXS_DRAWCONTEXT_CXX11
      std::mutex &currentContextFunctionsMutex()
      {
        static std::mutex *m = new std::mutex; // never deleted (usable at exit)
        return *m;
      }
#endif

      // Loaded at the first call, and loaded again as long as none was found: the host may load
      // libEGL or libGLX after the first draw. Serialized in C++11 and later (the C++98 build
      // doesn't lock: then the first calls must not be concurrent).
      CurrentContextFunctions currentContextFunctions()
      {
#ifdef OFXS_DRAWCONTEXT_CXX11
        std::lock_guard<std::mutex> lock( currentContextFunctionsMutex() );
#endif
        static CurrentContextFunctions c; // zero-initialized
        if (c.count == 0) {
          c = loadCurrentContextFunctions();
        }
        return c;
      }

      // the backend only exists if openGLFunctions() is not NULL
      inline const GLFunctions &gl()
      {
        return *openGLFunctions();
      }

      // The colours returned by getColour(), indexed by OfxStandardColour. The spec gives no
      // values: these are close to the colours of the Natron and SupportExt OpenGL overlays.
      const OfxRGBAColourF kStandardColours[] = {
        { 0.f, 0.f, 0.f, 1.f },     // kOfxStandardColourOverlayBackground: black (the overlays' shadow)
        { 0.f, 1.f, 0.f, 1.f },     // kOfxStandardColourOverlayActive: green (hovered, being dragged)
        { 1.f, 0.f, 0.f, 1.f },     // kOfxStandardColourOverlaySelected: red
        { 0.8f, 0.8f, 0.8f, 1.f },  // kOfxStandardColourOverlayDeselected: light grey (the default)
        { 0.5f, 0.8f, 1.f, 1.f },   // kOfxStandardColourOverlayMarqueeFG: light blue (Natron's selection rectangle)
        { 0.5f, 0.8f, 1.f, 0.4f },  // kOfxStandardColourOverlayMarqueeBG: translucent light blue
        { 1.f, 1.f, 1.f, 1.f },     // kOfxStandardColourOverlayText: white
      };
      const int kStandardColourCount = (int)(sizeof(kStandardColours) / sizeof(kStandardColours[0]));

      // GL_LINE_STIPPLE patterns, in screen pixels (bit 0 is the first pixel), indexed by
      // OfxDrawLineStipplePattern (solid: no stipple)
      const GLushort kStipplePatterns[] = {
        0xFFFF, // kOfxDrawLineStipplePatternSolid   -----
        0x3333, // kOfxDrawLineStipplePatternDot     .....  2 on, 2 off
        0x00FF, // kOfxDrawLineStipplePatternDash    - - -  8 on, 8 off
        0xFF00, // kOfxDrawLineStipplePatternAltDash  - - - 8 off, 8 on
        0x1FE3, // kOfxDrawLineStipplePatternDotDash .-.-.- 2 on, 3 off, 8 on, 3 off
      };
      const int kStipplePatternCount = (int)(sizeof(kStipplePatterns) / sizeof(kStipplePatterns[0]));

      // limits, so that the conversions to OpenGL types are defined
      const double kMaxScreenPixelRatio = 64.;
      const double kMaxLineWidth = 1e4; // device pixels

      // text metrics, in screen pixels (one font pixel per screen pixel)
      const int kTextAscent = kFontGlyphHeight; // the glyphs sit on the baseline
      const int kTextDescent = kFontCellHeight - kFontGlyphHeight;

      inline bool isFinitePositive(double v)
      {
        return v > 0. && v < 1e300; // false for NaN and infinities
      }

      // Saves the OpenGL state that the backend changes, and sets the state common to all the
      // primitives: no texture (1D, 2D, rectangle), lighting, depth or alpha test, face culling
      // or polygon smoothing/stipple, filled polygons, and "over" compositing ("The host should
      // use "over" compositing when using a non-opaque colour"). The destructor restores it.
      // Not handled (the host's overlay state is expected to have the defaults, as for any
      // OpenGL 1.x overlay): a bound GLSL program (glUseProgram), glBlendEquation, glColorMask,
      // the stencil and scissor tests, and the texture units other than the active one.
      class GLStateSaver {
      public:
        explicit GLStateSaver(const OfxRGBAColourF &colour)
        {
          gl().PushAttrib(GL_CURRENT_BIT | GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT | GL_LINE_BIT |
                       GL_POLYGON_BIT | GL_HINT_BIT);
          gl().Disable(GL_TEXTURE_1D);
          gl().Disable(GL_TEXTURE_2D);
#if defined(GL_TEXTURE_RECTANGLE)
          gl().Disable(GL_TEXTURE_RECTANGLE);
#elif defined(GL_TEXTURE_RECTANGLE_ARB)
          gl().Disable(GL_TEXTURE_RECTANGLE_ARB);
#endif
          gl().Disable(GL_LIGHTING);
          gl().Disable(GL_DEPTH_TEST);
          gl().Disable(GL_CULL_FACE);
          gl().Disable(GL_ALPHA_TEST);
          gl().Disable(GL_POLYGON_SMOOTH);
          gl().Disable(GL_POLYGON_STIPPLE);
          gl().PolygonMode(GL_FRONT_AND_BACK, GL_FILL);
          gl().Enable(GL_BLEND);
          gl().BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
          gl().Color4f(colour.r, colour.g, colour.b, colour.a);
        }
        ~GLStateSaver()
        {
          gl().PopAttrib();
        }
      private:
        GLStateSaver(const GLStateSaver &);
        GLStateSaver &operator=(const GLStateSaver &);
      };

      class OpenGLBackend : public DrawContextBackend {
      public:
        // user-provided, so that a const object can be default-initialized (C++98 to C++14)
        OpenGLBackend() {}

        virtual OfxStatus getColour(const DrawContext & /*ctx*/, OfxStandardColour stdColour, OfxRGBAColourF *colour) const
        {
          if (!colour || (int)stdColour < 0 || (int)stdColour >= kStandardColourCount) {
            return kOfxStatErrValue; // "std_colour was invalid"
          }
          *colour = kStandardColours[(int)stdColour];
          return kOfxStatOK;
        }

        virtual OfxStatus setColour(const DrawContext &ctx, const OfxRGBAColourF *c) const
        {
          if (!c) {
            return kOfxStatErrValue; // as HostSupport's DrawSuite
          }
          colour(ctx) = *c;
          return kOfxStatOK;
        }

        virtual OfxStatus setLineWidth(const DrawContext &ctx, float width) const
        {
          if ( !(width >= 0.f && width < 1e30f) ) { // negative, NaN or infinite
            return kOfxStatFailed; // (the spec lists no kOfxStatErrValue for setLineWidth)
          }
          lineWidth(ctx) = width;
          return kOfxStatOK;
        }

        virtual OfxStatus setLineStipple(const DrawContext &ctx, OfxDrawLineStipplePattern pattern) const
        {
          if ( (int)pattern < 0 || (int)pattern >= kStipplePatternCount ) {
            return kOfxStatErrValue; // "pattern was not valid"
          }
          lineStipple(ctx) = pattern;
          return kOfxStatOK;
        }

        virtual OfxStatus draw(const DrawContext &ctx, OfxDrawPrimitive primitive, const OfxPointD *points, int pointCount) const
        {
          int minCount, maxCount;
          switch (primitive) {
          case kOfxDrawPrimitiveLines:
          case kOfxDrawPrimitiveLineStrip:
          case kOfxDrawPrimitiveLineLoop:
            minCount = 2; maxCount = -1; break;
          case kOfxDrawPrimitivePolygon:
            minCount = 3; maxCount = -1; break;
          case kOfxDrawPrimitiveRectangle:
          case kOfxDrawPrimitiveEllipse:
            minCount = 2; maxCount = 2; break; // "defined by 2 opposite corner points"
          default:
            return kOfxStatErrValue; // "invalid primitive"
          }
          if ( !points || pointCount < minCount || (maxCount >= 0 && pointCount > maxCount) ||
               (primitive == kOfxDrawPrimitiveLines && pointCount % 2 != 0) ) { // pairs, as HostSupport's DrawSuite
            return kOfxStatErrValue; // "point_count not valid for primitive"
          }

          GLStateSaver saver(colour(ctx));
          switch (primitive) {
          case kOfxDrawPrimitiveLines:
            setLineState(ctx);
            drawVertices(GL_LINES, points, pointCount);
            break;
          case kOfxDrawPrimitiveLineStrip:
            setLineState(ctx);
            drawVertices(GL_LINE_STRIP, points, pointCount);
            break;
          case kOfxDrawPrimitiveLineLoop:
            setLineState(ctx);
            drawVertices(GL_LINE_LOOP, points, pointCount);
            break;
          case kOfxDrawPrimitivePolygon:
            drawVertices(GL_POLYGON, points, pointCount);
            break;
          case kOfxDrawPrimitiveRectangle:
            gl().Begin(GL_QUADS);
            gl().Vertex2d(points[0].x, points[0].y);
            gl().Vertex2d(points[1].x, points[0].y);
            gl().Vertex2d(points[1].x, points[1].y);
            gl().Vertex2d(points[0].x, points[1].y);
            gl().End();
            break;
          case kOfxDrawPrimitiveEllipse:
            setLineState(ctx);
            drawEllipse(ctx, points[0], points[1]);
            break;
          default:
            break;
          }
          return kOfxStatOK;
        }

        virtual OfxStatus drawText(const DrawContext &ctx, const char *text, const OfxPointD *pos, int alignment) const
        {
          if (!text || !pos) {
            return kOfxStatErrValue; // "text or pos were not defined"
          }
          // the glyphs: printable ASCII, '?' for the other characters (one per UTF-8 sequence),
          // nothing for the control characters
          std::string glyphs;
          for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
            if (*p < 0x80) {
              if (*p >= 0x20 && *p < 0x7F) {
                glyphs += (char)*p;
              }
            } else if ( (*p & 0xC0) != 0x80 ) { // the first byte of a UTF-8 sequence
              glyphs += '?';
            }
          }
          if ( glyphs.empty() ) {
            return kOfxStatOK;
          }

          OfxPointD ps;
          getPixelScale(ctx, ps);
          const double width = (double)glyphs.size() * kFontCellWidth - (kFontCellWidth - kFontGlyphWidth);

          // horizontal: Left (default), Right, or both (CenterH)
          double x0 = pos->x;
          const bool left = (alignment & kOfxDrawTextAlignmentLeft) != 0;
          const bool right = (alignment & kOfxDrawTextAlignmentRight) != 0;
          if (right && left) {
            x0 -= 0.5 * width * ps.x;
          } else if (right) {
            x0 -= width * ps.x;
          }
          // vertical: Baseline (default), Top, Bottom, or Top with Bottom or Baseline (centred:
          // kOfxDrawTextAlignmentCenterV is Top | Baseline)
          double baseline = pos->y;
          const bool top = (alignment & kOfxDrawTextAlignmentTop) != 0;
          const bool bottom = (alignment & kOfxDrawTextAlignmentBottom) != 0;
          const bool onBaseline = (alignment & kOfxDrawTextAlignmentBaseline) != 0;
          if ( top && (bottom || onBaseline) ) {
            baseline -= 0.5 * (kTextAscent - kTextDescent) * ps.y;
          } else if (top) {
            baseline -= kTextAscent * ps.y;
          } else if (bottom) {
            baseline += kTextDescent * ps.y;
          }

          GLStateSaver saver(colour(ctx));
          // one quad per vertical run of pixels in each glyph column. A quad of one screen
          // pixel covers exactly one pixel centre, so the text is sharp at any position.
          gl().Begin(GL_QUADS);
          for (size_t i = 0; i < glyphs.size(); ++i) {
            const unsigned char *glyph = kFont5x7[(unsigned char)glyphs[i] - 0x20];
            for (int col = 0; col < kFontGlyphWidth; ++col) {
              const double xl = x0 + ( (int)i * kFontCellWidth + col ) * ps.x;
              const double xr = xl + ps.x;
              int row = 0;
              while (row < kFontGlyphHeight) {
                if ( !( (glyph[col] >> row) & 1 ) ) {
                  ++row;
                  continue;
                }
                const int first = row; // top row of the run
                while ( row < kFontGlyphHeight && ( (glyph[col] >> row) & 1 ) ) {
                  ++row;
                }
                // row kFontGlyphHeight - 1 sits on the baseline
                const double yt = baseline + (kFontGlyphHeight - first) * ps.y;
                const double yb = baseline + (kFontGlyphHeight - row) * ps.y;
                gl().Vertex2d(xl, yb);
                gl().Vertex2d(xr, yb);
                gl().Vertex2d(xr, yt);
                gl().Vertex2d(xl, yt);
              }
            }
          }
          gl().End();
          return kOfxStatOK;
        }

      private:
        // the screen pixel ratio, 1 if the host doesn't give it, at most kMaxScreenPixelRatio
        static double getScreenPixelRatio(const DrawContext &ctx)
        {
          const double r = screenPixelRatio(ctx);
          return isFinitePositive(r) ? std::min(r, kMaxScreenPixelRatio) : 1.;
        }

        // the size of a screen pixel in canonical coordinates (1 if the host gave none)
        static void getPixelScale(const DrawContext &ctx, OfxPointD &ps)
        {
          const OfxPointD &s = pixelScale(ctx);
          ps.x = isFinitePositive(std::fabs(s.x)) ? std::fabs(s.x) : 1.;
          ps.y = isFinitePositive(std::fabs(s.y)) ? std::fabs(s.y) : 1.;
        }

        // the line width and stipple, for the lines and the ellipse
        static void setLineState(const DrawContext &ctx)
        {
          // "Use width 0 for a single pixel line or non-zero for a smooth line of the desired
          // width. The host should adjust for screen density.": widths are in screen pixels,
          // and OpenGL's are device pixels
          const double ratio = getScreenPixelRatio(ctx);
          const float width = lineWidth(ctx);
          if (width > 0.f) {
            gl().Enable(GL_LINE_SMOOTH);
            gl().Hint(GL_LINE_SMOOTH_HINT, GL_DONT_CARE);
            // (OpenGL clamps it to its own maximum, usually much smaller)
            gl().LineWidth( (GLfloat)std::min( (double)width * ratio, kMaxLineWidth ) );
          } else {
            gl().Disable(GL_LINE_SMOOTH);
            gl().LineWidth( (GLfloat)std::max(1., std::floor(ratio + 0.5)) );
          }
          const OfxDrawLineStipplePattern pattern = lineStipple(ctx);
          if (pattern == kOfxDrawLineStipplePatternSolid) {
            gl().Disable(GL_LINE_STIPPLE);
          } else {
            gl().Enable(GL_LINE_STIPPLE);
            // the patterns are in screen pixels
            const GLint factor = (GLint)std::min(256., std::max(1., std::floor(ratio + 0.5)));
            gl().LineStipple(factor, kStipplePatterns[(int)pattern]);
          }
        }

        static void drawVertices(GLenum mode, const OfxPointD *points, int pointCount)
        {
          gl().Begin(mode);
          for (int i = 0; i < pointCount; ++i) {
            gl().Vertex2d(points[i].x, points[i].y);
          }
          gl().End();
        }

        // "an axis-aligned elliptical line (not filled) within the rectangle defined by 2
        // opposite corner points": a line loop, within 1/4 screen pixel of the ellipse
        static void drawEllipse(const DrawContext &ctx, const OfxPointD &p0, const OfxPointD &p1)
        {
          OfxPointD ps;
          getPixelScale(ctx, ps);
          const double cx = 0.5 * (p0.x + p1.x);
          const double cy = 0.5 * (p0.y + p1.y);
          const double rx = 0.5 * std::fabs(p1.x - p0.x);
          const double ry = 0.5 * std::fabs(p1.y - p0.y);
          // the largest radius in screen pixels: the sagitta of a segment of angle a is
          // r (1 - cos(a/2)), at most tolerance for a = 2 acos(1 - tolerance / r)
          const double r = std::max(rx / ps.x, ry / ps.y);
          const double tolerance = 0.25;
          const double pi = 3.14159265358979323846;
          int n = 8;
          if ( r > tolerance && r < 1e300 ) {
            const double a = 2. * std::acos(1. - tolerance / r);
            if (a > 0.) {
              n = (int)std::min( 4096., std::max( 8., std::ceil(2. * pi / a) ) );
            } else {
              n = 4096;
            }
          }
          gl().Begin(GL_LINE_LOOP);
          for (int i = 0; i < n; ++i) {
            const double t = 2. * pi * i / n;
            gl().Vertex2d(cx + rx * std::cos(t), cy + ry * std::sin(t));
          }
          gl().End();
        }
      };

    } // namespace

#endif // !OFXS_DRAWCONTEXT_NO_OPENGL

    const DrawContextBackend *openGLDrawContextBackend()
    {
#ifndef OFXS_DRAWCONTEXT_NO_OPENGL
      static const OpenGLBackend backend;
      // NULL if the OpenGL functions can't be loaded: getDrawContext() then draws nothing
      return openGLFunctions() ? &backend : NULL;
#else
      return NULL;
#endif
    }

    bool openGLContextIsCurrent()
    {
#ifndef OFXS_DRAWCONTEXT_NO_OPENGL
      // no function to check it (e.g. an unknown OpenGL platform): not current, so that no
      // OpenGL call is made without a context
      const CurrentContextFunctions c = currentContextFunctions();
      for (int i = 0; i < c.count; ++i) {
        if ( c.f[i]() ) {
          return true;
        }
      }
#endif
      return false;
    }

    int openGLCurrentContextFunctionCount()
    {
#ifndef OFXS_DRAWCONTEXT_NO_OPENGL
      return currentContextFunctions().count;
#else
      return 0;
#endif
    }

  } // namespace Private

  bool DrawArgs::isOpenGLContextCurrent() const
  {
    // (loads the OpenGL functions, which the check needs)
    return Private::openGLDrawContextBackend() && Private::openGLContextIsCurrent();
  }

  const DrawContext &DrawArgs::getDrawContext() const
  {
    const DrawContext &drawSuite = Private::DrawArgsAccess::drawSuiteContext(*this);
    switch ( Private::DrawArgsAccess::getOverlayRegistration(*this) ) {
    case Private::eOverlayRegistrationV2:
      // "If this is called via kOfxImageEffectPluginPropOverlayInteractV2, drawing MUST use
      // DrawSuite": the DrawSuite, or nothing if the host passed no draw context
      return drawSuite;
    case Private::eOverlayRegistrationV1:
      // "If this is called via kOfxImageEffectPluginPropOverlayInteractV1, drawing SHOULD use
      // OpenGL": OpenGL, even if the host passed a draw context (parameter interacts too)
      break;
    case Private::eOverlayRegistrationUnknown:
    default:
      // DrawArgs not made by the dispatcher: the draw context if there is one
      if ( drawSuite.usesDrawSuite() ) {
        return drawSuite;
      }
      break;
    }
    DrawContext &openGL = Private::DrawArgsAccess::openGLContext(*this);
    // OpenGL, only if it can be loaded and an OpenGL context is current: a host may have the
    // DrawSuite and no OpenGL, and legacy OpenGL calls without a current context usually crash
    const Private::DrawContextBackend *backend = Private::openGLDrawContextBackend();
    if ( backend && Private::openGLContextIsCurrent() ) {
      Private::DrawArgsAccess::setOpenGLBackend(openGL, backend);
      return openGL;
    }
    // No OpenGL context: the host's DrawSuite context if it passed one ("Some existing plugins
    // may use DrawSuite via kOfxImageEffectPluginPropOverlayInteractV1 if it's supported by the
    // host, but this is discouraged"), else a context that draws nothing
    if ( drawSuite.usesDrawSuite() ) {
      return drawSuite;
    }
    Private::DrawArgsAccess::setOpenGLBackend(openGL, NULL);
    return openGL;
  }

} // namespace OFX
