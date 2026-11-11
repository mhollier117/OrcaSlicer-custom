#pragma once

#include "FmViewModel.hpp"

#include <wx/panel.h>
#include <wx/timer.h>

#include <cstdint>
#include <memory>
#include <string>

class wxWebView;
class wxWebViewEvent;
class wxCommandEvent;
class wxTimerEvent;

namespace Slic3r::GUI {

// The Filament Manager tab: one web view showing the page, and the view-model that answers it.
// See FilamentManager/page/BRIDGE.md for the page contract.
class FilamentManagerPanel : public wxPanel
{
public:
    explicit FilamentManagerPanel(wxWindow* parent);
    ~FilamentManagerPanel() override;

    // The activation hook: the tab's show and hide reach it through LazyPage.
    bool Show(bool show = true) override;

private:
    void create_browser();
    void reset_browser();
    void navigate();
    void on_loaded(wxWebViewEvent& evt);
    void on_new_window(wxWebViewEvent& evt);
    void on_navigating(wxWebViewEvent& evt);
    void on_script_message(wxWebViewEvent& evt);
    void on_recreated(wxCommandEvent& evt);
    void on_timer(wxTimerEvent& evt);

    void handle_raw(const std::string& raw, const std::string& url, bool use_url_generation);
    void push(const std::string& type, std::uint64_t seq, const nlohmann::json& body);
    void create_custom(const std::string& vendor, const std::string& client_request_id);
    Fm::Host make_host();

    wxWebView*                  m_browser{nullptr};
    std::unique_ptr<Fm::ViewModel> m_vm;
    unsigned long long          m_generation{0};
    bool                        m_loaded{false};
    bool                        m_navigated_once{false};
    bool                        m_reset_on_show{false};
    std::uint64_t               m_report_seq{0};
    wxTimer                     m_timer;
    bool                        m_last_signed_in{false};
    std::string                 m_last_uid;
    std::shared_ptr<int>        m_alive;
};

} // namespace Slic3r::GUI
