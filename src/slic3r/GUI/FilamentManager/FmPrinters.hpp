#pragma once

#include "slic3r/Utils/BblFilament/Linker.hpp"
#include "slic3r/Utils/BblFilament/PrinterTap.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {
class DeviceManager;
class MachineObject;
} // namespace Slic3r

namespace Slic3r::GUI::Fm {

// Hands a message of a printer to the printer tap. UI thread; cheap unless it is a status message. Does nothing unless
// the Filament Manager is enabled and the printer is a Bambu printer. Never throws.
void on_printer_message(MachineObject& printer, const std::string& message);

// The Bambu printers of the signed-in account that are not in LAN mode, by dev id.
std::map<std::string, MachineObject*> account_printers(DeviceManager& devices);

// One PrinterView per printer the linker may look at: the Bambu printers of the account, plus the selected printer
// when it is a Bambu printer in LAN mode. Built from the device manager and the tap.
std::vector<BblFilament::PrinterView> printer_views(DeviceManager* devices, const BblFilament::PrinterTap& tap, long long now);

// The linker's settings from the app config: Off when not enabled, else DryRun when dry_run, else On; `only` is the
// comma-separated list of SlotKey::text() values (empty = every slot). A `mode_override` of "off", "dry_run" or "on"
// decides the mode in place of `enabled` and `dry_run`; any other text is ignored.
BblFilament::LinkConfig link_config(bool enabled, bool dry_run, const std::string& only, const std::string& mode_override);
// True when link_config() honours `mode_override`.
bool is_link_mode_override(const std::string& mode_override);
// The environment variable ORCA_FM_LINK_MODE, read once per process ("" when unset).
const std::string& link_mode_env();
std::set<std::string>   parse_only_slots(const std::string& only);
std::string             join_only_slots(const std::set<std::string>& slots);

} // namespace Slic3r::GUI::Fm
