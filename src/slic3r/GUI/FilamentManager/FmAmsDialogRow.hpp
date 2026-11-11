#pragma once

#include "slic3r/Utils/BblFilament/Spool.hpp"

#include <wx/panel.h>

#include <optional>
#include <string>
#include <vector>

class Button;
class CheckBox;
class Label;

namespace Slic3r::GUI {
class AMSMaterialsSetting;
class HyperLink;
} // namespace Slic3r::GUI

namespace Slic3r::GUI::Fm {

// The Filament Manager row of the AMS slot dialog: choose a spool of the account for the slot, or add the filament
// that is set as a new spool. Either way the linker binds the spool to the slot once the printer reports it.
class AmsDialogRow : public wxPanel
{
public:
    AmsDialogRow(wxWindow* parent, AMSMaterialsSetting* dlg);

    // Each time the dialog is shown.
    void on_popup();
    // Right before the dialog closes after a successful Confirm, with the colours that were sent. Never throws.
    void on_confirmed(const std::string& tray_color, const std::vector<std::string>& tray_colors, int tray_ctype);

private:
    std::string dev_id() const;
    void        choose();
    void        clear_choice();
    void        update_texts(); // the spool text, the Clear link and the check box
    void        update_box();   // enabled when the filament selected in the dialog can be added
    void        relayout();

    AMSMaterialsSetting*             m_dlg{nullptr};
    Button*                          m_choose{nullptr};
    Label*                           m_spool_text{nullptr};
    HyperLink*                       m_clear{nullptr};
    CheckBox*                        m_box{nullptr};
    Label*                           m_box_label{nullptr};
    std::optional<BblFilament::Spool> m_chosen;
    std::optional<BblFilament::Spool> m_current; // the spool the cloud has in this slot
    bool                             m_combo_bound{false};
};

} // namespace Slic3r::GUI::Fm
