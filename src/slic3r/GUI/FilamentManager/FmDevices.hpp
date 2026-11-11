#pragma once

#include "FmColorCodes.hpp"
#include "FmPresets.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace Slic3r {
class DevAmsTray;
class DeviceManager;
} // namespace Slic3r

namespace Slic3r::GUI::Fm {

using json = nlohmann::json;

struct TrayContext
{
    const ColorCodes* colors{nullptr};
    std::string       lang;
    ToBambuId         to_bambu;
};

// 0,0 -> "A1"; 1,3 -> "B4"; ams 128..153 -> "HT-A".."HT-Z"; else "".
std::string tray_label(int ams_id, int slot_index);
json        tray_json(const DevAmsTray& tray, const std::string& slot_id, const std::string& label, bool exists, const TrayContext& ctx);
json        machine_list(DeviceManager* mgr);
json        ams_data(DeviceManager* mgr, const TrayContext& ctx);
bool        request_pushall(DeviceManager* mgr, const std::string& dev_id, std::string& used_dev_id);
void        select_machine(DeviceManager* mgr, const std::string& dev_id);

} // namespace Slic3r::GUI::Fm
