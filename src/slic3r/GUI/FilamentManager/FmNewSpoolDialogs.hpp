// Ported from Bambu Studio, src/slic3r/GUI/AMSNewFilamentDialogs.hpp (tag v02.09.01.70), GNU AGPL v3,
// copyright Bambu Lab; adapted for OrcaSlicer.

#pragma once

#include "slic3r/GUI/GUI_Utils.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"

#include <wx/gdicmn.h>

#include <map>
#include <optional>
#include <string>

class Button;
class ComboBox;
class wxCommandEvent;
class wxSizer;
class wxStaticBitmap;
class wxStaticText;

namespace Slic3r::GUI::Fm {

// A new official spool the cloud may have matched to spools of the account: add it as a new spool, or confirm the
// match (the proposed spool, or one of the candidates chosen in the list).
class NewOfficialSpoolDialog : public DPIDialog
{
public:
    enum class Choice { RecordNew, LinkExisting, Skip };

    // `entry` empty: the cloud has nothing queued for this spool.
    NewOfficialSpoolDialog(wxWindow* parent, std::optional<BblFilament::SoftMatchEntry> entry);

    Choice             choice() const { return m_choice; }
    const std::string& hit_spool_id() const { return m_hit_spool_id; }                   // empty without an entry
    const std::string& selected_candidate_id() const { return m_selected_candidate_id; } // empty when none is chosen

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void create();
    void populate_link_combo();
    void on_combo_selected(wxCommandEvent& evt);
    void on_confirm(wxCommandEvent& evt);
    void on_record_new(wxCommandEvent& evt);

    Choice          m_choice{Choice::RecordNew};
    wxStaticText*   m_title{nullptr};
    wxStaticBitmap* m_hit_card{nullptr};
    wxStaticText*   m_match_label{nullptr};
    Button*         m_btn_record_new{nullptr};
    Button*         m_btn_confirm{nullptr};

    ::ComboBox*                       m_combo_link{nullptr};
    wxSizer*                          m_combo_row{nullptr};
    std::map<int, BblFilament::Spool> m_combo_idx_to_spool;

    std::optional<BblFilament::SoftMatchEntry> m_entry;
    int                                        m_prev_combo_sel{-1}; // the last candidate picked (-1 = none), for the re-click toggle
    std::string                                m_hit_spool_id;
    std::string                                m_selected_candidate_id;
    bool                                       m_only_hit{false};
};

// The cloud recorded a new official spool by itself.
class SpoolRecordedDialog : public DPIDialog
{
public:
    SpoolRecordedDialog(wxWindow* parent, const BblFilament::Spool& spool);

protected:
    void on_dpi_changed(const wxRect& suggested_rect) override;

private:
    void create(const BblFilament::Spool& spool);
};

} // namespace Slic3r::GUI::Fm
