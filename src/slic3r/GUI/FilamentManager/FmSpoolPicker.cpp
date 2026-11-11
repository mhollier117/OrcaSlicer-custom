#include "FmSpoolPicker.hpp"

#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/DialogButtons.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/TextInput.hpp"

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/dcmemory.h>
#include <wx/event.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/pen.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>

#include <cctype>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI::Fm {

namespace fil = BblFilament;

namespace {

int hex_digit(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    const char l = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return l >= 'a' && l <= 'f' ? l - 'a' + 10 : -1;
}

// "#RRGGBB" / "#RRGGBBAA" / without '#'; an invalid colour comes back not Ok().
wxColour parse_colour(const std::string& text)
{
    std::string s = text;
    if (!s.empty() && s.front() == '#')
        s.erase(0, 1);
    if (s.size() != 6 && s.size() != 8)
        return wxColour();
    unsigned char v[4] = {0, 0, 0, 255};
    for (std::size_t i = 0; i < s.size(); i += 2) {
        const int hi = hex_digit(s[i]), lo = hex_digit(s[i + 1]);
        if (hi < 0 || lo < 0)
            return wxColour();
        v[i / 2] = static_cast<unsigned char>(hi * 16 + lo);
    }
    return wxColour(v[0], v[1], v[2], v[3]);
}

wxBitmap swatch(const std::vector<wxColour>& colours, int size)
{
    wxBitmap   bmp(size, size);
    wxMemoryDC dc(bmp);
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(wxColour(128, 128, 128)));
    dc.DrawRectangle(0, 0, size, size);
    const int n = static_cast<int>(colours.size());
    for (int i = 0; i < n; ++i) {
        const int x0 = size * i / n, x1 = size * (i + 1) / n;
        dc.SetBrush(wxBrush(wxColour(colours[i].Red(), colours[i].Green(), colours[i].Blue())));
        dc.DrawRectangle(x0, 0, x1 - x0, size);
    }
    dc.SetBrush(*wxTRANSPARENT_BRUSH);
    dc.SetPen(wxPen(wxColour(172, 172, 172)));
    dc.DrawRectangle(0, 0, size, size);
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

wxString utf8(const std::string& s) { return wxString::FromUTF8(s.c_str()); }

wxString display_name(const fil::Spool& s)
{
    if (s.vendor.empty())
        return utf8(s.name);
    return utf8(s.vendor + " " + s.name);
}

} // namespace

std::vector<wxColour> spool_colours(const fil::Spool& spool)
{
    std::vector<wxColour> out;
    for (const std::string& c : spool.colors.empty() ? std::vector<std::string>{spool.color} : spool.colors) {
        const wxColour col = parse_colour(c);
        if (col.IsOk())
            out.push_back(col);
    }
    return out;
}

SpoolPicker::SpoolPicker(wxWindow* parent, std::vector<fil::Spool> spools, const std::function<bool(const fil::Spool&)>& choosable)
    : DPIDialog(parent, wxID_ANY, _L("Choose a spool"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX | wxRESIZE_BORDER)
{
    // Choosable spools first, each group in the service's order.
    std::vector<fil::Spool> greyed;
    for (fil::Spool& s : spools) {
        if (choosable && choosable(s))
            m_spools.push_back(std::move(s));
        else
            greyed.push_back(std::move(s));
    }
    m_ok.assign(m_spools.size(), true);
    for (fil::Spool& s : greyed) {
        m_spools.push_back(std::move(s));
        m_ok.push_back(false);
    }

    SetBackgroundColour(*wxWHITE);
    const int em = wxGetApp().em_unit();

    m_search = new TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(520), -1), 0);
    m_search->GetTextCtrl()->SetHint(_L("Search by brand, name, type or note"));

    m_help = new Label(this, wxEmptyString);
    m_help->SetFont(Label::Body_13);
    m_help->SetForegroundColour(wxColour(107, 107, 107));

    m_list = new wxListView(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SIMPLE);
    m_list->SetMinSize(wxSize(FromDIP(520), FromDIP(300)));
    m_list->AppendColumn(_L("Spool"), wxLIST_FORMAT_LEFT, FromDIP(250));
    m_list->AppendColumn(_L("Remaining"), wxLIST_FORMAT_LEFT, FromDIP(110));
    m_list->AppendColumn(_L("Note"), wxLIST_FORMAT_LEFT, FromDIP(150));
    const int size   = FromDIP(16);
    auto*     images = new wxImageList(size, size, false, static_cast<int>(m_spools.size()));
    for (const fil::Spool& s : m_spools)
        images->Add(swatch(spool_colours(s), size));
    m_list->AssignImageList(images, wxIMAGE_LIST_SMALL);

    m_buttons = new DialogButtons(this, {"OK", "Cancel"});
    m_buttons->GetOK()->Disable();

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_search, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);
    sizer->Add(m_help, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, em);
    sizer->Add(m_list, 1, wxEXPAND | wxALL, em);
    sizer->Add(m_buttons, 0, wxEXPAND);

    m_search->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent& e) {
        fill();
        e.Skip();
    });
    m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent& e) { m_buttons->GetOK()->Enable(row_choosable(e.GetIndex())); });
    m_list->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent&) { m_buttons->GetOK()->Disable(); });
    m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent& e) { pick(e.GetIndex()); });
    m_list->Bind(wxEVT_MOTION, [this](wxMouseEvent& e) {
        int        flags = 0;
        const long row   = m_list->HitTest(e.GetPosition(), flags);
        const bool tip   = row >= 0 && !row_choosable(row);
        if (tip != m_tip_shown) {
            m_tip_shown = tip;
            if (tip)
                m_list->SetToolTip(_L("This filament has no preset for this printer."));
            else
                m_list->UnsetToolTip();
        }
        e.Skip();
    });
    m_buttons->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { pick(m_list->GetFirstSelected()); });
    m_buttons->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });

    fill();
    SetSizerAndFit(sizer);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void SpoolPicker::fill()
{
    const wxString query = m_search->GetTextCtrl()->GetValue().Strip(wxString::both).Lower();
    const auto     matches = [&query](const fil::Spool& s) {
        if (query.empty())
            return true;
        for (const std::string* field : {&s.vendor, &s.name, &s.type, &s.note})
            if (utf8(*field).Lower().Find(query) != wxNOT_FOUND)
                return true;
        return false;
    };

    m_list->Freeze();
    m_list->DeleteAllItems();
    m_rows.clear();
    for (std::size_t i = 0; i < m_spools.size(); ++i) {
        const fil::Spool& s = m_spools[i];
        if (!matches(s))
            continue;
        const long row = m_list->InsertItem(static_cast<long>(m_rows.size()), display_name(s), static_cast<int>(i));
        // TRN The remaining and the total weight of a spool, in grams.
        m_list->SetItem(row, 1, wxString::Format(_L("%d of %d g"), static_cast<int>(fil::bounded_grams(s.net_weight)),
                                                 static_cast<int>(fil::bounded_grams(s.total_net_weight))));
        m_list->SetItem(row, 2, utf8(s.note));
        if (!m_ok[i])
            m_list->SetItemTextColour(row, wxColour(172, 172, 172));
        m_rows.push_back(i);
    }
    m_list->Thaw();
    m_buttons->GetOK()->Disable();

    if (m_spools.empty())
        m_help->SetLabel(_L("No spool can be chosen: spools with an RFID tag, spools in a printer and spools with a change waiting are not listed."));
    else if (m_rows.empty())
        m_help->SetLabel(_L("No spool matches the search."));
    m_help->Show(m_rows.empty());
    Layout();
}

bool SpoolPicker::row_choosable(long row) const
{
    return row >= 0 && static_cast<std::size_t>(row) < m_rows.size() && m_ok[m_rows[static_cast<std::size_t>(row)]];
}

void SpoolPicker::pick(long row)
{
    if (!row_choosable(row))
        return;
    m_chosen = m_spools[m_rows[static_cast<std::size_t>(row)]].id;
    EndModal(wxID_OK);
}

void SpoolPicker::on_dpi_changed(const wxRect&)
{
    Layout();
    Fit();
    Refresh();
}

} // namespace Slic3r::GUI::Fm
