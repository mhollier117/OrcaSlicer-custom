#pragma once

#include "Linker.hpp"
#include "PrinterTap.hpp"
#include "Spool.hpp"
#include "SpoolCloud.hpp"

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::BblFilament {

// The AMS slots that hold a newly registered official spool, for the marker on the slot. Memory only; one thread.
class NewSpools
{
public:
    void add(const SlotKey& slot, const std::string& rfid);
    void dismiss(const SlotKey& slot);
    // Dismisses the marker only while it still names this RFID; true when it did.
    bool dismiss_if(const SlotKey& slot, const std::string& rfid);
    // Drops markers whose slot is empty or reports another spool, on printers that take part with fresh data.
    void reconcile(const std::vector<PrinterView>& printers);
    bool                 has(const SlotKey& slot) const;
    std::vector<SlotKey> of_printer(const std::string& dev_id) const;
    std::string          rfid(const SlotKey& slot) const; // empty when none
    void                 clear();

private:
    std::map<SlotKey, std::string> m_slots; // slot -> the RFID of the spool registered there
};

// The entry of the soft-match queue whose hit carries this RFID; null when there is none or the RFID is empty.
const SoftMatchEntry* entry_for_rfid(const SoftMatchQueue& queue, const std::string& rfid);

enum class NewSpoolChoice { AddNew, Confirm };

struct SoftMatchAct
{
    std::string action; // "create_new", "link_other" or "accept"
    std::string spool_id, target_spool_id;
};
// What the new-spool dialog's answer sends: "Add as new filament" creates a new spool from the hit, "Confirm" links
// the hit to the candidate picked when that is another spool, else accepts the hit. Nothing without a hit.
std::optional<SoftMatchAct> soft_match_act_for(NewSpoolChoice choice, const std::string& hit_id, const std::string& candidate_id);

// A spool that shows what a tray reports (for a spool the list does not show yet).
Spool spool_from_tray(const TrayFacts& tray);

} // namespace Slic3r::BblFilament
