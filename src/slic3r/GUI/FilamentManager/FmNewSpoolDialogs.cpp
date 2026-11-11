// Ported from Bambu Studio, src/slic3r/GUI/AMSNewFilamentDialogs.cpp (tag v02.09.01.70), GNU AGPL v3,
// copyright Bambu Lab; adapted for OrcaSlicer: the spools are BblFilament::Spool rows, the cloud's queue entry is a
// BblFilament::SoftMatchEntry, spool ids are strings, colour names come from the Filament Manager's colour table, and
// the buttons use OrcaSlicer's button styles.

#include "FmNewSpoolDialogs.hpp"

#include "FmRuntime.hpp"
#include "FmSpoolPicker.hpp"
#include "FmViewModel.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/Widgets/Button.hpp"
#include "slic3r/GUI/Widgets/ComboBox.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"
#include "slic3r/GUI/Widgets/StateColor.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

#include <nlohmann/json.hpp>

#include <wx/bitmap.h>
#include <wx/brush.h>
#include <wx/control.h>
#include <wx/dcmemory.h>
#include <wx/graphics.h>
#include <wx/pen.h>
#include <wx/sizer.h>
#include <wx/statbmp.h>
#include <wx/stattext.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI::Fm {

namespace fil = BblFilament;

namespace {

wxString utf8(const std::string& s) { return wxString::FromUTF8(s.c_str()); }

wxString color_name_of(const fil::Spool& sp)
{
    nlohmann::json page = fil::spool_to_page(sp);
    decorate_color_name(page, Runtime::get().colors(), Runtime::get().lang());
    auto it = page.find("color_name");
    return it != page.end() && it->is_string() ? utf8(it->get<std::string>()) : wxString();
}

// Studio's spool_display_name, with the colour name of the colour table added after the name.
wxString spool_display_name(const fil::Spool& sp)
{
    const wxString color = color_name_of(sp);
    wxString       name_part;
    if (!sp.name.empty())
        name_part = utf8(sp.name);
    else if (!sp.type.empty())
        name_part = utf8(sp.type);
    else if (!color.empty())
        name_part = color;
    else
        name_part = _L("Filament");
    if (!color.empty() && name_part != color)
        name_part += ", " + color;
    if (!sp.vendor.empty())
        return utf8(sp.vendor) + " " + name_part;
    return name_part;
}

int remain_grams(const fil::Spool& sp) { return static_cast<int>(fil::bounded_grams(sp.net_weight)); }

wxBitmap make_spool_color_chip(wxWindow* ctx, const fil::Spool& sp)
{
    const int    size_px = ctx ? ctx->FromDIP(36) : 36;
    const double radius  = std::max(2.0, std::round(size_px / 6.0));

    wxBitmap bmp(size_px, size_px);
#if defined(__WXMSW__) || defined(__WXOSX__)
    bmp.UseAlpha();
#endif
    wxMemoryDC dc(bmp);
    dc.SetBackground(*wxTRANSPARENT_BRUSH);
    dc.Clear();

    wxGraphicsContext* gc = wxGraphicsContext::Create(dc);
    if (!gc) {
        dc.SelectObject(wxNullBitmap);
        return bmp;
    }
    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    wxGraphicsPath chip_path = gc->CreatePath();
    chip_path.AddRoundedRectangle(0, 0, size_px, size_px, radius);

    const std::vector<wxColour> colors        = spool_colours(sp);
    const bool                  is_multicolor = (sp.color_type == 1) && colors.size() > 1;
    const bool                  is_gradient   = (sp.color_type == 0) && colors.size() > 1;

    if (is_multicolor) {
        const int n          = static_cast<int>(colors.size());
        const int base_strip = size_px / n;
        int       x          = 0;
        for (int i = 0; i < n; ++i) {
            const int w = (i == n - 1) ? (size_px - base_strip * (n - 1)) : base_strip;
            gc->PushState();
            gc->Clip(x, 0, w, size_px);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(colors[i]));
            gc->FillPath(chip_path);
            gc->ResetClip();
            gc->PopState();
            x += w;
        }
    } else if (is_gradient) {
        const int n = static_cast<int>(colors.size());
        if (n == 2) {
            gc->PushState();
            gc->Clip(0, 0, size_px, size_px);
            wxGraphicsBrush gb = gc->CreateLinearGradientBrush(0, 0, size_px, 0, colors[0], colors[1]);
            gc->SetBrush(gb);
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->FillPath(chip_path);
            gc->ResetClip();
            gc->PopState();
        } else {
            const double seg_w = static_cast<double>(size_px) / (n - 1);
            for (int i = 0; i < n - 1; ++i) {
                const double x0 = i * seg_w;
                const double x1 = (i == n - 2) ? size_px : (i + 1) * seg_w;
                gc->PushState();
                gc->Clip(x0, 0, x1 - x0, size_px);
                wxGraphicsBrush gb = gc->CreateLinearGradientBrush(x0, 0, x1, 0, colors[i], colors[i + 1]);
                gc->SetBrush(gb);
                gc->SetPen(*wxTRANSPARENT_PEN);
                gc->FillPath(chip_path);
                gc->ResetClip();
                gc->PopState();
            }
        }
    } else {
        const wxColour fill = colors.empty() ? wxColour(0x88, 0x88, 0x88) : colors.front();
        gc->SetBrush(wxBrush(fill));
        gc->SetPen(*wxTRANSPARENT_PEN);
        gc->FillPath(chip_path);
    }

    // Inset 1px ring (stroked on slightly inset path so it stays inside).
    const bool     dark       = wxGetApp().dark_mode();
    const wxColour ring_color = dark ? wxColour(255, 255, 255, 26) : wxColour(0, 0, 0, 46);
    wxGraphicsPath ring_path  = gc->CreatePath();
    ring_path.AddRoundedRectangle(0.5, 0.5, size_px - 1.0, size_px - 1.0, radius);
    gc->SetBrush(*wxTRANSPARENT_BRUSH);
    gc->SetPen(wxPen(ring_color, 1));
    gc->StrokePath(ring_path);

    delete gc;
    dc.SelectObject(wxNullBitmap);
    return bmp;
}

wxString spool_note_line(const fil::Spool& sp)
{
    wxString            note     = sp.note.empty() ? wxString("--") : utf8(sp.note);
    constexpr size_t    kMaxNote = 30;
    if (note.length() > kMaxNote)
        note = note.SubString(0, kMaxNote - 3) + "...";
    // TRN The note of a spool of the Filament Manager.
    return wxString::Format(_L("Remark: %s"), note);
}

// A candidate row of the list: colour chip, name and remaining grams, then the note.
wxBitmap render_spool_row_bitmap(wxWindow* ctx, const fil::Spool& sp, int width_px, bool dimmed = false, bool* note_truncated = nullptr)
{
    const int pad_x     = ctx->FromDIP(8);
    const int pad_y     = ctx->FromDIP(6);
    const int icon_px   = ctx->FromDIP(36);
    const int gap       = ctx->FromDIP(8);
    const int row_h     = ctx->FromDIP(52);
    const int line_h    = ctx->FromDIP(20);
    const int right_pad = ctx->FromDIP(12);

    const int total_w = std::max(width_px, ctx->FromDIP(260));
    const int total_h = row_h;

    wxBitmap bmp(total_w, total_h);
#ifdef __WXOSX__
    bmp.UseAlpha();
#endif
    wxMemoryDC dc(bmp);
    const bool dark_row = wxGetApp().dark_mode();
    dc.SetBackground(wxBrush(dark_row ? wxColour(0x2D, 0x2D, 0x31) : *wxWHITE));
    dc.Clear();
    dc.SetPen(*wxTRANSPARENT_PEN);

    wxBitmap icon = make_spool_color_chip(ctx, sp);
    if (icon.IsOk())
        dc.DrawBitmap(icon, pad_x, (total_h - icon_px) / 2, true);

    const int text_x     = pad_x + icon_px + gap;
    const int weight_col = ctx->FromDIP(60);
    const int text_max_w = std::max(total_w - right_pad - weight_col - text_x, ctx->FromDIP(40));

    const wxString weight_str = wxString::Format("%dg", remain_grams(sp));

    wxFont name_font = ::Label::Body_14;
    wxFont note_font = ::Label::Body_14;
    note_font.SetPointSize(std::max(8, note_font.GetPointSize() - 2));

    const int top_y    = pad_y;
    const int bottom_y = pad_y + line_h;

    // Name
    dc.SetFont(name_font);
    dc.SetTextForeground(dimmed ? wxColour(0xB0, 0xB0, 0xB0) : (dark_row ? wxColour(0xE5, 0xE5, 0xE6) : wxColour(0x26, 0x2E, 0x30)));
    wxString name = spool_display_name(sp);
    if (dc.GetTextExtent(name).GetWidth() > text_max_w)
        name = wxControl::Ellipsize(name, dc, wxELLIPSIZE_END, text_max_w);
    dc.DrawText(name, text_x, top_y);

    // Weight (right-aligned, same font as name)
    const wxSize w_sz = dc.GetTextExtent(weight_str);
    dc.DrawText(weight_str, total_w - right_pad - w_sz.GetWidth(), top_y);

    // Note
    dc.SetFont(note_font);
    dc.SetTextForeground(dimmed ? wxColour(0xC0, 0xC0, 0xC0) : wxColour(0x90, 0x90, 0x90));
    constexpr size_t kNoteHardCap      = 30;
    const bool       note_hardcap_cut  = !sp.note.empty() && utf8(sp.note).length() > kNoteHardCap;
    wxString         note              = spool_note_line(sp);
    const int        note_max_w        = std::max(total_w - text_x - right_pad, ctx->FromDIP(40));
    const bool       is_note_truncated = note_hardcap_cut || dc.GetTextExtent(note).GetWidth() > note_max_w;
    if (dc.GetTextExtent(note).GetWidth() > note_max_w)
        note = wxControl::Ellipsize(note, dc, wxELLIPSIZE_END, note_max_w);
    if (note_truncated)
        *note_truncated = is_note_truncated;
    dc.DrawText(note, text_x, bottom_y);

    if (dimmed) {
        if (wxGraphicsContext* gc = wxGraphicsContext::Create(dc)) {
            gc->SetPen(*wxTRANSPARENT_PEN);
            gc->SetBrush(wxBrush(dark_row ? wxColour(0, 0, 0, 100) : wxColour(255, 255, 255, 140)));
            gc->DrawRectangle(0, 0, total_w, total_h);
            delete gc;
        }
    }

    dc.SelectObject(wxNullBitmap);
    return bmp;
}

// Single-line filament card: colour chip + name + right-aligned weight + rounded border. Shared by the recorded
// dialog, the only-hit card and the closed box of the candidate list.
//   width_px    : -1 => default FromDIP(280); otherwise the target pixel width.
//   with_border : draw the rounded outer border (off when the combo frames it).
//   with_chevron: paint a down-chevron on the right (collapsed combo display).
//   gray_bg     : fill the background with the combo's light grey; off => white / dark card background.
wxBitmap make_filament_card_bitmap(wxWindow* ctx, const fil::Spool& sp, int width_px = -1, bool with_border = true, bool with_chevron = false,
                                   bool gray_bg = false)
{
    const int pad_x           = ctx->FromDIP(8);
    const int icon_px         = ctx->FromDIP(36);
    const int gap             = ctx->FromDIP(8);
    const int row_h           = ctx->FromDIP(48);
    const int right_pad       = ctx->FromDIP(12);
    const int total_w         = (width_px > 0) ? std::max(width_px, ctx->FromDIP(120)) : std::max(ctx->FromDIP(280), ctx->FromDIP(240));
    const int total_h         = row_h;
    const int chevron_reserve = with_chevron ? ctx->FromDIP(20) : 0;

    wxBitmap bmp(total_w, total_h);
#ifdef __WXOSX__
    bmp.UseAlpha();
#endif
    wxMemoryDC     dc(bmp);
    const bool     dark_mode = wxGetApp().dark_mode();
    const wxColour bg        = gray_bg ? (dark_mode ? wxColour(0x36, 0x36, 0x3C) : wxColour(0xF8, 0xF8, 0xF8)) :
                                         (dark_mode ? wxColour(0x2D, 0x2D, 0x31) : *wxWHITE);
    dc.SetBackground(wxBrush(bg));
    dc.Clear();
    dc.SetPen(*wxTRANSPARENT_PEN);

    // Colour chip
    wxBitmap chip = make_spool_color_chip(ctx, sp);
    if (chip.IsOk())
        dc.DrawBitmap(chip, pad_x, (total_h - icon_px) / 2, true);

    const int text_x     = pad_x + icon_px + gap;
    const int weight_col = ctx->FromDIP(60);
    const int text_max_w = std::max(total_w - right_pad - chevron_reserve - weight_col - text_x, ctx->FromDIP(40));

    const wxString weight_str = wxString::Format("%dg", remain_grams(sp));

    // Name (vertically centered, left)
    wxFont name_font = ::Label::Body_14;
    dc.SetFont(name_font);
    dc.SetTextForeground(dark_mode ? wxColour(0xE5, 0xE5, 0xE6) : wxColour(0x26, 0x2E, 0x30));
    wxString name = spool_display_name(sp);
    if (dc.GetTextExtent(name).GetWidth() > text_max_w)
        name = wxControl::Ellipsize(name, dc, wxELLIPSIZE_END, text_max_w);
    const int text_h = dc.GetTextExtent(name).GetHeight();
    const int top_y  = (total_h - text_h) / 2;
    dc.DrawText(name, text_x, top_y);

    // Weight (vertically centered, right-aligned, left of the chevron reserve)
    const wxSize w_sz = dc.GetTextExtent(weight_str);
    dc.DrawText(weight_str, total_w - right_pad - chevron_reserve - w_sz.GetWidth(), top_y);

    if (with_border || with_chevron) {
        if (wxGraphicsContext* gc = wxGraphicsContext::Create(dc)) {
            const int pen_w = std::max(1, ctx->FromDIP(1));
            // Rounded border, inset by 0.5px so the stroke stays inside the bitmap
            if (with_border) {
                gc->SetPen(wxPen(dark_mode ? wxColour(0x50, 0x50, 0x54) : wxColour(0xD0, 0xD0, 0xD0), pen_w));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                gc->DrawRoundedRectangle(0.5, 0.5, total_w - 1.0, total_h - 1.0, ctx->FromDIP(6));
            }
            // Down-chevron on the right
            if (with_chevron) {
                gc->SetPen(wxPen(dark_mode ? wxColour(0xB0, 0xB0, 0xB2) : wxColour(0x6B, 0x6B, 0x6B), pen_w));
                gc->SetBrush(*wxTRANSPARENT_BRUSH);
                const double   cw   = ctx->FromDIP(10);
                const double   chh  = ctx->FromDIP(3);
                const double   cx   = total_w - right_pad - cw;
                const double   cy   = total_h / 2.0;
                wxGraphicsPath path = gc->CreatePath();
                path.MoveToPoint(cx, cy - chh);
                path.AddLineToPoint(cx + cw / 2.0, cy + chh);
                path.AddLineToPoint(cx + cw, cy - chh);
                gc->StrokePath(path);
            }
            delete gc;
        }
    }

    dc.SelectObject(wxNullBitmap);
    return bmp;
}

wxSize combo_size(wxWindow* w) { return wxSize(w->FromDIP(360), w->FromDIP(56)); }

} // namespace

// NewOfficialSpoolDialog

NewOfficialSpoolDialog::NewOfficialSpoolDialog(wxWindow* parent, std::optional<fil::SoftMatchEntry> entry)
    : DPIDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX), m_entry(std::move(entry))
{
    create();
    populate_link_combo();
    wxGetApp().UpdateDlgDarkUI(this);
}

void NewOfficialSpoolDialog::create()
{
    SetBackgroundColour(*wxWHITE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Title row: hint icon + bold title (text set per case in populate_link_combo())
    auto* title_row = new wxBoxSizer(wxHORIZONTAL);
    auto* info_bmp  = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("ams_filament_hint", this, 16), wxDefaultPosition,
                                         wxSize(FromDIP(16), FromDIP(16)));
    m_title         = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    m_title->SetFont(::Label::Head_16);
    m_title->Wrap(FromDIP(340));
    title_row->Add(info_bmp, 0, wxALIGN_TOP | wxRIGHT, FromDIP(6));
    title_row->Add(m_title, 1, wxEXPAND);
    sizer->Add(title_row, 0, wxALL, FromDIP(12));

    // Only-hit case: a single filament card
    m_hit_card = new wxStaticBitmap(this, wxID_ANY, wxNullBitmap);
    sizer->Add(m_hit_card, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    sizer->Hide(m_hit_card);

    // Hit and candidates case: "Matched filament" label + list
    m_match_label = new wxStaticText(this, wxID_ANY, _L("Matched filament"), wxDefaultPosition, wxDefaultSize, wxALIGN_LEFT);
    m_match_label->SetFont(::Label::Body_13);
    m_match_label->SetForegroundColour(StateColor::darkModeColorFor(wxColour(0x90, 0x90, 0x90)));
    sizer->Add(m_match_label, 0, wxLEFT | wxRIGHT, FromDIP(12));
    sizer->AddSpacer(FromDIP(6));
    sizer->Hide(m_match_label);

    // Tall enough for the closed box to show a 48 px filament card (the closed box does not grow for the icon).
    m_combo_link = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, combo_size(this), 0, nullptr, wxCB_READONLY);
    m_combo_link->SetMinSize(combo_size(this));
    // Light grey rounded box without a visible border (border == fill); one Normal state also drops the read-only
    // combo's hover and focus colours.
    m_combo_link->SetCornerRadius(FromDIP(8));
    StateColor combo_gray(std::make_pair(wxColour(248, 248, 248), (int) StateColor::Normal));
    m_combo_link->SetBackgroundColor(combo_gray);
    m_combo_link->SetBorderColor(combo_gray);
    m_combo_row = new wxBoxSizer(wxHORIZONTAL);
    m_combo_row->Add(m_combo_link, 1, wxEXPAND);
    sizer->Add(m_combo_row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));
    sizer->Hide(m_combo_row, true);

    m_combo_link->Bind(wxEVT_COMBOBOX, &NewOfficialSpoolDialog::on_combo_selected, this);

    // Bottom buttons: [Add as new filament] (secondary)  [Confirm] (primary)
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer();

    m_btn_record_new = new Button(this, _L("Add as new filament"));
    m_btn_record_new->SetStyle(ButtonStyle::Regular, ButtonType::Choice);
    m_btn_record_new->Bind(wxEVT_BUTTON, &NewOfficialSpoolDialog::on_record_new, this);
    btn_sizer->Add(m_btn_record_new, 0, wxRIGHT, FromDIP(12));

    m_btn_confirm = new Button(this, _L("Confirm"));
    m_btn_confirm->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    m_btn_confirm->Bind(wxEVT_BUTTON, &NewOfficialSpoolDialog::on_confirm, this);
    btn_sizer->Add(m_btn_confirm, 0);

    sizer->Add(btn_sizer, 0, wxEXPAND | wxALL, FromDIP(12));

    SetSizerAndFit(sizer);
    Centre();
}

void NewOfficialSpoolDialog::on_record_new(wxCommandEvent&)
{
    m_choice = Choice::RecordNew;
    EndModal(wxID_OK);
}

void NewOfficialSpoolDialog::on_confirm(wxCommandEvent&)
{
    // Confirm = accept the shown hit, or link to the chosen candidate.
    m_choice = Choice::LinkExisting;
    EndModal(wxID_OK);
}

void NewOfficialSpoolDialog::on_combo_selected(wxCommandEvent& evt)
{
    const int sel      = evt.GetSelection();
    const int closed_w = m_combo_link->GetSize().GetWidth() - m_combo_link->FromDIP(16);

    // Clicking the selected candidate again deselects it, and the closed box shows the hit again. The widget has
    // selected `sel` before this handler runs, so the index picked last is compared instead.
    if (sel == m_prev_combo_sel) {
        m_combo_link->SetSelection(-1);
        m_combo_link->SetLabel(wxEmptyString);
        if (m_entry)
            m_combo_link->SetIcon(make_filament_card_bitmap(m_combo_link, m_entry->hit, closed_w, false, true, true));
        m_prev_combo_sel = -1;
        m_selected_candidate_id.clear();
        return;
    }

    // The list holds the candidates only. The closed box shows the chosen one as a card.
    auto sit = m_combo_idx_to_spool.find(sel);
    if (sit != m_combo_idx_to_spool.end()) {
        m_combo_link->SetLabel(wxEmptyString);
        m_combo_link->SetIcon(make_filament_card_bitmap(m_combo_link, sit->second, closed_w, false, true, true));
        m_selected_candidate_id = sit->second.id;
    } else {
        m_selected_candidate_id.clear();
    }
    m_prev_combo_sel = sel;
}

void NewOfficialSpoolDialog::populate_link_combo()
{
    m_combo_link->Clear();
    m_combo_idx_to_spool.clear();
    m_selected_candidate_id.clear();
    m_prev_combo_sel = -1;

    wxSizer* dlg_sizer = GetSizer();

    if (!m_entry) {
        m_hit_spool_id.clear();
        m_only_hit = false;
        m_title->SetLabel(_L("AMS detected a new filament. Add it to Filament Manager?"));
        m_title->Wrap(FromDIP(340));
        if (dlg_sizer) {
            dlg_sizer->Hide(m_hit_card);
            dlg_sizer->Hide(m_match_label);
            dlg_sizer->Hide(m_combo_row, true);
        }
        Layout();
        Fit();
        Centre();
        return;
    }

    const fil::Spool& hit = m_entry->hit;
    m_hit_spool_id        = hit.id;
    m_only_hit            = m_entry->candidates.empty();

    if (m_only_hit) {
        m_title->SetLabel(_L("A similar filament was found in Filament Manager and matched automatically."));
        m_title->Wrap(FromDIP(340));

        m_hit_card->SetBitmap(make_filament_card_bitmap(this, hit));

        if (dlg_sizer) {
            dlg_sizer->Show(m_hit_card);
            dlg_sizer->Hide(m_match_label);
            dlg_sizer->Hide(m_combo_row, true);
        }
    } else {
        m_title->SetLabel(_L("Multiple similar filaments were found in Filament Manager. Please confirm the match."));
        m_title->Wrap(FromDIP(340));

        // The list keeps about 30 px on the left of each row for the check mark.
        const int check_reserve = m_combo_link->FromDIP(30);
        const int row_width     = std::max(m_combo_link->GetSize().GetWidth() - check_reserve, m_combo_link->FromDIP(230));

        for (const fil::Spool& cand : m_entry->candidates) {
            bool           note_truncated = false;
            const wxBitmap row_bmp        = render_spool_row_bitmap(m_combo_link, cand, row_width, false, &note_truncated);
            const int      idx            = m_combo_link->Append(wxEmptyString, row_bmp);
            if (idx < 0)
                continue;
            m_combo_idx_to_spool[idx] = cand;
            if (note_truncated && !cand.note.empty())
                // TRN The note of a spool of the Filament Manager.
                m_combo_link->SetItemTooltip(static_cast<unsigned int>(idx), wxString::Format(_L("Remark: %s"), utf8(cand.note)));
        }

        const int closed_w = m_combo_link->GetSize().GetWidth() - m_combo_link->FromDIP(16);
        m_combo_link->SetLabel(wxEmptyString);
        m_combo_link->SetIcon(make_filament_card_bitmap(m_combo_link, hit, closed_w, false, true, true));
        m_combo_link->Enable(true);

        if (dlg_sizer) {
            dlg_sizer->Hide(m_hit_card);
            dlg_sizer->Show(m_match_label);
            dlg_sizer->Show(m_combo_row, true);
        }
    }

    Layout();
    Fit();
    Centre();
}

void NewOfficialSpoolDialog::on_dpi_changed(const wxRect&)
{
    m_btn_confirm->Rescale();
    m_btn_record_new->Rescale();
    m_combo_link->SetMinSize(combo_size(this));
    // The cards are plain bitmaps: draw them again at the new DPI.
    populate_link_combo();
    Fit();
}

// SpoolRecordedDialog

SpoolRecordedDialog::SpoolRecordedDialog(wxWindow* parent, const fil::Spool& spool)
    : DPIDialog(parent, wxID_ANY, _L("New Filament"), wxDefaultPosition, wxDefaultSize, wxCAPTION | wxCLOSE_BOX)
{
    create(spool);
    wxGetApp().UpdateDlgDarkUI(this);
}

void SpoolRecordedDialog::create(const fil::Spool& spool)
{
    SetBackgroundColour(*wxWHITE);
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    // Title row: hint icon + bold info text
    auto* title_row = new wxBoxSizer(wxHORIZONTAL);
    auto* hint_bmp  = new wxStaticBitmap(this, wxID_ANY, create_scaled_bitmap("ams_filament_hint", this, 16), wxDefaultPosition,
                                         wxSize(FromDIP(16), FromDIP(16)));
    auto* label     = new wxStaticText(this, wxID_ANY, _L("New filament information has been recorded."), wxDefaultPosition, wxDefaultSize,
                                       wxALIGN_LEFT);
    label->SetFont(::Label::Head_16);
    label->Wrap(FromDIP(340));
    title_row->Add(hint_bmp, 0, wxALIGN_TOP | wxRIGHT, FromDIP(6));
    title_row->Add(label, 1, wxEXPAND);
    sizer->Add(title_row, 0, wxALL, FromDIP(12));

    // Card: colour chip, name and weight
    auto* bmp_ctrl = new wxStaticBitmap(this, wxID_ANY, make_filament_card_bitmap(this, spool));
    sizer->Add(bmp_ctrl, 0, wxALIGN_CENTER_HORIZONTAL | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    // OK button
    auto* btn_sizer = new wxBoxSizer(wxHORIZONTAL);
    btn_sizer->AddStretchSpacer();
    auto* btn_ok = new Button(this, _L("OK"));
    btn_ok->SetStyle(ButtonStyle::Confirm, ButtonType::Choice);
    btn_ok->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_OK); });
    btn_sizer->Add(btn_ok, 0);
    sizer->Add(btn_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(12));

    SetSizerAndFit(sizer);
    Centre();
}

void SpoolRecordedDialog::on_dpi_changed(const wxRect&) { Fit(); }

} // namespace Slic3r::GUI::Fm
