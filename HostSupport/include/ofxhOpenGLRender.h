#ifndef OFXH_OPENGL_RENDER_H
#define OFXH_OPENGL_RENDER_H

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxhOpenGLRender.h

  OpenGL renders of HostSupport effect instances (ofxGPURender.h, OFX 1.5.1), on the contexts
  of ofxhOpenGLContext.h and the textures of ofxhOpenGLTexture.h. Part of
  openfx::HostSupportOpenGL; needs OFX_SUPPORTS_OPENGLRENDER.

  - chooseRenderMode(): CPU or OpenGL, from kOfxImageEffectPropOpenGLRenderSupported and
    kOfxImageEffectPropCPURenderSupported, and the CPU retry after a failed OpenGL render;
  - Attachment: the context an instance is attached to (kOfxActionOpenGLContextAttached and
    Detached) and its context data (kNatronOfxImageEffectPropOpenGLContextData);
  - RenderSession: one OpenGL render (Begin/Render/EndSequenceRender with
    kOfxImageEffectPropOpenGLEnabled 1): it attaches the instance, provides the default
    ClipInstance::loadTexture() (input clips: the image uploaded to a texture; the output clip:
    the render target) for the host's override to call, deletes the textures the plug-in
    didn't free, and reads the output back.

  A host's render with OpenGL, one context per render tree and one render action at a time per
  context (renders on other threads that a fetch starts on demand use tryAcquire() with a key of
  their own, and the CPU if it gives nothing):

    ContextPool::Lease lease(pool, tree);
    CurrentContext current(*lease.context());
    RenderSession session(attachmentOf(effect), current);
    if (session.ok()) {
      session.setOutput(*outputImage, renderWindow);  // the host's output image
      // the host's ClipInstance::loadTexture() override calls session.loadTexture() during:
      st = effect.renderAction(..., openGLRender = true, ...);
      if (st == kOfxStatOK && !session.inputFailed()) session.readOutput();
    }
    // the mode of each render: chooseRenderMode(effect, hostHasGL && !attachment.failed());
    // after a failure, retry on the CPU if chooseRenderMode(effect, false) == RenderMode::CPU
*/

#ifdef OFX_SUPPORTS_OPENGLRENDER

#include "ofxhClip.h"
#include "ofxhImageEffect.h"
#include "ofxhOpenGLContext.h"
#include "ofxhOpenGLTexture.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <set>

namespace OFX {
  namespace Host {
    namespace OpenGL {

      /// how to render an instance
      enum class RenderMode {
        CPU,     ///< kOfxImageEffectPropOpenGLEnabled 0
        OpenGL,  ///< kOfxImageEffectPropOpenGLEnabled 1, in a RenderSession
        None     ///< the plug-in needs OpenGL, and the host has none
      };

      /// The mode of the next render of effect, with an OpenGL context (hostHasOpenGL) or not:
      /// OpenGL if both can; else the CPU if the plug-in can (not "needed", and its
      /// kOfxImageEffectPropCPURenderSupported isn't "false"); else None. Also the retry
      /// after a failed OpenGL render: chooseRenderMode(effect, false).
      /// Reads the instance properties, which the plug-in may change in kOfxActionInstanceChanged.
      /// Pass hostHasOpenGL false for an instance whose Attachment::failed().
      RenderMode chooseRenderMode(const ImageEffect::Instance &effect, bool hostHasOpenGL);

      /// The OpenGL context an instance is attached to. The host keeps one per instance, and
      /// destroys it before the instance (that sends kOfxActionOpenGLContextDetached) and before
      /// the contexts.
      class Attachment {
      public:
        explicit Attachment(ImageEffect::Instance &effect) : _effect(effect) {}
        /// detach()
        ~Attachment();
        Attachment(const Attachment &) = delete;
        Attachment &operator=(const Attachment &) = delete;

        /// Sends kOfxActionOpenGLContextDetached if attached, with the context it was attached to
        /// current on this thread (waiting until no other thread holds it). Call it from a
        /// thread that holds no context: else (unless it holds the attached context and no
        /// render of the instance runs) Detached isn't sent, the instance stays attached, and this
        /// returns kOfxStatFailed at once. kOfxStatErrMemory: still attached.
        OfxStatus detach();
        /// Attached or Detached returned kOfxStatFailed (or another error but
        /// kOfxStatErrMemory): "the host should not attempt to run the plug-in in OpenGL render
        /// mode", so no RenderSession on this attachment is ok() any more
        bool failed() const;
        /// the context, nullptr if not attached
        Context *context() const;
        /// the context data the plug-in returned from kOfxActionOpenGLContextAttached
        void *contextData() const;

      private:
        friend class RenderSession;
        OfxStatus beginRender(CurrentContext &current);
        void endRender();
        OfxStatus detachLocked(std::unique_lock<std::mutex> &lock, CurrentContext *current);
        class Switch;

        ImageEffect::Instance &_effect;
        mutable std::mutex _mutex;
        std::condition_variable _changed;
        Context *_context = nullptr;
        void *_contextData = nullptr;
        int _renders = 0;          ///< RenderSessions in progress on _context
        bool _switching = false;   ///< a thread is detaching or attaching
        bool _failed = false;      ///< see failed()
      };

      /// One OpenGL render of an instance, on the context current in current, for the duration
      /// of the render actions. Destroy it before the CurrentContext.
      class RenderSession {
      public:
        /// Attaches the instance to the context if it isn't (kOfxActionOpenGLContextAttached,
        /// after kOfxActionOpenGLContextDetached on its previous context). An instance renders on
        /// one context at a time: this waits for its renders on another context. While it waits
        /// or detaches, current is released (with its enclosing scopes on this thread), then
        /// made current again.
        RenderSession(Attachment &attachment, CurrentContext &current);
        /// deletes the textures the plug-in didn't free, ends the render
        ~RenderSession();
        RenderSession(const RenderSession &) = delete;
        RenderSession &operator=(const RenderSession &) = delete;

        /// the kOfxActionOpenGLContextAttached status (kOfxStatOK if attached already)
        OfxStatus status() const { return _status; }
        /// render with OpenGL: attached (kOfxStatOK or kOfxStatReplyDefault)
        bool ok() const { return _status == kOfxStatOK || _status == kOfxStatReplyDefault; }
        Context &context() const { return _current.context(); }

        /// Sets the output of the render: a cleared texture of renderWindow (within image's
        /// bounds) with image's depth and components, bound as the render target, with the
        /// viewport on it (the matrices are left to the plug-in); readOutput() copies it to image.
        /// image must outlive the session.
        Texture &setOutput(ImageEffect::Image &image, const OfxRectI &renderWindow);
        /// the output texture, nullptr before setOutput()
        Texture *output() const { return _output.get(); }
        /// waits for OpenGL to finish, and copies the output texture to the output image
        void readOutput();

        /// ClipInstance::loadTexture() for a clip of this render: the output clip's texture,
        /// bound as the render target, or the image of an input clip (clip.getImage()) in a new
        /// texture, in the format the plug-in asked for (format, else kOfxOpenGLPropPixelDepth).
        /// One render action at a time per context: during clip.getImage() the context is
        /// suspended but stays reserved to this render (CurrentContext::suspend(): the session's
        /// scope must be the innermost one of the thread), so the plug-in's GL state persists
        /// across its render action. Renders on other threads during the fetch take another
        /// context (ContextPool::tryAcquire()) or render on the CPU, and never wait for this one.
        /// A render nested on this thread may use this context (a new scope on it): its GL
        /// state changes (bound textures, program, enables, matrices) are the nested plug-in's,
        /// and only the framebuffer and the viewport are restored after it.
        /// nullptr if there is no image or no output (clipLoadTexture: kOfxStatFailed); throws
        /// std::bad_alloc if OpenGL is out of memory (kOfxStatErrMemory).
        ImageEffect::Texture *loadTexture(ImageEffect::ClipInstance &clip, OfxTime time, const char *format,
                                          const OfxRectD *region);
        /// A clip had an image that couldn't be given as a texture (an OpenGL error, no texture
        /// format), or the context was lost (it couldn't be made current again after
        /// getImage(): the rest of the plug-in's render ran without a current context, and the
        /// session leaves their OpenGL names undeleted): loadTexture() returned nullptr, so the plug-in
        /// rendered with a black input (clipLoadTexture's kOfxStatFailed). Drop this output, or
        /// render it again on the CPU. (Unlike Attachment::failed(), it concerns this render only.)
        bool inputFailed() const { return _failed; }
        /// the texture handles not freed (clipFreeTexture) yet (a diagnostic, for tests)
        int liveTextures() const { return static_cast<int>(_handles.size()); }

      private:
        class Handle;
        friend class Handle;
        Attachment &_attachment;
        CurrentContext &_current;
        OfxStatus _status;
        std::unique_ptr<Texture> _output;
        ImageEffect::Image *_outputImage = nullptr;
        std::set<Handle *> _handles;
        bool _failed = false;
      };

    } // OpenGL
  } // Host
} // OFX

#endif // OFX_SUPPORTS_OPENGLRENDER

#endif // OFXH_OPENGL_RENDER_H
