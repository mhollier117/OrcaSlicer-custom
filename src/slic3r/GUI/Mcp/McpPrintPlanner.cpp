#include "McpPrintPlanner.hpp"

#include "McpTypes.hpp"
#include "slic3r/GUI/DeviceCore/DevDefs.h"
#include "slic3r/GUI/DeviceCore/DevExtruderSystem.h"
#include "slic3r/GUI/DeviceCore/DevFilaSwitch.h"
#include "slic3r/GUI/DeviceCore/DevFilaSystem.h"
#include "slic3r/GUI/DeviceCore/DevMapping.h"
#include "slic3r/GUI/DeviceCore/DevMappingNozzle.h"
#include "slic3r/GUI/DeviceCore/DevNozzleRack.h"
#include "slic3r/GUI/DeviceCore/DevNozzleSystem.h"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/GUI/DeviceCore/DevConfig.h"
#include "slic3r/GUI/DeviceCore/DevPrintOptions.h"
#include "slic3r/GUI/DeviceCore/DevExtensionTool.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GuiColor.hpp"
#include "libslic3r/Print.hpp"
#include "slic3r/GUI/DeviceCore/DevConfigUtil.h"
#include "slic3r/GUI/DeviceCore/DevUtilBackend.h"
#include "slic3r/GUI/BitmapCache.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Jobs/PrintJob.hpp"
#include "slic3r/GUI/PartPlate.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Model.hpp"
#include <boost/filesystem/path.hpp>
#include <boost/lexical_cast.hpp>

#include <boost/algorithm/string/case_conv.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

constexpr int kFilamentMapLeft  = 1; // FilamentMapNozzleId::NOZZLE_LEFT
constexpr int kFilamentMapRight = 2;
constexpr int kCloudNozzleRight = 0; // CloudTaskNozzleId
constexpr int kCloudNozzleLeft  = 1;

bool is_virtual_id(const std::string& ams_id) { return ams_id == VIRTUAL_AMS_MAIN_ID_STR || ams_id == VIRTUAL_AMS_DEPUTY_ID_STR; }

std::string join_numbers(const std::vector<int>& values)
{
    std::string out;
    for (int v : values)
        out += (out.empty() ? "" : ", ") + std::to_string(v);
    return out;
}

std::string fmt(const char* format, double a)
{
    char buf[512];
    std::snprintf(buf, sizeof(buf), format, a);
    return buf;
}

const PlanFilament* find_filament(const PlanProject& project, int id)
{
    for (const PlanFilament& f : project.filaments)
        if (f.id == id)
            return &f;
    return nullptr;
}

FilamentInfo* find_result(MappingRun& run, int id)
{
    for (FilamentInfo& f : run.result)
        if (f.id == id)
            return &f;
    return nullptr;
}

const FilamentInfo* find_result(const MappingRun& run, int id)
{
    for (const FilamentInfo& f : run.result)
        if (f.id == id)
            return &f;
    return nullptr;
}

// mirrors SelectMachine.cpp:3169 _HasExt / :3183 _HasAms
bool has_ext(const std::vector<FilamentInfo>& result)
{
    if (result.empty())
        return true;
    for (const FilamentInfo& info : result)
        if (info.ams_id == VIRTUAL_AMS_MAIN_ID_STR || (info.ams_id == VIRTUAL_AMS_DEPUTY_ID_STR && !info.ams_id.empty()))
            return true;
    return false;
}

bool has_ams(const std::vector<FilamentInfo>& result)
{
    for (const FilamentInfo& info : result)
        if (info.ams_id != VIRTUAL_AMS_MAIN_ID_STR && info.ams_id != VIRTUAL_AMS_DEPUTY_ID_STR && !info.ams_id.empty())
            return true;
    return false;
}

std::string hex_color(const wxColour& c)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", c.Red(), c.Green(), c.Blue(), c.Alpha());
    return buf;
}

} // namespace

// ---- filaments -------------------------------------------------------------------------------

// mirrors SelectMachine.cpp:5562 reset_and_sync_ams_list
std::vector<PlanFilament> filaments_from(const std::vector<PlanSlot>& slots, const std::vector<int>& used_ids,
                                         const std::vector<int>& filament_maps, int nozzle_count)
{
    std::vector<PlanFilament> out;
    for (int used : used_ids) {
        const int extruder = used - 1;
        if (extruder < 0 || extruder >= int(slots.size()))
            continue;
        const PlanSlot& slot = slots[size_t(extruder)];
        PlanFilament    f;
        f.id           = extruder;
        f.type         = slot.type;
        f.filament_id  = slot.filament_id;
        f.color        = slot.color;
        f.brand        = slot.vendor;
        f.preset_name  = slot.preset_name;
        f.filament_map = (nozzle_count > 1 && extruder < int(filament_maps.size())) ? filament_maps[size_t(extruder)] : 0;
        f.nozzle       = f.filament_map == kFilamentMapLeft ? kCloudNozzleLeft : kCloudNozzleRight;
        out.push_back(std::move(f));
    }
    return out;
}

// ---- mapping ---------------------------------------------------------------------------------

// mirrors SelectMachine.cpp:1217 do_ams_mapping
MappingRun run_auto_mapping(MachineObject& obj, const std::vector<PlanFilament>& filaments, int nozzle_nums,
                            const std::vector<int>& filament_maps, bool use_ams)
{
    MappingRun run;
    if (!obj.HasAms())
        use_ams = false;

    std::vector<FilamentInfo> infos;
    for (const PlanFilament& f : filaments) {
        FilamentInfo info;
        info.id          = f.id;
        info.type        = f.type;
        info.brand       = f.brand;
        info.filament_id = f.filament_id;
        info.color       = f.color;
        infos.push_back(std::move(info));
    }

    int filament_result = 0;
    if (nozzle_nums > 1 && obj.GetExtderSystem()->GetTotalExtderCount() > 1) {
        run.dual = true;
        std::vector<FilamentInfo> left, right, result_left, result_right;
        for (const FilamentInfo& it : infos) {
            if (it.id < 0 || it.id > int(filament_maps.size())) {
                run.error = "filament " + std::to_string(it.id + 1) + " has no entry in the plate's filament map";
                return run;
            }
            if (it.id < int(filament_maps.size())) {
                if (filament_maps[size_t(it.id)] == kFilamentMapLeft)
                    left.push_back(it);
                else if (filament_maps[size_t(it.id)] == kFilamentMapRight)
                    right.push_back(it);
            }
        }

        // Which sides have an AMS, by the extruder each AMS is installed on (GetExtruderId, as the dialog does).
        bool has_left_ams = false, has_right_ams = false;
        for (const auto& ams_item : obj.GetFilaSystem()->GetAmsList()) {
            if (ams_item.second->GetExtruderId() == 0)
                has_right_ams = true; // is_main_extruder_on_left() is false
            else if (ams_item.second->GetExtruderId() == 1)
                has_left_ams = true;
            if (has_left_ams && has_right_ams)
                break;
        }

        // four values: use_left_ams, use_right_ams, use_left_ext, use_right_ext
        std::vector<bool> map_opt = {true, false, !has_left_ams, false};
        if (!use_ams) {
            map_opt[0] = false;
            map_opt[2] = true;
        }
        const int first = DevMappingUtil::ams_filament_mapping(&obj, left, result_left, map_opt);
        map_opt         = {false, true, false, !has_right_ams};
        if (!use_ams) {
            map_opt[1] = false;
            map_opt[3] = true;
        }
        const int second = DevMappingUtil::ams_filament_mapping(&obj, right, result_right, map_opt);

        run.result = result_left;
        run.result.insert(run.result.end(), result_right.begin(), result_right.end());
        std::stable_sort(run.result.begin(), run.result.end(), [](const FilamentInfo& a, const FilamentInfo& b) { return a.id < b.id; });
        filament_result = (first && second);
    } else {
        std::vector<bool> map_opt = {false, true, false, false};
        if (!use_ams) {
            map_opt[1] = false;
            map_opt[3] = true;
        }
        filament_result = DevMappingUtil::ams_filament_mapping(&obj, infos, run.result, map_opt);
    }

    run.filament_result = filament_result;
    if (filament_result == 0) {
        run.valid = DevMappingUtil::is_valid_mapping_result(&obj, run.result);
    } else {
        run.valid = DevMappingUtil::is_valid_mapping_result(&obj, run.result);
        if (filament_result != 1 && !run.valid) {
            for (FilamentInfo& r : run.result) {
                r.tray_id  = -1;
                r.distance = 99999;
            }
        }
    }
    return run;
}

// mirrors DevMapping.cpp:95-153 (tray_index)
int tray_index_of(MachineObject& obj, const std::string& ams_id, const std::string& slot_id)
{
    if (is_virtual_id(ams_id)) {
        for (const DevAmsTray& t : obj.vt_slot)
            if (t.id == ams_id)
                return std::atoi(ams_id.c_str());
        return -1;
    }
    DevAms* ams = obj.GetFilaSystem()->GetAmsById(ams_id);
    if (ams == nullptr || ams->GetTrays().find(slot_id) == ams->GetTrays().end())
        return -1;
    auto type = ams->GetAmsType();
    if (type == DevAms::AMS_LITE && ams->IsAmsLiteMixed())
        type = DevAms::AMS_LITE_MIXED;
    const int a = std::atoi(ams_id.c_str()), s = std::atoi(slot_id.c_str());
    if (type == DevAms::AMS || type == DevAms::AMS_LITE || type == DevAms::N3F)
        return a * 4 + s;
    if (type == DevAms::AMS_LITE_MIXED)
        return AMS_LITE_MIXED_TRAY_INDEX_OFFSET + s;
    if (type == DevAms::N3S)
        return a + s;
    return -1;
}

namespace {

enum class Side { Left, Right, Both, Dynamic };

// mirrors SelectMachine.cpp:1526 get_filament_mapping_show_type
Side side_of(MachineObject& obj, const PlanProject& project, const PlanFilament& f)
{
    if (project.nozzle_count < 2)
        return Side::Right;
    if (obj.GetFilaSwitch()->IsInstalled())
        return Side::Dynamic;
    if (f.filament_map == kFilamentMapLeft)
        return Side::Left;
    if (f.filament_map == kFilamentMapRight)
        return Side::Right;
    return Side::Both;
}

const char* side_name(Side s)
{
    switch (s) {
    case Side::Left: return "left";
    case Side::Right: return "right";
    case Side::Dynamic: return "either (filament switch)";
    default: return "either";
    }
}

} // namespace

// mirrors AmsMappingPopupUpdate.cpp:add_ams_mapping / add_ext_ams_mapping and AmsMappingPopup.cpp:1197 on_left_down,
// then SelectMachine.cpp:3879 on_set_finish_mapping
void apply_overrides(MachineObject& obj, const PlanProject& project, MappingRun& run, const json& overrides)
{
    if (!overrides.is_array())
        return;
    for (const json& ov : overrides) {
        if (!ov.is_object() || !ov.contains("filament") || !ov["filament"].is_number_integer())
            throw ToolFailure("each override needs a 1-based filament slot");
        const int          slot = ov["filament"].get<int>();
        const std::string  what = "override of filament " + std::to_string(slot);
        const PlanFilament* f   = find_filament(project, slot - 1);
        if (f == nullptr)
            throw ToolFailure(what + ": filament " + std::to_string(slot) + " is not used by this plate",
                              {{"filament", slot}});
        FilamentInfo* target = find_result(run, f->id);
        if (target == nullptr)
            throw ToolFailure(what + ": the filament has no mapping entry", {{"filament", slot}});
        const Side side = side_of(obj, project, *f);

        std::string ams_id, slot_id;
        if (ov.contains("external")) {
            const std::string ext = ov["external"].get<std::string>();
            if (ext == "left")
                ams_id = VIRTUAL_AMS_DEPUTY_ID_STR;
            else if (ext == "right" || ext == "main")
                ams_id = VIRTUAL_AMS_MAIN_ID_STR;
            else
                throw ToolFailure(what + ": external must be left, right or main");
            slot_id = "0";
        } else {
            if (!ov.contains("ams_id") || !ov.contains("slot_id"))
                throw ToolFailure(what + ": give ams_id and slot_id, or external");
            ams_id  = ov["ams_id"].is_string() ? ov["ams_id"].get<std::string>() : std::to_string(ov["ams_id"].get<int>());
            slot_id = ov["slot_id"].is_string() ? ov["slot_id"].get<std::string>() : std::to_string(ov["slot_id"].get<int>());
        }

        const DevAmsTray* tray = nullptr;
        if (is_virtual_id(ams_id)) {
            const bool left = ams_id == VIRTUAL_AMS_DEPUTY_ID_STR;
            if (side == Side::Dynamic)
                throw ToolFailure(what + ": external spools are not supported since a Filament Track Switch is installed",
                                  {{"filament", slot}});
            if ((left && side == Side::Right) || (!left && side == Side::Left))
                throw ToolFailure(what + ": filament " + std::to_string(slot) + " prints on the " + side_name(side) +
                                      " nozzle, so it cannot use the " + (left ? "left" : "right") + " external spool",
                                  {{"filament", slot}});
            for (const DevAmsTray& t : obj.vt_slot)
                if (t.id == ams_id)
                    tray = &t;
            if (tray == nullptr)
                throw ToolFailure(what + ": the printer has no " + std::string(left ? "left" : "right") + " external spool",
                                  {{"filament", slot}});
            // ext spools are exempt from the type check (SelectMachine.cpp:220 EnableExtMappingFilaTypeCheck(false))
        } else {
            DevAms* ams = obj.GetFilaSystem()->GetAmsById(ams_id);
            if (ams == nullptr)
                throw ToolFailure(what + ": the printer has no AMS " + ams_id, {{"filament", slot}});
            auto it = ams->GetTrays().find(slot_id);
            if (it == ams->GetTrays().end())
                throw ToolFailure(what + ": AMS " + ams_id + " has no slot " + slot_id, {{"filament", slot}});
            const std::set<int>& bound = ams->GetBindedExtruderSet();
            if (bound.empty())
                throw ToolFailure(what + ": AMS " + ams_id + " is not bound to an extruder right now", {{"filament", slot}});
            if (side != Side::Dynamic) {
                const bool on_left = *bound.begin() == DEPUTY_EXTRUDER_ID;
                if ((on_left && side == Side::Right) || (!on_left && side == Side::Left))
                    throw ToolFailure(what + ": filament " + std::to_string(slot) + " prints on the " + side_name(side) +
                                          " nozzle, but AMS " + ams_id + " feeds the " + (on_left ? "left" : "right") + " nozzle",
                                      {{"filament", slot}});
            }
            tray = it->second;
            if (!tray->is_exists)
                throw ToolFailure(what + ": AMS " + ams_id + " slot " + slot_id + " is empty", {{"filament", slot}});
            if (tray->is_tray_info_ready()) {
                DevAmsTray copy = *tray;
                if (copy.get_filament_type() != f->type)
                    throw ToolFailure(what + ": AMS " + ams_id + " slot " + slot_id + " holds " + copy.get_filament_type() +
                                          " but filament " + std::to_string(slot) + " is " + f->type,
                                      {{"filament", slot}});
            } // a tray without readable info (third party) is pickable whatever its type
        }

        // The popup's tray id (AmsMappingPopupUpdate.cpp:211-231 sGetTrayData): GetAmsType() reports AMS-Lite-mixed as
        // AMS-Lite, so such a tray is ams*4+slot there.
        if (is_virtual_id(ams_id)) {
            target->tray_id = std::atoi(ams_id.c_str());
        } else {
            DevAms*    ams  = obj.GetFilaSystem()->GetAmsById(ams_id);
            const auto type = ams->GetAmsType();
            const int  a = std::atoi(ams_id.c_str()), sl = std::atoi(slot_id.c_str());
            target->tray_id = type == DevAms::N3S ? a + sl : a * 4 + sl;
        }
        // An unreadable tray (third party, no info) shows as grey in the popup, and that colour is what the dialog stores
        // (AmsMappingPopupUpdate.cpp:604-608, SelectMachine.cpp:3914).
        const bool readable = tray->is_tray_info_ready();
        target->color       = readable ? hex_color(DevAmsTray::decode_color(tray->color)) : std::string("#CECECEFF");
        target->ctype       = readable ? tray->ctype : 0;
        target->colors.clear();
        if (readable)
            for (const std::string& c : tray->cols)
                target->colors.push_back(hex_color(DevAmsTray::decode_color(c)));
        target->ams_id  = ams_id;
        target->slot_id = slot_id;
        // the dialog leaves FilamentInfo::type and filament_id as they were (on_set_finish_mapping)
        target->distance = GUI::calc_color_distance(wxColour(f->color), DevAmsTray::decode_color(tray->color));
        run.overridden.insert(f->id);
    }
    std::vector<FilamentInfo> copy = run.result;
    run.valid                      = DevMappingUtil::is_valid_mapping_result(&obj, copy);
}

// ---- encodings -------------------------------------------------------------------------------

json build_nozzles_info(const PlanProject& project)
{
    json out = json::array();
    // mirrors SelectMachine.cpp:1472 build_nozzles_info (config index 0 = left, 1 = right) and :160 the flow strings
    if (!project.nozzle_volume_option || project.nozzle_diameters.size() != 2)
        return out;
    for (size_t i = 0; i < 2; ++i) {
        json item;
        item["id"]   = i == 0 ? kCloudNozzleLeft : kCloudNozzleRight;
        item["type"] = nullptr;
        if (i < project.nozzle_volume_types.size()) {
            const int   v    = project.nozzle_volume_types[i];
            const char* name = v == int(nvtStandard) ? "standard_flow" : v == int(nvtHighFlow) ? "high_flow" :
                               v == int(nvtTPUHighFlow) ? "tpu_high_flow" : v == int(nvtE3DHighFlow) ? "e3d_high_flow" :
                               v == int(nvtHybrid) ? "hybrid_flow" : "";
            item["flowSize"] = name;
        }
        item["diameter"] = project.nozzle_diameters[i];
        out.push_back(item);
    }
    return out;
}

// mirrors SelectMachine.cpp:1358 get_ams_mapping_result (FROM_NORMAL), nozzleId included: the plate's own filament
// map (PartPlate::get_filament_maps) decides, so nothing is written when the plate follows the project. Lookups are
// by filament id, not by position in a parallel vector.
Encodings encode_mapping(MachineObject& obj, const PlanProject& project, const MappingRun& run)
{
    Encodings enc;
    enc.nozzles_info = build_nozzles_info(project);
    // on_send_print's task_use_ams (SelectMachine.cpp:3775)
    if (obj.HasAms()) {
        const bool e = has_ext(run.result), a = has_ams(run.result);
        enc.task_use_ams = (a && !e) ? true : (e && !a) ? false : true;
    }
    if (run.result.empty())
        return enc;
    int invalid = 0;
    for (const FilamentInfo& r : run.result)
        if (r.tray_id == -1)
            ++invalid;
    enc.all_mapped = invalid == 0;
    if (invalid == int(run.result.size()))
        return enc;
    enc.available = true;

    for (size_t i = 0; i < project.slots; ++i) {
        int  tray_id = -1;
        json v1      = {{"ams_id", 0xff}, {"slot_id", 0xff}};
        json info    = {{"ams", tray_id}, {"targetColor", ""}, {"filamentId", ""}, {"filamentType", ""}};
        if (const FilamentInfo* r = find_result(run, int(i))) {
            const PlanFilament* f = find_filament(project, int(i));
            tray_id               = r->tray_id;
            info["ams"]           = tray_id;
            info["filamentType"]  = f != nullptr ? f->type : r->type;
            info["filamentId"]    = i < project.slot_filament_ids.size() ? project.slot_filament_ids[i] : std::string();
            if (i < project.plate_filament_maps.size()) { // convert_filament_map_nozzle_id_to_task_nozzle_id
                const int m = project.plate_filament_maps[i];
                info["nozzleId"] = m == kFilamentMapLeft ? kCloudNozzleLeft : m == kFilamentMapRight ? kCloudNozzleRight : m;
            }
            info["sourceColor"] = f != nullptr ? f->color : std::string();
            info["targetColor"] = r->color;
            if (tray_id == VIRTUAL_TRAY_MAIN_ID || tray_id == VIRTUAL_TRAY_DEPUTY_ID)
                tray_id = -1;
            try {
                if (r->ams_id.empty() || r->slot_id.empty()) {
                    v1["ams_id"]  = VIRTUAL_TRAY_MAIN_ID;
                    v1["slot_id"] = VIRTUAL_TRAY_MAIN_ID;
                } else {
                    v1["ams_id"]  = std::stoi(r->ams_id);
                    v1["slot_id"] = std::stoi(r->slot_id);
                }
            } catch (...) {
            }
        }
        enc.ams_mapping.push_back(tray_id);
        enc.ams_mapping2.push_back(v1);
        enc.ams_mapping_info.push_back(info);
    }

    return enc;
}

// ---- checks ----------------------------------------------------------------------------------

namespace {

// mirrors SelectMachine.cpp:1589 get_mapped_nozzles
std::vector<DevNozzle> mapped_nozzles(MachineObject& obj, const PlanProject& project, int fila_id)
{
    std::vector<DevNozzle> out;
    if (project.nozzle_count != obj.GetExtderSystem()->GetTotalExtderCount())
        return out;
    DevNozzleSystem* sys = obj.GetNozzleSystem();
    if (sys == nullptr)
        return out;
    if (!sys->GetNozzleRack()->IsSupported()) {
        if (project.nozzle_count == 1)
            out.push_back(sys->GetNozzleByPosId(MAIN_EXTRUDER_ID));
        else if (project.nozzle_count == 2 && fila_id >= 0 && size_t(fila_id) < project.filament_maps.size()) {
            if (project.filament_maps[size_t(fila_id)] == kFilamentMapLeft)
                out.push_back(sys->GetNozzleByPosId(DEPUTY_EXTRUDER_ID));
            else if (project.filament_maps[size_t(fila_id)] == kFilamentMapRight)
                out.push_back(sys->GetNozzleByPosId(MAIN_EXTRUDER_ID));
        }
    } else {
        for (int pos : obj.get_nozzle_mapping_result()->GetMappedNozzlePosVecByFilaId(fila_id))
            out.push_back(sys->GetNozzleByPosId(pos));
    }
    out.erase(std::remove_if(out.begin(), out.end(), [](const DevNozzle& n) { return n.IsEmpty(); }), out.end());
    return out;
}

bool rack_managed(MachineObject& obj, int nozzle_idx)
{
    if (nozzle_idx != MAIN_EXTRUDER_ID)
        return false;
    DevNozzleSystem* sys = obj.GetNozzleSystem();
    return sys != nullptr && sys->GetNozzleRack() && sys->GetNozzleRack()->IsSupported();
}

std::string nozzle_name(int total, int ext_id)
{
    if (total == 2)
        return ext_id == MAIN_EXTRUDER_ID ? "right nozzle" : "left nozzle";
    return "nozzle";
}

std::string steel_name(NozzleType type)
{
    return DevNozzle::GetNozzleTypeStr(type).utf8_string();
}

std::string ext_loc_str(const std::set<int>& extruders, int total)
{
    if (total == 1)
        return "extruder";
    if (total == 2) {
        if (extruders.size() == 2)
            return "both extruders";
        if (extruders.size() == 1)
            return *extruders.begin() == MAIN_EXTRUDER_ID ? "right extruder" : "left extruder";
    }
    return {};
}

} // namespace

// mirrors SelectMachine.cpp:4763 update_show_status (blocking errors and warnings, in its order) and :2751 on_ok_btn
// (the confirmation warnings, tagged needs_confirmation). Every check runs, where the dialog stops at the first error.
CheckOutcome run_checks(MachineObject& obj, const PlanProject& project, const PrinterFacts& facts, const MappingRun& run,
                        const PlanOptions& options)
{
    CheckOutcome out;
    auto add = [&](const std::string& id, const char* severity, const std::string& message, json details = json::object()) {
        for (const PlanCheck& c : out.checks)
            if (c.id == id)
                return;
        out.checks.push_back({id, severity, message, std::move(details)});
    };
    auto skip = [&](const std::string& id, const std::string& why) { out.skipped.push_back({{"id", id}, {"skipped", true}, {"why", why}}); };
    auto confirm = [&](const std::string& id, const std::string& message, json details = json::object()) {
        details["needs_confirmation"] = true;
        add(id, "warning", message, std::move(details));
    };

    skip("print_all_plates_unsupported", "a plan is always for a single plate");

    // ---- printer-level errors (update_show_status ~4770-4900)
    if (!facts.agent_present) {
        add("no_network_agent", "error", "No login account, only printers in LAN mode are displayed.");
        return out;
    }
    if (!project.gcode_valid) {
        add("blank_plate", "error", "Cannot send a print job for an empty plate.");
        return out;
    }
    if (!obj.is_lan_mode_printer() && !facts.server_connected) {
        add("cloud_server_not_connected", "error", "Connecting to server...");
        return out;
    }
    if (!obj.is_info_ready()) {
        add("printer_info_not_ready", "error", "Synchronizing device information...");
        return out;
    }
    if (obj.upgrade_force_upgrade)
        add("needs_force_upgrade", "error", "Cannot send the print job to a printer whose firmware must be updated.");
    if (obj.upgrade_consistency_request)
        add("needs_consistency_upgrade", "error", "Cannot send the print job to a printer whose firmware must be updated.");
    if (!obj.is_fdm_type())
        add("printer_not_fdm_mode", "error", "Cannot send a print job when the printer is not at FDM mode.");
    if (facts.model_optional)
        add("printer_model_unknown", "warning",
            "The selected printer (" + facts.printer_display_name + ") has an unknown model, so compatibility with the print file configuration (" +
                facts.source_display_name + ") cannot be verified. Please verify the printer preset before sending.");
    if (facts.model_blocked)
        add("printer_model_incompatible", "error",
            "The selected printer (" + facts.printer_display_name + ") is incompatible with the print file configuration (" + facts.source_display_name +
                "). Please adjust the printer preset in the prepare page or choose a compatible printer on this page.");
    if (obj.is_in_upgrading())
        add("printer_updating_firmware", "error", "Cannot send a print job while the printer is updating firmware.");
    if (obj.is_system_printing())
        add("printer_system_printing", "error", "The printer is executing instructions. Please restart printing after it ends.");
    if (obj.is_in_printing() || obj.ams_status_main == AMS_STATUS_MAIN_FILAMENT_CHANGE)
        add("printer_busy", "error", "The printer is busy with another print job.");
    const auto sd_state = obj.GetStorage()->get_sdcard_state();
    if (!obj.GetConfig()->SupportPrintWithoutSD() && sd_state == DevStorage::SdcardState::NO_SDCARD)
        add("no_storage", "error", "Insert storage or free up space before printing.");
    int max_color = std::max(obj.get_max_filament_color_count(), 16);
    if (project.slots > size_t(max_color) && !obj.is_enable_ams_np && !obj.is_enable_np)
        add("too_many_filaments", "error",
            "The current firmware supports a maximum of " + std::to_string(max_color) + " materials. You can either reduce the number of materials to " +
                std::to_string(max_color) + " or fewer on the Preparation Page, or try updating the firmware. If you are still restricted after the update, please wait for subsequent firmware support.");
    if (obj.is_lan_mode_printer()) {
        if (sd_state == DevStorage::SdcardState::NO_SDCARD && !obj.can_use_emmc_print())
            add("lan_storage_missing", "error", "Storage needs to be inserted before printing via LAN.");
        else if (sd_state == DevStorage::SdcardState::HAS_SDCARD_READONLY ||
                 (sd_state == DevStorage::SdcardState::HAS_SDCARD_ABNORMAL && !facts.allow_abnormal_storage))
            add("lan_storage_unavailable", "error", "Storage is in abnormal state or is in read-only mode.");
    }

    // TPU / flow calibration (SelectMachine.cpp:4936)
    const bool flow_cali_shown = facts.flow_cali_supported;
    bool       unsupported_auto_cali = false;
    for (const PlanFilament& f : project.filaments) {
        const std::string pid = facts.to_printer_filament_id ? facts.to_printer_filament_id(f.filament_id) : f.filament_id;
        if (std::find(facts.unsupported_auto_cali_filaments.begin(), facts.unsupported_auto_cali_filaments.end(), pid) !=
            facts.unsupported_auto_cali_filaments.end())
            unsupported_auto_cali = true;
    }
    if (unsupported_auto_cali && flow_cali_shown) {
        if (options.flow_cali == "auto")
            add("tpu_unsupported_auto_flow_cali", "error", "TPU 90A/TPU 85A is too soft and does not support automatic Flow Dynamics calibration.");
        else if (options.flow_cali == "on")
            add("tpu_flow_cali_on_advisory", "warning",
                "TPU 90A/TPU 85A are too soft. It is recommended to perform manual flow calibration on the 'Calibration' page. If 'Dynamic Flow Calibration' is set to auto/on, the system will use the previous calibration value and skip the flow calibration process.");
    }

    // ---- mapping validity
    std::vector<FilamentInfo> copy = run.result;
    const bool                valid = DevMappingUtil::is_valid_mapping_result(&obj, copy);
    std::vector<int>          unmapped;
    for (const FilamentInfo& r : run.result)
        if (r.ams_id.empty() && r.slot_id.empty())
            unmapped.push_back(r.id + 1);
    const std::string unmapped_msg = "Not all filaments used in slicing are mapped to the printer. Please check the mapping of filaments.";
    if (obj.GetExtderSystem()->GetTotalExtderCount() > 1 && !unmapped.empty())
        add("ams_mapping_invalid", "error", unmapped_msg, {{"unmapped_filaments", unmapped}});

    // ---- filament switch (CheckErrorDynamicSwitchNozzle ~1715)
    if (obj.is_support_check_track_switch_match_slice_printer && project.slicing_with_fila_switch != obj.GetFilaSwitch()->IsInstalled())
        add("filament_switch_slicing_mismatch", "warning",
            "The Filament Track Switch installed on the printer does not match the slicing file. Please re-slice to avoid print quality issues.");
    if (project.dynamic_nozzle_map) {
        if (!obj.GetFilaSwitch()->IsInstalled())
            add("filament_switch_missing", "error", "This print requires a Filament Track Switch. Please install it first.");
        else if (!obj.GetFilaSwitch()->IsReady())
            add("filament_switch_not_ready", "error", "The Filament Track Switch has not been setup. Please setup it first.");
    }

    // ---- hotend rack reading (CheckErrorRackStatus ~4517)
    DevNozzleSystem*     nozzle_sys = obj.GetNozzleSystem();
    const bool           rack       = nozzle_sys != nullptr && nozzle_sys->GetNozzleRack()->IsSupported();
    if (rack && nozzle_sys->GetReadingCount() > 0)
        add("rack_reading", "error",
            "Refreshing information of hotends(" + std::to_string(nozzle_sys->GetReadingIdx()) + "/" + std::to_string(nozzle_sys->GetReadingCount()) +
                "). Please wait a moment...");

    // ---- slicing nozzles against the printer's (CheckErrorExtruderNozzleWithSlicing ~4613)
    if (nozzle_sys != nullptr) {
        const int total = obj.GetExtderSystem()->GetTotalExtderCount();
        for (const PlanSlicingNozzle& want : project.slicing_nozzles) {
            const NozzleFlowType want_flow = NozzleFlowType(want.flow);
            if (want.extruder == MAIN_EXTRUDER_ID && rack) {
                if (nozzle_sys->CollectNozzles(MAIN_EXTRUDER_ID, want_flow, float(want.diameter)).empty()) {
                    std::string s = DevNozzle::GetNozzleFlowTypeStr(want_flow).utf8_string();
                    if (want.diameter > 0.0)
                        s += fmt(" %.1fmm", want.diameter);
                    std::string msg = "The printer has no nozzle matching the slicing file (" + s + "). ";
                    if (nozzle_sys->GetNozzleRack()->GetCaliStatus() != DevNozzleRack::Rack_CALI_OK)
                        msg += "Please complete the hotend rack setup and try again.";
                    else if (nozzle_sys->HasUnknownNozzles() || nozzle_sys->HasUnreliableNozzles())
                        msg += "Please refresh the nozzle information and try again.";
                    else
                        msg += "Please install a matching nozzle in the hotend rack, or set the corresponding printer preset while slicing.";
                    add("nozzle_no_matching_hotend", "error", msg);
                } else if (nozzle_sys->IsRackMaximumInstalled()) {
                    add("nozzle_rack_full", "error", "The toolhead and hotend rack are full. Please remove at least one hotend before printing.");
                }
                continue;
            }
            const DevNozzle installed = nozzle_sys->GetExtNozzle(want.extruder);
            if (obj.is_nozzle_flow_type_supported() && installed.GetNozzleFlowType() == NozzleFlowType::NONE_FLOWTYPE) {
                add("nozzle_data_invalid", "error", "Invalid nozzle information, please refresh or manually set nozzle information.");
                continue;
            }
            if (obj.is_nozzle_flow_type_supported() && want_flow != installed.GetNozzleFlowType()) {
                add("nozzle_flow_mismatch", "error",
                    "The nozzle flow setting of " + nozzle_name(total, want.extruder) + "(" + installed.GetNozzleFlowTypeStr().utf8_string() +
                        ") doesn't match with the slicing file(" + DevNozzle::GetNozzleFlowTypeStr(want_flow).utf8_string() +
                        "). Please make sure the nozzle installed matches with settings in printer, then set the corresponding printer preset while slicing.");
                continue;
            }
            if (installed.GetNozzleDiameter() > 0.0f && float(want.diameter) != installed.GetNozzleDiameter()) {
                std::string msg;
                char        buf[512];
                if (total == 2)
                    std::snprintf(buf, sizeof(buf),
                                  "The %s diameter(%.1fmm) of current printer doesn't match with the slicing file (%.1fmm). Please make sure the nozzle installed matches with settings in printer, then set the corresponding printer preset when slicing.",
                                  nozzle_name(total, want.extruder).c_str(), installed.GetNozzleDiameter(), want.diameter);
                else
                    std::snprintf(buf, sizeof(buf),
                                  "The current nozzle diameter (%.1fmm) doesn't match with the slicing file (%.1fmm). Please make sure the nozzle installed matches with settings in printer, then set the corresponding printer preset when slicing.",
                                  installed.GetNozzleDiameter(), want.diameter);
                msg = buf;
                // Orca: overridable; the dialog gates Send on an acknowledgement checkbox (options.acknowledge_nozzle_diameter_mismatch).
                add("nozzle_diameter_mismatch", options.acknowledge_nozzle_diameter_mismatch ? "warning" : "error",
                    options.acknowledge_nozzle_diameter_mismatch ? msg : msg + " Pass options.acknowledge_nozzle_diameter_mismatch: true if you have checked the installed nozzle and want to print anyway.",
                    {{"acknowledgement_required", true}});
            }
        }

        // hardness gate on the mounted nozzles (SelectMachine.cpp:5000)
        for (const DevExtder& ext : obj.GetExtderSystem()->GetExtruders()) {
            if (project.used_nozzle_idxes.count(ext.GetNozzleId()) == 0 || rack_managed(obj, ext.GetNozzleId()))
                continue;
            if (ext.GetNozzleType() == NozzleType::ntUndefine)
                continue;
            const int nozzle_hrc = Print::get_hrc_by_nozzle_type(ext.GetNozzleType());
            for (const PlanFilament& f : project.filaments) {
                auto it = project.filament_hrc.find(f.id);
                if (it != project.filament_hrc.end() && std::abs(it->second) > std::abs(nozzle_hrc)) {
                    add("nozzle_hardness_mismatch", "error",
                        "The hardness of current material (" + f.type + ") exceeds the hardness of " + nozzle_name(total, ext.GetNozzleId()) + "(" +
                            steel_name(ext.GetNozzleType()) + "). Please verify the nozzle or material settings and try again.");
                    break;
                }
            }
        }
    }

    // ---- hotend rack nozzle mapping (CheckErrorSyncNozzleMappingResultV0/V1 ~1802/1869)
    // the V0 request is skipped only when a nozzle grouping exists and lists no right-extruder nozzles (SelectMachine.cpp:1883)
    if (rack && (project.dynamic_nozzle_map || !(project.nozzle_group_present && project.rack_needed_nozzles.empty()))) {
        auto mapping = obj.get_nozzle_mapping_result();
        if (!mapping->HasResult()) {
            add("rack_nozzle_mapping_pending", "error", "The printer is calculating nozzle mapping. Please wait a moment...",
                {{"deferred", true}, {"requires_round_trip", true}});
        } else if (mapping->GetResultStr() == "failed") {
            add("rack_nozzle_mapping_failed", "error",
                "Failed to receive nozzle auto-mapping table from printer { msg: " + mapping->GetMqttReason() + " }. Please refresh the printer information.");
        } else if (mapping->GetResultStr() == "fail") {
            add("rack_nozzle_mapping_failed", "error",
                "The printer failed to build the nozzle auto-mapping table { code: " + std::to_string(mapping->GetErrno()) + " }. Please refresh nozzle information.");
        } else if (mapping->GetNozzleMappingMap().empty()) {
            add("rack_nozzle_mapping_failed", "error",
                "Failed to receive nozzle auto-mapping table from printer { msg: empty table }. Please refresh the printer information.");
        } else if (mapping->GetFlushWeightBase() != -1 && mapping->GetFlushWeightCurrent() != -1 &&
                   mapping->GetFlushWeightCurrent() > mapping->GetFlushWeightBase()) {
            add("rack_nozzle_mapping_waste", "warning",
                fmt("The current nozzle mapping may produce an extra %0.2f g of waste.", mapping->GetFlushWeightCurrent() - mapping->GetFlushWeightBase()));
        }
    }

    // ---- AMS / external spool use
    if (!facts.ams_ext_mix_supported && has_ams(run.result) && has_ext(run.result) && !run.result.empty())
        add("ams_and_external_mixed", "error", "Please do not mix-use the Ext with AMS.");
    if (obj.GetFilaSystem()->IsAmsSettingUp())
        add("ams_setting_up", "error", "AMS is setting up. Please try again later.");
    if (!valid)
        add("ams_mapping_invalid", "error", unmapped_msg, {{"unmapped_filaments", unmapped}});

    if (facts.tpu_left_firmware_check) {
        bool tpu_left = false, bad = false;
        for (const FilamentInfo& fila : run.result) {
            if (!obj.contains_tray(fila.ams_id, fila.slot_id)) {
                bad = true;
                break;
            }
            if (obj.get_extruder_id_by_ams_id(fila.ams_id) == DEPUTY_EXTRUDER_ID && obj.get_tray(fila.ams_id, fila.slot_id).get_filament_type() == "TPU") {
                tpu_left = true;
                break;
            }
        }
        if (bad)
            add("ams_mapping_invalid", "error", unmapped_msg, {{"unmapped_filaments", unmapped}});
        else if (tpu_left && !obj.m_firmware_support_print_tpu_left.value_or(false))
            add("firmware_no_tpu_on_left", "error",
                "Your current firmware version cannot start this print job. Please update to the latest version and try again.");
    }

    // ---- filament blacklist (SelectMachine.cpp:5073; the warnings repeat in on_ok_btn)
    if (!facts.blacklist) {
        skip("filament_blacklist", "the filament blacklist needs the app config; not available in this context");
    } else {
        for (const FilamentInfo& r : run.result) {
            const PlanFilament* pf = find_filament(project, r.id);
            DevFilaBlacklist::CheckFilamentInfo info;
            info.dev_id      = obj.get_dev_id();
            info.model_id    = obj.printer_type;
            info.fila_id     = r.filament_id;
            info.fila_type   = boost::to_upper_copy(r.type);
            info.fila_vendor = pf != nullptr ? pf->brand : std::string();
            if (project.filament_lookup)
                if (auto name = project.filament_lookup(info.fila_id))
                    info.fila_name = name->first;
            info.ams_id              = r.get_ams_id();
            info.slot_id             = r.get_slot_id();
            info.has_filament_switch = obj.GetFilaSwitch()->IsInstalled();
            const std::vector<DevNozzle> nozzles = mapped_nozzles(obj, project, r.id);
            const size_t                 iterations = nozzles.empty() ? 1 : nozzles.size();
            for (size_t n = 0; n < iterations; ++n) {
                if (n < nozzles.size()) {
                    info.extruder_id     = nozzles[n].GetExtruderId();
                    info.nozzle_flow     = DevNozzle::GetNozzleFlowTypeString(nozzles[n].GetNozzleFlowType());
                    info.nozzle_diameter = nozzles[n].GetNozzleDiameter();
                }
                const auto result = facts.blacklist(info);
                for (const auto& item : result.get_items_by_action("prohibition"))
                    add("filament_blacklisted_" + std::to_string(r.id + 1), "error", item.info_msg.utf8_string(), {{"filament", r.id + 1}});
                for (const auto& item : result.get_items_by_action("warning")) {
                    add("filament_blacklist_warning_" + std::to_string(r.id + 1), "warning", item.info_msg.utf8_string(), {{"filament", r.id + 1}});
                    confirm("filament_blacklist_confirm_" + std::to_string(r.id + 1), item.info_msg.utf8_string(), {{"filament", r.id + 1}});
                }
                if (n < nozzles.size() && rack_managed(obj, nozzles[n].GetExtruderId()) && nozzles[n].GetNozzleType() != NozzleType::ntUndefine) {
                    auto it = project.filament_hrc.find(r.id);
                    if (it != project.filament_hrc.end() && std::abs(it->second) > std::abs(Print::get_hrc_by_nozzle_type(nozzles[n].GetNozzleType())))
                        add("rack_nozzle_hardness_warning_" + std::to_string(r.id + 1), "warning",
                            "The hardness of current material (" + info.fila_type + ") exceeds the hardness of the mapped hotend (" +
                                steel_name(nozzles[n].GetNozzleType()) + "). It may cause nozzle wear, leading to material leakage and unstable flow. Please exercise caution when using it.");
                }
            }
        }
    }

    // ---- warnings (SelectMachine.cpp:5160)
    {
        std::map<int, std::pair<bool, bool>> per_extruder; // has_ams, has_vt
        for (const FilamentInfo& item : run.result) {
            if (item.ams_id.empty())
                continue;
            const int ext = obj.get_extruder_id_by_ams_id(item.ams_id);
            if (is_virtual_id(item.ams_id))
                per_extruder[ext].second = true;
            else
                per_extruder[ext].first = true;
        }
        for (const auto& [ext, flags] : per_extruder)
            if (flags.first && flags.second)
                add("ams_and_external_on_one_extruder", "warning",
                    "You have selected both external and AMS filaments for an extruder. You will need to manually switch the external filament during printing.");
    }
    if (obj.GetExtderSystem()->GetTotalExtderCount() == 2) {
        for (const FilamentInfo& fila : run.result) {
            std::set<int> used;
            for (const DevNozzle& n : mapped_nozzles(obj, project, fila.id))
                used.insert(n.GetExtruderId());
            if (used.size() >= 2) {
                add("filament_cross_extruder", "warning",
                    "Some filaments may switch between extruders during printing. Manual K-value calibration cannot be applied throughout the entire print, which may affect print quality. Enabling Flow Dynamics Calibration is recommended.");
                break;
            }
        }
    }
    if (facts.blob_detect_supported && facts.blob_detect_value != 2 && project.stringing_prone_filament)
        add("smart_nozzle_blob_detection_recommended", "warning",
            "There is stringing-prone filament in this file. For best print quality, we recommend switching nozzle clumping detection to Auto mode.");
    if (flow_cali_shown && options.flow_cali != "off" && !facts.auto_on_cali_warning_filaments.empty()) {
        for (const FilamentInfo& fila : run.result) {
            const std::string pid = facts.to_printer_filament_id ? facts.to_printer_filament_id(fila.filament_id) : fila.filament_id;
            if (std::find(facts.auto_on_cali_warning_filaments.begin(), facts.auto_on_cali_warning_filaments.end(), pid) != facts.auto_on_cali_warning_filaments.end()) {
                add("tpu_flow_cali_not_suggested", "warning",
                    "If 'Dynamic Flow Calibration' is set to Auto/On, the system will use the manual calibration value or the default value and skip the flow calibration process. You can perform a manual flow calibration for TPU filament on the 'Calibration' page.");
                break;
            }
        }
    }
    // timelapse (SelectMachine.cpp:5229): with a valid mapping, missing storage replaces the slice's timelapse warning
    if (valid) {
        if (options.timelapse && sd_state == DevStorage::SdcardState::NO_SDCARD && !obj.m_has_timelapse_kit && !obj.is_support_internal_timelapse) {
            add("timelapse_needs_storage", "warning", "Storage needs to be inserted to record timelapse.");
        } else {
            const auto it = std::find_if(project.slice_warnings.begin(), project.slice_warnings.end(),
                                         [](const PlanSliceWarning& w) { return w.msg == NOT_GENERATE_TIMELAPSE; });
            if (it != project.slice_warnings.end()) {
                std::string text = it->error_code == "10014001" ? "When spiral vase mode is enabled, machines with I3 structure will not generate timelapse videos." :
                                   it->error_code == "10014002" ? "The current printer does not support timelapse in Traditional Mode when printing By-Object." : it->text;
                add("timelapse_not_generated", "warning", text);
            }
        }
    }
    if (options.timelapse && obj.is_support_internal_timelapse && !options.timelapse_storage.empty() && obj.is_timelapse_storage_low(options.timelapse_storage))
        add("timelapse_storage_low", "warning",
            std::string(options.timelapse_storage == "internal" ? "Internal" : "External") +
                " space less than 20MB. Timelapse may not save properly. You can turn it off or clean up files.");
    // external filament that is not the sliced type
    for (const FilamentInfo& m : run.result) {
        if (!is_virtual_id(m.ams_id))
            continue;
        const PlanFilament* f = find_filament(project, m.id);
        if (f != nullptr && m.type != f->type) {
            add("external_filament_type_mismatch", "warning",
                "The type of external filament is unknown or does not match with the filament type in the slicing file. Please make sure you have installed the correct filament in the external spool.");
            break;
        }
    }
    // k value not used
    if (flow_cali_shown && options.flow_cali == "auto") {
        std::vector<std::string> names;
        for (const FilamentInfo& info : run.result) {
            if (!obj.contains_tray(info.ams_id, std::to_string(info.tray_id)))
                continue;
            if (obj.get_tray(info.ams_id, std::to_string(info.tray_id)).cali_idx == -1)
                continue;
            names.push_back(info.tray_id == VIRTUAL_TRAY_MAIN_ID ? "Right-Ext" : info.tray_id == VIRTUAL_TRAY_DEPUTY_ID ? "Left-Ext" : std::to_string(info.tray_id));
        }
        if (!names.empty())
            add("custom_k_value_not_used", "warning", "Set dynamic flow calibration to 'OFF' to enable custom dynamic flow value.");
    }
    // high chamber temperature / softening
    {
        std::set<std::string> high_temp;
        std::set<int>         known, unknown;
        for (const FilamentInfo& item : run.result) {
            auto it = project.chamber_temp.find(item.id);
            if (it == project.chamber_temp.end())
                continue;
            const int chamber = it->second;
            if (chamber >= 40)
                high_temp.insert(item.get_display_filament_type());
            for (const DevExtder& ext : obj.GetExtderSystem()->GetExtruders()) {
                if (!ext.HasFilamentInExt())
                    continue;
                const std::string fid = obj.get_filament_id(ext.GetSlotNow().ams_id, ext.GetSlotNow().slot_id);
                std::optional<std::pair<std::string, int>> info;
                if (project.filament_lookup)
                    info = project.filament_lookup(fid);
                if (info) {
                    if (info->second - chamber <= 5)
                        known.insert(ext.GetExtId());
                } else if (43 - chamber <= 5) {
                    unknown.insert(ext.GetExtId());
                }
            }
        }
        if (!high_temp.empty()) {
            std::string joined;
            for (const std::string& s : high_temp)
                joined += (joined.empty() ? "" : ", ") + s;
            add(obj.GetConfig()->HasChamber() ? "high_chamber_temperature_close_door" : "high_chamber_temperature", "warning",
                "[ " + joined + " ] requires printing in a high-temperature environment." + (obj.GetConfig()->HasChamber() ? " Please close the door." : ""));
        }
        if (!known.empty())
            add("filament_may_soften", "warning", "The filament on " + ext_loc_str(known, obj.GetExtderSystem()->GetTotalExtderCount()) + " may soften. Please unload.");
        if (!unknown.empty())
            add("unknown_filament_may_soften", "warning",
                "The filament on " + ext_loc_str(unknown, obj.GetExtderSystem()->GetTotalExtderCount()) + " is unknown and may soften. Please set filament.");
    }
    // extension tool cooling fan
    if (facts.fan_f000_unmounted && project.filament_lookup) {
        for (const FilamentInfo& item : run.result) {
            if (auto info = project.filament_lookup(item.filament_id); info && info->second <= 50) {
                add("toolhead_cooling_fan_missing", "warning", "Install toolhead enhanced cooling fan to prevent filament softening.");
                break;
            }
        }
    }
    // rack nozzle supply
    if (rack && nozzle_sys != nullptr && !project.rack_needed_nozzles.empty()) {
        std::map<std::pair<int, int>, int> need;
        for (const PlanSlicingNozzle& n : project.rack_needed_nozzles)
            need[{int(n.diameter * 1000), n.flow}]++;
        for (const auto& [key, count] : need) {
            const auto installed = nozzle_sys->CollectNozzles(MAIN_EXTRUDER_ID, NozzleFlowType(key.second), float(key.first) / 1000.f);
            int        reliable  = 0;
            for (const DevNozzle& n : installed)
                if (n.IsInfoReliable())
                    ++reliable;
            if (count > int(installed.size())) {
                std::string msg = "There are not enough available hotends currently. ";
                if (nozzle_sys->GetNozzleRack()->GetCaliStatus() != DevNozzleRack::Rack_CALI_OK)
                    msg += "Please complete the hotend rack setup and try again.";
                else if (nozzle_sys->HasUnknownNozzles())
                    msg += "Please refresh the nozzle information and try again.";
                else
                    msg += "Please re-slice to avoid filament waste.";
                add("rack_not_enough_hotends", "warning", msg);
                break;
            }
            if (count > reliable && nozzle_sys->HasUnreliableNozzles())
                add("rack_hotend_info_unreliable", "warning", "The reported hotend information may be unreliable. Please refresh the nozzle information and try again.");
        }
    }

    // ---- confirmation warnings of on_ok_btn (SelectMachine.cpp:2751)
    {
        std::set<std::string> main_ext, deputy_ext;
        for (const FilamentInfo& m : run.result) {
            if (m.ams_id == VIRTUAL_AMS_MAIN_ID_STR)
                main_ext.insert(m.filament_id);
            else if (m.ams_id == VIRTUAL_AMS_DEPUTY_ID_STR)
                deputy_ext.insert(m.filament_id);
        }
        if (main_ext.size() > 1 || deputy_ext.size() > 1) {
            if (!options.ext_change_assist)
                confirm("external_spool_shared", "More than one filament types have been mapped to the same external spool, which may cause printing issues. The printer won't pause during printing.");
        } else {
            for (const FilamentInfo& m : run.result) {
                if (!is_virtual_id(m.ams_id))
                    continue;
                const std::string t = obj.get_filament_type(m.ams_id, m.slot_id);
                if (!t.empty() && t != m.type) {
                    confirm("external_spool_type_differs", "The filament type setting of external spool is different from the filament in the slicing file.");
                    break;
                }
            }
        }
    }
    if (facts.model_differs)
        confirm("printer_model_differs",
                "The printer type selected when generating G-code is not consistent with the currently selected printer. It is recommended that you use the same printer type for slicing.");
    for (const PlanSliceWarning& w : project.slice_warnings) {
        if (w.msg == NOT_SUPPORT_TRADITIONAL_TIMELAPSE && options.timelapse)
            confirm("slice_warning_traditional_timelapse", w.text);
        else if (w.msg == SMOOTH_TIMELAPSE_WITHOUT_PRIME_TOWER && options.timelapse)
            confirm("slice_warning_smooth_timelapse_no_prime_tower", w.text);
        else if (w.msg == NOZZLE_HRC_CHECKER)
            confirm("slice_warning_nozzle_hardness", w.text.empty() ? w.msg : w.text);
    }
    {
        // AmsMapingPopup::parse_ams_mapping (AmsMappingPopup.cpp:1113) ids every tray ams*4+slot (AMS_TOTAL_COUNT), so
        // the dialog only matches trays whose mapping index is that: not AMS-HT, AMS-Lite-mixed or external spools.
        bool unknown = false;
        for (const FilamentInfo& r : run.result) {
            for (const auto& ams : obj.GetFilaSystem()->GetAmsList())
                for (const auto& tray : ams.second->GetTrays()) {
                    const int id = std::atoi(ams.first.c_str()) * 4 + std::atoi(tray.first.c_str());
                    if (id != r.tray_id)
                        continue;
                    if (!tray.second->is_exists || !tray.second->is_tray_info_ready())
                        unknown = true;
                }
        }
        if (unknown)
            confirm("unknown_filament_in_mapping",
                    "There are some unknown filaments in the AMS mappings. Please check whether they are the required filaments. If they are okay, click \"Confirm\" to start printing.");
    }
    skip("filament_remaining_not_enough", "the dialog declares this check but does not wire it");
    return out;
}

// ---- plan ------------------------------------------------------------------------------------

PrintPlan make_plan(MachineObject& obj, PlanProject project, const PrinterFacts& facts, PlanOptions options, const json& overrides)
{
    PrintPlan plan;
    plan.plate     = project.plate;
    plan.filaments = project.filaments;
    plan.run       = run_auto_mapping(obj, project.filaments, project.nozzle_count, project.filament_maps, true);
    plan.dual_nozzle = plan.run.dual;
    if (plan.run.error.empty())
        apply_overrides(obj, project, plan.run, overrides);
    {
        // The option stays off unless more than one filament sits on one external spool (SelectMachine.cpp:4870).
        int main_ext = 0, deputy_ext = 0;
        for (const FilamentInfo& r : plan.run.result) {
            main_ext += r.ams_id == VIRTUAL_AMS_MAIN_ID_STR;
            deputy_ext += r.ams_id == VIRTUAL_AMS_DEPUTY_ID_STR;
        }
        if (main_ext <= 1 && deputy_ext <= 1)
            options.ext_change_assist = false;
        plan.options = options;
    }
    plan.enc = encode_mapping(obj, project, plan.run);

    CheckOutcome outcome = run_checks(obj, project, facts, plan.run, options);
    if (!plan.run.error.empty())
        outcome.checks.push_back({"filament_map_missing", "error", plan.run.error, json::object()});
    plan.checks         = std::move(outcome.checks);
    plan.checks_skipped = std::move(outcome.skipped);
    plan.nozzles_info   = plan.enc.nozzles_info;

    bool blocked = false;
    for (const PlanCheck& c : plan.checks) {
        if (c.severity == "error" && !(c.details.contains("deferred") && options.skip_round_trip))
            blocked = true;
        if (c.details.value("needs_confirmation", false))
            plan.needs_confirmation = true;
    }
    plan.sendable = !blocked;

    for (const PlanFilament& f : project.filaments) {
        PlanTarget t;
        t.filament_id = f.id;
        t.nozzle      = f.nozzle;
        const FilamentInfo* r = find_result(plan.run, f.id);
        if (r != nullptr && r->tray_id >= 0 && !r->ams_id.empty()) {
            t.ams_id      = r->ams_id;
            t.slot_id     = r->slot_id;
            t.tray_index  = r->tray_id;
            t.external    = is_virtual_id(r->ams_id);
            t.tray_color  = r->color;
            t.color_distance = r->distance;
            if (t.external)
                t.nozzle = r->ams_id == VIRTUAL_AMS_DEPUTY_ID_STR ? kCloudNozzleLeft : kCloudNozzleRight;
            if (DevAmsTray* tray = t.external ? nullptr : obj.GetFilaSystem()->GetAmsTray(r->ams_id, r->slot_id))
                t.tray_type = tray->get_display_filament_type();
            else if (t.external)
                for (DevAmsTray& v : obj.vt_slot)
                    if (v.id == r->ams_id)
                        t.tray_type = v.get_display_filament_type();
            t.source = plan.run.overridden.count(f.id) ? "override" : "auto";
            t.reason = t.source == "override" ? "set by an override" :
                       t.external ? "closest match; an external spool is used when the side has no AMS" : "closest colour of the same filament type";
        } else {
            t.source = "auto";
            t.reason = "no tray of type " + f.type + " on the " + (f.nozzle == kCloudNozzleLeft ? "left" : "right") + " side";
        }
        plan.mapping.push_back(std::move(t));
    }
    plan.project = std::move(project);
    return plan;
}

json plan_to_json(const PrintPlan& plan)
{
    json out;
    out["plate"]       = plan.plate;
    out["dual_nozzle"] = plan.dual_nozzle;
    out["sendable"]    = plan.sendable;
    out["needs_confirmation"] = plan.needs_confirmation;
    json fil = json::array();
    for (const PlanFilament& f : plan.filaments)
        fil.push_back({{"filament", f.id + 1}, {"type", f.type}, {"color", f.color}, {"filament_id", f.filament_id}, {"preset", f.preset_name},
                       {"nozzle", plan.dual_nozzle || f.filament_map != 0 ? (f.nozzle == kCloudNozzleLeft ? "left" : "right") : "single"}});
    out["filaments"] = fil;
    json map = json::array();
    for (const PlanTarget& t : plan.mapping) {
        json m = {{"filament", t.filament_id + 1}, {"mapped", t.tray_index >= 0}, {"source", t.source}, {"reason", t.reason}};
        if (t.tray_index >= 0) {
            m["ams_id"]         = t.ams_id;
            m["slot_id"]        = t.slot_id;
            m["tray_index"]     = t.tray_index;
            m["external"]       = t.external;
            m["tray_type"]      = t.tray_type;
            m["tray_color"]     = t.tray_color;
            m["color_distance"] = t.color_distance;
            m["nozzle"]         = t.nozzle == kCloudNozzleLeft ? "left" : "right";
        }
        map.push_back(std::move(m));
    }
    out["mapping"] = map;
    out["encodings"] = {{"available", plan.enc.available},
                        {"ams_mapping", plan.enc.ams_mapping},
                        {"ams_mapping2", plan.enc.ams_mapping2},
                        {"ams_mapping_info", plan.enc.ams_mapping_info},
                        {"nozzles_info", plan.enc.nozzles_info},
                        {"task_use_ams", plan.enc.task_use_ams}};
    out["nozzle_mapping"] = plan.nozzle_mapping;
    json errors = json::array(), warnings = json::array();
    for (const PlanCheck& c : plan.checks) {
        json item = {{"id", c.id}, {"message", c.message}};
        if (!c.details.empty())
            item["details"] = c.details;
        (c.severity == "error" ? errors : warnings).push_back(std::move(item));
    }
    out["errors"]         = errors;
    out["warnings"]       = warnings;
    out["checks_skipped"] = plan.checks_skipped;
    out["options"] = {{"bed_leveling", plan.options.bed_leveling},
                      {"flow_cali", plan.options.flow_cali},
                      {"nozzle_offset_cali", plan.options.nozzle_offset_cali},
                      {"timelapse", plan.options.timelapse},
                      {"ext_change_assist", plan.options.ext_change_assist}};
    return out;
}


// ---- gathering from the app ------------------------------------------------------------------

namespace {

std::string color_rrggbbaa(const std::string& colour)
{
    unsigned char rgba[4];
    BitmapCache::parse_color4(colour, rgba);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", rgba[0], rgba[1], rgba[2], rgba[3]);
    return buf;
}

// off / on / auto choice: the request, else the printer's stored value, else the dialog's default
// (SelectMachine.cpp:3205 load_option_vals). `allow_auto` is whether the printer's menu offers auto.
std::string choose_option(const json& options, const char* key, const std::string& stored, bool allow_auto)
{
    auto allowed = [&](const std::string& v) { return v == "on" || v == "off" || (allow_auto && v == "auto"); };
    if (options.is_object() && options.contains(key) && options[key].is_string()) {
        const std::string v = options[key].get<std::string>();
        if (!allowed(v))
            throw ToolFailure(std::string("options.") + key + " \"" + v + "\" is not offered by this printer (" +
                              (allow_auto ? "auto, on, off" : "on, off") + ")");
        return v;
    }
    if (allowed(stored))
        return stored;
    return allow_auto ? "auto" : "on";
}

int option_int(const std::string& v) { return v == "off" ? 0 : v == "on" ? 1 : 2; }

} // namespace

// mirrors SelectMachine.cpp:4429 s_get_slicing_extuder_nozzles, :2638 _get_used_nozzle_idxes, :5562 reset_and_sync_ams_list
PrintPlan ui_build_plan(Plater& plater, int plate_idx, MachineObject& obj, const json& overrides, const json& options)
{
    PartPlateList& plates = plater.get_partplate_list();
    PartPlate*     plate  = plates.get_plate(plate_idx);
    if (plate == nullptr)
        throw ToolFailure("plate " + std::to_string(plate_idx) + " does not exist; there are " + std::to_string(plates.get_plate_count()));
    if (!plate->is_slice_result_valid())
        throw ToolFailure("Plate " + std::to_string(plate_idx) + " is not sliced; call slice {plates: " + std::to_string(plate_idx) + "} first",
                          {{"plate", plate_idx}});
    PresetBundle* pb = wxGetApp().preset_bundle;
    if (pb == nullptr)
        throw ToolFailure("The preset bundle is not available");
    // Pull-mode agents only refresh their filament data when asked (SelectMachine.cpp:4829).
    if (NetworkAgent* pull_agent = wxGetApp().getAgent(); pull_agent != nullptr && pull_agent->get_filament_sync_mode() == FilamentSyncMode::pull)
        pull_agent->fetch_filament_info(obj.get_dev_id());

    const DynamicPrintConfig full = pb->full_config();
    PlanProject              pr;
    pr.plate       = plate_idx;
    pr.gcode_valid = plate->is_valid_gcode_file();

    std::vector<PlanSlot> slots;
    for (size_t i = 0; i < pb->filament_presets.size(); ++i) {
        PlanSlot      slot;
        const Preset* preset = pb->filaments.find_preset(pb->filament_presets[i], false);
        slot.preset_name     = pb->filament_presets[i];
        if (preset != nullptr) {
            std::string display;
            slot.type        = const_cast<Preset*>(preset)->config.get_filament_type(display);
            slot.filament_id = preset->filament_id;
            if (auto* vendor = dynamic_cast<const ConfigOptionStrings*>(preset->config.option("filament_vendor")); vendor && !vendor->values.empty())
                slot.vendor = vendor->values[0];
        }
        slot.color = color_rrggbbaa(pb->project_config.opt_string("filament_colour", unsigned(i)));
        pr.slot_filament_ids.push_back(slot.filament_id);
        slots.push_back(std::move(slot));
    }
    pr.slots = slots.size();

    const auto* nozzle_diameter = full.option<ConfigOptionFloats>("nozzle_diameter");
    pr.nozzle_count             = nozzle_diameter ? int(nozzle_diameter->values.size()) : 1;
    pr.filament_maps            = plate->get_real_filament_maps(pb->project_config);
    pr.plate_filament_maps      = plate->get_filament_maps();
    pr.filaments                = filaments_from(slots, plate->get_used_filaments(), pr.filament_maps, pr.nozzle_count);

    const auto* edited_diameter = pb->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto* volume_type     = dynamic_cast<const ConfigOptionEnumsGeneric*>(pb->project_config.option("nozzle_volume_type"));
    if (edited_diameter != nullptr)
        pr.nozzle_diameters = edited_diameter->values;
    if (volume_type != nullptr)
        for (size_t i = 0; i < volume_type->size(); ++i)
            pr.nozzle_volume_types.push_back(volume_type->get_at(i));

    GCodeProcessorResult*                                    gcode = plate->get_slice_result();
    std::shared_ptr<MultiNozzleUtils::NozzleGroupResultBase> group = gcode ? gcode->nozzle_group_result : nullptr;
    pr.nozzle_group_present     = group != nullptr;
    pr.nozzle_volume_option     = volume_type != nullptr;
    pr.dynamic_nozzle_map       = group && group->is_support_dynamic_nozzle_map();
    pr.slicing_with_fila_switch = pr.dynamic_nozzle_map;
    if (!pr.slicing_with_fila_switch)
        if (auto* sw = pb->project_config.option<ConfigOptionBool>("has_filament_switcher"))
            pr.slicing_with_fila_switch = sw->value;
    pr.source_printer_model = pb->printers.get_edited_preset().get_printer_type(pb);

    // the nozzles the plate asks for
    try {
        bool done = false;
        if (group && group->is_support_dynamic_nozzle_map() && volume_type && volume_type->values.size() == 2) {
            for (const auto& used : group->get_used_nozzles_in_extruder()) {
                PlanSlicingNozzle n;
                n.diameter = boost::lexical_cast<float>(used.diameter);
                n.flow     = int(DevNozzle::ToNozzleFlowType(used.volume_type));
                n.extruder = used.extruder_id == 0 ? DEPUTY_EXTRUDER_ID : MAIN_EXTRUDER_ID;
                if (used.extruder_id == 0 || used.extruder_id == 1)
                    pr.slicing_nozzles.push_back(n);
            }
            done = true;
        }
        for (int used : plate->get_used_filaments()) {
            const int physical = plate->get_physical_extruder_by_filament_id(full, used);
            const int logical  = plate->get_logical_extruder_by_filament_id(full, used);
            if (physical >= 0)
                pr.used_nozzle_idxes.insert(physical);
            if (done || physical < 0 || logical < 0 || edited_diameter == nullptr || volume_type == nullptr)
                continue;
            PlanSlicingNozzle n;
            n.extruder    = physical;
            n.diameter    = float(edited_diameter->get_at(size_t(logical)));
            const auto vt = NozzleVolumeType(volume_type->get_at(size_t(logical)));
            if (vt == NozzleVolumeType::nvtHybrid) {
                bool seen = false;
                for (const PlanSlicingNozzle& e : pr.slicing_nozzles)
                    seen = seen || e.extruder == physical;
                if (seen || !group)
                    continue;
                for (const auto& info : group->get_used_nozzles_in_extruder(logical)) {
                    n.flow = int(DevNozzle::ToNozzleFlowType(info.volume_type));
                    pr.slicing_nozzles.push_back(n);
                }
                continue;
            }
            n.flow = int(DevNozzle::ToNozzleFlowType(vt));
            pr.slicing_nozzles.push_back(n);
        }
    } catch (const std::exception&) {
    }
    if (group)
        for (const auto& used : group->get_used_nozzles_in_extruder(LOGIC_R_EXTRUDER_ID)) {
            try {
                PlanSlicingNozzle n;
                n.diameter = boost::lexical_cast<float>(used.diameter);
                n.flow     = int(DevNozzle::ToNozzleFlowType(used.volume_type));
                n.extruder = MAIN_EXTRUDER_ID;
                pr.rack_needed_nozzles.push_back(n);
            } catch (const std::exception&) {
            }
        }

    const auto* chamber = full.option<ConfigOptionInts>("chamber_temperature");
    for (const PlanFilament& f : pr.filaments) {
        pr.filament_hrc[f.id] = pb->get_required_hrc_by_filament_type(f.type);
        if (chamber != nullptr && f.id < int(chamber->values.size()))
            pr.chamber_temp[f.id] = chamber->values[size_t(f.id)];
        if (!f.filament_id.empty())
            for (double d : pr.nozzle_diameters)
                if (is_stringing_prone_filament(f.filament_id, float(d)))
                    pr.stringing_prone_filament = true;
    }
    if (gcode != nullptr)
        for (auto& w : gcode->warnings)
            pr.slice_warnings.push_back({w.msg, w.error_code, Plater::get_slice_warning_string(w).utf8_string()});
    pr.filament_lookup = [pb](const std::string& id) -> std::optional<std::pair<std::string, int>> {
        if (auto info = pb->get_filament_by_filament_id(id))
            return std::make_pair(info->filament_name, info->temperature_vitrification);
        return std::nullopt;
    };

    // the printer
    PrinterFacts  facts;
    NetworkAgent* agent    = wxGetApp().getAgent();
    facts.agent_present    = agent != nullptr;
    facts.server_connected = agent != nullptr && agent->is_server_connected(wxGetApp().get_printer_cloud_provider());
    facts.model_blocked    = wxGetApp().is_blocking_printing(&obj, pr.source_printer_model);
    facts.model_optional   = DevPrinterConfigUtil::is_optional_printer_model_id(obj.printer_type) ||
                           DevPrinterConfigUtil::is_optional_printer_model_id(pr.source_printer_model);
    {
        // mirrors SelectMachine.cpp:2703 is_same_printer_model
        const std::string source = pr.source_printer_model, target = obj.printer_type;
        bool              same = true;
        if (!facts.model_optional && source != target)
            same = (source == "C12" && target == "C11") || (source == "C11" && target == "C12") ||
                   ((obj.is_support_upgrade_kit && obj.installed_upgrade_kit) && source == "C12");
        else if (!facts.model_optional && obj.is_support_upgrade_kit && obj.installed_upgrade_kit)
            same = false;
        facts.model_differs = !same;
    }
    facts.printer_display_name = obj.get_printer_type_display_str().utf8_string();
    facts.source_display_name  = DevPrinterConfigUtil::get_printer_display_name(pr.source_printer_model);
    for (std::string* n : {&facts.printer_display_name, &facts.source_display_name}) {
        const std::string prefix = "Bambu Lab ";
        if (n->compare(0, prefix.size(), prefix) == 0)
            n->erase(0, prefix.size());
    }
    facts.ams_ext_mix_supported   = DevPrinterConfigUtil::support_ams_ext_mix_print(obj.printer_type);
    facts.tpu_left_firmware_check = DevPrinterConfigUtil::support_print_check_firmware_for_tpu_left(obj.printer_type);
    AppConfig* config             = wxGetApp().app_config;
    facts.allow_abnormal_storage  = config != nullptr && config->get("allow_abnormal_storage") == "true";
    facts.unsupported_auto_cali_filaments = DevPrinterConfigUtil::get_unsupport_auto_cali_filaments(obj.printer_type);
    facts.auto_on_cali_warning_filaments =
        DevPrinterConfigUtil::get_value_from_config<std::vector<std::string>>(obj.printer_type, "auto_on_cali_warning_tpu_filaments");
    facts.to_printer_filament_id       = [agent](const std::string& id) { return agent ? agent->from_orca_filament_id(id) : id; };
    facts.bed_leveling_support         = obj.is_support_bed_leveling;
    facts.nozzle_offset_cali_supported = obj.GetConfig()->SupportCalibrationNozzleOffset();
    facts.flow_cali_supported          = obj.is_support_pa_calibration;
    {
        // can_support_pa_auto_cali(): no used filament is on the printer's unsupported list
        bool unsupported = false;
        for (const PlanFilament& f : pr.filaments)
            unsupported = unsupported || std::find(facts.unsupported_auto_cali_filaments.begin(), facts.unsupported_auto_cali_filaments.end(),
                                                   facts.to_printer_filament_id(f.filament_id)) != facts.unsupported_auto_cali_filaments.end();
        facts.flow_cali_auto_offered = obj.GetConfig()->SupportCalibrationPA_FlowAuto() && !unsupported;
    }
    facts.pa_mode_supported = obj.is_support_pa_mode;
    {
        wxString reason;
        facts.timelapse_allowed = obj.canEnableTimelapse(reason);
        for (const PlanSliceWarning& w : pr.slice_warnings)
            if (w.msg == NOT_GENERATE_TIMELAPSE) {
                facts.timelapse_allowed = false;
                reason                  = w.text;
            }
        facts.timelapse_block_reason = reason.utf8_string();
    }
    if (auto tool = obj.GetExtensionTool().lock())
        facts.fan_f000_unmounted = DevPrinterConfigUtil::support_print_check_extension_fan_f000_mounted(obj.printer_type) && tool->IsToolTypeFanF000() &&
                                   !tool->IsMounted();
    if (auto* opts = obj.GetPrintOptions())
        if (auto* opt = opts->GetDetectionOption(PrintOptionEnum::Smart_Nozzle_Blob_Detection)) {
            facts.blob_detect_supported = opt->is_support_detect;
            facts.blob_detect_value     = opt->current_detect_value;
        }
    facts.blacklist = [](const DevFilaBlacklist::CheckFilamentInfo& info) { return DevFilaBlacklist::check_filaments_in_blacklist(info); };

    // options
    PlanOptions  po;
    auto         stored = [&](const char* key) { return config != nullptr ? config->get(obj.printer_type, key) : std::string(); };
    const json   opts   = options.is_object() ? options : json::object();
    po.bed_leveling       = choose_option(opts, "bed_leveling", stored("bed_leveling"), facts.bed_leveling_support != 1);
    po.flow_cali          = choose_option(opts, "flow_cali", stored("flow_cali"), !facts.flow_cali_supported || facts.flow_cali_auto_offered);
    po.nozzle_offset_cali = choose_option(opts, "nozzle_offset_cali", stored("nozzle_offset_cali"), true);
    po.pa_value           = choose_option(opts, "pa_value", stored("pa_value"), false);
    std::string tl        = stored("timelapse");
    if (opts.contains("timelapse"))
        tl = opts["timelapse"].is_boolean() ? (opts["timelapse"].get<bool>() ? "on" : "off") : opts["timelapse"].get<std::string>();
    if (tl != "on" && tl != "off")
        tl = "on"; // load_option_vals: a stored value the menu does not offer falls back to "on"
    po.timelapse = tl == "on";
    if (po.timelapse && !facts.timelapse_allowed) {
        if (opts.contains("timelapse"))
            throw ToolFailure("timelapse cannot be enabled: " + facts.timelapse_block_reason);
        po.timelapse = false;
    }
    if (obj.is_support_internal_timelapse)
        po.timelapse_storage = opts.value("timelapse_storage", std::string("internal"));
    po.ext_change_assist = opts.value("ext_change_assist", false) && (obj.is_support_ext_change_assist_old || obj.is_support_ext_change_assist);
    po.acknowledge_nozzle_diameter_mismatch = opts.value("acknowledge_nozzle_diameter_mismatch", false);
    po.skip_round_trip = !opts.value("allow_round_trip", false);
    po.bed_type        = bed_type_to_gcode_string(plate->get_bed_type(true));

    PrintPlan plan = make_plan(obj, std::move(pr), facts, po, overrides);
    if (obj.GetNozzleSystem() && obj.GetNozzleSystem()->GetNozzleRack()->IsSupported()) {
        auto m = obj.get_nozzle_mapping_result();
        if (m->HasResult())
            plan.nozzle_mapping = {{"result", m->GetResultStr()}, {"mapping", m->GetNozzleMappingJson()}};
        else
            plan.nozzle_mapping = "requires printer round trip (H2C rack); not run in dry_run";
    }
    return plan;
}

// ---- print job -------------------------------------------------------------------------------

// mirrors SelectMachine.cpp:3584 on_send_print (PrintJob fields) and :5455 set_default (project name)
PrintSetup ui_print_setup(Plater& plater, int plate_idx, MachineObject& obj, const PrintPlan& plan)
{
    PrintSetup    s;
    AppConfig*    config = wxGetApp().app_config;
    NetworkAgent* agent  = wxGetApp().getAgent();
    s.dev_id             = obj.get_dev_id();
    s.dev_ip             = obj.get_dev_ip();
    s.ftp_folder         = obj.get_ftp_folder();
    s.access_code        = obj.get_access_code();
#if !BBL_RELEASE_TO_PUBLIC
    s.use_ssl_for_ftp  = config != nullptr && config->get("enable_ssl_for_ftp") == "true";
    s.use_ssl_for_mqtt = config != nullptr && config->get("enable_ssl_for_mqtt") == "true";
#else
    s.use_ssl_for_ftp  = obj.local_use_ssl_for_ftp;
    s.use_ssl_for_mqtt = obj.local_use_ssl;
#endif
    s.username         = agent != nullptr ? agent->default_lan_username() : std::string();
    s.connection_type  = obj.connection_type();
    s.cloud_print_only = obj.is_support_cloud_print_only;
    if (plan.enc.available) {
        s.ams_mapping      = plan.enc.ams_mapping.dump();
        s.ams_mapping2     = plan.enc.ams_mapping2.dump();
        s.ams_mapping_info = plan.enc.ams_mapping_info.dump();
    }
    s.nozzles_info = plan.enc.nozzles_info.dump();
    if (!obj.get_nozzle_mapping_result()->GetNozzleMappingJson().empty())
        s.nozzle_mapping = obj.get_nozzle_mapping_result()->GetNozzleMappingJson().dump();
    s.task_use_ams = plan.enc.task_use_ams;

    const PlanOptions& o = plan.options;
    s.sdcard_state       = int(obj.GetStorage()->get_sdcard_state());
    s.has_sdcard         = (config != nullptr && config->get("allow_abnormal_storage") == "true") ?
                               (s.sdcard_state == DevStorage::SdcardState::HAS_SDCARD_NORMAL || s.sdcard_state == DevStorage::SdcardState::HAS_SDCARD_ABNORMAL) :
                               s.sdcard_state == DevStorage::SdcardState::HAS_SDCARD_NORMAL;
    s.could_emmc_print = obj.can_use_emmc_print();
    bool disable_emmc  = true;
    if (config != nullptr) {
        const std::string v = config->get("disable_emmc_print");
        if (v == "0" || v == "false")
            disable_emmc = false;
    }
    s.try_emmc_print            = s.could_emmc_print && !disable_emmc;
    s.bed_leveling              = o.bed_leveling == "on";
    s.flow_cali                 = o.flow_cali == "on";
    s.record_timelapse          = o.timelapse;
    s.ext_change_assist         = o.ext_change_assist;
    s.auto_bed_leveling         = option_int(o.bed_leveling);
    s.auto_flow_cali            = option_int(o.flow_cali);
    s.auto_offset_cali          = option_int(o.nozzle_offset_cali);
    s.extruder_cali_manual_mode = obj.is_support_pa_mode ? (o.pa_value == "on" ? 0 : 1) : -1;
    if (o.timelapse && obj.is_support_internal_timelapse && !o.timelapse_storage.empty()) {
        s.timelapse_use_internal_set = true;
        s.timelapse_use_internal     = o.timelapse_storage == "internal";
    }
    s.bed_type    = o.bed_type;
    s.plate_index = plate_idx + 1;

    wxString filename = plater.get_export_gcode_filename("", true, false);
    if (filename.empty())
        filename = "Untitled";
    std::string name = boost::filesystem::path(filename.ToStdString()).filename().string();
    if (wxString::FromUTF8(name).find("Untitled") != wxString::npos) {
        if (PartPlate* part = plater.get_partplate_list().get_plate(plate_idx)) {
            auto objects = part->get_objects_on_this_plate();
            if (!objects.empty()) {
                name = objects[0]->name;
                for (size_t i = 1; i < objects.size(); ++i)
                    name += " + " + objects[i]->name;
                if (name.size() > 100)
                    name = name.substr(0, 97) + "...";
            }
        }
    }
    for (char c : std::string("<>[]:/\\|?*\""))
        name.erase(std::remove(name.begin(), name.end(), c), name.end());
    s.project_name = name;
    s.preset_name  = s.project_name + "_plate_" + std::to_string(s.plate_index);
    return s;
}

void configure_print_job(PrintJob& job, const PrintSetup& s, std::function<void()> on_ip_fail)
{
    job.m_dev_ip                = s.dev_ip;
    job.m_ftp_folder            = s.ftp_folder;
    job.m_access_code           = s.access_code;
    job.m_local_use_ssl_for_ftp = s.use_ssl_for_ftp;
    job.m_local_use_ssl         = s.use_ssl_for_mqtt;
    job.connection_type         = s.connection_type;
    job.cloud_print_only        = s.cloud_print_only;
    job.m_print_type            = "from_normal";
    job.set_project_name(s.project_name);
    job.task_ams_mapping      = s.ams_mapping;
    job.task_ams_mapping2     = s.ams_mapping2;
    job.task_ams_mapping_info = s.ams_mapping_info;
    job.task_nozzle_mapping   = s.nozzle_mapping;
    job.task_nozzles_info     = s.nozzles_info;
    job.sdcard_state          = DevStorage::SdcardState(s.sdcard_state);
    job.has_sdcard            = s.has_sdcard;
    job.could_emmc_print      = s.could_emmc_print;
    if (s.timelapse_use_internal_set)
        job.task_timelapse_use_internal = s.timelapse_use_internal;
    job.set_print_config(s.bed_type, s.bed_leveling, s.flow_cali, s.vibration_cali, s.record_timelapse, s.layer_inspect, s.ext_change_assist,
                         s.auto_bed_leveling, s.auto_flow_cali, s.auto_offset_cali, s.extruder_cali_manual_mode);
    job.task_use_ams = s.task_use_ams;
    // The dialog opens InputIpAddressDialog here, and PrintJob calls the callback without a null check.
    job.on_check_ip_address_fail(std::move(on_ip_fail));
}

// mirrors Jobs/PrintJob.cpp:238-316 PrintParams assignments
json print_params_json(const PrintSetup& s)
{
    json p;
    p["dev_ip"]                      = s.dev_ip;
    p["use_ssl_for_ftp"]             = s.use_ssl_for_ftp;
    p["use_ssl_for_mqtt"]            = s.use_ssl_for_mqtt;
    p["username"]                    = s.username;
    p["password"]                    = s.access_code;
    p["dev_id"]                      = s.dev_id;
    p["ftp_folder"]                  = s.ftp_folder;
    p["filename"]                    = s.file_3mf;
    p["config_filename"]             = s.file_config_3mf;
    p["plate_index"]                 = s.plate_index;
    p["task_bed_leveling"]           = s.bed_leveling;
    p["task_flow_cali"]              = s.flow_cali;
    p["task_vibration_cali"]         = s.vibration_cali;
    p["task_layer_inspect"]          = s.layer_inspect;
    p["task_record_timelapse"]       = s.record_timelapse;
    p["task_timelapse_use_internal"] = s.timelapse_use_internal;
    p["nozzle_mapping"]              = s.nozzle_mapping;
    p["ams_mapping"]                 = s.ams_mapping;
    p["ams_mapping2"]                = s.ams_mapping2;
    p["ams_mapping_info"]            = s.ams_mapping_info;
    p["nozzles_info"]                = s.nozzles_info;
    p["connection_type"]             = s.connection_type;
    p["task_use_ams"]                = s.task_use_ams;
    p["task_bed_type"]               = s.bed_type;
    p["print_type"]                  = "from_normal";
    p["auto_bed_leveling"]           = s.auto_bed_leveling;
    p["auto_flow_cali"]              = s.auto_flow_cali;
    p["auto_offset_cali"]            = s.auto_offset_cali;
    p["extruder_cali_manual_mode"]   = s.extruder_cali_manual_mode;
    p["task_ext_change_assist"]      = s.ext_change_assist;
    p["try_emmc_print"]              = s.try_emmc_print;
    p["project_name"]                = s.project_name;
    p["preset_name"]                 = s.preset_name;
    p["has_sdcard"]                  = s.has_sdcard;
    p["sdcard_state"]                = s.sdcard_state;
    return p;
}

}}} // namespace Slic3r::GUI::Mcp
