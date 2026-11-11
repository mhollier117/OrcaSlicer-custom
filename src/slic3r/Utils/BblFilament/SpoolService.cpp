#include "SpoolService.hpp"

#include <algorithm>
#include <set>
#include <utility>

namespace Slic3r::BblFilament {

namespace {

FailKind to_fail_kind(CloudError e)
{
    switch (e) {
    case CloudError::Unavailable: return FailKind::Unavailable;
    case CloudError::NotFound: return FailKind::NotFound;
    case CloudError::Rejected: return FailKind::Rejected;
    default: return FailKind::Unknown;
    }
}

bool is_signed_in(const Session& s) { return s.signed_in && !s.closing; }

bool views_equal(const std::vector<Spool>& a, const std::vector<Spool>& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (spool_to_page(a[i]) != spool_to_page(b[i])) return false;
    return true;
}

const Spool* find_spool(const std::vector<Spool>& v, const std::string& id)
{
    auto it = std::find_if(v.begin(), v.end(), [&](const Spool& s) { return s.id == id; });
    return it == v.end() ? nullptr : &*it;
}

std::string string_field(const json& j, const char* key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

} // namespace

SpoolService::SpoolService(ServiceDeps deps) : deps_(std::move(deps)), alive_(std::make_shared<int>(0)) {}

SpoolService::~SpoolService()
{
    if (cancel_) cancel_->store(true);
}

long long SpoolService::now() const { return deps_.now ? deps_.now() : 0; }

// ---------------------------------------------------------------------------------------------------------------------
// views and state

std::vector<Spool> SpoolService::compute_view(std::size_t op_count) const
{
    std::vector<Spool> v = truth_;
    const std::size_t  n = std::min(op_count, ops_.size());
    for (std::size_t i = 0; i < n; ++i) {
        const Op& op = ops_[i];
        if (op.kind == Op::Kind::Update) {
            auto it = std::find_if(v.begin(), v.end(), [&](const Spool& s) { return s.id == op.id; });
            if (it != v.end()) apply_page_patch(*it, op.patch);
        } else if (op.kind == Op::Kind::Delete) {
            v.erase(std::remove_if(v.begin(), v.end(), [&](const Spool& s) { return std::find(op.ids.begin(), op.ids.end(), s.id) != op.ids.end(); }),
                    v.end());
        }
    }
    return v;
}

bool SpoolService::session_matches(const Session& s) const
{
    return is_signed_in(s) == session_signed_ && s.uid == session_uid_ && s.generation == session_generation_;
}

std::vector<Spool> SpoolService::view() const
{
    const Session s = deps_.session ? deps_.session() : Session{};
    if (!is_signed_in(s)) return {};
    if (!session_matches(s)) return {};
    return compute_view(ops_.size());
}

SyncState SpoolService::state() const
{
    SyncState st;
    const Session s = deps_.session ? deps_.session() : Session{};
    st.signed_in    = is_signed_in(s);
    st.is_pulling   = job_running_ && running_kind_ == JobKind::Pull;
    // A config fetch fires no state_changed when it ends, so it must not count as "syncing".
    const bool write_queued = std::any_of(ops_.begin(), ops_.end(), [](const Op& o) { return o.kind != Op::Kind::Config && o.state == Op::State::Queued; });
    st.is_syncing           = (job_running_ && running_kind_ != JobKind::Config) || write_queued || pull_wanted_;
    st.last_synced_at = last_synced_at_;
    st.has_error      = has_error_;
    st.error_kind     = error_kind_;
    st.error_message  = error_message_;
    return st;
}

ServiceStatus SpoolService::status() const
{
    ServiceStatus st;
    st.state          = state();
    st.have_list      = have_list_;
    st.spool_count    = static_cast<int>(view().size());
    st.pending_writes = static_cast<int>(std::count_if(ops_.begin(), ops_.end(), [](const Op& o) {
        return o.kind != Op::Kind::Config && o.state != Op::State::Settled;
    }));
    st.pull_wanted    = pull_wanted_;
    st.paging_suspect = paging_suspect_;
    st.skipped_rows   = skipped_rows_;
    return st;
}

const json* SpoolService::cached_config() const { return have_config_ ? &config_ : nullptr; }

bool SpoolService::fire_state_changed() { return call_event(events.state_changed); }

bool SpoolService::fire_spools_changed() { return call_event(events.spools_changed); }

void SpoolService::set_error(FailKind kind, const std::string& message)
{
    has_error_     = true;
    error_kind_    = kind;
    error_message_ = message;
}

void SpoolService::clear_error()
{
    has_error_ = false;
    error_message_.clear();
}

// ---------------------------------------------------------------------------------------------------------------------
// reset and session

bool SpoolService::pristine() const
{
    return !have_list_ && truth_.empty() && ops_.empty() && !pull_wanted_ && !job_running_ && !have_config_ && !has_error_;
}

bool SpoolService::do_reset(bool fire, const Session& s)
{
    ++epoch_;
    if (cancel_) cancel_->store(true);
    cancel_.reset();
    truth_.clear();
    ops_.clear();
    pull_wanted_ = false;
    config_      = json();
    have_config_ = false;
    clear_error();
    have_list_      = false;
    last_synced_at_ = 0;
    paging_suspect_ = false;
    skipped_rows_   = 0;
    job_running_    = false;
    running_kind_   = JobKind::None;
    running_serial_ = 0;
    running_ticket_ = 0;
    session_signed_     = is_signed_in(s);
    session_uid_        = s.uid;
    session_generation_ = s.generation;
    if (fire) {
        if (!fire_spools_changed()) return false;
        if (!fire_state_changed()) return false;
    }
    return true;
}

void SpoolService::reset() { do_reset(true, deps_.session ? deps_.session() : Session{}); }

// Called at the start of every request: a changed account, sign-in state or plugin generation starts a fresh state.
void SpoolService::sync_session() { refresh_session(); }

bool SpoolService::refresh_session()
{
    const Session s = deps_.session ? deps_.session() : Session{};
    if (session_matches(s)) return false;
    do_reset(!pristine(), s);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// requests

void SpoolService::queue_op(Op op)
{
    op.serial = next_serial_++;
    ops_.push_back(std::move(op));
}

void SpoolService::request_pull()
{
    sync_session();
    if (!is_signed_in(deps_.session ? deps_.session() : Session{})) return;
    if (pull_wanted_ || (job_running_ && running_kind_ == JobKind::Pull)) return;
    pull_wanted_ = true;
    if (!fire_state_changed()) return;
    pump();
}

Accept SpoolService::add(const json& page_spool, int quantity)
{
    sync_session();
    if (!is_signed_in(deps_.session ? deps_.session() : Session{})) return {false, FailKind::NotSignedIn};
    if (!page_spool.is_object()) return {false, FailKind::BadRequest};
    quantity        = std::clamp(quantity, 1, 99);
    const json body = create_body(page_spool, deps_.lookup);
    for (int i = 0; i < quantity; ++i) {
        Op op;
        op.kind = Op::Kind::Create;
        op.body = body;
        queue_op(std::move(op));
    }
    if (!fire_state_changed()) return {};
    pump();
    return {};
}

Accept SpoolService::enqueue_update(const json& patch, bool& queued)
{
    queued = false;
    if (!patch.is_object()) return {false, FailKind::BadRequest};
    const std::string id = string_field(patch, "spool_id");
    if (id.empty()) return {false, FailKind::BadRequest};
    const std::vector<Spool> v = compute_view(ops_.size());
    const Spool*             s = find_spool(v, id);
    if (!s) return {false, FailKind::BadRequest};
    const auto body = update_body(*s, patch, deps_.lookup);
    if (!body) return {false, FailKind::BadRequest};
    if (body->empty()) return {};
    Op op;
    op.kind  = Op::Kind::Update;
    op.id    = id;
    op.patch = patch;
    queue_op(std::move(op));
    queued = true;
    return {};
}

Accept SpoolService::update(const json& page_patch)
{
    sync_session();
    if (!is_signed_in(deps_.session ? deps_.session() : Session{})) return {false, FailKind::NotSignedIn};
    bool         queued = false;
    const Accept a      = enqueue_update(page_patch, queued);
    if (!a.ok || !queued) return a;
    if (!fire_spools_changed()) return a;
    if (!fire_state_changed()) return a;
    pump();
    return a;
}

Accept SpoolService::batch(const json& creates, const json& updates)
{
    sync_session();
    if (!is_signed_in(deps_.session ? deps_.session() : Session{})) return {false, FailKind::NotSignedIn};
    if (!creates.is_array() || !updates.is_array()) return {false, FailKind::BadRequest};

    bool any_update = false;
    for (const json& c : creates) {
        if (!c.is_object()) continue;
        Op op;
        op.kind = Op::Kind::Create;
        op.body = create_body(c, deps_.lookup);
        queue_op(std::move(op));
    }
    for (const json& u : updates) {
        bool         queued = false;
        const Accept a      = enqueue_update(u, queued);
        if (!a.ok) {
            if (!call_event(events.push_failed, std::string("update"), u.is_object() ? string_field(u, "spool_id") : std::string(), FailKind::BadRequest,
                            std::string()))
                return {};
            continue;
        }
        any_update = any_update || queued;
    }
    if (any_update && !fire_spools_changed()) return {};
    if (!fire_state_changed()) return {};
    pump();
    return {};
}

Accept SpoolService::remove(const std::vector<std::string>& ids)
{
    sync_session();
    if (!is_signed_in(deps_.session ? deps_.session() : Session{})) return {false, FailKind::NotSignedIn};
    const std::vector<Spool> v = compute_view(ops_.size());
    Op                       op;
    op.kind = Op::Kind::Delete;
    for (const std::string& id : ids) {
        if (!parse_spool_id(id)) continue;
        if (!find_spool(v, id)) continue;
        if (std::find(op.ids.begin(), op.ids.end(), id) != op.ids.end()) continue;
        op.ids.push_back(id);
    }
    if (op.ids.empty()) return {false, FailKind::BadRequest};
    queue_op(std::move(op));
    if (!fire_spools_changed()) return {};
    if (!fire_state_changed()) return {};
    pump();
    return {};
}

void SpoolService::request_config(bool force)
{
    sync_session();
    if (!is_signed_in(deps_.session ? deps_.session() : Session{})) return;
    const bool pending = std::any_of(ops_.begin(), ops_.end(), [](const Op& o) { return o.kind == Op::Kind::Config; });
    if (pending) return;
    if (have_config_ && !force) return;
    Op op;
    op.kind = Op::Kind::Config;
    queue_op(std::move(op));
    pump();
}

// ---------------------------------------------------------------------------------------------------------------------
// the pump and jobs

bool SpoolService::pump()
{
    for (;;) {
        if (job_running_) return true;
        auto it = std::find_if(ops_.begin(), ops_.end(), [](const Op& o) { return o.state == Op::State::Queued; });
        if (it != ops_.end()) {
            if (!start_op(static_cast<std::size_t>(it - ops_.begin()))) return false;
            continue;
        }
        if (pull_wanted_) {
            pull_wanted_ = false;
            if (!start_pull()) return false;
            continue;
        }
        return true;
    }
}

bool SpoolService::start_pull()
{
    const Session s = deps_.session ? deps_.session() : Session{};
    if (!is_signed_in(s)) return finish(true, 0, JobResult{false, FailKind::NotSignedIn, {}, {}, {}});
    if (!session_matches(s)) return do_reset(true, s);
    return launch(
        [](ISpoolCloud& cloud, const std::function<bool()>& cancelled) {
            JobResult r;
            r.list = list_all(cloud, cancelled);
            r.ok   = r.list.ok;
            if (!r.ok) {
                r.kind    = to_fail_kind(r.list.failure.error);
                r.message = r.list.failure.message;
            }
            return r;
        },
        JobKind::Pull, 0, s);
}

SpoolService::JobResult SpoolService::from_reply(const CloudReply& reply)
{
    JobResult r;
    r.ok      = reply.ok();
    r.kind    = to_fail_kind(reply.error);
    r.message = reply.message;
    r.body    = reply.body;
    return r;
}

bool SpoolService::start_op(std::size_t index)
{
    const Session s = deps_.session ? deps_.session() : Session{};
    Op&           op = ops_[index];
    const auto    serial = op.serial;

    if (!is_signed_in(s)) return finish(false, serial, JobResult{false, FailKind::NotSignedIn, {}, {}, {}});
    if (!session_matches(s)) return do_reset(true, s);

    switch (op.kind) {
    case Op::Kind::Create: {
        op.state = Op::State::Running;
        json body = op.body;
        return launch([body](ISpoolCloud& cloud, const std::function<bool()>&) { return from_reply(cloud.create(body)); }, JobKind::Write, serial, s);
    }
    case Op::Kind::Update: {
        // The body is built now, so that an earlier edit the cloud refused cannot leak into this one.
        const std::vector<Spool> before = compute_view(index);
        const Spool*             spool  = find_spool(before, op.id);
        if (!spool) return finish(false, serial, JobResult{false, FailKind::NotFound, {}, {}, {}});
        const auto body = update_body(*spool, op.patch, deps_.lookup);
        if (!body || body->empty()) {
            const std::vector<Spool> shown = compute_view(ops_.size());
            ops_.erase(ops_.begin() + index);
            if (!views_equal(shown, compute_view(ops_.size()))) return fire_spools_changed();
            return true;
        }
        op.state       = Op::State::Running;
        std::string id = op.id;
        json        b  = *body;
        return launch([id, b](ISpoolCloud& cloud, const std::function<bool()>&) { return from_reply(cloud.update(id, b)); }, JobKind::Write, serial, s);
    }
    case Op::Kind::Delete: {
        op.state = Op::State::Running;
        std::vector<std::string> ids = op.ids;
        return launch([ids](ISpoolCloud& cloud, const std::function<bool()>&) { return from_reply(cloud.remove(ids)); }, JobKind::Write, serial, s);
    }
    case Op::Kind::Config:
        op.state = Op::State::Running;
        return launch([](ISpoolCloud& cloud, const std::function<bool()>&) { return from_reply(cloud.config()); }, JobKind::Config, serial, s);
    }
    return true;
}

bool SpoolService::launch(Work work, JobKind kind, unsigned long long serial, const Session& s)
{
    cancel_ = std::make_shared<std::atomic<bool>>(false);
    auto                flag    = cancel_;
    auto                cloud   = deps_.cloud;
    auto                post    = deps_.post_to_ui;
    std::weak_ptr<int>  weak    = alive_;
    const auto          epoch   = epoch_;
    SpoolService*       self    = this;
    const bool          is_pull = kind == JobKind::Pull;
    const auto          ticket  = next_ticket_++;

    job_running_    = true;
    running_kind_   = kind;
    running_serial_ = serial;
    running_ticket_ = ticket;

    // Hands a job's result back to the service's thread; used by the job itself and by the guard.
    std::function<void(JobResult)> deliver = [post, weak, epoch, self, s, is_pull, serial, ticket](JobResult r) {
        if (!post) return;
        post([weak, epoch, self, s, is_pull, serial, ticket, r = std::move(r)]() mutable {
            if (weak.lock() == nullptr) return;
            self->on_completion(epoch, s, is_pull, serial, ticket, std::move(r));
        });
    };

    // When the worker throws the job away unrun (drain), destroying the closure completes the job as Unavailable,
    // unless the job was cancelled by a reset or the service's destruction (then nobody waits for it).
    struct Guard
    {
        std::function<void(JobResult)>      deliver;
        std::shared_ptr<std::atomic<bool>>  cancel;
        std::atomic<bool>                   finished{false};
        ~Guard()
        {
            if (finished.load() || cancel->load() || !deliver) return;
            JobResult r;
            r.kind = FailKind::Unavailable;
            try {
                deliver(std::move(r));
            } catch (...) {
            }
        }
    };
    auto guard     = std::make_shared<Guard>();
    guard->deliver = deliver;
    guard->cancel  = flag;

    auto job = [work, flag, cloud, guard]() {
        guard->finished = true;
        if (flag->load()) return; // the account changed: no cloud call, no completion
        JobResult r;
        try {
            if (cloud) r = work(*cloud, [flag] { return flag->load(); });
            else r.kind = FailKind::Unavailable;
        } catch (...) {
            r      = JobResult{};
            r.kind = FailKind::Unknown;
        }
        guard->deliver(std::move(r));
    };

    if (!deps_.run_async || !deps_.run_async(std::move(job))) {
        guard->finished = true; // completed synchronously below; this local `guard` outlives the refused closure
        JobResult r;
        r.kind = FailKind::Unavailable;
        return finish(is_pull, serial, std::move(r));
    }
    return true;
}

void SpoolService::on_completion(unsigned long long epoch, const Session& started, bool is_pull, unsigned long long serial, unsigned long long ticket,
                                 JobResult result)
{
    if (epoch != epoch_) return;
    if (!job_running_ || ticket != running_ticket_) return;
    const Session cur = deps_.session ? deps_.session() : Session{};
    if (!is_signed_in(cur) || cur.uid != started.uid || cur.generation != started.generation) {
        reset();
        return;
    }
    if (!finish(is_pull, serial, std::move(result))) return;
    pump();
}

bool SpoolService::finish(bool is_pull, unsigned long long serial, JobResult result)
{
    job_running_    = false;
    running_kind_   = JobKind::None;
    running_serial_ = 0;
    running_ticket_ = 0;
    return is_pull ? finish_pull(result) : finish_op(serial, result);
}

bool SpoolService::finish_pull(const JobResult& r)
{
    if (!r.ok) {
        set_error(r.kind, r.message);
        return fire_state_changed();
    }
    std::set<std::string> old_ids;
    for (const Spool& s : truth_) old_ids.insert(s.id);
    int added = 0;
    int kept  = 0;
    for (const Spool& s : r.list.spools) (old_ids.count(s.id) ? kept : added) += 1;

    truth_ = r.list.spools;
    ops_.erase(std::remove_if(ops_.begin(), ops_.end(), [](const Op& o) { return o.state == Op::State::Settled; }), ops_.end());
    have_list_      = true;
    last_synced_at_ = now();
    clear_error();
    paging_suspect_ = r.list.paging_suspect;
    skipped_rows_   = r.list.skipped_rows;

    if (!call_event(events.pull_done, added, kept)) return false;
    if (!fire_spools_changed()) return false;
    return fire_state_changed();
}

bool SpoolService::finish_op(unsigned long long serial, const JobResult& r)
{
    auto it = std::find_if(ops_.begin(), ops_.end(), [&](const Op& o) { return o.serial == serial; });
    if (it == ops_.end()) return true;
    const Op op = *it;

    if (op.kind == Op::Kind::Config) {
        ops_.erase(it);
        if (r.ok && r.body.is_object()) {
            config_         = r.body;
            have_config_    = true;
            const json copy = config_;
            return call_event(events.config_fetched, copy);
        }
        return true;
    }

    const std::vector<Spool> before   = compute_view(ops_.size());
    const std::string        first_id = op.kind == Op::Kind::Delete && !op.ids.empty() ? op.ids.front() : op.id;
    const std::string        name     = op.kind == Op::Kind::Create ? "create" : op.kind == Op::Kind::Update ? "update" : "delete";
    const std::string        spool_id = op.kind == Op::Kind::Create ? std::string() : first_id;

    if (r.ok) {
        if (op.kind == Op::Kind::Create) ops_.erase(it);
        else it->state = Op::State::Settled;
        last_synced_at_ = now();
        clear_error();
        pull_wanted_ = true;
        if (!call_event(events.push_done, name, spool_id)) return false;
    } else {
        ops_.erase(it);
        set_error(r.kind, r.message);
        const bool refetch = op.kind == Op::Kind::Create ? r.kind == FailKind::Unknown
                                                         : (r.kind == FailKind::Unknown || r.kind == FailKind::NotFound);
        if (refetch) pull_wanted_ = true;

        // An edit whose reply was lost may have reached the server. Every update carries the filament name, so a later
        // queued edit of the same spool, built from the old row, could undo it: drop those edits too.
        int dropped = 0;
        if (op.kind == Op::Kind::Update && r.kind == FailKind::Unknown) {
            for (auto i = ops_.begin(); i != ops_.end();) {
                if (i->kind == Op::Kind::Update && i->state == Op::State::Queued && i->id == op.id) {
                    i = ops_.erase(i);
                    ++dropped;
                } else {
                    ++i;
                }
            }
        }
        if (!call_event(events.push_failed, name, spool_id, r.kind, r.message)) return false;
        for (int i = 0; i < dropped; ++i)
            if (!call_event(events.push_failed, name, spool_id, FailKind::Unknown, std::string())) return false;
    }

    if (!views_equal(before, compute_view(ops_.size())) && !fire_spools_changed()) return false;
    return fire_state_changed();
}

} // namespace Slic3r::BblFilament
