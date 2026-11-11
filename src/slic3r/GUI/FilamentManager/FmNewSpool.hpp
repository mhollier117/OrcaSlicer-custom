#pragma once

#include <string>

class wxWindow;

namespace Slic3r {
class MachineObject;
namespace GUI {
class AMSControl;
}
} // namespace Slic3r

namespace Slic3r::GUI::Fm {

// Shows the marker on every slot of the shown printer that holds a newly registered official spool, and takes down the
// markers this control showed that were dropped since. Does nothing when the Filament Manager is off. Never throws.
void apply_new_spool_hints(AMSControl* control, MachineObject* obj);

// The user clicked the marker of a slot of `obj`. Never throws.
void on_new_spool_hint(wxWindow* parent, MachineObject* obj, int ams_id, const std::string& slot_id);

} // namespace Slic3r::GUI::Fm
