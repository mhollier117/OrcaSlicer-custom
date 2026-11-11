// NewSpools::reconcile follows calibrate_pending_badges of Bambu Studio (src/slic3r/GUI/fila_manager/wgtFilaManagerSync.cpp),
// soft_match_act_for and spool_from_tray follow StatusPanel::show_new_official_filament_dlg and
// open_new_official_filament_hint (src/slic3r/GUI/StatusPanel.cpp); tag v02.09.01.70, GNU AGPL v3, copyright Bambu Lab;
// adapted for OrcaSlicer.

#include "NewSpools.hpp"

#include <algorithm>
#include <iterator>

namespace Slic3r::BblFilament {

void NewSpools::add(const SlotKey& slot, const std::string& rfid) { m_slots[slot] = rfid; }

void NewSpools::dismiss(const SlotKey& slot) { m_slots.erase(slot); }

bool NewSpools::dismiss_if(const SlotKey& slot, const std::string& rfid)
{
    auto it = m_slots.find(slot);
    if (it == m_slots.end() || it->second != rfid)
        return false;
    m_slots.erase(it);
    return true;
}

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

const SoftMatchEntry* entry_for_rfid(const SoftMatchQueue& queue, const std::string& rfid)
{
    if (rfid.empty())
        return nullptr;
    for (const SoftMatchEntry& e : queue.entries)
        if (e.hit.rfid == rfid)
            return &e;
    return nullptr;
}

std::optional<SoftMatchAct> soft_match_act_for(NewSpoolChoice choice, const std::string& hit_id, const std::string& candidate_id)
{
    if (hit_id.empty())
        return std::nullopt;
    if (choice == NewSpoolChoice::AddNew)
        return SoftMatchAct{"create_new", hit_id, {}};
    if (!candidate_id.empty() && candidate_id != hit_id)
        return SoftMatchAct{"link_other", hit_id, candidate_id};
    return SoftMatchAct{"accept", hit_id, {}};
}

Spool spool_from_tray(const TrayFacts& tray)
{
    Spool s;
    s.rfid             = valid_rfid(tray.uuid) ? tray.uuid : std::string();
    s.filament_id      = tray.filament_id;
    s.name             = tray.sub_brands;
    s.type             = tray.type;
    s.tray_id_name     = tray.tray_id_name;
    s.color            = tray.color;
    s.colors           = tray.colors;
    s.color_type       = tray.color_type;
    s.total_net_weight = static_cast<double>(tray.total_g);
    if (tray.remain_g >= 0)
        s.net_weight = tray.remain_g;
    else if (tray.total_g > 0 && tray.remain >= 0)
        s.net_weight = static_cast<double>(tray.total_g) * std::min(tray.remain, 100) / 100.0;
    return s;
}

} // namespace Slic3r::BblFilament
