#pragma once

#include "SpoolWorker.hpp"

namespace Slic3r::BblFilament {

// The one spool worker of the process; created on first use and never destroyed (the network plug-in singleton is
// destroyed during static destruction, after any static of ours, and must still find it).
SpoolWorker& shared_worker();

// Changes every time the plug-in's agent goes away.
unsigned long long plugin_generation();

// The worker must not be inside a plug-in call while the agent is destroyed or the DLL unloaded.
void before_plugin_agent_destroyed(); // ++generation; drains the worker
void after_plugin_agent_created();    // lets the worker take jobs again

} // namespace Slic3r::BblFilament
