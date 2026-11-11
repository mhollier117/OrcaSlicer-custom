#pragma once

#include "Linker.hpp"
#include "Spool.hpp"
#include "SpoolService.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
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

    void            tick(const std::vector<PrinterView>& printers); // call every couple of seconds
    void            assign(const Assignment& a);                    // replaces an assignment for the same slot
    Accept          unassign(const SlotKey& slot, const std::vector<PrinterView>& printers); // unbind now (manual spools)
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

    long long now() const;
    bool      waiting_for_list() const;
    void      start_round(const std::vector<PrinterView>& printers);
    void      op_finished(unsigned long long round, const OpRecord& record);
    void      record_slot_results(const std::string& dev_id, const SpoolService::LinkOutcome& out); // refusals and accepted items

    LinkDriverDeps deps_;
    LinkerState    state_;
    LinkPlan       last_plan_;

    unsigned long long    generation_{0};      // grows with every reset: completions of an older one are ignored
    unsigned long long    round_{0};           // grows with every round and every reset
    bool                  round_in_flight_{false};
    int                   round_pending_{0};   // ops of the round whose `done` has not been called yet
    bool                  have_round_{false};  // a round has ended since the last reset
    long long             round_ended_at_{0};
    unsigned long long    round_end_serial_{0};
    long long             pull_asked_at_{0};
    bool                  skip_spacing_{false};
    std::vector<OpRecord> round_ops_, last_round_ops_;

    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace Slic3r::BblFilament
