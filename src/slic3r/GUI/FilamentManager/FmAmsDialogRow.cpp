#include "FmAmsDialogRow.hpp"

#include "FmPresets.hpp"
#include "FmRuntime.hpp"
#include "FmSpoolPicker.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/GUI/AMSMaterialsSetting.hpp"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/CheckBox.hpp"
#include "slic3r/GUI/Widgets/HyperLink.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/Utils/BBLPrinterAgent.hpp"
#include "slic3r/Utils/BblFilament/Linker.hpp"
#include "slic3r/Utils/BblFilament/SpoolService.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"

#include <nlohmann/json.hpp>

#include <wx/colour.h>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include <cstddef>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace Slic3r::GUI::Fm {

namespace fil = BblFilament;

namespace {

std::string to_orca_id(const std::string& bambu_id)
{
    NetworkAgent* agent = wxGetApp().getAgent();
    return agent != nullptr ? agent->to_orca_filament_id(bambu_id) : bambu_id;
}

wxString spool_label(const fil::Spool& s)
{
    return wxString::FromUTF8(s.vendor.empty() ? s.name : s.vendor + " " + s.name);
}

int grams_left(const fil::Spool& s) { return static_cast<int>(fil::bounded_grams(s.net_weight)); }

std::vector<std::string> colours_of(const fil::Spool& s) { return s.colors.empty() ? std::vector<std::string>{s.color} : s.colors; }

} // namespace

AmsDialogRow::AmsDialogRow(wxWindow* parent, AMSMaterialsSetting* dlg) : wxPanel(parent, wxID_ANY), m_dlg(dlg)
{
    SetBackgroundColour(*wxWHITE);

    auto* title = new wxStaticText(this, wxID_ANY, _L("Filament Manager"), wxDefaultPosition, wxSize(AMS_MATERIALS_SETTING_LABEL_WIDTH, -1), 0);
    title->SetFont(::Label::Body_13);
    title->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    title->Wrap(AMS_MATERIALS_SETTING_LABEL_WIDTH);

    m_choose = new Button(this, _L("Choose spool…"));
    m_choose->SetStyle(ButtonStyle::Regular, ButtonType::Parameter);
    m_choose->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { choose(); });

    m_spool_text = new Label(this, wxEmptyString);
    m_spool_text->SetFont(::Label::Body_13);
    m_spool_text->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);

    m_clear = new HyperLink(this, _L("Clear"));
    m_clear->SetFont(::Label::Body_13);
    m_clear->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        clear_choice();
        e.Skip();
    });

    m_box = new ::CheckBox(this);
    m_box_label = new Label(this, _L("Add this filament to the Filament Manager"));
    m_box_label->SetFont(::Label::Body_13);
    m_box_label->SetForegroundColour(AMS_MATERIALS_SETTING_GREY800);
    m_box_label->Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& e) {
        if (m_box->IsEnabled())
            m_box->SetValue(!m_box->GetValue());
        e.Skip();
    });

    auto* top = new wxBoxSizer(wxHORIZONTAL);
    top->Add(title, 0, wxALIGN_CENTER_VERTICAL, 0);
    top->Add(m_choose, 0, wxALIGN_CENTER_VERTICAL, 0);

    auto* spool = new wxBoxSizer(wxHORIZONTAL);
    spool->AddSpacer(AMS_MATERIALS_SETTING_LABEL_WIDTH);
    spool->Add(m_spool_text, 0, wxALIGN_CENTER_VERTICAL, 0);
    spool->Add(m_clear, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(10));

    auto* box = new wxBoxSizer(wxHORIZONTAL);
    box->AddSpacer(AMS_MATERIALS_SETTING_LABEL_WIDTH);
    box->Add(m_box, 0, wxALIGN_CENTER_VERTICAL, 0);
    box->Add(m_box_label, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(5));

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(top, 0, wxEXPAND, 0);
    sizer->Add(spool, 0, wxEXPAND | wxTOP, FromDIP(8));
    sizer->Add(box, 0, wxEXPAND | wxTOP, FromDIP(8));
    SetSizer(sizer);

    Hide(); // until on_popup decides
}

std::string AmsDialogRow::dev_id() const { return m_dlg != nullptr && m_dlg->obj != nullptr ? m_dlg->obj->get_dev_id() : std::string(); }

void AmsDialogRow::on_popup()
{
    m_chosen.reset();
    m_current.reset();
    m_box->SetValue(false);
    bool show = false;
    try {
        if (!m_combo_bound && m_dlg->m_comboBox_filament != nullptr) {
            // Bound after the dialog's own handler, so it runs first; it lets that one run too.
            m_dlg->m_comboBox_filament->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& e) {
                e.Skip();
                if (IsShown())
                    update_box();
            });
            m_combo_bound = true;
        }
        AppConfig*     config = wxGetApp().app_config;
        MachineObject* obj    = m_dlg->obj;
        show = config != nullptr && config->get_bool("filament_manager_enabled") && obj != nullptr && m_dlg->m_is_third && !m_dlg->m_view_only &&
               m_dlg->ams_id >= 0 && m_dlg->ams_id < VIRTUAL_TRAY_DEPUTY_ID && Runtime::get().link_available(obj->get_dev_id());
        if (show) {
            // A spool with an RFID tag is found by its tag: there is nothing to choose for it.
            if (std::shared_ptr<DevFilaSystem> fila = obj->GetFilaSystem())
                if (const DevAmsTray* tray = fila->GetAmsTray(std::to_string(m_dlg->ams_id), std::to_string(m_dlg->slot_id)))
                    if (fil::valid_rfid(tray->uuid))
                        show = false;
        }
        if (show) {
            const std::string dev  = obj->get_dev_id();
            const std::string slot = std::to_string(m_dlg->slot_id);
            for (const fil::Spool& r : Runtime::get().service().confirmed())
                if (r.rfid.empty() && r.in_printer && r.dev_id == dev && r.ams_id == m_dlg->ams_id && r.slot_id == slot) {
                    m_current = r;
                    break;
                }
        }
    } catch (...) {
        show = false;
    }
    Show(show);
    if (show) {
        update_texts();
        update_box();
    }
    relayout();
}

void AmsDialogRow::update_texts()
{
    const bool chosen = m_chosen.has_value();
    if (chosen)
        // TRN A spool of the Filament Manager: its brand and name, then the grams left on it.
        m_spool_text->SetLabel(wxString::Format(_L("%s, %d g left"), spool_label(*m_chosen), grams_left(*m_chosen)));
    else if (m_current)
        // TRN The spool the Filament Manager has in this AMS slot: its brand and name, then the grams left on it.
        m_spool_text->SetLabel(wxString::Format(_L("In this slot: %s, %d g left"), spool_label(*m_current), grams_left(*m_current)));
    m_spool_text->Show(chosen || m_current.has_value());
    m_clear->Show(chosen);
    const bool box = !chosen && !m_current;
    if (!box)
        m_box->SetValue(false);
    m_box->Show(box);
    m_box_label->Show(box);
}

void AmsDialogRow::update_box()
{
    bool can = false;
    try {
        const AMSMaterialsSetting::FilamentInfos* info   = m_dlg->selected_filament_info();
        PresetBundle*                             bundle = wxGetApp().preset_bundle;
        if (info != nullptr && bundle != nullptr && !info->filament_id.empty())
            can = !page_spool_for_filament(bundle->filaments, info->filament_id, &BBLPrinterAgent::bambu_filament_id, "#FFFFFFFF", {}, 2).is_null();
    } catch (...) {
        can = false;
    }
    if (!can)
        m_box->SetValue(false);
    m_box->Enable(can);
    m_box_label->Enable(can);
    const wxString tip = can ? wxString() : _L("Bambu does not know this filament, so it cannot be added to the Filament Manager.");
    m_box->SetToolTip(tip);
    m_box_label->SetToolTip(tip);
}

void AmsDialogRow::choose()
{
    if (m_dlg == nullptr || m_dlg->obj == nullptr)
        return;
    fil::SpoolService&          service = Runtime::get().service();
    const std::set<std::string> busy    = service.busy_ids();
    std::vector<fil::Spool>     spools;
    for (const fil::Spool& r : service.confirmed())
        if (r.rfid.empty() && !r.in_printer && busy.count(r.id) == 0)
            spools.push_back(r);
    std::set<std::string> listed; // the Orca filament ids of the dialog's filament list for this printer
    for (const auto& item : m_dlg->map_filament_items)
        listed.insert(item.second.filament_id);

    SpoolPicker picker(m_dlg, spools, [&listed](const fil::Spool& s) { return !s.filament_id.empty() && listed.count(to_orca_id(s.filament_id)) > 0; });
    if (picker.ShowModal() != wxID_OK)
        return;
    for (const fil::Spool& s : spools)
        if (s.id == picker.chosen_id())
            m_chosen = s;
    if (!m_chosen)
        return;

    // Select the spool's filament the way Popup() preselects the tray's.
    const std::string orca_id = to_orca_id(m_chosen->filament_id);
    m_dlg->ams_filament_id    = orca_id; // set_color() names the colour of this filament; the selection event sets it too
    m_dlg->rebuild_filament_dropdown_items(orca_id);
    const int selection = m_dlg->m_comboBox_filament->GetSelection();
    m_dlg->post_select_event(selection);
    if (selection < 0)
        m_dlg->m_comboBox_filament->SetValue(wxEmptyString);

    // The colours, as the colour picker sets them (several only where the printer takes them).
    std::vector<wxColour> colours = spool_colours(*m_chosen);
    if (colours.size() > 1 && (colours.size() > ColorPickerPopup::MAX_EDITABLE_COLORS || !m_dlg->obj->is_support_filament_manual_multi_color))
        colours.resize(1);
    if (!colours.empty()) {
        m_dlg->set_ctype(colours.size() > 1 ? m_chosen->color_type : 2);
        m_dlg->set_color(colours.front());
        m_dlg->set_colors(colours);
    }

    update_texts();
    relayout();
}

void AmsDialogRow::clear_choice()
{
    m_chosen.reset();
    update_texts();
    update_box();
    relayout();
}

void AmsDialogRow::relayout()
{
    Layout();
    if (wxWindow* parent = GetParent()) {
        parent->InvalidateBestSize();
        parent->Layout();
    }
    if (m_dlg != nullptr) {
        m_dlg->Layout();
        m_dlg->Fit();
    }
}

void AmsDialogRow::on_confirmed(const std::string& tray_color, const std::vector<std::string>& tray_colors, int tray_ctype)
{
    try {
        if (!IsShown() || m_dlg == nullptr || m_dlg->obj == nullptr)
            return;
        const fil::SlotKey slot{dev_id(), m_dlg->ams_id, std::to_string(m_dlg->slot_id)};
        if (m_chosen) {
            // Only while the dialog still sets the spool's filament and colours.
            if (to_orca_id(m_chosen->filament_id) != m_dlg->ams_filament_id)
                return;
            const std::vector<std::string> sent  = tray_colors.empty() ? std::vector<std::string>{tray_color} : tray_colors;
            const std::vector<std::string> spool = colours_of(*m_chosen);
            if (sent.size() != spool.size())
                return;
            for (std::size_t i = 0; i < sent.size(); ++i)
                if (!fil::same_rgb(sent[i], spool[i]))
                    return;
            Runtime::get().assign(m_chosen->id, slot);
            return;
        }
        if (!m_box->IsShown() || !m_box->GetValue())
            return;
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (bundle == nullptr)
            return;
        const nlohmann::json page_spool = page_spool_for_filament(bundle->filaments, m_dlg->ams_filament_id, &BBLPrinterAgent::bambu_filament_id,
                                                                  tray_color, tray_colors, tray_ctype);
        if (page_spool.is_null())
            return;
        Runtime::get().create_and_assign(page_spool, slot);
    } catch (...) {
    }
}

} // namespace Slic3r::GUI::Fm
