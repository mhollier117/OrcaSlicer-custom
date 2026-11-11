#pragma once

#include "Linker.hpp"
#include "Spool.hpp"
#include "SpoolService.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r::BblFilament {

struct LinkDriverDeps
{
    SpoolService*              service{nullptr};
    FilamentLookup             lookup;
    std::function<long long()> now; // unix seconds
};

struct AutoPushSummary
{
    int  pushed{0};           // weights the cloud took in one op
    int  skipped_cooldown{0}; // weights the plan held back on that printer (busy-printer cooldown)
    bool busy{false};         // that printer was busy
};

// Why an explicit write (an assignment, an unbind, an answer to the soft-match queue) is refused under `config`, or
// empty when it may go ahead: linking must be on (in a dry run only when `dry_run_answers`, for a write that then
// answers what it would send) and, when `slot` is given, the slot must be in the allow-list unless that is empty.
std::string write_refusal(const LinkConfig& config, const SlotKey* slot, bool dry_run_answers = false);

// Runs the linker's plans as rounds of link ops through the spool service. Lives on the service's thread and is not
// thread-safe. It must not outlive the service.
class LinkDriver
{
public:
    explicit LinkDriver(LinkDriverDeps deps);

    LinkDriver(const LinkDriver&)            = delete;
    LinkDriver& operator=(const LinkDriver&) = delete;

    LinkConfig                                                    config;
    std::function<void(const AutoPushSummary&)>                   on_auto_push;  // after an op that pushed weights
    std::function<void(const SlotKey&, const std::string& rfid)> on_registered; // a new official spool was sent

    // Call every couple of seconds. A config (mode or allow-list) other than the one of the previous tick first drops
    // the ops still queued (they send nothing); the rest of the state is kept.
    void            tick(const std::vector<PrinterView>& printers);
    // Replaces an assignment for the same slot. Refused (Rejected) when write_refusal() names a reason.
    Accept          assign(const Assignment& a);
    // Creates a spool from a page-shaped spool object and assigns it to the slot once it has shown up in the list; a
    // plain add when write_refusal() names a reason.
    Accept          create_and_assign(const json& page_spool, const SlotKey& slot);
    // Unbinds a manual spool from its slot now. Refused (Rejected) when write_refusal() names a reason; in a dry run it
    // sends nothing and, when `would_unbind` is given, sets it to the spool it would unbind. When it refuses for a reason
    // of its own (no service, write_refusal, no spool in the slot, an RFID spool, an unknown AMS) and `why` is given,
    // sets it to a plain sentence that names no serial and no RFID.
    Accept          unassign(const SlotKey& slot, const std::vector<PrinterView>& printers, std::string* would_unbind = nullptr,
                             std::string* why = nullptr);
    void            sync_now();                                     // forget the round spacing, the busy cooldowns and what was sent lately, once
    void            reset();                                        // session changed: forget everything; ops still queued send nothing
    const LinkPlan& last_plan() const { return last_plan_; }
    json            status_json(const std::vector<PrinterView>& printers) const;

    static constexpr long long k_round_spacing_seconds = 5;
    static constexpr long long k_refusal_seconds       = 600;
    static constexpr long long k_pull_retry_seconds    = 60; // while the list the last round asked for has not come

private:
    struct OpRecord
    {
        std::string kind; // "ams", "register" or "slots"
        bool        ok{false};
        FailKind    fail_kind{FailKind::Unknown};
        int         items{0};
    };

    // A spool created for a slot, waiting for the list that shows it.
    struct Creation
    {
        std::set<std::string>    baseline; // the ids of the rows when it was created
        SlotKey                  slot;
        std::string              filament_id;
        std::vector<std::string> colors;
        long long                created_at{0};
        unsigned long long       list_serial{0};
    };
    struct CreationNote
    {
        SlotKey     slot;
        std::string text;
    };

    long long now() const;
    void      store_assignment(const Assignment& a);
    void      drop_queued(); // queued ops send nothing and the round in flight is forgotten
    bool      waiting_for_list() const;
    void      resolve_creations();
    void      start_round(const std::vector<PrinterView>& printers);
    void      op_finished(unsigned long long round, const OpRecord& record);
    void      record_slot_results(const std::string& dev_id, const SpoolService::LinkOutcome& out); // refusals and accepted items

    LinkDriverDeps deps_;
    LinkerState    state_;
    LinkPlan       last_plan_;

    unsigned long long    generation_{0};      // grows with every reset: completions of an older one are ignored
    unsigned long long    epoch_{0};           // grows with every reset and config change: older ops build nothing
    unsigned long long    round_{0};           // grows with every round, every reset and every config change
    std::optional<LinkConfig> last_config_;    // the config of the previous tick
    bool                  round_in_flight_{false};
    int                   round_pending_{0};   // ops of the round whose `done` has not been called yet
    bool                  have_round_{false};  // a round has ended since the last reset
    long long             round_ended_at_{0};
    unsigned long long    round_end_serial_{0};
    long long             pull_asked_at_{0};
    bool                  skip_spacing_{false};
    std::vector<OpRecord> round_ops_, last_round_ops_;
    std::vector<Creation>     creations_;
    std::vector<CreationNote> creation_notes_; // the latest few

    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace Slic3r::BblFilament
