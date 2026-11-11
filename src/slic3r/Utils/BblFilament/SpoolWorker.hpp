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
    // Stops taking jobs, forgets the queued ones (destroying them outside the lock) and waits for the running one.
    // Idempotent. It returns with the worker closed; only a later resume() reopens it.
    void drain();
    void resume();              // take jobs again; waits for a drain in progress to finish first
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
    int                     m_draining{0};   // drain() calls in progress
    bool                    m_stop{false};
};

} // namespace Slic3r::BblFilament
