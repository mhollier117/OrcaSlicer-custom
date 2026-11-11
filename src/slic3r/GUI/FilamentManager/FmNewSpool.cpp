// on_new_spool_hint follows StatusPanel::open_new_official_filament_hint and show_new_official_filament_dlg of Bambu
// Studio (src/slic3r/GUI/StatusPanel.cpp, tag v02.09.01.70), GNU AGPL v3, copyright Bambu Lab; adapted for OrcaSlicer.

#include "FmNewSpool.hpp"

#include "FilamentManagerPanel.hpp"
#include "FmNewSpoolDialogs.hpp"
#include "FmRuntime.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/DeviceCore/DevFirmware.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MsgDialog.hpp"
#include "slic3r/GUI/Widgets/AMSControl.hpp"
#include "slic3r/Utils/BblFilament/Linker.hpp"
#include "slic3r/Utils/BblFilament/NewSpools.hpp"
#include "slic3r/Utils/BblFilament/PrinterTap.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"
#include "slic3r/Utils/BblFilament/SpoolService.hpp"

#include <wx/app.h>
#include <wx/string.h>
#include <wx/weakref.h>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI::Fm {

namespace fil = BblFilament;

namespace {

// What one AMS control shows, so that a marker dropped since is taken down once.
struct Shown
{
    wxWeakRef<AMSControl> control;
    std::string           dev_id;
    std::set<fil::SlotKey> slots;
};

std::map<AMSControl*, Shown>& shown_by_control()
{
    static std::map<AMSControl*, Shown> shown;
    return shown;
}

bool enabled()
{
    AppConfig* config = wxGetApp().app_config;
    return config != nullptr && config->get_bool("filament_manager_enabled");
}

void show_hints(AMSControl* control, const std::string& dev_id)
{
    std::set<fil::SlotKey> now;
    if (enabled() && !dev_id.empty())
        for (const fil::SlotKey& slot : Runtime::get().new_spools().of_printer(dev_id))
            now.insert(slot);

    Shown& shown = shown_by_control()[control];
    for (const fil::SlotKey& slot : shown.slots)
        if (now.count(slot) == 0)
            control->dismiss_filament_hint(std::to_string(slot.ams_id), slot.slot_id);
    for (const fil::SlotKey& slot : now)
        control->show_filament_hint(std::to_string(slot.ams_id), slot.slot_id);
    shown.control = control;
    shown.dev_id  = dev_id;
    shown.slots   = std::move(now);
}

// Every control that shows this printer, at once (after a marker was dismissed).
void refresh_shown(const std::string& dev_id)
{
    auto& all = shown_by_control();
    for (auto it = all.begin(); it != all.end();) {
        AMSControl* control = it->second.control.get();
        if (control == nullptr) {
            it = all.erase(it);
            continue;
        }
        const bool same = it->second.dev_id == dev_id;
        ++it;
        if (same)
            show_hints(control, dev_id);
    }
}

void dismiss(const fil::SlotKey& slot)
{
    Runtime::get().new_spools().dismiss(slot);
    refresh_shown(slot.dev_id);
}

void show_failure(wxWindow* parent, fil::FailKind kind, const std::string& message)
{
    const wxString text = _L("The Filament Manager could not be reached.") + "\n" + wxString::FromUTF8(fail_text(kind, message));
    MessageDialog(parent, text, _L("Filament Manager"), wxOK | wxICON_WARNING).ShowModal();
}

// Slots whose soft-match queue is being read or whose dialog is open.
std::set<fil::SlotKey>& in_flight()
{
    static std::set<fil::SlotKey> slots;
    return slots;
}

void send_choice(wxWindow* parent, const fil::SlotKey& slot, const fil::SoftMatchAct& act)
{
    fil::SpoolService::LinkRequest req;
    req.kind            = fil::SpoolService::LinkRequest::Kind::SoftMatchAct;
    req.dev_id          = slot.dev_id;
    req.action          = act.action;
    req.spool_id        = act.spool_id;
    req.target_spool_id = act.target_spool_id;
    wxWeakRef<wxWindow> weak(parent);
    req.done = [weak](const fil::SpoolService::LinkOutcome& out) {
        if (out.ok)
            return; // the service pulls the list by itself
        const fil::FailKind kind = out.kind;
        const std::string   msg  = out.message;
        wxTheApp->CallAfter([weak, kind, msg] {
            if (wxWindow* p = weak.get())
                show_failure(p, kind, msg);
        });
    };
    const fil::Accept a = Runtime::get().service().link(std::move(req));
    if (!a.ok)
        show_failure(parent, a.kind, std::string());
}

// The tray as the printer reports it, for a spool the list does not show yet.
fil::Spool spool_of_tray(const fil::SlotKey& slot)
{
    for (const fil::PrinterView& v : Runtime::get().views()) {
        if (v.dev_id != slot.dev_id)
            continue;
        for (const fil::TrayFacts& t : v.trays)
            if (!t.external && t.ams_id == slot.ams_id && t.slot_id == slot.slot_id)
                return fil::spool_from_tray(t);
    }
    return {};
}

// On the UI thread, outside the service's call: what the cloud has queued for the slot's spool.
void on_soft_match(wxWindow* parent, const fil::SlotKey& slot, const std::string& rfid, const fil::SpoolService::LinkOutcome& out)
{
    if (!out.ok) {
        show_failure(parent, out.kind, out.message); // the marker stays
        return;
    }
    const fil::SoftMatchQueue  queue = fil::parse_soft_match(out.body);
    const fil::SoftMatchEntry* entry = fil::entry_for_rfid(queue, rfid);
    if (entry == nullptr) {
        // The cloud recorded the spool by itself.
        std::optional<fil::Spool> row;
        for (const fil::Spool& r : Runtime::get().service().confirmed())
            if (r.rfid == rfid)
                row = r;
        SpoolRecordedDialog dlg(parent, row ? *row : spool_of_tray(slot));
        dlg.ShowModal();
        dismiss(slot);
        return;
    }

    NewOfficialSpoolDialog dlg(parent, *entry);
    if (dlg.ShowModal() != wxID_OK)
        return; // closed: nothing is sent and the marker stays
    dismiss(slot);
    const fil::NewSpoolChoice choice = dlg.choice() == NewOfficialSpoolDialog::Choice::RecordNew ? fil::NewSpoolChoice::AddNew :
                                                                                                   fil::NewSpoolChoice::Confirm;
    if (const std::optional<fil::SoftMatchAct> act = fil::soft_match_act_for(choice, dlg.hit_spool_id(), dlg.selected_candidate_id()))
        send_choice(parent, slot, *act);
}

} // namespace

void apply_new_spool_hints(AMSControl* control, MachineObject* obj)
{
    try {
        if (control == nullptr)
            return;
        show_hints(control, obj != nullptr ? obj->get_dev_id() : std::string());
    } catch (...) {
    }
}

void on_new_spool_hint(wxWindow* parent, MachineObject* obj, int ams_id, const std::string& slot_id)
{
    try {
        if (parent == nullptr || obj == nullptr || !enabled())
            return;
        const fil::SlotKey slot{obj->get_dev_id(), ams_id, slot_id};
        const std::string  rfid = Runtime::get().new_spools().rfid(slot);
        if (rfid.empty() || in_flight().count(slot) > 0)
            return;

        std::string ams_sn;
        const auto  versions = obj->get_ams_version();
        if (auto it = versions.find(ams_id); it != versions.end())
            ams_sn = it->second.sn;

        fil::SpoolService::LinkRequest req;
        req.kind   = fil::SpoolService::LinkRequest::Kind::SoftMatchRead;
        req.dev_id = slot.dev_id;
        req.ams_sn = ams_sn;
        wxWeakRef<wxWindow> weak(parent);
        req.done = [weak, slot, rfid](const fil::SpoolService::LinkOutcome& out) {
            // The dialogs are modal: they run after the service's call has returned.
            wxTheApp->CallAfter([weak, slot, rfid, out] {
                try {
                    if (wxWindow* p = weak.get())
                        on_soft_match(p, slot, rfid, out);
                } catch (...) {
                }
                in_flight().erase(slot);
            });
        };
        in_flight().insert(slot);
        const fil::Accept a = Runtime::get().service().link(std::move(req));
        if (!a.ok) {
            in_flight().erase(slot);
            show_failure(parent, a.kind, std::string());
        }
    } catch (...) {
    }
}

} // namespace Slic3r::GUI::Fm
