// NewSpools::reconcile follows calibrate_pending_badges of Bambu Studio (src/slic3r/GUI/fila_manager/wgtFilaManagerSync.cpp,
// tag v02.09.01.70), GNU AGPL v3, copyright Bambu Lab; adapted for OrcaSlicer.

#include "NewSpools.hpp"

#include <iterator>

namespace Slic3r::BblFilament {

void NewSpools::add(const SlotKey& slot, const std::string& rfid) { m_slots[slot] = rfid; }

void NewSpools::dismiss(const SlotKey& slot) { m_slots.erase(slot); }

void NewSpools::reconcile(const std::vector<PrinterView>& printers)
{
    for (const PrinterView& p : printers) {
        if (!takes_part(p))
            continue; // no fresh data: its markers stay
        for (auto it = m_slots.begin(); it != m_slots.end();) {
            const SlotKey& slot = it->first;
            if (slot.dev_id != p.dev_id) {
                ++it;
                continue;
            }
            const TrayFacts* tray = nullptr;
            for (const TrayFacts& t : p.trays)
                if (!t.external && t.ams_id == slot.ams_id && t.slot_id == slot.slot_id)
                    tray = &t;
            // The unit or the tray is gone, the tray is empty, or it reports another spool (an empty uuid is being read).
            const bool drop = tray == nullptr || !tray->occupied || (!tray->uuid.empty() && tray->uuid != it->second);
            it              = drop ? m_slots.erase(it) : std::next(it);
        }
    }
}

bool NewSpools::has(const SlotKey& slot) const { return m_slots.count(slot) > 0; }

std::vector<SlotKey> NewSpools::of_printer(const std::string& dev_id) const
{
    std::vector<SlotKey> out;
    for (const auto& entry : m_slots)
        if (entry.first.dev_id == dev_id)
            out.push_back(entry.first);
    return out;
}

std::string NewSpools::rfid(const SlotKey& slot) const
{
    auto it = m_slots.find(slot);
    return it != m_slots.end() ? it->second : std::string();
}

void NewSpools::clear() { m_slots.clear(); }

} // namespace Slic3r::BblFilament
