#pragma once

#include "BaseStore.hpp"
#include "Cloud.hpp"
#include "Forms.hpp"
#include "Keys.hpp"
#include "Ledger.hpp"
#include "Snapshot.hpp"
#include "Types.hpp"

#include <boost/filesystem/path.hpp>

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r::BblPresetSync {

constexpr long long LIST_INTERVAL_SECONDS = 300;
constexpr long long BACKOFF_FIRST_SECONDS = 60;
constexpr long long BACKOFF_MAX_SECONDS   = 3600;
constexpr int       WRITE_GAP_MS          = 100;

struct Session
{
    uint64_t    generation{0};
    std::string orca_user_id;
    std::string bambu_uid;
    bool        operator==(const Session& o) const
    {
        return generation == o.generation && orca_user_id == o.orca_user_id && bambu_uid == o.bambu_uid;
    }
};

enum class OpResult { Applied, Postponed, Unresolvable, Failed };
struct OpOutcome
{
    OpResult    result{OpResult::Failed};
    std::string reason;
    std::string new_hash; // upload hash of the preset after a pull
    ValueMap    new_upload; // the preset upload map after a pull
};

struct PullOp
{
    Session     session;
    Key         key;                  // local name to create or replace
    ValueMap    body;                 // validated Bambu body
    bool        backup_local{false};
    bool        rename_from_set{false};
    Key         rename_from{};        // delete this local preset after the pull succeeded
    std::string expected_local_hash;  // snapshot hash of the preset at `key`; "" when it did not exist
};
struct DeleteOp
{
    Session     session;
    Key         key;
    bool        backup_local{true};
    std::string expected_local_hash;
};

// Implemented by the app (UI thread work); faked in tests.
class ILocalStore
{
public:
    virtual ~ILocalStore() = default;
    // Blocks until the UI thread built the snapshot; nullopt when cancelled or when Orca's first pull is not applied yet.
    virtual std::optional<LocalSnapshot> snapshot(const Session& session) = 0;
    // Queue the change; `done` is called exactly once, from any thread, possibly later.
    virtual void apply_pull(PullOp op, std::function<void(OpOutcome)> done)     = 0;
    virtual void delete_local(DeleteOp op, std::function<void(OpOutcome)> done) = 0;
    // A pass brackets its local changes: between the two calls apply_pull and delete_local may be applied together.
    virtual void begin_batch() = 0;
    virtual void end_batch()   = 0;
};

struct FirstRunSummary
{
    std::vector<Key>         uploads, downloads, links, conflicts_local_wins, conflicts_remote_wins, remote_deletes;
    std::vector<Key>         candidates; // Candidate items of the snapshot that have no ledger entry
    std::vector<std::string> notes;
};

class IHost
{
public:
    virtual ~IHost() = default;
    virtual std::optional<Session> session()  = 0; // nullopt: preconditions not met (not signed in, disabled, stealth, closing)
    virtual bool                   cancelled() = 0; // sync token expired or app closing
    virtual long long              now()       = 0; // unix seconds
    virtual void                   sleep_ms(int ms) = 0;
    virtual std::string            bundle_version() = 0; // BBL vendor profile version
    virtual boost::filesystem::path resources_dir() = 0; // holds printers/bambu_studio_option_keys.json
    virtual boost::filesystem::path data_dir()      = 0;
    virtual void                   on_first_run(const FirstRunSummary& summary) = 0;
    // Filaments that became conversion candidates after the first run; called when the list changes.
    virtual void on_candidates(const std::vector<Key>& candidates) = 0;
    virtual void on_halt(const std::string& kind, const std::vector<Key>& local_deletes, const std::vector<Key>& remote_deletes) = 0;
    // "preset_limit", "auth", "list_failed"
    virtual void notify(const std::string& code, const std::string& detail) = 0;
};

enum class Trigger { Tick, Manual };
enum class HaltChoice { ProceedLocalDeletes, KeepLocal, ProceedRemoteDeletes, RestoreFromBambu, Later };

struct Status // for the MCP tool
{
    long long                last_pass{0};
    std::string              last_result; // "ok", "skipped: <why>", "list failed: ...", "dry run", "halted: <kind>"
    std::vector<std::string> notes;
    std::vector<std::string> planned; // "<action> <key>" of the last plan
    std::vector<std::string> waiting; // "<key>: <why>" for postponed ops and backoff
    std::string              halted;
    bool                     first_run_confirmed{false};
    size_t                   linked{0};
    std::string              studio_keys_source; // "bundled <version>" or "BambuStudio <tag>"; empty before the first pass
    size_t                   translated_keys{0}; // settings whose type differs between the two slicers (rules of the Forms in use)
};

class Mirror
{
public:
    Mirror(IHost& host, ICloud& cloud, ILocalStore& store);

    void                    run_pass(Trigger trigger); // called on the sync thread
    void                    confirm_first_run(bool accepted); // any thread
    void                    resolve_halt(HaltChoice choice);  // any thread
    void                    reset_candidates_prompt();        // any thread: the next pass announces the candidates again
    void                    reset_first_run_prompt();         // any thread: the next dry-run pass shows the summary again
    void                    set_declined(const Key& key);     // any thread: conversion refused
    void                    forget(const Key& key);           // any thread: the preset is treated as new on the next pass
    Status                  status() const;                   // any thread
    std::shared_ptr<Ledger> ledger() const;                   // current ledger or null
    std::shared_ptr<BaseStore> base_store() const;            // current base copies or null
    void                    set_studio_keys(StudioKeys keys, std::string source); // any thread; takes effect from the next pass
    size_t                  studio_key_count() const;         // keys loaded from the Studio list (0: file missing)
    size_t                  cached_body_count() const;        // bodies held in the in-memory cache

private:
    struct Backoff
    {
        int       fail_count{0};
        long long retry_after{0};
    };

    LedgerView view_of(const Ledger& ledger) const;
    void       end_pass(const std::string& result, long long now);
    struct PullFollowUp
    {
        ValueMap                   remote_body;   // what Bambu holds now: becomes the remote base
        ValueMap                   old_local_base; // the local base before the pull
        std::set<std::string>      pending_local; // keys whose local edit is still to be uploaded
        bool                       pairing{false};
        std::shared_ptr<BaseStore> base;
    };
    void       finish_pull(const Session& s, const std::shared_ptr<Ledger>& ledger, const Action& action, long long row_update_time,
                           const OpOutcome& outcome, const PullFollowUp& follow);
    void       finish_delete(const Session& s, const std::shared_ptr<Ledger>& ledger, const Action& action, const OpOutcome& outcome,
                             const std::shared_ptr<BaseStore>& base);
    void       record_failure(const Key& key, long long now);

    IHost&       m_host;
    ICloud&      m_cloud;
    ILocalStore& m_store;

    mutable std::mutex      m_mutex;
    std::optional<Session>  m_session;
    std::shared_ptr<Ledger> m_ledger;
    std::shared_ptr<BaseStore> m_base;
    StudioKeys              m_studio; // loaded on the first pass; read on the sync thread only
    Forms                   m_forms;  // built from m_studio whenever it is set; read on the sync thread only
    bool                    m_studio_loaded{false};
    std::atomic<size_t>     m_studio_count{0};
    std::atomic<size_t>     m_forms_count{0};
    std::optional<std::pair<StudioKeys, std::string>> m_studio_pending; // set_studio_keys, applied at the start of a pass (m_mutex)
    std::string             m_studio_source;                              // (m_mutex)
    struct CachedBody
    {
        long long update_time{0};
        ValueMap  body;
    };
    std::map<std::string, CachedBody> m_body_cache; // setting id -> last valid body drained (guarded by m_mutex)
    std::set<Key>           m_pending;
    std::map<Key, Backoff>  m_backoff;
    bool                    m_auth_blocked{false};
    bool                    m_creates_blocked{false};
    bool                    m_list_failed_notified{false};
    bool                    m_have_list{false};
    long long               m_last_list{0};
    std::optional<FirstRunSummary> m_last_summary;
    bool                    m_summary_declined{false};
    bool                    m_proceed_local{false};
    bool                    m_proceed_remote{false};
    std::vector<Key>        m_halted_local, m_halted_remote;
    std::vector<Key>        m_last_candidates;
    Status                  m_status;
};

} // namespace Slic3r::BblPresetSync
