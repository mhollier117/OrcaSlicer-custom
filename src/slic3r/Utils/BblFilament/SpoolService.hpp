#pragma once

#include "Spool.hpp"
#include "SpoolCloud.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::BblFilament {

struct Session
{
    bool               signed_in{false};
    std::string        uid;             // Bambu user id
    unsigned long long generation{0};   // changes whenever the network plugin is (re)loaded
    bool               closing{false};
};

enum class FailKind { NotSignedIn, Unavailable, BadRequest, NotFound, Rejected, Unknown };

struct Accept // the immediate answer to a request
{
    bool     ok{true};
    FailKind kind{FailKind::Unknown}; // meaningful when !ok
};

struct SyncState
{
    bool        signed_in{false};
    bool        is_syncing{false}; // a pull or a write is queued, running or wanted (config fetches do not count)
    bool        is_pulling{false};
    long long   last_synced_at{0}; // unix seconds of the last complete pull or successful write; 0 = never
    bool        has_error{false};
    FailKind    error_kind{FailKind::Unknown};
    std::string error_message;     // the server's short text, may be empty
};

struct ServiceStatus // for diagnostics
{
    SyncState state;
    bool      have_list{false}; // a complete pull has happened in this session
    int       spool_count{0};   // of view()
    int       pending_writes{0};// queued + running
    bool      pull_wanted{false};
    bool      paging_suspect{false};
    int       skipped_rows{0};
};

struct ServiceEvents // all optional; called on the service's thread
{
    std::function<void()>                                                                                              spools_changed; // view() changed
    std::function<void()>                                                                                              state_changed;  // state() changed
    std::function<void(int added, int kept)>                                                                           pull_done;      // a complete pull replaced the list
    std::function<void(const std::string& op, const std::string& spool_id)>                                            push_done;      // op: "create" | "update" | "delete"
    std::function<void(const std::string& op, const std::string& spool_id, FailKind kind, const std::string& message)> push_failed;
    std::function<void(const json& config)>                                                                            config_fetched;
};

struct ServiceDeps
{
    std::shared_ptr<ISpoolCloud>               cloud;       // used inside jobs only
    std::function<bool(std::function<void()>)> run_async;   // SpoolWorker::post in the app; false = not accepted
    std::function<void(std::function<void()>)> post_to_ui;  // may be called from the worker thread
    std::function<Session()>                   session;     // service thread
    FilamentLookup                             lookup;      // service thread
    std::function<long long()>                 now;         // unix seconds
};

// Lives on one thread (the UI thread in the app). Not thread-safe. It never calls the cloud itself: it hands jobs to
// run_async, and each job hands its result back through post_to_ui.
class SpoolService
{
public:
    explicit SpoolService(ServiceDeps deps);
    ~SpoolService();

    SpoolService(const SpoolService&)            = delete;
    SpoolService& operator=(const SpoolService&) = delete;

    ServiceEvents events;

    std::vector<Spool> view() const; // what the page shows
    SyncState          state() const;
    ServiceStatus      status() const;

    void   request_pull();
    Accept add(const json& page_spool, int quantity = 1);
    Accept batch(const json& creates, const json& updates);
    Accept update(const json& page_patch);
    Accept remove(const std::vector<std::string>& ids);

    void        request_config(bool force);
    const json* cached_config() const; // null until fetched

    void reset(); // the account, the sign-in state or the plugin changed

private:
    struct Op
    {
        enum class Kind { Create, Update, Delete, Config };
        enum class State { Queued, Running, Settled };

        Kind                     kind{Kind::Create};
        State                    state{State::Queued};
        unsigned long long       serial{0};
        json                     body;  // create
        std::string              id;    // update
        json                     patch; // update
        std::vector<std::string> ids;   // delete
    };

    struct JobResult
    {
        bool        ok{false};
        FailKind    kind{FailKind::Unknown};
        std::string message;
        json        body;
        ListResult  list;
    };

    static JobResult from_reply(const CloudReply& reply);

    using Work = std::function<JobResult(ISpoolCloud&, const std::function<bool()>&)>;

    enum class JobKind { None, Pull, Write, Config };

    std::vector<Spool> compute_view(std::size_t op_count) const;
    bool               session_matches(const Session& s) const;
    void               sync_session();
    bool               do_reset(bool fire, const Session& s); // false when an event handler destroyed the service
    bool               pristine() const;

    Accept enqueue_update(const json& patch, bool& queued);
    void   queue_op(Op op);

    // The functions below return false when an event handler destroyed the service: touch no member then.
    bool pump();
    bool start_op(std::size_t index);
    bool start_pull();
    bool launch(Work work, JobKind kind, unsigned long long serial, const Session& s);
    void on_completion(unsigned long long epoch, const Session& started, bool is_pull, unsigned long long serial, unsigned long long ticket, JobResult result);
    bool finish(bool is_pull, unsigned long long serial, JobResult result);
    bool finish_pull(const JobResult& r);
    bool finish_op(unsigned long long serial, const JobResult& r);

    void set_error(FailKind kind, const std::string& message);
    void clear_error();
    bool fire_state_changed();
    bool fire_spools_changed();

    // Calls a copy of an event handler (it may replace `events`); false when the service was destroyed meanwhile.
    template<class Fn, class... Args> bool call_event(const Fn& fn, Args&&... args)
    {
        if (!fn) return true;
        Fn                 copy = fn;
        std::weak_ptr<int> weak = alive_;
        copy(std::forward<Args>(args)...);
        return !weak.expired();
    }
    long long now() const;

    ServiceDeps deps_;

    std::vector<Spool>  truth_;
    bool                have_list_{false};
    std::deque<Op>      ops_;
    bool                pull_wanted_{false};
    json                config_;
    bool                have_config_{false};
    bool                has_error_{false};
    FailKind            error_kind_{FailKind::Unknown};
    std::string         error_message_;
    unsigned long long  epoch_{0};
    unsigned long long  next_serial_{1};
    long long           last_synced_at_{0};
    bool                paging_suspect_{false};
    int                 skipped_rows_{0};

    bool                job_running_{false};
    JobKind             running_kind_{JobKind::None};
    unsigned long long  running_serial_{0};
    unsigned long long  running_ticket_{0};
    unsigned long long  next_ticket_{1};
    std::shared_ptr<std::atomic<bool>> cancel_;

    bool               session_signed_{false};
    std::string        session_uid_;
    unsigned long long session_generation_{0};

    std::shared_ptr<int> alive_;
};

} // namespace Slic3r::BblFilament
