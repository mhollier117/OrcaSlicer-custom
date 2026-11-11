#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace Slic3r::BblFilament {

// One thread that runs jobs in order. It owns every plugin call of the filament manager, so the app can wait for it
// before the plugin is unloaded.
class SpoolWorker
{
public:
    using Job = std::function<void()>;

    SpoolWorker();
    ~SpoolWorker(); // drains, then joins

    SpoolWorker(const SpoolWorker&)            = delete;
    SpoolWorker& operator=(const SpoolWorker&) = delete;

    bool post(Job job);         // false (job not taken) while draining or drained
    void drain();               // stop taking jobs, forget the queued ones, wait for the running one. Idempotent.
    void resume();              // take jobs again
    bool accepting() const;
    bool busy() const;          // a job is running or queued

private:
    void run();

    mutable std::mutex      m_mutex;
    std::condition_variable m_wake;  // a job was queued, or the worker should stop
    std::condition_variable m_idle;  // the running job finished
    std::deque<Job>         m_queue;
    std::thread             m_thread;
    bool                    m_accepting{true};
    bool                    m_running{false};
    bool                    m_stop{false};
};

} // namespace Slic3r::BblFilament
