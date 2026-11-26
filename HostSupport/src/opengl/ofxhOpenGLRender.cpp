// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

// ofxhOpenGLRender.h: OpenGL renders of HostSupport effect instances (ofxGPURender.h).

#include "ofxhOpenGLRender.h"
#include "ofxhOpenGLFunctions.h"

#ifdef OFX_SUPPORTS_OPENGLRENDER

#include "ofxGPURender.h"
#include "ofxOpenGLRender.h"

#include <algorithm>
#include <stdexcept>
#include <vector>

namespace OFX {
  namespace Host {
    namespace OpenGL {

      RenderMode chooseRenderMode(const ImageEffect::Instance &effect, bool hostHasOpenGL)
      {
        // kOfxImageEffectPropOpenGLRenderSupported: "true" - "a host or plug-in can support OpenGL
        // accelerated rendering, in the case of plug-ins this also means that it is capable of CPU
        // based rendering in the absence of a GPU"; "needed" - "only for plug-ins, this means that
        // an plug-in has to have OpenGL support, without which it cannot work."
        // kOfxImageEffectPropCPURenderSupported: "false" - "the host or plug-in does not support CPU
        // rendering" (ofxGPURender.h, OFX 1.5.1)
        const Property::Set &props = effect.getProps();
        const std::string &gl = props.getStringProperty(kOfxImageEffectPropOpenGLRenderSupported);
        const bool needed = gl == "needed";
        if (hostHasOpenGL && (needed || gl == "true")) {
          return RenderMode::OpenGL;
        }
        if (!needed && props.getStringProperty(kOfxImageEffectPropCPURenderSupported) != "false") {
          return RenderMode::CPU;
        }
        return RenderMode::None;
      }

      ////////////////////////////////////////////////////////////////////////////////
      // Attachment

      static OfxStatus attachedAction(ImageEffect::Instance &effect, void *&contextData)
      {
        contextData = nullptr;
#ifdef OFX_EXTENSIONS_NATRON
        return effect.contextAttachedAction(contextData);
#else
        return effect.contextAttachedAction();
#endif
      }

      static OfxStatus detachedAction(ImageEffect::Instance &effect, void *contextData)
      {
#ifdef OFX_EXTENSIONS_NATRON
        return effect.contextDetachedAction(contextData);
#else
        (void)contextData;
        return effect.contextDetachedAction();
#endif
      }

      Attachment::~Attachment()
      {
        try {
          detach();
        } catch (...) {
          // a destructor doesn't throw
        }
      }

      Context *Attachment::context() const
      {
        std::lock_guard<std::mutex> lock(_mutex);
        return _context;
      }

      void *Attachment::contextData() const
      {
        std::lock_guard<std::mutex> lock(_mutex);
        return _contextData;
      }

      bool Attachment::failed() const
      {
        std::lock_guard<std::mutex> lock(_mutex);
        return _failed;
      }

      /// the status of Attached or Detached: kOfxStatFailed, "the host should not attempt to run
      /// the plug-in in OpenGL render mode" (other errors too); kOfxStatErrMemory, "this may be
      /// called again after a memory purge"
      static bool refusesOpenGL(OfxStatus st)
      {
        return st != kOfxStatOK && st != kOfxStatReplyDefault && st != kOfxStatErrMemory;
      }

      OfxStatus Attachment::detachLocked(std::unique_lock<std::mutex> &lock, CurrentContext *current)
      {
        // "The host must call this with the same OpenGL context active as it called with the
        // corresponding kOfxActionOpenGLContextAttached." (kOfxActionOpenGLContextDetached).
        // The caller's context is released (with its enclosing scopes) while waiting for that
        // one: no thread holds a context while it waits for another, so two threads that swap
        // instances between the same two contexts don't deadlock (like the lock order of CP.21,
        // for contexts).
        Context *previous = _context;
        void *data = _contextData;
        lock.unlock();
        if (current) {
          current->release();
        }
        bool sent = false;
        OfxStatus st = kOfxStatFailed;
        {
          // the caller's context is current again at the end, also if Detached throws (E.6)
          struct Reacquire {
            CurrentContext *c;
            ~Reacquire() { if (c) c->reacquire(); }
          } reacquire = { current };
          CurrentContext other(*previous);
          if (other.ok()) {
            st = detachedAction(_effect, data);
            sent = true;
          }
        }
        lock.lock();
        if (!sent || st == kOfxStatErrMemory) {
          // not sent (the thread holds another context, or the backend failed), or to be sent
          // again: still attached, so no Attached elsewhere
          return sent ? st : kOfxStatFailed;
        }
        _context = nullptr;
        _contextData = nullptr;
        _failed = _failed || refusesOpenGL(st);
        return st;
      }

      /// sets _switching for its lifetime, and resets it (with the lock) and notifies the waiters
      /// at its end, also when an action throws (E.6)
      class Attachment::Switch {
      public:
        Switch(Attachment &a, std::unique_lock<std::mutex> &lock) : _a(a), _lock(lock) { _a._switching = true; }
        ~Switch()
        {
          if (!_lock.owns_lock()) {
            _lock.lock();
          }
          _a._switching = false;
          _a._changed.notify_all();
        }
      private:
        Attachment &_a;
        std::unique_lock<std::mutex> &_lock;
      };

      OfxStatus Attachment::detach()
      {
        Context *const held = heldContext();
        std::unique_lock<std::mutex> lock(_mutex);
        // a thread that holds a context waits for nothing: it can send Detached only on that
        // context, and only if no render of the instance runs
        if ((held && (held != _context || _renders > 0 || _switching)) || (!held && reservedContext())) {
          return _context ? kOfxStatFailed : kOfxStatReplyDefault;
        }
        _changed.wait(lock, [&] { return !_switching && _renders == 0; });
        if (!_context) {
          return kOfxStatReplyDefault;
        }
        Switch guard(*this, lock);
        return detachLocked(lock, nullptr);
      }

      OfxStatus Attachment::beginRender(CurrentContext &current)
      {
        if (!current.ok()) {
          return kOfxStatFailed;
        }
        Context *const target = &current.context();
        std::unique_lock<std::mutex> lock(_mutex);
        // wait for the renders of the instance on another context, and for another thread's
        // switch, without holding this context
        while (!_failed && (_switching || (_renders > 0 && _context != target))) {
          if (reservedContext()) {
            return kOfxStatFailed; // a render of this thread reserves a context: no waiting
          }
          lock.unlock();
          current.release();
          lock.lock();
          _changed.wait(lock, [&] { return _failed || (!_switching && (_renders == 0 || _context == target)); });
          lock.unlock();
          const bool ok = current.reacquire();
          lock.lock();
          if (!ok) {
            return kOfxStatFailed;
          }
        }
        if (_failed) {
          return kOfxStatFailed; // a kOfxStatFailed from Attached or Detached: no OpenGL any more
        }
        if (_context == target) {
          ++_renders;
          return kOfxStatOK;
        }
        // "A host cannot call kOfxActionOpenGLContextAttached on the same instance without an
        // intervening kOfxActionOpenGLContextDetached. A host can have a plug-in swap OpenGL
        // contexts by issuing a attach/detach for the first context then another attach for the
        // next context." (kOfxActionOpenGLContextAttached)
        Switch guard(*this, lock);
        OfxStatus st = kOfxStatOK;
        if (_context) {
          st = detachLocked(lock, &current);
        }
        if (_context || !current.ok()) {
          // Detached wasn't sent, or must be sent again, or this context is lost
          return st == kOfxStatOK || st == kOfxStatReplyDefault ? kOfxStatFailed : st;
        }
        if (_failed) {
          return kOfxStatFailed;
        }
        lock.unlock();
        void *data = nullptr;
        st = attachedAction(_effect, data);
        lock.lock();
        if (st == kOfxStatOK || st == kOfxStatReplyDefault) {
          _context = target;
          _contextData = data;
          ++_renders;
        }
        _failed = refusesOpenGL(st);
        return st;
      }

      void Attachment::endRender()
      {
        std::lock_guard<std::mutex> lock(_mutex);
        --_renders;
        _changed.notify_all();
      }

      ////////////////////////////////////////////////////////////////////////////////
      // RenderSession

      /// A texture handle (clipLoadTexture). It owns the texture of an input clip: "For input
      /// clips, this also deletes the texture from OpenGL. [...] for the Output clip, it just
      /// releases the handle but does not delete the texture (since the host will need to read
      /// it)." (clipFreeTexture)
      class RenderSession::Handle : public ImageEffect::Texture {
      public:
        Handle(RenderSession &session, ImageEffect::ClipInstance &clip, const ImageEffect::ImageBase &image,
               OpenGL::Texture &texture, std::unique_ptr<OpenGL::Texture> owned)
          : ImageEffect::Texture(clip,
                                 image.getDoubleProperty(kOfxImageEffectPropRenderScale, 0),
                                 image.getDoubleProperty(kOfxImageEffectPropRenderScale, 1),
                                 static_cast<int>(texture.index()), texture.target(), texture.bounds(), image.getROD(),
                                 texture.rowBytes(), image.getStringProperty(kOfxImagePropField),
                                 image.getStringProperty(kOfxImagePropUniqueIdentifier))
          , _session(session)
          , _owned(std::move(owned))
        {
          // the texture's, which may differ from the clip's ("because the image is being directly
          // loaded into a texture by the host it need not obey the Clip Preferences action to remap
          // the image to the pixel depth the effect requested", ofxGPURender.h)
          setStringProperty(kOfxImageEffectPropPixelDepth, pixelDepthToOfx(texture.depth()));
          setStringProperty(kOfxImageEffectPropComponents, componentsToOfx(texture.components()));
          setStringProperty(kOfxImageEffectPropPreMultiplication, image.getStringProperty(kOfxImageEffectPropPreMultiplication));
          setDoubleProperty(kOfxImagePropPixelAspectRatio, image.getDoubleProperty(kOfxImagePropPixelAspectRatio));
          _session._handles.insert(this);
        }

        ~Handle() override { _session._handles.erase(this); }
        /// leaves its texture (the context is lost: no OpenGL call)
        void abandon()
        {
          if (_owned) {
            _owned->abandon();
          }
        }

      private:
        RenderSession &_session;
        std::unique_ptr<OpenGL::Texture> _owned;
      };

      RenderSession::RenderSession(Attachment &attachment, CurrentContext &current)
        : _attachment(attachment)
        , _current(current)
        , _status(attachment.beginRender(current))
      {
      }

      RenderSession::~RenderSession()
      {
        // "texture handle to be disposed of by clipFreeTexture before the action returns"
        // (clipLoadTexture): the handles left are the plug-in's leaks
        if (!_handles.empty() && context().hooks().log) {
          context().hooks().log("OpenGL: the plug-in didn't free " + std::to_string(_handles.size()) +
                                " texture(s) with clipFreeTexture");
        }
        const std::vector<Handle *> left(_handles.begin(), _handles.end());
        const bool lost = !_current.ok(); // no GL call without the context: the GL names are left
        for (Handle *h : left) {
          if (lost) {
            h->abandon();
          }
          delete h;
        }
        if (lost && _output) {
          _output->abandon();
        }
        _output.reset();
        if (ok()) {
          _attachment.endRender();
        }
      }

      /// the image rectangle of an image handle; false if its depth or components aren't a
      /// texture's
      static bool imageRect(const ImageEffect::Image &image, ImageRect &rect)
      {
        rect.data = image.getPointerProperty(kOfxImagePropData);
        rect.bounds = image.getBounds();
        rect.rowBytes = image.getIntProperty(kOfxImagePropRowBytes);
        return pixelDepthFromOfx(image.getStringProperty(kOfxImageEffectPropPixelDepth), rect.depth) &&
          componentsFromOfx(image.getStringProperty(kOfxImageEffectPropComponents), rect.components);
      }

      /// the depth of the texture of an image of depth imageDepth: "When a specific format is
      /// requested, the host ensures it gives the requested format." "When set to NULL, the host
      /// decides the format based on the plug-in's kOfxOpenGLPropPixelDepth setting."
      /// (clipLoadTexture): the image's depth if listed (or the list is empty), else the deepest
      /// one listed
      static PixelDepth textureDepth(const ImageEffect::Instance &effect, const char *format, PixelDepth imageDepth)
      {
        PixelDepth depth;
        if (format && pixelDepthFromOfx(format, depth)) {
          return depth;
        }
        if (effect.isOpenGLPixelDepthSupported(pixelDepthToOfx(imageDepth))) {
          return imageDepth;
        }
        for (PixelDepth d : { PixelDepth::Float, PixelDepth::Half, PixelDepth::Short, PixelDepth::Byte }) {
          if (effect.isOpenGLPixelDepthSupported(pixelDepthToOfx(d))) {
            return d;
          }
        }
        return imageDepth;
      }

      Texture &RenderSession::setOutput(ImageEffect::Image &image, const OfxRectI &renderWindow)
      {
        ImageRect rect;
        if (!imageRect(image, rect)) {
          throw std::runtime_error("OpenGL: the output image has no texture format");
        }
        // the render window within the image. kOfxOpenGLPropPixelDepth: "the target buffers where
        // the plug-in renders to will be set to one of the supported formats" (readOutput()
        // converts to the image's depth)
        const OfxRectI bounds = Detail::intersect(rect.bounds, renderWindow);
        _output.reset(new Texture(context(), bounds, textureDepth(_attachment._effect, nullptr, rect.depth),
                                  rect.components));
        _outputImage = &image;
        _output->clear();
        // "Prior to calling the render action the host may also choose to bind the output texture
        // as the current color buffer (render target)"; "The host will create a default OpenGL
        // viewport that is the size of the render window passed to the render action. [...]
        // After this, it is completely up to the effect to choose what OpenGL operations to render
        // with, including projections and so on." So the viewport only: the matrices are the
        // plug-in's.
        _output->bindAsRenderTarget();
        const int w = bounds.x2 - bounds.x1, h = bounds.y2 - bounds.y1;
        context().driverCall([&] { context().gl().Viewport(0, 0, w, h); });
        return *_output;
      }

      void RenderSession::readOutput()
      {
        if (!_output || !_outputImage) {
          return;
        }
        ImageRect rect;
        imageRect(*_outputImage, rect);
        _output->readback(rect); // glReadPixels waits for the rendering
      }

      ImageEffect::Texture *RenderSession::loadTexture(ImageEffect::ClipInstance &clip, OfxTime time,
                                                       const char *format, const OfxRectD *region)
      {
        if (!ok()) {
          return nullptr;
        }
        try {
          if (clip.isOutput()) {
            // "When the clip specified is the "Output" clip, the format is ignored and the host
            // must bind the resulting texture as the current color buffer (render target)."
            if (!_output) {
              return nullptr;
            }
            _output->bindAsRenderTarget();
            return new Handle(*this, clip, *_outputImage, *_output, nullptr);
          }
          // the region semantics are clipGetImage's ("If the region parameter is set to non-NULL,
          // then it will be clipped to the clip's Region of Definition [...]")
          // A host that renders inputs on demand may run renders in getImage(). "Effects can use
          // the graphics context as they see fit. They may be doing several render passes with
          // fetch back from the card to main memory via 'render to texture' mechanisms
          // interleaved with passes performed on the CPU." (ofxGPURender.h), and the context is
          // current for the whole render action: the plug-in's GL state must persist across
          // clipLoadTexture. So the context stays reserved to this render (suspend()): renders
          // on other threads get another context (ContextPool::tryAcquire()) or the CPU, and
          // nothing waits for it while this thread waits for them (it holds no current context
          // meanwhile). A render nested on this thread may use it (the thread is serial): it
          // binds its own framebuffer and viewport, which are restored after, also on a throw.
          Detail::GLint framebuffer = 0, viewport[4] = { 0, 0, 0, 0 };
          context().driverCall([&] {
            context().gl().GetIntegerv(Detail::kGLFramebufferBinding, &framebuffer);
            context().gl().GetIntegerv(Detail::kGLViewport, viewport);
          });
          auto restore = [&] {
            context().driverCall([&] {
              context().gl().BindFramebuffer(Detail::kGLFramebuffer, static_cast<Detail::GLuint>(framebuffer));
              context().gl().Viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
            });
          };
          _current.suspend();
          ImageEffect::Image *image = nullptr;
          try {
            image = clip.getImage(time, region);
          } catch (...) {
            if (_current.resume()) {
              restore();
            }
            throw;
          }
          if (!_current.resume()) {
            _failed = true; // the context is lost: no OpenGL for the rest of this render
            if (image) {
              image->releaseReference();
            }
            return nullptr;
          }
          restore();
          if (!image) {
            return nullptr; // no image in the clip: "assume the image was black and transparent"
          }
          std::unique_ptr<ImageEffect::Image, void (*)(ImageEffect::Image *)> release(
            image, [](ImageEffect::Image *i) { i->releaseReference(); });
          ImageRect rect;
          if (!imageRect(*image, rect)) {
            _failed = true; // an image, but not one a texture can have
            return nullptr;
          }
          std::unique_ptr<Texture> texture(new Texture(context(), rect.bounds,
                                                       textureDepth(_attachment._effect, format, rect.depth),
                                                       rect.components));
          texture->upload(rect);
          Texture &t = *texture;
          return new Handle(*this, clip, *image, t, std::move(texture));
        } catch (const std::runtime_error &e) {
          // kOfxStatFailed: "the image could not be fetched because it does not exist in the clip
          // at the indicated time and/or region, the plug-in should continue operation, but assume
          // the image was black and transparent" (clipLoadTexture). The image exists here, so the
          // render is wrong: inputFailed() tells the host
          _failed = true;
          if (context().hooks().log) {
            context().hooks().log(e.what());
          }
          return nullptr;
        }
      }

    } // OpenGL
  } // Host
} // OFX

#endif // OFX_SUPPORTS_OPENGLRENDER
