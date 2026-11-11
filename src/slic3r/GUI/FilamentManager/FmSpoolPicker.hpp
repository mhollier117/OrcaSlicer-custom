#pragma once

#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"

#include <wx/colour.h>
#include <wx/gdicmn.h>
#include <wx/string.h>

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

class TextInput;
class Label;
class wxListView;

namespace Slic3r::GUI {
class DialogButtons;
}

namespace Slic3r::GUI::Fm {

// The colours of a spool ("#RRGGBB" or "#RRGGBBAA"); colours that cannot be read are left out.
std::vector<wxColour> spool_colours(const BblFilament::Spool& spool);

// How the picker and the slot dialog name a spool: the brand and the name, or the name alone when it starts with the brand
// and a space (case ignored).
std::string spool_display_name(const BblFilament::Spool& spool);

// Whether a slot can show the spool's colours: one colour, or several on a printer that takes several colours per slot
// and no more than `max_colours` (what the slot dialog's colour picker edits).
bool slot_shows_colours(const BblFilament::Spool& spool, bool several_per_slot, std::size_t max_colours);

// Lets the user choose one of the given spools (modal). A spool for which `why_not` gives a text is shown greyed at the
// end, with that text as its tooltip, and cannot be chosen.
class SpoolPicker : public DPIDialog
{
public:
    SpoolPicker(wxWindow* parent, std::vector<BblFilament::Spool> spools, const std::function<wxString(const BblFilament::Spool&)>& why_not);

    // The id of the chosen spool after ShowModal() returned wxID_OK.
    const std::string& chosen_id() const { return m_chosen; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void fill();
    bool row_choosable(long row) const;
    void pick(long row);

    std::vector<BblFilament::Spool> m_spools; // choosable ones first
    std::vector<wxString>           m_why;    // per spool: empty when it can be chosen, else why not
    std::vector<std::size_t>        m_rows;   // list row -> index into m_spools
    std::string                     m_chosen;
    long                            m_tip_row{-1};

    TextInput*     m_search{nullptr};
    wxListView*    m_list{nullptr};
    Label*         m_help{nullptr};
    DialogButtons* m_buttons{nullptr};
};

} // namespace Slic3r::GUI::Fm
