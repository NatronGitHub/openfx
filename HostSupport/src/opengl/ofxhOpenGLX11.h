#ifndef OFXH_OPENGL_X11_H
#define OFXH_OPENGL_X11_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// Private to openfx::HostSupportOpenGL (Linux and BSD): libX11, loaded at run time (no header or
// library needed to build, nor to run the other backends), for the GLX backend.
//
// Xlib has process-wide state: the connections' bookkeeping (XOpenDisplay, XCloseDisplay) and the
// X error handler. The library serializes its own calls that touch it with one mutex (x11Mutex()),
// and sets an error handler only while it creates or destroys a context (X11ErrorTrap), so that an
// X error (GLXBadProfileARB, BadAlloc...) on the context's connection fails that attempt instead
// of reaching Xlib's default handler, which exits the process; the errors of other connections go
// on to the host's handler. Making a context current at render time isn't trapped (an XSync per
// call would cost too much): it was done once at creation, under the trap. The library doesn't
// call XInitThreads(): before libX11 1.8 that must be the first Xlib call of the process, which
// only the host can make. A host that uses Xlib from several threads with libX11 older than 1.8
// (e.g. EL9's 1.7) calls XInitThreads() first.

#if !defined(__APPLE__) && !defined(_WIN32)

#include <dlfcn.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace OFX {
  namespace Host {
    namespace OpenGL {
      namespace Detail {

        /// libX11's functions used here; the library is loaded once and never unloaded
        struct X11Library {
          typedef int (*ErrorHandler)(void *display, void *event);
          void *(*OpenDisplay)(const char *) = nullptr;
          int (*CloseDisplay)(void *) = nullptr;
          int (*DefaultScreen)(void *) = nullptr;
          int (*Free)(void *) = nullptr;
          int (*Sync)(void *, int) = nullptr;
          ErrorHandler (*SetErrorHandler)(ErrorHandler) = nullptr;

          /// the process's libX11, nullptr if it isn't there
          static const X11Library *get()
          {
            static const X11Library *const library = []() -> const X11Library * {
              void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL | RTLD_NODELETE);
              if (!x11) {
                return nullptr;
              }
              std::unique_ptr<X11Library> l(new X11Library);
              l->OpenDisplay = reinterpret_cast<decltype(l->OpenDisplay)>(dlsym(x11, "XOpenDisplay"));
              l->CloseDisplay = reinterpret_cast<decltype(l->CloseDisplay)>(dlsym(x11, "XCloseDisplay"));
              l->DefaultScreen = reinterpret_cast<decltype(l->DefaultScreen)>(dlsym(x11, "XDefaultScreen"));
              l->Free = reinterpret_cast<decltype(l->Free)>(dlsym(x11, "XFree"));
              l->Sync = reinterpret_cast<decltype(l->Sync)>(dlsym(x11, "XSync"));
              l->SetErrorHandler = reinterpret_cast<decltype(l->SetErrorHandler)>(dlsym(x11, "XSetErrorHandler"));
              if (!l->OpenDisplay || !l->CloseDisplay || !l->DefaultScreen || !l->Free || !l->Sync ||
                  !l->SetErrorHandler) {
                return nullptr;
              }
              return l.release(); // (never destroyed: the library is never unloaded either)
            }();
            return library;
          }
        };

        /// the mutex of the library's Xlib calls on process-wide state (see the file comment)
        inline std::mutex &x11Mutex()
        {
          static std::mutex mutex;
          return mutex;
        }

        /// A connection of its own to the default X server ($DISPLAY: XOpenDisplay(NULL)), closed
        /// with XCloseDisplay when the last reference goes (both under x11Mutex()); nullptr and
        /// error if none. A context uses its connection on one thread at a time (CurrentContext).
        inline std::shared_ptr<void> openX11Display(std::string &error)
        {
          const X11Library *x11 = X11Library::get();
          if (!x11) {
            error = "libX11 not found";
            return nullptr;
          }
          void *display = nullptr;
          {
            std::lock_guard<std::mutex> lock(x11Mutex());
            display = x11->OpenDisplay(nullptr);
          }
          if (!display) {
            error = "no X display (XOpenDisplay failed: is $DISPLAY set?)";
            return nullptr;
          }
          return std::shared_ptr<void>(display, [x11](void *d) {
            std::lock_guard<std::mutex> lock(x11Mutex());
            x11->CloseDisplay(d);
          });
        }

        /// While it lives, with x11Mutex() held, the X errors of display are recorded instead of
        /// going to the host's handler (by default Xlib's, which exits); those of other displays
        /// (e.g. the host's GUI thread) go to the handler that was set before. failed() first
        /// syncs display, so that the errors of the requests sent so far have arrived. That
        /// handler is set back at the end, unless the host set another one meanwhile, which then
        /// stays. Xlib's handler is process-wide: without XInitThreads() (libX11 < 1.8), setting
        /// it while another thread uses Xlib can't be fully safe.
        class X11ErrorTrap {
        public:
          X11ErrorTrap(const X11Library &x11, void *display)
            : _x11(x11), _display(display), _lock(x11Mutex())
          {
            code() = 0;
            active() = display;
            previous() = _x11.SetErrorHandler(&X11ErrorTrap::record);
          }
          ~X11ErrorTrap()
          {
            _x11.Sync(_display, 0); // (the errors of the last requests come here, not to the host)
            const X11Library::ErrorHandler current = _x11.SetErrorHandler(previous());
            if (current != &X11ErrorTrap::record) {
              _x11.SetErrorHandler(current); // (the host's, set during the trap: it wins)
            }
            active() = nullptr;
          }
          X11ErrorTrap(const X11ErrorTrap &) = delete;
          X11ErrorTrap &operator=(const X11ErrorTrap &) = delete;

          /// an X error since the trap was set, or since the last clear()
          bool failed()
          {
            _x11.Sync(_display, 0);
            return code() != 0;
          }
          void clear() { code() = 0; }

        private:
          /// nonzero after an X error on the trap's display (written by the thread that gets the
          /// error, which may not hold x11Mutex())
          static std::atomic<int> &code()
          {
            static std::atomic<int> value(0);
            return value;
          }
          /// the display of the active trap, nullptr if none
          static std::atomic<void *> &active()
          {
            static std::atomic<void *> display(nullptr);
            return display;
          }
          /// the handler set before the active trap
          static std::atomic<X11Library::ErrorHandler> &previous()
          {
            static std::atomic<X11Library::ErrorHandler> handler(nullptr);
            return handler;
          }
          static int record(void *display, void *event)
          {
            if (display && display == active().load()) {
              code() = 1;
              return 0;
            }
            // another connection's error: the host's (XErrorHandler returns an int Xlib ignores)
            const X11Library::ErrorHandler host = previous().load();
            return host ? host(display, event) : 0;
          }

          const X11Library &_x11;
          void *_display;
          std::lock_guard<std::mutex> _lock;
        };

      } // Detail
    } // OpenGL
  } // Host
} // OFX

#endif

#endif // OFXH_OPENGL_X11_H
