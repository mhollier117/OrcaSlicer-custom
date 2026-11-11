#include "PluginGate.hpp"

#include <atomic>

namespace Slic3r::BblFilament {

namespace {
std::atomic<unsigned long long>& generation()
{
    static std::atomic<unsigned long long>* g = new std::atomic<unsigned long long>(0);
    return *g;
}
} // namespace

SpoolWorker& shared_worker()
{
    static SpoolWorker* worker = new SpoolWorker;
    return *worker;
}

unsigned long long plugin_generation() { return generation().load(); }

void before_plugin_agent_destroyed()
{
    generation().fetch_add(1);
    shared_worker().drain();
}

void after_plugin_agent_created() { shared_worker().resume(); }

} // namespace Slic3r::BblFilament
