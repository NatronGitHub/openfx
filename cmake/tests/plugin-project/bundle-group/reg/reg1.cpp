// SPDX-License-Identifier: BSD-3-Clause
// manual registration: REGISTER registerTinyR1
#include "../tiny.h"

TINY_FACTORY(R1Factory, "R1")
void registerTinyR1(OFX::PluginFactoryArray &ids);
void registerTinyR1(OFX::PluginFactoryArray &ids)
{
  static R1Factory p("org.example.reg.R1Plugin", 1, 0);
  ids.push_back(&p);
}
