#ifndef _ofxsRenderMessage_H_
#define _ofxsRenderMessage_H_
// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file The render errors of the sample plug-ins. The render action's statuses (OFX 1.5.1
    ofxImageEffect.h) are kOfxStatOK, kOfxStatErrMemory, kOfxStatFailed ("something wrong, but no
    error code appropriate, plugin to post message") and kOfxStatErrFatal: the samples post a
    persistent message, fail with kOfxStatFailed, and clear the message once a render works. */

#include <atomic>

#include "ofxsImageEffect.h"

namespace OFX {

  /** @brief The error message of an instance's renders. Atomic, for a fully safe render, whose
      render calls on one instance may be concurrent. */
  class RenderMessage {
  public:
    /** @brief post the message, then fail the render with kOfxStatFailed */
    void fail(ImageEffect& effect, const char* message)
    {
      effect.setPersistentMessage(Message::eMessageError, "", message);
      _posted = true;
      throwSuiteStatusException(kOfxStatFailed);
    }

    /** @brief the images are right: remove the message of an earlier failed render */
    void clear(ImageEffect& effect)
    {
      if (_posted.exchange(false)) {
        effect.clearPersistentMessage();
      }
    }

    /** @brief the output image at the render time. It must exist, and have the depth and
        components of the clip, from which render() chose the processor. */
    auto_ptr<Image> fetchOutput(ImageEffect& effect, Clip& clip, const RenderArguments& args)
    {
      auto_ptr<Image> dst(clip.fetchImage(args.time));
      if (!dst.get()) {
        fail(effect, "The host gave no output image");
      }
      if (dst->getPixelDepth() != clip.getPixelDepth() || dst->getPixelComponents() != clip.getPixelComponents()) {
        fail(effect, "The host gave an output image with the wrong depth or components");
      }
      effect.checkBadRenderScaleOrField(dst, args);
      return dst;
    }

    /** @brief a source image must have the depth and components of the output image */
    void checkSource(ImageEffect& effect, const Image& src, const Image& dst)
    {
      if (src.getPixelDepth() != dst.getPixelDepth() || src.getPixelComponents() != dst.getPixelComponents()) {
        fail(effect, "The source and output images differ in depth or components");
      }
    }

  private:
    std::atomic<bool> _posted{false};
  };

} // namespace OFX

#endif
