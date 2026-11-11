#pragma once

#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"

#include <wx/colour.h>
#include <wx/gdicmn.h>

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

// Lets the user choose one of the given spools (modal). A spool for which `choosable` is false is shown greyed at the
// end and cannot be chosen.
class SpoolPicker : public DPIDialog
{
public:
    SpoolPicker(wxWindow* parent, std::vector<BblFilament::Spool> spools, const std::function<bool(const BblFilament::Spool&)>& choosable);

    // The id of the chosen spool after ShowModal() returned wxID_OK.
    const std::string& chosen_id() const { return m_chosen; }

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void fill();
    bool row_choosable(long row) const;
    void pick(long row);

    std::vector<BblFilament::Spool> m_spools; // choosable ones first
    std::vector<bool>               m_ok;     // per spool
    std::vector<std::size_t>        m_rows;   // list row -> index into m_spools
    std::string                     m_chosen;
    bool                            m_tip_shown{false};

    TextInput*     m_search{nullptr};
    wxListView*    m_list{nullptr};
    Label*         m_help{nullptr};
    DialogButtons* m_buttons{nullptr};
};

} // namespace Slic3r::GUI::Fm
