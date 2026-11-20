// SPDX-License-Identifier: BSD-3-Clause
// self-registering, in a group with REGISTER plug-ins
#include "../tiny.h"

TINY_FACTORY(R3Factory, "R3")
static R3Factory p("org.example.reg.R3Plugin", 1, 0);
mRegisterPluginFactoryInstance(p)
