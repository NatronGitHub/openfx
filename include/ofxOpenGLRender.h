
#ifndef _ofxOpenGLRender_h_
#define _ofxOpenGLRender_h_

// Copyright OpenFX and contributors to the OpenFX project.
// SPDX-License-Identifier: BSD-3-Clause

/** @file ofxOpenGLRender.h
 Compatibility header. Since OFX 1.5 the OpenGL render suite and properties are
 defined in ofxGPURender.h, together with the CUDA, Metal and OpenCL render
 extensions. Upstream reduced this file to an empty stub; this fork keeps it as a
 shim so that existing hosts and plug-ins that include it keep compiling.
*/
#include "ofxGPURender.h"

#endif
