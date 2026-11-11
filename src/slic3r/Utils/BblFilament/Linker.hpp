#pragma once

#include "PrinterTap.hpp"
#include "Spool.hpp"
#include "SpoolCloud.hpp"

#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r::BblFilament {

struct SlotKey
{
    std::string dev_id;
    int         ams_id{0};
    std::string slot_id;
    bool        operator<(const SlotKey& o) const;
    bool        operator==(const SlotKey& o) const;
    std::string text() const; // "<dev_id>:<ams_id>:<slot_id>"
};

// One printer, as the app sees it now.
struct PrinterView
{
    std::string                dev_id, name;
    bool                       bound{false};    // a printer of the signed-in account
    bool                       lan_only{false};
    bool                       selected{false};
    bool                       busy{false};     // printing, calibrating, or its AMS not idle
    bool                       fresh{false};    // a full state since it connected and a recent message
    unsigned long long         observations{0}; // status messages seen (PrinterFacts::observations)
    std::map<int, std::string> ams_sn;          // AMS id -> serial; missing or empty = unknown
    std::vector<TrayFacts>     trays;           // filament ids already translated to Bambu's
};

// "This spool goes into this slot", made by the user.
struct Assignment
{
    SlotKey                  slot;
    std::string              spool_id;
    std::string              filament_id; // what the tray must report
    std::vector<std::string> colors;      // at least one
    long long                created_at{0};
};

// Memory only.
struct LinkerState
{
    std::map<SlotKey, Assignment> assignments;
    struct Streak
    {
        int                count{0};
        unsigned long long last_observation{0};
        long long          first_at{0};
        unsigned long long list_serial{0};
    };
    std::map<std::string, Streak> unmatched; // by spool id
    struct Push
    {
        long long sent_at{0};
        long long grams{-1};
    };
    std::map<std::string, Push> pushed; // by RFID
    struct Registration
    {
        int       attempts{0};
        long long next_at{0};
        bool      done{false};
    };
    std::map<std::string, Registration> registrations; // by RFID
    struct Refusal
    {
        long long until{0};
        int       code{0};
    };
    std::map<std::string, Refusal> refused; // by SlotKey::text() + "|" + spool id ("" for an unbind)
};

enum class LinkMode { Off, DryRun, On };

struct LinkConfig
{
    LinkMode              mode{LinkMode::On};
    std::set<std::string> only_slots; // SlotKey::text() values; empty = every slot
};

struct PlannedMount
{
    SlotKey                  slot;
    std::string              ams_sn;
    int                      ams_type{0};
    std::string              spool_id;
    bool                     rfid{false};
    std::optional<long long> grams;
};
struct PlannedWeight
{
    SlotKey     slot;
    std::string ams_sn;
    int         ams_type{0};
    std::string spool_id;
    long long   grams{0};
};
struct PlannedUnbind
{
    SlotKey     slot;
    std::string ams_sn;
    int         ams_type{0};
    std::string spool_id;
};
struct PlannedRegister
{
    SlotKey     slot;
    std::string ams_sn;
    int         ams_type{0};
    TrayFacts   tray;
};
struct TrayReport
{
    SlotKey     slot;
    bool        occupied{false};
    std::string spool_id;
    std::string rule; // "rfid", "assignment", "cloud" or ""
    std::string note;
};

struct LinkPlan
{
    std::vector<PlannedMount>    mounts;
    std::vector<PlannedWeight>   weights;
    std::vector<PlannedUnbind>   unbinds;
    std::vector<PlannedRegister> registrations;
    std::vector<TrayReport>      trays; // every tray of every printer that takes part
    std::vector<std::string>     notes; // why something was not done
    bool                         want_pull{false};
    std::map<std::string, int>   skipped_cooldown; // by dev_id: weight pushes held back by the busy-printer cooldown
    bool                         has_actions() const;
};

constexpr int       k_unbind_observations = 3;
constexpr long long k_unbind_seconds      = 30;
constexpr long long k_busy_push_seconds   = 600;
constexpr long long k_assignment_seconds  = 120;

// bound && fresh && (!lan_only || selected): only these printers' trays are looked at.
bool takes_part(const PrinterView& p);

LinkPlan plan_links(const std::vector<PrinterView>& printers, const std::vector<Spool>& rows, const std::set<std::string>& busy,
                    unsigned long long list_serial, LinkerState& state, const LinkConfig& config, long long now);

// The items a round sends (used by the driver when an op starts, and by tests).
AmsItem  ams_item_from_row(const Spool& row, const std::string& ams_sn, int ams_id, int ams_type, const std::string& slot_id,
                           std::optional<long long> grams);
AmsItem  ams_item_from_tray(const TrayFacts& tray, const std::string& ams_sn, const FilamentLookup& lookup);
SlotItem slot_bind(const std::string& ams_sn, int ams_id, int ams_type, const std::string& slot_id, const Spool& row);
SlotItem slot_unbind(const std::string& ams_sn, int ams_id, int ams_type, const std::string& slot_id);
std::optional<long long> measured_grams(const TrayFacts& tray, const Spool& row);
bool                     same_rgb(const std::string& a, const std::string& b); // "#RRGGBBAA" / "#RRGGBB", alpha ignored, case ignored

} // namespace Slic3r::BblFilament
