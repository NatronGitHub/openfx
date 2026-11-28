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
    Detached), or the contexts, with their context data, of a plug-in that returns
    kNatronOfxImageEffectPropOpenGLContextData;
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
#include <vector>

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

      class RenderSession;

      /// After an OpenGL action of session that returned st: whether OpenGL failed, in a way after
      /// which the host may render on the CPU: an input texture failed (RenderSession::inputFailed());
      /// kOfxStatGLRenderFailed, "the host should retry the render with OpenGL rendering disabled";
      /// kOfxStatGLOutOfMemory, after which "the host may choose to free resources on the GPU and
      /// retry the OpenGL render, rather than immediately falling back to CPU rendering";
      /// kOfxStatErrMemory from clipLoadTexture, "after which the host can decide to retry the
      /// operation with CPU based processing" (ofxGPURender.h). The statuses are
      /// ImageEffect::gpuRenderFailed() (ofxhImageEffect.h), shared with the other GPU components.
      /// After kOfxStatGLOutOfMemory, a host with a ContextPool may first release its idle contexts
      /// (flushResources(), ofxhOpenGLContext.h) and render with OpenGL again.
      bool openGLFailed(const RenderSession &session, OfxStatus st);

      /// After an OpenGL action of effect in session that returned st: the host may render it again
      /// on the CPU. OpenGL failed (openGLFailed()), and the plug-in can render on the CPU
      /// (chooseRenderMode(effect, false) is RenderMode::CPU: not "needed", and
      /// kOfxImageEffectPropCPURenderSupported isn't "false")
      bool cpuRetryAllowed(const ImageEffect::Instance &effect, const RenderSession &session, OfxStatus st);

      /// The OpenGL contexts an instance is attached to. The host keeps one per instance, and
      /// destroys it before the instance (that sends kOfxActionOpenGLContextDetached) and before
      /// the contexts.
      ///
      /// OFX 1.5.1: "A host cannot call kOfxActionOpenGLContextAttached on the same instance without
      /// an intervening kOfxActionOpenGLContextDetached": one context at a time, renders of the
      /// instance on another one wait, and a switch detaches first. With OFX_EXTENSIONS_NATRON, a
      /// plug-in that returns kNatronOfxImageEffectPropOpenGLContextData from Attached is attached to
      /// several contexts at once, each with its data: "This permits running several parallel renders
      /// on different OpenGL contexts, but it violates the OpenFX spec [...] If the plugin returns
      /// NULL [...], it is supposed to follow the OpenFX spec" (ofxNatron.h).
      class Attachment {
      public:
        explicit Attachment(ImageEffect::Instance &effect) : _effect(effect) {}
        /// detach()
        ~Attachment();
        Attachment(const Attachment &) = delete;
        Attachment &operator=(const Attachment &) = delete;

        /// Sends kOfxActionOpenGLContextDetached on each context the instance is attached to, with
        /// that context current on this thread (waiting until no other thread holds it). Call it
        /// from a thread that holds no context: else (unless it holds the one attached context and
        /// no render of the instance runs) Detached isn't sent, the instance stays attached, and
        /// this returns kOfxStatFailed at once. kOfxStatErrMemory: still attached there.
        OfxStatus detach();
        /// Attached or Detached returned kOfxStatFailed (or another error but
        /// kOfxStatErrMemory): "the host should not attempt to run the plug-in in OpenGL render
        /// mode", so no RenderSession on this attachment is ok() any more
        bool failed() const;
        /// the contexts the instance is attached to (more than one only with context data)
        int contexts() const;
        /// the context it is attached to (OFX mode: the only one; with context data, the first),
        /// nullptr if none. A render's context data is RenderSession::contextData().
        Context *context() const;

      private:
        friend class RenderSession;
        struct Attached {
          Context *context;
          void *data;      ///< kNatronOfxImageEffectPropOpenGLContextData, or nullptr
          int renders;     ///< RenderSessions in progress on context
        };
        OfxStatus beginRender(CurrentContext &current, void *&data);
        void endRender(Context *context);
        Attached *find(const Context *context);
        /// Detached on context (attached), made current on this thread; erased if it was sent
        OfxStatus detachLocked(std::unique_lock<std::mutex> &lock, CurrentContext *current, Context *context);
        /// detachLocked() on every context but keep, until one isn't detached (then still
        /// attached there)
        OfxStatus detachAll(std::unique_lock<std::mutex> &lock, CurrentContext *current, Context *keep = nullptr);
        class Switch;

        ImageEffect::Instance &_effect;
        mutable std::mutex _mutex;
        std::condition_variable _changed;
        std::vector<Attached> _attached;
        bool _switching = false;   ///< a thread is detaching or attaching
        bool _attachOnly = false;  ///< ... only attaching, every attached context having context
                                   ///< data: renders on those contexts join (none during a detach)
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

        /// the context data of the instance on this session's context (pass it as
        /// kNatronOfxImageEffectPropOpenGLContextData to the render actions), nullptr if none
        void *contextData() const { return _contextData; }
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
        /// texture, in the format the plug-in asked for (format, else kOfxOpenGLPropPixelDepth): a
        /// format that isn't kOfxBitDepthByte, Short, Half or Float, or a texture of that format
        /// that OpenGL can't create, is an input failure (inputFailed()), never another format.
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
        void *_contextData = nullptr;
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
