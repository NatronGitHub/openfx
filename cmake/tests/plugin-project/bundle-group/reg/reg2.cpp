// SPDX-License-Identifier: BSD-3-Clause
// manual registration of two factories: REGISTER tinyreg::registerR2 registerTinyR2b
#include "../tiny.h"

TINY_FACTORY(R2Factory, "R2")
TINY_FACTORY(R2bFactory, "R2b")
namespace tinyreg {
void registerR2(OFX::PluginFactoryArray &ids);
void registerR2(OFX::PluginFactoryArray &ids)
{
  static R2Factory p("org.example.reg.R2Plugin", 1, 0);
  ids.push_back(&p);
}
} // namespace tinyreg
void registerTinyR2b(OFX::PluginFactoryArray &ids);
void registerTinyR2b(OFX::PluginFactoryArray &ids)
{
  static R2bFactory p("org.example.reg.R2bPlugin", 1, 0);
  ids.push_back(&p);
}
