#include "FmNewSpool.hpp"

#include "FmRuntime.hpp"
#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/Widgets/AMSControl.hpp"
#include "slic3r/Utils/BblFilament/Linker.hpp"
#include "slic3r/Utils/BblFilament/NewSpools.hpp"

#include <wx/weakref.h>

#include <map>
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

void on_new_spool_hint(wxWindow*, MachineObject*, int, const std::string&) {}

} // namespace Slic3r::GUI::Fm
