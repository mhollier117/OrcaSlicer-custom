#include "FmDevices.hpp"

#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevManager.h"
#include "slic3r/GUI/DeviceManager.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <vector>

namespace Slic3r::GUI::Fm {

namespace {

std::string cut_color(const std::string& c)
{
    if (c.empty())
        return {};
    return ("#" + c).substr(0, 9);
}

// The whole text must be a number.
bool parse_number(const std::string& s, double& out)
{
    if (s.empty())
        return false;
    char* end = nullptr;
    out       = std::strtod(s.c_str(), &end);
    return end != nullptr && *end == '\0' && std::isfinite(out);
}

json machine_entry(MachineObject& m)
{
    const std::string id   = m.get_dev_id();
    const std::string name = m.get_dev_name();
    return {{"dev_id", id}, {"dev_name", name.empty() ? id : name}, {"is_online", m.is_online()}, {"is_lan", m.is_lan_mode_printer()}};
}

json empty_ams()
{
    return {{"selected_dev_id", ""}, {"ams_units", json::array()}, {"ext_slots", json::array()}, {"is_rack", false}};
}

} // namespace

std::string tray_label(int ams_id, int slot_index)
{
    if (slot_index < 0)
        return {};
    if (ams_id >= 0 && ams_id < 26)
        return std::string(1, static_cast<char>('A' + ams_id)) + std::to_string(slot_index + 1);
    if (ams_id >= 128 && ams_id <= 153)
        return std::string("HT-") + static_cast<char>('A' + (ams_id - 128));
    return {};
}

json tray_json(const DevAmsTray& tray, const std::string& slot_id, const std::string& label, bool exists, const TrayContext& ctx)
{
    json j = {{"slot_id", slot_id}, {"tray_label", label}, {"is_exists", exists}};
    if (!exists)
        return j;

    const std::string setting_id = ctx.to_bambu ? ctx.to_bambu(tray.setting_id) : tray.setting_id;
    const std::string color      = cut_color(tray.color);

    std::vector<std::string> colors;
    for (const std::string& c : tray.cols)
        if (!c.empty())
            colors.push_back(cut_color(c));
    if (tray.cols.empty() && !color.empty())
        colors.push_back(color);

    const int color_type = (tray.ctype >= 0 && tray.ctype <= 2) ? tray.ctype : (colors.size() > 1 ? 1 : 2);

    json remain_weight = nullptr;
    double weight      = 0;
    if (parse_number(tray.weight, weight) && weight > 0 && tray.remain >= 0 && tray.remain <= 100)
        remain_weight = static_cast<long long>(std::llround(weight * tray.remain / 100.0));

    j["is_unknown"]    = !tray.is_tray_info_ready();
    j["tag_uid"]       = (tray.tag_uid.size() == 16 && tray.tag_uid.compare(12, 2, "01") == 0) ? tray.uuid : std::string();
    j["tray_id_name"]  = "";
    j["setting_id"]    = setting_id;
    j["fila_type"]     = tray.m_fila_type;
    j["sub_brands"]    = tray.sub_brands;
    j["color"]         = color;
    j["weight"]        = tray.weight;
    j["remain"]        = tray.remain;
    j["remain_g"]      = -1;
    j["remain_weight"] = std::move(remain_weight);
    j["diameter"]      = tray.diameter;
    j["is_bbl"]        = tray.is_bbl;
    j["colors"]        = colors;
    j["color_type"]    = color_type;
    if (ctx.colors != nullptr)
        if (const ColorEntry* e = ctx.colors->find(setting_id, colors, color_type)) {
            j["color_name"]      = ColorCodes::name_for(*e, ctx.lang);
            j["fila_color_code"] = e->fila_color_code;
        }
    return j;
}

json machine_list(DeviceManager* mgr)
{
    json machines = json::array();
    json out      = {{"machines", json::array()}, {"selected_dev_id", ""}};
    if (mgr == nullptr)
        return out;
    try {
        std::map<std::string, MachineObject*> all = mgr->get_my_machine_list();
        for (const auto& [id, m] : mgr->get_user_machinelist())
            all.emplace(id, m);
        for (const auto& [id, m] : all)
            if (m != nullptr)
                machines.push_back(machine_entry(*m));
        out["machines"] = std::move(machines);
        if (MachineObject* sel = mgr->get_selected_machine())
            out["selected_dev_id"] = sel->get_dev_id();
    } catch (const std::exception&) {
    }
    return out;
}

json ams_data(DeviceManager* mgr, const TrayContext& ctx)
{
    if (mgr == nullptr)
        return empty_ams();
    try {
        MachineObject* m = mgr->get_selected_machine();
        if (m == nullptr)
            return empty_ams();

        json units = json::array();
        if (std::shared_ptr<DevFilaSystem> fila = m->GetFilaSystem()) {
            for (auto& entry : fila->GetAmsList()) {
                const DevAms* ams = entry.second;
                if (ams == nullptr)
                    continue;
                const std::string ams_id_text = ams->GetAmsId();
                const int         ams_id      = std::atoi(ams_id_text.c_str());
                json              trays       = json::array();
                for (int i = 0; i < ams->GetSlotCount(); ++i) {
                    const DevAmsTray* tray = ams->GetTray(std::to_string(i));
                    if (tray != nullptr)
                        trays.push_back(tray_json(*tray, std::to_string(i), tray_label(ams_id, i), tray->is_exists, ctx));
                }
                const wxString name = ams->GetDisplayName();
                units.push_back({{"ams_id", ams_id_text},
                                 {"ams_type", static_cast<int>(ams->GetAmsType())},
                                 {"display_name", std::string(name.utf8_str())},
                                 {"trays", std::move(trays)}});
            }
        }

        std::vector<const DevAmsTray*> ext;
        for (const DevAmsTray& t : m->vt_slot)
            ext.push_back(&t);
        std::stable_sort(ext.begin(), ext.end(), [](const DevAmsTray* a, const DevAmsTray* b) { return std::atoi(a->id.c_str()) > std::atoi(b->id.c_str()); });
        json ext_slots = json::array();
        for (const DevAmsTray* t : ext) {
            json e       = tray_json(*t, "0", "Ext", t->is_exists || t->is_tray_info_ready(), ctx);
            e["ams_id"] = t->id;
            ext_slots.push_back(std::move(e));
        }
        return {{"selected_dev_id", m->get_dev_id()}, {"ams_units", std::move(units)}, {"ext_slots", std::move(ext_slots)}, {"is_rack", false}};
    } catch (const std::exception&) {
        return empty_ams();
    }
}

bool request_pushall(DeviceManager* mgr, const std::string& dev_id, std::string& used_dev_id)
{
    if (mgr == nullptr)
        return false;
    MachineObject* m = dev_id.empty() ? mgr->get_selected_machine() : mgr->get_my_machine(dev_id);
    if (m == nullptr)
        return false;
    m->command_get_version();
    m->command_request_push_all(true);
    used_dev_id = m->get_dev_id();
    return true;
}

void select_machine(DeviceManager* mgr, const std::string& dev_id)
{
    if (mgr == nullptr || dev_id.empty())
        return;
    MachineObject* sel = mgr->get_selected_machine();
    if (sel == nullptr || sel->get_dev_id() != dev_id)
        mgr->set_selected_machine(dev_id);
}

} // namespace Slic3r::GUI::Fm
