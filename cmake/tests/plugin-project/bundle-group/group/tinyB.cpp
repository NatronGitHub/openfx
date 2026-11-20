// SPDX-License-Identifier: BSD-3-Clause
// a self-registering plug-in; TinyA and TinyB are in the same directory
#include "tiny.h"
#include "tinyab.h" // INCLUDE_DIRECTORIES of TinyA and TinyB (the same: once)

#if !defined(TINY_AB) || !defined(TINY_AB_SHARED)
#error "the plug-in's COMPILE_DEFINITIONS were not applied"
#endif
#if defined(TINY_C_ONLY) || defined(TINY_C_OPT)
#error "the COMPILE_DEFINITIONS or COMPILE_OPTIONS of another plug-in leaked"
#endif

TINY_FACTORY(TinyBFactory, "TinyB " TINY_AB_LABEL)
static TinyBFactory p("org.example.tiny.TinyBPlugin", 1, 0);
mRegisterPluginFactoryInstance(p)
