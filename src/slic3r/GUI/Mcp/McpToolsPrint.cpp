// Printer MCP tools (fork): filament_sync, print_plan, printer_print.
// Design: C:/claude-code/orca_mcp/docs/specs/2026-11-02-printer-mcp-design.md

#include "McpPrintPlanner.hpp"
#include "McpPrinterCommon.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "slic3r/GUI/I18N.hpp"

#include "slic3r/GUI/DeviceCore/DevFilaSwitch.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevMappingNozzle.h"
#include "slic3r/GUI/DeviceCore/DevNozzleRack.h"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/DeviceCore/DevConfigUtil.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Jobs/BoostThreadWorker.hpp"
#include "slic3r/GUI/Jobs/PlaterWorker.hpp"
#include "slic3r/GUI/Jobs/PrintJob.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/Tab.hpp"
#include "slic3r/Utils/PublishCapture.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

#include <boost/algorithm/string/join.hpp>

#include <atomic>
#include <chrono>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

// ---- shared argument handling ----------------------------------------------------------------

json overrides_prop()
{
    json item = {{"type", "object"},
                 {"properties",
                  {{"filament", {{"type", "integer"}, {"minimum", 1}, {"description", "1-based project filament slot (as print_plan lists it)."}}},
                   {"ams_id", {{"type", {"string", "integer"}}, {"description", "AMS unit id of the tray (\"0\", \"128\" for an AMS-HT...)."}}},
                   {"slot_id", {{"type", {"string", "integer"}}, {"description", "Slot of the tray within its AMS unit."}}},
                   {"external", {{"type", "string"}, {"enum", {"left", "right", "main"}}, {"description", "Use the external spool of that nozzle instead of an AMS tray."}}}}},
                 {"required", {"filament"}},
                 {"additionalProperties", false}};
    return {{"type", "array"},
            {"items", item},
            {"description", "Manual tray choices that replace the automatic mapping of single filaments, checked by the Print dialog's rules: "
                            "same filament type, tray not empty, tray on the side of the filament's nozzle, no external spool with a filament switch."}};
}

json options_prop()
{
    return {{"type", "object"},
            {"properties",
             {{"bed_leveling", {{"type", "string"}, {"enum", {"off", "on", "auto"}}}},
              {"flow_cali", {{"type", "string"}, {"enum", {"off", "on", "auto"}}, {"description", "Flow dynamics calibration."}}},
              {"nozzle_offset_cali", {{"type", "string"}, {"enum", {"off", "on", "auto"}}}},
              {"pa_value", {{"type", "string"}, {"enum", {"off", "on"}}, {"description", "Shared PA profile (printers with pa_mode)."}}},
              {"timelapse", {{"type", "boolean"}}},
              {"timelapse_storage", {{"type", "string"}, {"enum", {"internal", "external"}}}},
              {"ext_change_assist", {{"type", "boolean"}}},
              {"acknowledge_nozzle_diameter_mismatch", {{"type", "boolean"}, {"description", "Print although the installed nozzle diameter differs from the slicing file (the dialog's acknowledgement checkbox)."}}}}},
            {"additionalProperties", false},
            {"description", "Print options. What is left out comes from the app config for this printer model, as in the Print dialog."}};
}

// The plate a call acts on (0-based, as slice and the other plate tools number them). UI thread.
int plate_arg(const json& args)
{
    PartPlateList& plates = ui_plater().get_partplate_list();
    const int      index  = arg_int(args, "plate", plates.get_curr_plate_index());
    if (index < 0 || index >= plates.get_plate_count())
        throw ToolFailure("plate must be 0.." + std::to_string(plates.get_plate_count() - 1));
    return index;
}

json plan_args(const json& args, bool allow_round_trip)
{
    json options = args.value("options", json::object());
    if (allow_round_trip)
        options["allow_round_trip"] = true;
    return options;
}

// ---- filament_sync ---------------------------------------------------------------------------

std::string diameter_string(double diameter)
{
    // mirrors Plater.cpp:375 get_diameter_string
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << float(diameter);
    std::string s = stream.str();
    if (s.find('.') != std::string::npos) {
        s.erase(s.find_last_not_of('0') + 1);
        if (s.back() == '.')
            s += '0';
    }
    return s;
}

const char* flow_name(NozzleVolumeType type)
{
    switch (type) {
    case NozzleVolumeType::nvtStandard: return "standard";
    case NozzleVolumeType::nvtHighFlow: return "high_flow";
    case NozzleVolumeType::nvtHybrid: return "hybrid";
    case NozzleVolumeType::nvtTPUHighFlow: return "tpu_high_flow";
    case NozzleVolumeType::nvtE3DHighFlow: return "e3d_high_flow";
    default: return "unknown";
    }
}

struct NozzleSync
{
    int                           extruder_nums = 1;
    std::vector<float>            diameters;       // by physical extruder id
    std::vector<NozzleVolumeType> types;           // by config extruder index
    int                           ams[2][2]{};     // [config extruder (0 left/deputy, 1 right/main)][0: AMS 4-slot, 1: single-slot]
    bool                          switch_ready = false;
    json                          warnings = json::array();
};

// mirrors Plater.cpp:2327 Sidebar::priv::sync_extruder_list (the data part, without the widgets and dialogs)
NozzleSync compute_nozzle_sync(Plater& plater, MachineObject& obj)
{
    PresetBundle& pb = *wxGetApp().preset_bundle;
    if (!plater.check_printer_initialized(&obj, false, false))
        throw ToolFailure("The printer is not initialized yet; wait for it to report its nozzle information");
    const std::string target_model = pb.printers.get_selected_preset().get_printer_type(&pb);
    const bool        optional_printer = DevPrinterConfigUtil::is_optional_printer_model_id(obj.printer_type);
    const bool        optional_target  = DevPrinterConfigUtil::is_optional_printer_model_id(target_model);
    if (!optional_printer && !optional_target && obj.get_show_printer_type() != target_model)
        throw ToolFailure("The selected printer preset (" + target_model + ") does not match the connected printer (" + obj.get_show_printer_type() +
                              "); select the matching printer preset first (the Print dialog asks the same question)",
                          {{"selected_preset_model", target_model}, {"printer_model", obj.get_show_printer_type()}});

    NozzleSync        out;
    const Preset&     cur_preset = pb.printers.get_selected_preset();
    out.extruder_nums            = pb.get_printer_extruder_count();
    if (obj.GetExtderSystem()->GetTotalExtderCount() != out.extruder_nums)
        throw ToolFailure("The printer reports " + std::to_string(obj.GetExtderSystem()->GetTotalExtderCount()) + " extruders but the printer preset has " +
                          std::to_string(out.extruder_nums));
    std::vector<int> extruder_map(size_t(out.extruder_nums));
    for (int i = 0; i < out.extruder_nums; ++i)
        extruder_map[size_t(i)] = i;
    if (const auto* m = cur_preset.config.option<ConfigOptionInts>("physical_extruder_map"); m != nullptr && int(m->values.size()) == out.extruder_nums)
        extruder_map = m->values;

    if (const auto* max_count = cur_preset.config.option<ConfigOptionIntsNullable>("extruder_max_nozzle_count"))
        for (int v : max_count->values)
            if (v > 1 && v != ConfigOptionIntsNullable::nil_value())
                throw ToolFailure("This printer holds several nozzles per extruder (hotend rack); choosing the nozzle set needs the sync dialog, which this tool does not replace");

    out.diameters.assign(size_t(out.extruder_nums), 0.f);
    out.types.assign(size_t(out.extruder_nums), NozzleVolumeType::nvtStandard);
    for (int index = 0; index < out.extruder_nums; ++index) {
        const int extruder_id = extruder_map[size_t(index)];
        if (extruder_id < 0 || extruder_id >= out.extruder_nums)
            throw ToolFailure("physical_extruder_map of the printer preset is out of range");
        out.diameters[size_t(extruder_id)] = obj.GetExtderSystem()->GetNozzleDiameter(index);
        NozzleVolumeType target = NozzleVolumeType::nvtStandard;
        if (obj.is_nozzle_flow_type_supported()) {
            if (obj.GetExtderSystem()->GetNozzleFlowType(index) == NozzleFlowType::NONE_FLOWTYPE) {
                out.warnings.push_back("extruder " + std::to_string(index) + " has no nozzle type set on the printer; its flow type was left as it is");
                continue;
            }
            if (std::fabs(out.diameters[size_t(extruder_id)] - 0.2) > EPSILON)
                target = DevNozzle::ToNozzleVolumeType(obj.GetExtderSystem()->GetNozzleFlowType(extruder_id));
        }
        out.types[size_t(index)] = target;
    }

    // switch readiness (Sidebar::priv::is_fila_switch_ready)
    out.switch_ready = plater.is_same_printer_for_connected_and_selected(false) && obj.is_online() && obj.GetFilaSwitch() != nullptr &&
                       obj.GetFilaSwitch()->IsInstalled() && obj.GetFilaSwitch()->IsReady();
    for (const auto& ams : obj.GetFilaSystem()->GetAmsList()) {
        for (int extruder_id : ams.second->GetBindedExtruderSet()) {
            if (out.switch_ready) {
                auto pos = ams.second->GetSwitcherPos();
                if (!pos)
                    continue;
                if (extruder_id != static_cast<int>(pos.value())) // is_main_extruder_on_left() is false
                    continue;
            }
            const bool is_n3s = ams.second->GetAmsType() == DevAms::N3S;
            // Main (first) extruder is id 0 and sits on the right: config index 1. Deputy: index 0.
            if (extruder_id == 0)
                ++out.ams[1][is_n3s ? 1 : 0];
            else if (extruder_id == 1)
                ++out.ams[0][is_n3s ? 1 : 0];
        }
    }
    return out;
}

json nozzle_sync_json(const NozzleSync& sync)
{
    PresetBundle& pb = *wxGetApp().preset_bundle;
    const auto*   diameters = pb.printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto*   volumes   = dynamic_cast<const ConfigOptionEnumsGeneric*>(pb.project_config.option("nozzle_volume_type"));
    json          nozzles   = json::array();
    for (int i = 0; i < sync.extruder_nums; ++i) {
        json n;
        n["extruder"] = sync.extruder_nums == 1 ? "single" : (i == 0 ? "left" : "right");
        n["before"]   = {{"diameter", diameters && size_t(i) < diameters->values.size() ? json(diameters->values[size_t(i)]) : json()},
                         {"flow", volumes && size_t(i) < volumes->size() ? json(flow_name(NozzleVolumeType(volumes->get_at(size_t(i))))) : json()}};
        n["after"]    = {{"diameter", sync.diameters[size_t(i)]}, {"flow", flow_name(sync.types[size_t(i)])}};
        nozzles.push_back(std::move(n));
    }
    json counts = json::array();
    if (sync.extruder_nums > 1)
        for (int i = 0; i < 2; ++i) {
            json c;
            c["extruder"] = i == 0 ? "left" : "right";
            int b4 = 0, b1 = 0;
            if (pb.extruder_ams_counts.size() == 2) {
                auto it4 = pb.extruder_ams_counts[size_t(i)].find(4);
                auto it1 = pb.extruder_ams_counts[size_t(i)].find(1);
                b4 = it4 != pb.extruder_ams_counts[size_t(i)].end() ? it4->second : 0;
                b1 = it1 != pb.extruder_ams_counts[size_t(i)].end() ? it1->second : 0;
            }
            c["before"] = {{"ams", b4}, {"ams_single_slot", b1}};
            c["after"]  = {{"ams", sync.ams[i][0]}, {"ams_single_slot", sync.ams[i][1]}};
            counts.push_back(std::move(c));
        }
    return {{"nozzles", nozzles}, {"ams_counts", counts}, {"filament_switch", {{"ready", sync.switch_ready}}}, {"warnings", sync.warnings}};
}

// mirrors Plater.cpp:1896 switch_diameter_to. Returns whether the printer preset is right afterwards.
bool switch_diameter_to(const std::string& diameter)
{
    PresetBundle& pb             = *wxGetApp().preset_bundle;
    Preset&       printer_preset = pb.printers.get_edited_preset();
    auto*         nozzle_diameter = dynamic_cast<const ConfigOptionFloats*>(printer_preset.config.option("nozzle_diameter"));
    const bool    nozzles_apart = nozzle_diameter && nozzle_diameter->size() > 1 && pb.is_bbl_vendor() &&
                               std::any_of(nozzle_diameter->values.begin(), nozzle_diameter->values.end(),
                                           [&diameter](double value) { return diameter_string(value) != diameter; });
    const std::string& printer_variant = printer_preset.config.opt_string("printer_variant");
    if (printer_variant == diameter && !nozzles_apart)
        return true;
    if (printer_variant.empty() && nozzle_diameter && nozzle_diameter->size() > 0 && diameter_string(nozzle_diameter->values[0]) == diameter)
        return true;
    Preset* preset = pb.get_similar_printer_preset({}, diameter);
    if (preset == nullptr)
        throw ToolFailure("The printer has a " + diameter + " mm nozzle but no printer preset is configured for that diameter");
    preset->is_visible = true;
    return wxGetApp().get_tab(Preset::TYPE_PRINTER)->select_preset(preset->name);
}

void apply_nozzle_sync(Plater& plater, const NozzleSync& sync)
{
    PresetBundle& pb = *wxGetApp().preset_bundle;
    if (sync.extruder_nums > 1) {
        // mirrors Plater.cpp:1866 switch_diameter(false)
        const std::string left = diameter_string(sync.diameters[0]), right = diameter_string(sync.diameters[1]);
        if (left != right) {
            Tab*                tab      = wxGetApp().get_tab(Preset::TYPE_PRINTER);
            DynamicPrintConfig  new_conf = pb.printers.get_edited_preset().config;
            auto*               opt      = new_conf.option<ConfigOptionFloats>("nozzle_diameter");
            if (tab == nullptr || opt == nullptr || opt->size() < 2)
                throw ToolFailure("The printer preset has no per-extruder nozzle diameters");
            opt->values[0] = sync.diameters[0];
            opt->values[1] = sync.diameters[1];
            tab->load_config(new_conf);
        } else {
            switch_diameter_to(left);
        }
        // mirrors AMSCountPopupWindow::SetAMSCount (Plater.cpp:1480)
        pb.extruder_ams_counts.resize(2);
        for (int i = 0; i < 2; ++i) {
            pb.extruder_ams_counts[size_t(i)][4] = sync.ams[i][0];
            pb.extruder_ams_counts[size_t(i)][1] = sync.ams[i][1];
        }
        const std::vector<std::string> strs = save_extruder_ams_count_to_string(pb.extruder_ams_counts);
        wxGetApp().app_config->set("presets", "extruder_ams_count", boost::algorithm::join(strs, ","));
        plater.update();
    } else {
        switch_diameter_to(diameter_string(sync.diameters[0]));
    }
    // after switching the preset, so these values override the old ones stored in the config
    auto* printer_tab = dynamic_cast<TabPrinter*>(wxGetApp().get_tab(Preset::TYPE_PRINTER));
    if (printer_tab != nullptr)
        for (size_t idx = 0; idx < sync.types.size(); ++idx)
            printer_tab->set_extruder_volume_type(int(idx), sync.types[idx]);
    // Copy the live switch state into the project (Plater.cpp:2496)
    if (auto* dyn = pb.project_config.opt<ConfigOptionBool>("enable_filament_dynamic_map"))
        dyn->value = sync.switch_ready;
    if (auto* has = pb.project_config.opt<ConfigOptionBool>("has_filament_switcher"))
        has->value = sync.switch_ready;
}

json slots_json()
{
    PresetBundle& pb = *wxGetApp().preset_bundle;
    json          out = json::array();
    const auto*   colours = pb.project_config.option<ConfigOptionStrings>("filament_colour");
    for (size_t i = 0; i < pb.filament_presets.size(); ++i)
        out.push_back({{"slot", i + 1},
                       {"preset", pb.filament_presets[i]},
                       {"color", colours && i < colours->values.size() ? colours->values[i] : std::string()}});
    return out;
}

// The AMS part's preview: which preset each tray would select (PresetBundle::sync_ams_list' rule for a known filament
// id; an unknown one falls back to a generic preset of its type, which only the sync itself decides).
json ams_preview(PresetBundle& pb)
{
    json out = json::array();
    for (const auto& [key, tray] : pb.filament_ams_list) {
        const std::string filament_id = tray.opt_string("filament_id", 0u);
        json              t;
        t["tray"]      = tray.opt_string("tray_name", 0u);
        t["ams_id"]    = tray.opt_string("ams_id", 0u);
        t["slot_id"]   = tray.opt_string("slot_id", 0u);
        t["type"]      = tray.opt_string("filament_type", 0u);
        t["color"]     = tray.opt_string("filament_colour", 0u);
        t["filament_id"] = filament_id;
        if (filament_id.empty()) {
            t["preset"] = tray.has("filament_slot_placeholder") && tray.opt_bool("filament_slot_placeholder", 0u) ? json("(placeholder)") : json(nullptr);
        } else {
            std::string name;
            for (const Preset& f : pb.filaments)
                if (f.is_compatible && pb.filaments.get_preset_base(f) == &f && f.filament_id == filament_id) {
                    name = f.name;
                    break;
                }
            t["preset"] = name.empty() ? json("(unknown id; a generic " + tray.opt_string("filament_type", 0u) + " preset is chosen)") : json(name);
        }
        out.push_back(std::move(t));
    }
    return out;
}

// mirrors Plater.cpp:6297 Sidebar::sync_ams_list with the dialog's OK result for "override": direct_sync = true.
// The mapping page of the dialog (non-empty projects) is not replaced: the project's filaments are overwritten by the
// trays in order, as the dialog's Override mode does.
json sync_ams(Plater& plater, MachineObject& obj, bool color_only, bool dry_run)
{
    PresetBundle& pb = *wxGetApp().preset_bundle;
    Sidebar&      sidebar = plater.sidebar();
    sidebar.load_ams_list(&obj);
    auto& list = pb.filament_ams_list;
    if (list.empty()) {
        if (obj.is_connected() && obj.m_push_count > 0)
            throw ToolFailure("The printer answered but reports no filament system (no AMS and no external spool)");
        throw ToolFailure("The printer reports no filament slots; is it connected?");
    }
    bool any = false;
    for (const auto& [key, cur] : list)
        if (!cur.opt_string("filament_type", 0u).empty() || cur.opt_bool("filament_exist", 0u)) {
            any = true;
            break;
        }
    if (!any)
        throw ToolFailure(obj.is_filament_installed() ? "All filament slots of the printer are empty" : "No filament is installed in the printer");
    if (!plater.is_same_printer_for_connected_and_selected(false))
        throw ToolFailure("The selected printer preset does not match the connected printer; select the matching printer preset first");

    json result = {{"before", slots_json()}, {"mode", color_only ? "color_only" : "override"}};
    if (dry_run) {
        result["trays"] = ams_preview(pb);
        return result;
    }

    // ams_filament_ids bookkeeping (the dialog's OK is wxID_YES: every tray counts as changed)
    AppConfig*               config = wxGetApp().app_config;
    std::vector<std::string> ids;
    {
        const std::string stored = config->get("ams_filament_ids", obj.get_dev_id());
        if (!stored.empty())
            boost::algorithm::split(ids, stored, boost::algorithm::is_any_of(","));
        ids.resize(list.size());
        auto it = list.begin();
        for (size_t i = 0; i < list.size(); ++i, ++it) {
            it->second.set_key_value("filament_changed", new ConfigOptionBool{true});
            ids[i] = it->second.opt_string("filament_id", 0u);
        }
    }
    std::vector<std::string> colors_before;
    for (const std::string& c : pb.project_config.option<ConfigOptionStrings>("filament_colour")->values)
        colors_before.push_back(c);

    MergeFilamentInfo                                         merge_info;
    std::vector<std::pair<DynamicPrintConfig*, std::string>> unknowns;
    std::map<int, AMSMapInfo>                                maps;
    const bool enable_append = config->get_bool("enable_append_color_by_sync_ams");
    const auto n             = pb.sync_ams_list(unknowns, false, maps, enable_append, merge_info, color_only);
    json       unknown_json  = json::array();
    for (auto& [tray, why] : unknowns)
        unknown_json.push_back({{"tray", tray->opt_string("tray_name", 0u)}, {"type", tray->opt_string("filament_type", 0u)}, {"note", why}});
    if (n == 0)
        throw ToolFailure("There are no compatible filaments, and sync is not performed", {{"unknown", unknown_json}});

    for (size_t i = 0; i < ids.size() && i < pb.filament_presets.size(); ++i)
        if (ids[i] == UNKNOWN_FILAMENT_ID)
            if (const Preset* resolved = pb.filaments.find_preset(pb.filament_presets[i]))
                ids[i] = resolved->filament_id;
    config->set("ams_filament_ids", obj.get_dev_id(), boost::algorithm::join(ids, ","));

    if (!color_only)
        plater.on_filament_count_change(n);
    for (auto* c : sidebar.combos_filament())
        c->update();
    sidebar.update_filaments_area_height();
    const auto* colors_after = pb.project_config.option<ConfigOptionStrings>("filament_colour");
    for (size_t i = 0; i < sidebar.combos_filament().size(); ++i) {
        if (i >= colors_before.size())
            sidebar.auto_calc_flushing_volumes(int(i));
        else if (colors_after && i < colors_after->values.size() && colors_before[i] != colors_after->values[i] &&
                 config->get("auto_calculate_flush") != "disabled")
            sidebar.auto_calc_flushing_volumes(int(i));
    }
    if (!color_only) {
        pb.install_slot_filaments(*config);
        wxGetApp().get_tab(Preset::TYPE_FILAMENT)->select_preset(pb.filament_presets[0]);
        pb.export_selections(*config);
        sidebar.update_dynamic_filament_list();
    } else {
        plater.update_filament_colors_in_full_config();
        for (auto* c : sidebar.combos_filament())
            c->update();
        sidebar.obj_list()->update_filament_colors();
        sidebar.update_dynamic_filament_list();
    }
    // badges of the synced slots (the first branch of Sidebar::sync_ams_list' badge block)
    sidebar.clear_combos_filament_badge();
    size_t combo_index = 0;
    for (const auto& [key, tray] : pb.filament_ams_list) {
        const bool has_filament   = !tray.opt_string("filament_id", 0u).empty();
        const bool is_placeholder = tray.has("filament_slot_placeholder") && tray.opt_bool("filament_slot_placeholder", 0u);
        if (!has_filament && !is_placeholder)
            continue;
        if (combo_index >= sidebar.combos_filament().size())
            break;
        sidebar.combos_filament()[combo_index]->set_sync_badge(has_filament && !is_placeholder);
        ++combo_index;
    }
    if (!merge_info.is_empty() && config->get_bool("enable_merge_color_by_sync_ams"))
        result["note"] = "enable_merge_color_by_sync_ams is on: merging identical filaments is a dialog step and was not applied";
    result["after"]   = slots_json();
    result["trays"]   = ams_preview(pb);
    result["unknown"] = unknown_json;
    return result;
}

// ---- printer_print ---------------------------------------------------------------------------

// Records what a PrintJob reports through its worker's progress indicator.
class JobProgress : public ProgressIndicator
{
public:
    void clear_percent() override {}
    void show_error_info(wxString msg, int code, wxString description, wxString extra) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_error = msg.utf8_string();
        m_code  = code;
        m_detail = (description + " " + extra).utf8_string();
    }
    void set_range(int range) override { m_range = range; }
    void set_cancel_callback(CancelFn) override {}
    void set_progress(int pr) override { m_progress = pr; }
    void set_status_text(const char* text) override
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_status = text ? text : "";
    }
    int get_range() const override { return m_range; }

    std::string status() const { std::lock_guard<std::mutex> lock(m_mutex); return m_status; }
    std::string error() const { std::lock_guard<std::mutex> lock(m_mutex); return m_error; }
    std::string detail() const { std::lock_guard<std::mutex> lock(m_mutex); return m_detail; }
    int         code() const { return m_code; }
    int         progress() const { return m_progress; }

private:
    mutable std::mutex m_mutex;
    std::string        m_status, m_error, m_detail;
    std::atomic<int>   m_code{0}, m_progress{0}, m_range{100};
};

// Holds the last print worker so a cancelled call leaves no dangling thread behind.
std::shared_ptr<Worker>& last_worker()
{
    static std::shared_ptr<Worker> worker;
    return worker;
}

json checks_for_error(const PrintPlan& plan, const char* severity)
{
    json out = json::array();
    for (const PlanCheck& c : plan.checks)
        if (c.severity == severity)
            out.push_back({{"id", c.id}, {"message", c.message}});
    return out;
}

} // namespace

void register_print_tools(McpService& service)
{
    // ---- filament_sync -----------------------------------------------------------------------
    service.registry().add(mutate_tool(
        "filament_sync", L("Sync printer information"),
        "Bring the project in line with a printer, as the sidebar's \"Sync printer information\" and \"Sync AMS\" do. "
        "Step 1 (include_nozzles, default true): per extruder nozzle diameter (switching the printer preset to the one for "
        "that diameter) and flow type, the number of AMS units per extruder, and the filament switch flags. Step 2: the project's "
        "filaments take the presets, colours and types of the printer's AMS trays and external spools (mode \"override\": "
        "the filaments are replaced by the trays in order; color_only keeps the presets and only copies colours). "
        "The printer is selected first and its state awaited. dry_run returns the planned changes ({nozzles, ams_counts, "
        "filaments}) and changes nothing; the AMS part lists which preset each tray selects (unknown filaments fall back "
        "to a generic preset, decided when the sync runs). Printers that hold several nozzles per extruder (hotend rack) are "
        "refused in step 1: their nozzle picker is a dialog. May raise the dialogs of the GUI (unsaved preset changes).",
        object_schema({{"dev_id", dev_id_prop()},
                       {"include_nozzles", prop("boolean", "Also sync nozzle diameters, flow types and AMS counts (default true).")},
                       {"color_only", prop("boolean", "Only copy tray colours into the project; keep its filament presets (default false).")},
                       {"dry_run", dry_run_prop()}}),
        [](const json& args, ToolContext& ctx) {
            const bool include_nozzles = arg_bool(args, "include_nozzles", true);
            const bool color_only      = arg_bool(args, "color_only", false);
            const bool dry_run         = arg_bool(args, "dry_run", false);
            PrinterTarget target       = resolve_printer(ctx, args, true, true);
            json result = ctx.run_ui([&]() -> json {
                MachineObject& obj    = ui_printer(target.dev_id);
                Plater&        plater = ui_plater();
                json           out    = {{"dev_id", target.dev_id}, {"dry_run", dry_run}, {"selected_now", target.selected_now}};
                if (include_nozzles) {
                    NozzleSync sync = compute_nozzle_sync(plater, obj);
                    out["nozzle_sync"] = nozzle_sync_json(sync);
                    if (!dry_run)
                        apply_nozzle_sync(plater, sync);
                }
                out["ams_sync"] = sync_ams(plater, obj, color_only, dry_run);
                return out;
            });
            if (!dry_run)
                settle(ctx);
            return ToolResult::ok(redact(std::move(result)));
        }));

    // ---- print_plan --------------------------------------------------------------------------
    service.registry().add(inspect_tool(
        "print_plan", L("Plan a print"),
        "Show what the Print dialog would do for a sliced plate on the selected printer, without sending anything: the automatic "
        "filament to AMS tray / external spool mapping exactly as the dialog computes it (single nozzle, dual nozzle with each "
        "filament limited to its own side's AMS, external spools only where a side has no AMS, filament switch), your overrides "
        "validated by the dialog's rules, the encodings the job carries (ams_mapping: tray index per project slot with -1 for unused or "
        "external; ams_mapping2: {ams_id, slot_id}, external 254 left / 255 right, unused 255/255; ams_mapping_info; nozzles_info for two "
        "nozzles), the blocking errors (the dialog's checks, with stable ids and its wording) and the warnings, "
        "needs_confirmation for those the dialog confirms in a popup before sending, and sendable. plate is the 0-based plate "
        "index as slice and list_plates number them (default: the current plate); the plate must be sliced. dev_id must be the "
        "selected printer (printer_list select); this call selects nothing. On a hotend-rack printer the nozzle mapping needs a "
        "round trip to the printer, which printer_print performs.",
        object_schema({{"plate", prop("integer", "0-based plate index; default the current plate.")},
                       {"dev_id", dev_id_prop()},
                       {"overrides", overrides_prop()},
                       {"options", options_prop()}}),
        [](const json& args, ToolContext& ctx) {
            json result = ctx.run_ui([&]() -> json {
                MachineObject* printer = ui_selected_printer();
                const std::string wanted = arg_string(args, "dev_id");
                if (!wanted.empty()) {
                    MachineObject& named = ui_printer(wanted);
                    if (printer == nullptr || printer->get_dev_id() != wanted)
                        throw ToolFailure("Printer \"" + named.get_dev_name() + "\" is not the selected printer; select it with printer_list {action: \"select\"} first",
                                          {{"dev_id", wanted}});
                }
                if (printer == nullptr)
                    throw ToolFailure("No printer is selected; select one with printer_list {action: \"select\"}");
                const int plate = plate_arg(args);
                PrintPlan plan  = ui_build_plan(ui_plater(), plate, *printer, args.value("overrides", json::array()), plan_args(args, false));
                json      out   = plan_to_json(plan);
                out["printer"]  = {{"dev_id", printer->get_dev_id()}, {"name", printer->get_dev_name()}};
                return redact(std::move(out));
            });
            return ToolResult::ok(std::move(result));
        }));

    // ---- printer_print -----------------------------------------------------------------------
    ToolDef print_tool = mutate_tool(
        "printer_print", L("Print on a printer"),
        "Send a sliced plate to a printer and start the print, as the Print dialog's Send does. Runs the same plan as print_plan "
        "(same arguments: plate, overrides, options) and refuses with the blocking errors in details when it is not sendable; "
        "warnings the dialog confirms in a popup (plan needs_confirmation) refuse the call unless confirm_warnings is true. "
        "The plate is selected, its sliced 3mf (and for cloud printers its config 3mf) exported like Plater::send_gcode, and "
        "the job runs on a worker: on a hotend-rack printer the printer is first asked for its nozzle mapping (up to 10 s). "
        "dry_run builds the plan and the PrintParams the job would send (secrets redacted) and sends nothing: no file is exported "
        "and no job is created. Real runs return {started, message}; the Monitor tab is shown afterwards as in the dialog. "
        "plate is the 0-based plate index. A LAN printer that rejects the access code or address fails the call instead of opening "
        "the dialog's address prompt.",
        object_schema({{"plate", prop("integer", "0-based plate index; default the current plate.")},
                       {"dev_id", dev_id_prop()},
                       {"overrides", overrides_prop()},
                       {"options", options_prop()},
                       {"confirm_warnings", prop("boolean", "Proceed although the plan has warnings the dialog asks to confirm.")},
                       {"dry_run", dry_run_prop()}}),
        [](const json& args, ToolContext& ctx) {
            const bool dry_run = arg_bool(args, "dry_run", false);
            PrinterTarget target = resolve_printer(ctx, args, true, true);
            const json overrides = args.value("overrides", json::array());

            // Phase 1: select the plate, and for a hotend rack ask the printer for its nozzle mapping.
            int  plate_idx      = 0;
            bool needs_round_trip = false;
            ctx.run_ui([&]() -> json {
                MachineObject& obj    = ui_printer(target.dev_id);
                Plater&        plater = ui_plater();
                plate_idx             = plate_arg(args);
                if (plater.is_slicing_any())
                    throw ToolFailure("Slicing is in progress; wait {for:\"idle\"} and try again");
                if (plate_idx != plater.get_partplate_list().get_curr_plate_index() && plater.select_plate(plate_idx) != 0)
                    throw ToolFailure("Plate " + std::to_string(plate_idx) + " could not be selected");
                if (dry_run)
                    return json();
                PrintPlan plan = ui_build_plan(plater, plate_idx, obj, overrides, plan_args(args, true));
                for (const PlanCheck& c : plan.checks)
                    if (c.id == "rack_nozzle_mapping_pending") {
                        ctx.begin_operation();
                        const int flow_cali = plan.options.flow_cali == "off" ? 0 : plan.options.flow_cali == "on" ? 1 : 2;
                        const int pa        = obj.is_support_pa_mode ? (plan.options.pa_value == "on" ? 0 : 1) : 1;
                        const int rc = plan.project.dynamic_nozzle_map ?
                                           obj.get_nozzle_mapping_result()->CtrlGetAutoNozzleMappingV1(&plater) :
                                           obj.get_nozzle_mapping_result()->CtrlGetAutoNozzleMappingV0(&plater, plan.run.result, flow_cali, pa);
                        if (rc != 0)
                            throw ToolFailure("Failed to send the nozzle auto-mapping request to the printer (code " + std::to_string(rc) + ")");
                        needs_round_trip = true;
                    }
                return json();
            });
            if (needs_round_trip) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
                for (;;) {
                    const bool got = ctx.observe_ui([&]() -> json { return ui_printer(target.dev_id).get_nozzle_mapping_result()->HasResult(); }).get<bool>();
                    if (got)
                        break;
                    if (ctx.cancelled())
                        throw ToolFailure("Cancelled while waiting for the printer's nozzle mapping");
                    if (std::chrono::steady_clock::now() >= deadline)
                        throw ToolFailure("The printer did not answer the nozzle mapping request within 10 s");
                    std::this_thread::sleep_for(std::chrono::milliseconds(250));
                }
            }

            // Phase 2: the plan, the export and the job.
            auto state = std::make_shared<std::atomic<bool>>(false); // the job's LAN check failed
            auto progress = std::make_shared<JobProgress>();
            std::shared_ptr<Worker> worker;
            std::shared_ptr<PrintJob> job;
            json prepared = ctx.run_ui([&]() -> json {
                MachineObject& obj    = ui_printer(target.dev_id);
                Plater&        plater = ui_plater();
                PrintPlan      plan   = ui_build_plan(plater, plate_idx, obj, overrides, plan_args(args, false));
                if (!plan.sendable)
                    throw ToolFailure("The plan has blocking errors; print_plan shows them", {{"errors", checks_for_error(plan, "error")}, {"plan", redact(plan_to_json(plan))}});
                if (plan.needs_confirmation && !arg_bool(args, "confirm_warnings", false) && !dry_run) {
                    json confirm = json::array();
                    for (const PlanCheck& c : plan.checks)
                        if (c.details.value("needs_confirmation", false))
                            confirm.push_back({{"id", c.id}, {"message", c.message}});
                    throw ToolFailure("The Print dialog asks to confirm these warnings first; review them and pass confirm_warnings: true", {{"warnings", confirm}});
                }
                PrintSetup setup = ui_print_setup(plater, plate_idx, obj, plan);
                PartPlate* plate = plater.get_partplate_list().get_plate(plate_idx);
                boost::filesystem::path p3mf(plate->get_tmp_gcode_path());
                p3mf.replace_extension("3mf");
                setup.file_3mf = p3mf.string();
                if (!obj.is_lan_mode_printer())
                    setup.file_config_3mf = plate->get_temp_config_3mf_path();

                if (dry_run) {
                    ScopedPublishCapture capture; // nothing below sends, and a print start would be refused
                    json out = {{"dry_run", true},
                                {"plan", plan_to_json(plan)},
                                {"print_params", print_params_json(setup)},
                                {"files", {{"3mf", setup.file_3mf}, {"config_3mf", setup.file_config_3mf}, {"exported", false}}}};
                    return redact(std::move(out));
                }

                ctx.begin_operation();
                // exactly Plater::send_gcode / export_config_3mf (the plate is the current one)
                if (plater.send_gcode(plate_idx, nullptr) < 0)
                    throw ToolFailure("Abnormal print file data. Please slice again");
                if (!obj.is_lan_mode_printer() && plater.export_config_3mf(plate_idx, nullptr) < 0)
                    throw ToolFailure("The config 3mf could not be exported");

                job = std::make_shared<PrintJob>(target.dev_id);
                configure_print_job(*job, setup, [state]() { state->store(true); });
                worker = std::make_shared<PlaterWorker<BoostThreadWorker>>(&plater, progress, "mcp_print_worker");
                last_worker() = worker;
                replace_job(*worker, job);
                return json{{"plan", redact(plan_to_json(plan))}, {"print_params", redact(print_params_json(setup))}};
            });
            if (dry_run)
                return ToolResult::ok(std::move(prepared));

            // Wait for the job: the worker is idle once the job and its finalize message were handled.
            for (;;) {
                const bool idle = ctx.observe_ui([&]() -> json { return worker->is_idle(); }).get<bool>();
                if (idle)
                    break;
                if (ctx.cancelled()) {
                    ctx.observe_ui([&]() -> json { worker->cancel_all(); return json(); });
                    throw ToolFailure("Cancelled while sending the print job; the printer may or may not have received it");
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
            }
            if (state->load())
                throw ToolFailure("The printer rejected the LAN connection (access code or address); fix it with printer_list {action: \"connect\"}");
            const bool finished = job->is_finished();
            json       out      = {{"started", finished},
                                   {"message", progress->error().empty() ? progress->status() : progress->error()},
                                   {"plan", prepared["plan"]}};
            if (!finished) {
                json details = {{"message", progress->error().empty() ? progress->status() : progress->error()}, {"detail", progress->detail()}};
                throw ToolFailure("The print job was not sent: " + details["message"].get<std::string>(), details);
            }
            settle(ctx);
            return ToolResult::ok(redact(std::move(out)));
        },
        /*destructive*/ true);
    print_tool.budget_ms = 300000;
    service.registry().add(std::move(print_tool));
}

}}} // namespace Slic3r::GUI::Mcp
