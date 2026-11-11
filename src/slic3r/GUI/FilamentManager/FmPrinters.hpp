#pragma once

#include "slic3r/Utils/BblFilament/Linker.hpp"
#include "slic3r/Utils/BblFilament/PrinterTap.hpp"

#include <map>
#include <optional>
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

// "off", "dry_run" or "on"; nothing for any other text.
std::optional<BblFilament::LinkMode> parse_link_mode(const std::string& text);
const char*                          link_mode_text(BblFilament::LinkMode mode);

// The linker's settings. The mode is the session's override when there is one, else `env` when parse_link_mode()
// reads it, else On when `enabled` (the Preferences check box) and Off when not. The allow-list is the session's
// (SlotKey::text() values; empty = every slot).
BblFilament::LinkConfig link_config(bool enabled, const std::string& env, const std::optional<BblFilament::LinkMode>& session_mode,
                                    const std::set<std::string>& session_only);
// Where link_config() takes the mode from: "session", "env" or "config".
const char* link_mode_source(const std::string& env, const std::optional<BblFilament::LinkMode>& session_mode);
// The environment variable ORCA_FM_LINK_MODE, read once per process ("" when unset).
const std::string& link_mode_env();

} // namespace Slic3r::GUI::Fm
