// SPDX-License-Identifier: BSD-3-Clause
// configured only (never compiled): a REGISTER function named getPluginIDs in another namespace
// gives no warning (the plug-in's factory: "t.BPlugin")
namespace ns {
void getPluginIDs(OFX::PluginFactoryArray &ids)
{
}
}
