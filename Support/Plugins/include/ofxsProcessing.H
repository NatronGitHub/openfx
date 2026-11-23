#ifndef _ofxsProcessing_h_
#define _ofxsProcessing_h_

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

#include <cassert>
#include <algorithm>

#include "ofxsImageEffect.h"
#include "ofxsMultiThread.h"
#include "ofxsLog.h"

/** @file This file contains a useful base class that can be used to process images 

The code below is not so much a skin on the base OFX classes, but code used in implementing 
specific image processing algorithms. As such it does not sit in the support include lib, but in
its own include directory.
*/

namespace OFX {

    ////////////////////////////////////////////////////////////////////////////////
    // base class to process images with
    class ImageProcessor : public OFX::MultiThread::Processor {
    protected :
        OFX::ImageEffect &_effect;        /**< @brief effect to render with */
        OFX::Image       *_dstImg;        /**< @brief image to process into */
        OfxRectI          _renderWindow;  /**< @brief render window to use */
        OfxPointD         _renderScale;   /**< @brief render scale to use */
        bool             _isEnabledOpenCLRender; /**< @brief is OpenCL Render Enabled */
        bool             _isEnabledCudaRender;   /**< @brief is Cuda Render Enabled */
        bool             _isEnabledMetalRender;   /**< @brief is Metal Render Enabled */
        void*            _pOpenCLCmdQ;           /**< @brief OpenCL Command Queue Handle */
        void*            _pCudaStream;           /**< @brief Cuda Stream Handle */
        void*            _pMetalCmdQ;           /**< @brief Metal Command Queue Handle */

    public :
        /** @brief ctor */
        ImageProcessor(OFX::ImageEffect &effect)
          : _effect(effect)
          , _dstImg(NULL)
          , _isEnabledOpenCLRender(false)
          , _isEnabledCudaRender(false)
          , _isEnabledMetalRender(false)
          , _pOpenCLCmdQ(NULL)
          , _pCudaStream(NULL)
          , _pMetalCmdQ(NULL)
        {
            _renderWindow.x1 = _renderWindow.y1 = _renderWindow.x2 = _renderWindow.y2 = 0;
            _renderScale.x = _renderScale.y = 1.;
        }  
        
        /** @brief set the destination image */
        void setDstImg(OFX::Image *v) {_dstImg = v; }

        /** @brief set the GPU render arguments (OpenCL, CUDA, Metal) */
        void setGPURenderArgs(const OFX::RenderArguments& args)
        {
            _isEnabledOpenCLRender = args.isEnabledOpenCLRender;
            _isEnabledCudaRender = args.isEnabledCudaRender;
            _isEnabledMetalRender = args.isEnabledMetalRender;

            if (_isEnabledOpenCLRender)
            {
                _pOpenCLCmdQ = args.pOpenCLCmdQ;
            }
            if (_isEnabledCudaRender)
            {
                _pCudaStream = args.pCudaStream;
            }
            if (_isEnabledMetalRender)
            {
                _pMetalCmdQ = args.pMetalCmdQ;
            }
        }

        /** @brief reset the render window */
        void setRenderWindow(const OfxRectI& rect, const OfxPointD& rs) {_renderWindow = rect; _renderScale = rs; }

        /** @brief reset the render window, with a render scale of 1 (upstream OpenFX's ofxsProcessing.h has no render scale) */
        void setRenderWindow(const OfxRectI& rect) { const OfxPointD rs = {1., 1.}; setRenderWindow(rect, rs); }

        /** @brief overridden from OFX::MultiThread::Processor. This function is called once on each SMP thread by the base class */
        void multiThreadFunction(unsigned int threadId, unsigned int nThreads)
        {
            OfxRectI win = _renderWindow;

            MultiThread::getThreadRange(threadId, nThreads, _renderWindow.y1, _renderWindow.y2, &win.y1, &win.y2);
            if ( (win.y2 - win.y1) > 0 ) {
                // and render that thread on each
                multiThreadProcessImages(win, _renderScale);
            }
        }
        
        /** @brief called before any MP is done */
        virtual void preProcess(void) {}

        /** @brief this is called by process to actually process images using OpenCL when isEnabledOpenCLRender is true, override in derived classes */
        virtual void processImagesOpenCL(void)
        {
            OFX::Log::print("processImagesOpenCL not implemented");
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        };

        /** @brief this is called by process to actually process images using CUDA when isEnabledCudaRender is true, override in derived classes.
            The default calls processImagesCuda(), so that processors written for upstream OpenFX's ofxsProcessing.h also work. */
        virtual void processImagesCUDA(void)
        {
            processImagesCuda();
        };

        /** @brief upstream OpenFX's name for processImagesCUDA(), called by the default processImagesCUDA(): override either one */
        virtual void processImagesCuda(void)
        {
            OFX::Log::print("processImagesCUDA not implemented");
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        };
        /** @brief this is called by process to actually process images using Metal when isEnabledMetalRender is true, override in derived classes */
        virtual void processImagesMetal(void)
        {
            OFX::Log::print("processImagesMetal not implemented");
            OFX::throwSuiteStatusException(kOfxStatErrUnsupported);
        };

        /** @brief this is called by multiThreadFunction to actually process images, override in derived classes */
        virtual void multiThreadProcessImages(const OfxRectI& window, const OfxPointD& rs) = 0;

        /** @brief called before any MP is done */
        virtual void postProcess(void) {}

        /** @brief called to process everything */
        virtual void process(void)
        {
            // If _dstImg was set, check that the _renderWindow is lying into dstBounds
            if (_dstImg) {
                const OfxRectI& dstBounds = _dstImg->getBounds();
                // is the renderWindow within dstBounds ?
                assert(dstBounds.x1 <= _renderWindow.x1 && _renderWindow.x2 <= dstBounds.x2 &&
                       dstBounds.y1 <= _renderWindow.y1 && _renderWindow.y2 <= dstBounds.y2);
                // exit gracefully in case of error
                if (!(dstBounds.x1 <= _renderWindow.x1 && _renderWindow.x2 <= dstBounds.x2 &&
                      dstBounds.y1 <= _renderWindow.y1 && _renderWindow.y2 <= dstBounds.y2) ||
                    (_renderWindow.x1 >= _renderWindow.x2) ||
                    (_renderWindow.y1 >= _renderWindow.y2)) {
                    return;
                }
            }

            // call the pre MP pass
            preProcess();

            if (_isEnabledOpenCLRender)
            {
                processImagesOpenCL();
            }
            else if (_isEnabledCudaRender)
            {
                processImagesCUDA();
            }
            else if (_isEnabledMetalRender)
            {
                processImagesMetal();
            }
            else // is CPU
            {
                // make sure there are at least 4096 pixels per CPU and at least 1 line par CPU
                unsigned int nCPUs = (unsigned int)(((std::min)((std::max)(0, _renderWindow.x2 - _renderWindow.x1), 4096) *
                                      ((std::max)(0, _renderWindow.y2 - _renderWindow.y1))) / 4096);
                // make sure the number of CPUs is valid (and use at least 1 CPU)
                nCPUs = (std::max)(1u, (std::min)(nCPUs, OFX::MultiThread::getNumCPUs()));

                // call the base multi threading code, should put a pre & post thread calls in too
                multiThread(nCPUs);
            }

            // call the post MP pass
            postProcess();
        }
       
    };


};
#endif
