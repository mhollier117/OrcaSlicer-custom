#pragma once

// Headless port of the print logic of SelectMachineDialog (fork): which AMS tray / external spool
// every project filament prints from (single and dual nozzle, filament switch, hotend rack), the
// ams_mapping / ams_mapping2 / ams_mapping_info / nozzles_info encodings, the dialog's blocking
// errors and confirmation warnings, and the PrintParams a print job is started with.
//
// The mapping is the dialog's: DevMappingUtil::ams_filament_mapping is called, not copied. Everything
// is keyed by filament id explicitly (the dialog indexes parallel vectors).
//
// Pure helpers (filaments_from, run_auto_mapping, apply_overrides, encode_mapping, run_checks,
// make_plan) take their inputs explicitly so tests need no Plater. ui_build_plan() gathers the inputs
// from the Plater, the preset bundle and the printer; UI thread only.
// Design: C:/claude-code/orca_mcp/docs/specs/2026-11-02-printer-mcp-design.md

#include "libslic3r/ProjectTask.hpp"
#include "slic3r/GUI/DeviceCore/DevFilaBlackList.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {
class MachineObject;
namespace GUI {
class Plater;
class PrintJob;
namespace Mcp {

// ---- inputs ----------------------------------------------------------------------------------

// A project filament slot as SelectMachineDialog::reset_and_sync_ams_list reads it.
struct PlanSlot
{
    std::string preset_name;
    std::string type;        // Preset::config.get_filament_type()
    std::string filament_id; // Preset::filament_id (the setting id)
    std::string vendor;      // first filament_vendor
    std::string color;       // #RRGGBBAA, project filament_colour
};

// A filament the plate prints with.
struct PlanFilament
{
    int         id = 0;        // 0-based project slot (FilamentInfo::id); the tools say id + 1
    std::string type;
    std::string filament_id;
    std::string color;         // #RRGGBBAA
    std::string brand;
    std::string preset_name;
    int         nozzle = 0;    // 0 right / main, 1 left / deputy (the cloud's nozzle ids)
    int         filament_map = 0; // the plate's filament map entry: 1 left, 2 right, 0 none
    bool        used = true;
};

// A nozzle the sliced plate asks for (s_get_slicing_extuder_nozzles).
struct PlanSlicingNozzle
{
    int    extruder = 0;       // physical extruder id: 0 right / main, 1 left / deputy
    double diameter = 0.0;
    int    flow = 0;           // NozzleFlowType
};

struct PlanSliceWarning
{
    std::string msg;
    std::string error_code;
    std::string text; // Plater::get_slice_warning_string
};

// What the checks and encodings need to know of the project and the sliced plate.
struct PlanProject
{
    std::vector<PlanFilament> filaments;                  // used by the plate, ascending id
    size_t                    slots = 0;                  // preset_bundle->filament_presets.size()
    std::vector<std::string>  slot_filament_ids;          // per slot: the filament preset's filament_id
    int                       nozzle_count = 1;           // size of the printer preset's nozzle_diameter
    std::vector<int>          filament_maps;              // PartPlate::get_real_filament_maps
    std::vector<double>       nozzle_diameters;           // printer preset, for nozzles_info
    std::vector<int>          nozzle_volume_types;        // project nozzle_volume_type, for nozzles_info
    bool                      gcode_valid = true;
    bool                      dynamic_nozzle_map = false; // use_dynamic_nozzle_map()
    bool                      slicing_with_fila_switch = false;
    std::string               source_printer_model;       // the edited printer preset's printer type
    std::vector<PlanSlicingNozzle> slicing_nozzles;
    std::set<int>             used_nozzle_idxes;          // physical extruders the plate prints with
    std::map<int, int>        filament_hrc;               // filament id -> required nozzle hardness
    std::map<int, int>        chamber_temp;               // filament id -> chamber_temperature
    std::vector<PlanSliceWarning> slice_warnings;
    bool                      stringing_prone_filament = false;
    std::vector<PlanSlicingNozzle> rack_needed_nozzles;   // right-extruder nozzles of the nozzle grouping
    // Looks a filament preset up by its filament id: {name, temperature_vitrification}.
    std::function<std::optional<std::pair<std::string, int>>(const std::string&)> filament_lookup;
    int                       plate = 0;                  // 0-based
};

// Printer / app facts the dialog reads from config files, the app config and the agent.
struct PrinterFacts
{
    bool agent_present     = true;
    bool server_connected  = true;
    bool model_blocked     = false;  // GUI_App::is_blocking_printing
    bool model_optional    = false;  // printer or source model unknown
    bool model_differs     = false;  // !is_same_printer_model()
    std::string printer_display_name, source_display_name;
    bool ams_ext_mix_supported = true;
    bool tpu_left_firmware_check = false;
    bool allow_abnormal_storage = false;
    bool fan_f000_unmounted = false; // extension tool: F000 fan expected but not mounted
    bool blob_detect_supported = false;
    int  blob_detect_value = 0;
    std::vector<std::string> unsupported_auto_cali_filaments;   // printer-side filament ids
    std::vector<std::string> auto_on_cali_warning_filaments;    // printer-side filament ids
    std::function<std::string(const std::string&)> to_printer_filament_id; // agent->from_orca_filament_id
    // option menus the printer offers (SelectMachineDialog::update_option_opts)
    int  bed_leveling_support = 0;       // 0 hidden, 1 on/off, 2 auto/on/off
    bool nozzle_offset_cali_supported = false;
    bool flow_cali_supported = false;
    bool flow_cali_auto_offered = false; // SupportCalibrationPA_FlowAuto && can_support_pa_auto_cali
    bool timelapse_allowed = true;       // canEnableTimelapse && no timelapse slice warning
    std::string timelapse_block_reason;
    bool pa_mode_supported = false;
    bool wait_sending_finish_supported = false;
    // DevFilaBlacklist::check_filaments_in_blacklist (reads the app config); null: the check is reported as skipped.
    std::function<DevFilaBlacklist::CheckResult(const DevFilaBlacklist::CheckFilamentInfo&)> blacklist;
};

// print options with the dialog's meaning (off/on/auto)
struct PlanOptions
{
    std::string bed_leveling       = "auto";
    std::string flow_cali          = "auto";
    std::string nozzle_offset_cali = "auto";
    std::string pa_value           = "on";     // Shared PA profile
    bool        timelapse          = false;
    std::string timelapse_storage;             // "internal" / "external" / ""
    bool        ext_change_assist  = false;
    bool        acknowledge_nozzle_diameter_mismatch = false;
    // The rack nozzle mapping needs a printer round trip; a plan never sends it.
    bool        skip_round_trip    = true;
    std::string bed_type;                      // task_bed_type (gcode string), filled by the caller
};

// ---- mapping ---------------------------------------------------------------------------------

struct MappingRun
{
    std::vector<FilamentInfo> result;           // one per used filament, dialog's m_ams_mapping_result
    int                       filament_result = 0;
    bool                      valid = false;    // DevMappingUtil::is_valid_mapping_result
    bool                      dual = false;     // the split left/right mapping ran
    std::string               error;            // the dialog's early false return
    std::set<int>             overridden;       // filament ids the caller overrode
};

// The dialog's filament list: the used slots with type, brand, colour and the nozzle they print on.
// `used_ids` are 1-based as PartPlate::get_used_filaments returns them; slots outside `slots` are
// skipped as reset_and_sync_ams_list skips them.
std::vector<PlanFilament> filaments_from(const std::vector<PlanSlot>& slots, const std::vector<int>& used_ids,
                                         const std::vector<int>& filament_maps, int nozzle_count);

// SelectMachineDialog::do_ams_mapping.
MappingRun run_auto_mapping(MachineObject& printer, const std::vector<PlanFilament>& filaments, int nozzle_count,
                            const std::vector<int>& filament_maps, bool use_ams = true);

// Applies overrides [{filament: <1-based slot>, ams_id, slot_id} | {filament, external: left|right|main}]
// under AmsMapingPopup's rules (type match, tray present, the filament's nozzle side, no ext spool with a
// filament switch). Throws ToolFailure naming the reason for an override the popup would not offer.
void apply_overrides(MachineObject& printer, const PlanProject& project, MappingRun& run, const nlohmann::json& overrides);

// The tray index SelectMachineDialog / DevMappingUtil use: ams_id*4+slot, ams_id+slot for N3S (128+),
// 24+slot for AMS-Lite-mixed, 254 / 255 for the external spools. -1 when the tray does not exist.
int tray_index_of(MachineObject& printer, const std::string& ams_id, const std::string& slot_id);

struct Encodings
{
    bool           available = false;  // false: nothing mapped at all (the job gets empty strings)
    bool           all_mapped = false;
    nlohmann::json ams_mapping = nlohmann::json::array();       // v0: tray index per project slot, -1 unused / external
    nlohmann::json ams_mapping2 = nlohmann::json::array();      // v1: {ams_id, slot_id} per slot
    nlohmann::json ams_mapping_info = nlohmann::json::array();  // per slot
    nlohmann::json nozzles_info = nlohmann::json::array();      // only for two nozzles
    bool           task_use_ams = false;
};

// SelectMachineDialog::get_ams_mapping_result (FROM_NORMAL) + build_nozzles_info + the task_use_ams rule
// of on_send_print. One entry per project filament slot (`project.slots`).
Encodings encode_mapping(MachineObject& printer, const PlanProject& project, const MappingRun& run);

nlohmann::json build_nozzles_info(const PlanProject& project);

// ---- checks ----------------------------------------------------------------------------------

struct PlanCheck
{
    std::string    id;        // stable snake_case
    std::string    severity;  // "error" | "warning"
    std::string    message;   // English, as the dialog words it
    nlohmann::json details;   // may carry "needs_confirmation": true (on_ok_btn's popup) or "deferred": true
};

struct CheckOutcome
{
    std::vector<PlanCheck> checks;
    nlohmann::json         skipped = nlohmann::json::array(); // [{id, skipped: true, why}]
};

// update_show_status' blocking errors / warnings and on_ok_btn's confirmation warnings.
CheckOutcome run_checks(MachineObject& printer, const PlanProject& project, const PrinterFacts& facts, const MappingRun& run,
                        const PlanOptions& options);

// ---- the plan --------------------------------------------------------------------------------

struct PlanTarget
{
    int         filament_id = 0;       // 0-based
    std::string ams_id, slot_id;       // "" when unmapped
    int         tray_index = -1;       // -1 none
    bool        external = false;
    int         nozzle = 0;            // nozzle the filament prints on: 0 right / 1 left
    std::string tray_type, tray_color;
    double      color_distance = -1.0;
    std::string source;                // "auto" | "override"
    std::string reason;
};

struct PrintPlan
{
    int                       plate = 0;
    std::vector<PlanFilament> filaments;
    std::vector<PlanTarget>   mapping;
    nlohmann::json            encodings = nlohmann::json::object();
    std::vector<PlanCheck>    checks;
    nlohmann::json            checks_skipped = nlohmann::json::array();
    nlohmann::json            nozzles_info = nlohmann::json::array();
    nlohmann::json            nozzle_mapping;      // rack nozzle mapping state or the round-trip note
    PlanOptions               options;
    bool                      dual_nozzle = false;
    bool                      sendable = false;
    bool                      needs_confirmation = false;
    // internals the print needs
    MappingRun                run;
    Encodings                 enc;
    PlanProject               project;
};

// The plan from explicit inputs (what ui_build_plan does after gathering them).
PrintPlan make_plan(MachineObject& printer, PlanProject project, const PrinterFacts& facts, PlanOptions options,
                    const nlohmann::json& overrides);

// Gathers the inputs from the plater (the plate must be sliced), the preset bundle and the printer, and
// makes the plan. UI thread. `overrides` and `options` are the tools' arguments.
PrintPlan ui_build_plan(Plater& plater, int plate_idx, MachineObject& printer, const nlohmann::json& overrides,
                        const nlohmann::json& options);

nlohmann::json plan_to_json(const PrintPlan& plan);

// ---- print job -------------------------------------------------------------------------------

// What the dialog's on_send_print sets on the PrintJob, and what PrintJob::process then puts into
// PrintParams.
struct PrintSetup
{
    std::string dev_id, dev_ip, ftp_folder, access_code, connection_type, project_name, bed_type, preset_name;
    std::string username;
    bool        use_ssl_for_ftp = true, use_ssl_for_mqtt = true;
    bool        cloud_print_only = false;
    std::string ams_mapping, ams_mapping2, ams_mapping_info, nozzles_info, nozzle_mapping;
    bool        task_use_ams = false;
    bool        bed_leveling = false, flow_cali = false, vibration_cali = false, record_timelapse = false, layer_inspect = true;
    bool        timelapse_use_internal_set = false, timelapse_use_internal = false;
    bool        ext_change_assist = false;
    int         auto_bed_leveling = 0, auto_flow_cali = 0, auto_offset_cali = 0, extruder_cali_manual_mode = -1;
    bool        has_sdcard = false, could_emmc_print = false, try_emmc_print = false;
    int         sdcard_state = 0;
    int         plate_index = 1;             // PrintParams::plate_index, 1-based
    std::string file_3mf, file_config_3mf;
};

// PrintJob fields from `setup`, with on_check_ip_address_fail set to `on_ip_fail` (the dialog opens
// InputIpAddressDialog there; a tool has no dialog).
void configure_print_job(PrintJob& job, const PrintSetup& setup, std::function<void()> on_ip_fail);

// The PrintParams JSON of the job (PrintJob::process' assignments), secrets included; pass it
// through redact() before returning it.
nlohmann::json print_params_json(const PrintSetup& setup);

// The setup for a plan on the selected printer: PrintJob fields from `on_send_print`, option defaults from
// the app config. UI thread. `files_*` are filled in by the caller after the 3mf export.
PrintSetup ui_print_setup(Plater& plater, int plate_idx, MachineObject& printer, const PrintPlan& plan);

}}} // namespace Slic3r::GUI::Mcp
