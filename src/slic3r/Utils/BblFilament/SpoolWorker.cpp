#include "SpoolWorker.hpp"

#include <boost/log/trivial.hpp>

#include <exception>
#include <utility>

namespace Slic3r::BblFilament {

SpoolWorker::SpoolWorker() = default;

SpoolWorker::~SpoolWorker()
{
    drain();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    if (m_thread.joinable()) m_thread.join();
}

bool SpoolWorker::post(Job job)
{
    if (!job) return false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_accepting || m_stop) return false;
        m_queue.push_back(std::move(job));
        if (!m_thread.joinable()) m_thread = std::thread([this] { run(); });
    }
    m_wake.notify_one();
    return true;
}

void SpoolWorker::drain()
{
    // The forgotten jobs are destroyed outside the lock: a job's captures may run code (and take other locks) when they die.
    std::deque<Job> dropped;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_accepting = false;
        ++m_draining;
        dropped.swap(m_queue);
    }
    dropped.clear();
    {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_idle.wait(lock, [this] { return !m_running; });
        --m_draining;
    }
    m_idle.notify_all();
}

void SpoolWorker::resume()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_idle.wait(lock, [this] { return m_draining == 0; }); // a drain in progress finishes first and stays closed until now
    if (!m_stop) m_accepting = true;
}

bool SpoolWorker::accepting() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_accepting;
}

bool SpoolWorker::busy() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_running || !m_queue.empty();
}

void SpoolWorker::run()
{
    std::unique_lock<std::mutex> lock(m_mutex);
    for (;;) {
        m_wake.wait(lock, [this] { return m_stop || !m_queue.empty(); });
        if (m_queue.empty()) {
            if (m_stop) return;
            continue;
        }
        Job job = std::move(m_queue.front());
        m_queue.pop_front();
        m_running = true;
        lock.unlock();
        try {
            job();
        } catch (const std::exception&) {
            BOOST_LOG_TRIVIAL(warning) << "bbl filament: a worker job threw an exception";
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "bbl filament: a worker job threw";
        }
        job = nullptr; // release what the job captured outside the lock
        lock.lock();
        m_running = false;
        m_idle.notify_all();
    }
}

} // namespace Slic3r::BblFilament
