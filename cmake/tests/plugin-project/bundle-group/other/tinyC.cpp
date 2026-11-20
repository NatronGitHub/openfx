// SPDX-License-Identifier: BSD-3-Clause
// a plug-in in another directory, with its own compile settings and an explicit ID
#include "tiny.h"
#include "tinyc.h" // its INCLUDE_DIRECTORIES

#ifndef TINY_C_ONLY
#error "the plug-in's COMPILE_DEFINITIONS were not applied"
#endif
#ifndef TINY_C_OPT
#error "the plug-in's COMPILE_OPTIONS were not applied"
#endif
#if defined(TINY_AB)
#error "the COMPILE_DEFINITIONS of another plug-in leaked"
#endif

#define kPluginIdentifier "org.example.tiny.ExplicitC"
TINY_FACTORY(TinyCFactory, TINY_C_LABEL)
static TinyCFactory p(kPluginIdentifier, 1, 0);
mRegisterPluginFactoryInstance(p)
