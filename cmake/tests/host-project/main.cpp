// SPDX-License-Identifier: BSD-3-Clause
// The plug-in cache of HostSupport, with the PUBLIC definitions of openfx::HostSupport
#include "ofxhPluginCache.h"
#ifndef OFX_SUPPORTS_MULTITHREAD
#error "OFX_SUPPORTS_MULTITHREAD is not a usage requirement of openfx::HostSupport"
#endif
int main() { OFX::Host::PluginCache::getPluginCache(); return 0; }
