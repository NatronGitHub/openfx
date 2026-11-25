// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxsDrawContextOpenGL.cpp
  OFX::DrawArgs::getDrawContext(), and the OpenGL backend of OFX::DrawContext: an emulation of the
  OFX 1.5 DrawSuite (include/ofxDrawSuite.h, OfxDrawSuiteV1) with OpenGL 1.x compatibility-profile
  calls, for the overlays registered as kOfxImageEffectPluginPropOverlayInteractV1. See the
  documentation of OFX::DrawContext in ofxsInteract.h.

  This is the only file of the Support library that calls OpenGL. The drawing itself is the
  OpenGL DrawSuite core, include/ofxDrawSuiteOpenGL.h, shared with the hosts: this file loads the
  OpenGL functions, checks that a context is current, and passes the DrawContext's state to the
  core. It needs no OpenGL header (the core declares the OpenGL types and constants it uses).
  ofxsInteract.cpp only calls this backend through virtual functions, so a plug-in that never
  calls getDrawContext() doesn't link this file from a static library (libOfxSupport.a, CMake's
  openfx::Support). With Support/Plugins/Makefile.master, whose SUPPORTOBJECTS are object files,
  every plug-in links it.

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
  (thread-safe).

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
  Without a current OpenGL context, getDrawContext() returns a context that draws nothing
  (canDraw() is false, every function returns false). It never uses the host's DrawSuite in a V1
  draw, even if the host passed a draw context: V1 draws use only OpenGL, V2 draws only the
  DrawSuite.

  Define OFXS_DRAWCONTEXT_NO_OPENGL to build without OpenGL (no OpenGL call, no run-time loading):
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
    gives no values. This backend returns the fixed colours of
    OFX::DrawSuiteOpenGL::standardColour() (ofxDrawSuiteOpenGL.h).
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
#include <windows.h> // GetModuleHandleA, LoadLibraryExA, GetProcAddress
#else
#include <dlfcn.h>
#endif

#include <cstddef>
#include <cstring>
#include <mutex>

// the drawing code, shared with the hosts (no OpenGL header: it declares the OpenGL types and
// constants it uses, and calls the functions loaded here)
#include "ofxDrawSuiteOpenGL.h"

#endif // !OFXS_DRAWCONTEXT_NO_OPENGL

namespace OFX {

  namespace Private {

#ifndef OFXS_DRAWCONTEXT_NO_OPENGL

    namespace {

      using DrawSuiteOpenGL::GLFunctions;

      // A function pointer can't be converted from a void* in standard C++: copy the bits. The
      // sizes are checked at compile time.
      template<class F>
      F functionFromPointer(void *p)
      {
        static_assert(sizeof(F) == sizeof(void *), "function and object pointers have the same size");
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
        static_assert(sizeof(f) == sizeof(void *), "FARPROC and object pointers have the same size");
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
        static_assert(sizeof(F) == sizeof(void *), "function and object pointers have the same size");
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

      // the symbol lookup of loadGLFunctions(): openGLSymbol() in the library userData
      void *openGLSymbolResolver(const char *name, void *lib)
      {
        return openGLSymbol(lib, name);
      }

      // all the functions from lib (fns is filled in, even if some are missing)
      bool loadGLFunctions(void *lib, GLFunctions &fns)
      {
        return DrawSuiteOpenGL::loadGLFunctions(fns, openGLSymbolResolver, lib);
      }

      // The functions that return the current OpenGL context (NULL if none), to check that one
      // is current before any OpenGL call: legacy OpenGL calls without a current context
      // usually crash (macOS). An OpenGL context is current if any of them returns non-NULL.
      typedef void *(OFX_DRAWGL_APIENTRY *CurrentContextFunction)(void);
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

      // loaded once, at the first call (thread-safe)
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

      std::mutex &currentContextFunctionsMutex()
      {
        static std::mutex *m = new std::mutex; // never deleted (usable at exit)
        return *m;
      }

      // Loaded at the first call, and loaded again as long as none was found: the host may load
      // libEGL or libGLX after the first draw. Serialized.
      CurrentContextFunctions currentContextFunctions()
      {
        std::lock_guard<std::mutex> lock( currentContextFunctionsMutex() );
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

      // The OpenGL backend: the DrawContext's state (colour, line width, stipple, pixel scale,
      // screen pixel ratio) as a DrawSuiteOpenGL::State, drawn by the core
      // (include/ofxDrawSuiteOpenGL.h)
      class OpenGLBackend : public DrawContextBackend {
      public:
        // user-provided, so that a const object can be default-initialized with every compiler
        OpenGLBackend() {}

        virtual OfxStatus getColour(const DrawContext & /*ctx*/, OfxStandardColour stdColour, OfxRGBAColourF *colour) const
        {
          return DrawSuiteOpenGL::standardColour(stdColour, colour); // "std_colour was invalid"
        }

        virtual OfxStatus setColour(const DrawContext &ctx, const OfxRGBAColourF *c) const
        {
          DrawSuiteOpenGL::State s = state(ctx);
          const OfxStatus st = DrawSuiteOpenGL::setColour(s, c);
          if (st == kOfxStatOK) {
            colour(ctx) = s.colour;
          }
          return st;
        }

        virtual OfxStatus setLineWidth(const DrawContext &ctx, float width) const
        {
          DrawSuiteOpenGL::State s = state(ctx);
          const OfxStatus st = DrawSuiteOpenGL::setLineWidth(s, width);
          if (st == kOfxStatOK) {
            lineWidth(ctx) = s.lineWidth;
          }
          return st;
        }

        virtual OfxStatus setLineStipple(const DrawContext &ctx, OfxDrawLineStipplePattern pattern) const
        {
          DrawSuiteOpenGL::State s = state(ctx);
          const OfxStatus st = DrawSuiteOpenGL::setLineStipple(s, pattern);
          if (st == kOfxStatOK) {
            lineStipple(ctx) = s.stipple;
          }
          return st;
        }

        virtual OfxStatus draw(const DrawContext &ctx, OfxDrawPrimitive primitive, const OfxPointD *points, int pointCount) const
        {
          return DrawSuiteOpenGL::drawPrimitive(gl(), state(ctx), primitive, points, pointCount);
        }

        virtual OfxStatus drawText(const DrawContext &ctx, const char *text, const OfxPointD *pos, int alignment) const
        {
          return DrawSuiteOpenGL::drawText(gl(), state(ctx), text, pos, alignment);
        }

      private:
        // the core's state for ctx (resetState() sanitises the pixel scale and the ratio); the
        // shadows: kNatronOfxInteractPropDrawSuiteShadows of the draw action
        static DrawSuiteOpenGL::State state(const DrawContext &ctx)
        {
          DrawSuiteOpenGL::State s;
          DrawSuiteOpenGL::resetState(s, pixelScale(ctx), screenPixelRatio(ctx), drawSuiteShadows(ctx));
          s.colour = colour(ctx);
          s.lineWidth = lineWidth(ctx);
          s.stipple = lineStipple(ctx);
          return s;
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
      // OpenGL": only OpenGL, even if the host passed a draw context (parameter interacts too)
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
    // No OpenGL context: a context that draws nothing. Never the host's DrawSuite in a V1 draw
    // (V1 draws use only OpenGL, V2 draws only the DrawSuite)
    Private::DrawArgsAccess::setOpenGLBackend(openGL, NULL);
    return openGL;
  }

} // namespace OFX
