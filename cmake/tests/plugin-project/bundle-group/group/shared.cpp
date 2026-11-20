// SPDX-License-Identifier: BSD-3-Clause
// listed by TinyA and TinyB, with the same settings: compiled once per bundle
#include "tinyab.h"
#if !defined(TINY_AB) || !defined(TINY_AB_SHARED)
#error "the COMPILE_DEFINITIONS of TinyA and TinyB were not applied"
#endif
int tinySharedHelper() { return 1; }
