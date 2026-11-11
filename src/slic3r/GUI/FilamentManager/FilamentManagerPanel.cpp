#include "FilamentManagerPanel.hpp"

#include "FmDevices.hpp"
#include "FmPresets.hpp"
#include "FmProtocol.hpp"
#include "FmRuntime.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/CreatePresetsDialog.hpp"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/Utils/BBLPrinterAgent.hpp"

#include <boost/filesystem/path.hpp>
#include <boost/log/trivial.hpp>

#include <wx/app.h>
#include <wx/event.h>
#include <wx/sizer.h>
#include <wx/uri.h>
#include <wx/utils.h>
#include <wx/webview.h>

#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI {

namespace fil = BblFilament;
using Fm::json;

namespace {

std::string to_utf8(const wxString& s) { return std::string(s.ToUTF8().data()); }

std::set<std::string> user_filament_names()
{
    std::set<std::string> names;
    if (wxGetApp().preset_bundle == nullptr)
        return names;
    for (const Preset& p : wxGetApp().preset_bundle->filaments)
        if (!p.is_system && !p.is_default)
            names.insert(p.name);
    return names;
}

} // namespace

FilamentManagerPanel::FilamentManagerPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize), m_timer(this), m_alive(std::make_shared<int>(0))
{
    SetSizer(new wxBoxSizer(wxVERTICAL));
    create_browser();
    m_reset_on_show = WebView::NeedsRecreateOnShow();
    GetSizer()->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    Fm::Runtime& runtime = Fm::Runtime::get();
    m_vm = std::make_unique<Fm::ViewModel>(runtime.service(), make_host(), [this](const json& body) { push("report", m_report_seq++, body); });
    runtime.attach(m_vm.get());

    Bind(wxEVT_TIMER, &FilamentManagerPanel::on_timer, this);
}

FilamentManagerPanel::~FilamentManagerPanel()
{
    m_timer.Stop();
    m_alive.reset();
    Fm::Runtime::get().detach(m_vm.get());
    m_vm.reset();
    SetEvtHandlerEnabled(false);
    if (m_browser != nullptr) {
        m_browser->Destroy();
        m_browser = nullptr;
    }
}

void FilamentManagerPanel::create_browser()
{
    m_browser = WebView::CreateWebView(this, "");
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &FilamentManagerPanel::on_script_message, this);
    m_browser->Bind(wxEVT_WEBVIEW_NAVIGATING, &FilamentManagerPanel::on_navigating, this);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &FilamentManagerPanel::on_loaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_NEWWINDOW, &FilamentManagerPanel::on_new_window, this);
    m_browser->Bind(EVT_WEBVIEW_RECREATED, &FilamentManagerPanel::on_recreated, this);
}

void FilamentManagerPanel::reset_browser()
{
    m_browser->Destroy(); // also removes it from the sizer
    create_browser();
    GetSizer()->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    Layout();
    m_loaded         = false;
    m_navigated_once = false;
}

bool FilamentManagerPanel::Show(bool show)
{
    if (show && std::exchange(m_reset_on_show, false))
        reset_browser();
    const bool changed = wxPanel::Show(show);
    if (show) {
        if (changed || !m_navigated_once)
            navigate();
        if (!m_timer.IsRunning())
            m_timer.Start(2000);
    } else {
        m_timer.Stop();
#ifdef __APPLE__
        // A hidden WKWebView can starve the app's idle events.
        if (m_browser != nullptr && m_navigated_once)
            WebView::LoadUrl(m_browser, "about:blank");
        m_loaded = false;
#endif
    }
    return changed;
}

void FilamentManagerPanel::navigate()
{
    if (m_browser == nullptr)
        return;
    ++m_generation;
    m_loaded         = false;
    m_navigated_once = true;
    const boost::filesystem::path index = boost::filesystem::path(resources_dir()) / "web" / "filament_manager" / "index.html";
    const std::string url = Fm::page_url(to_utf8(file_url_from_path(index)), Fm::Runtime::get().lang(), wxGetApp().dark_mode(), std::to_string(m_generation));
    WebView::LoadUrl(m_browser, wxString::FromUTF8(url));
}

void FilamentManagerPanel::on_loaded(wxWebViewEvent& evt)
{
    const wxString url = evt.GetURL();
    if (!url.IsEmpty() && url != "about:blank")
        m_loaded = true;
    evt.Skip();
}

void FilamentManagerPanel::on_new_window(wxWebViewEvent& evt)
{
    const wxString url = evt.GetURL();
    if (!url.empty())
        wxLaunchDefaultBrowser(url);
    evt.Veto();
}

void FilamentManagerPanel::on_recreated(wxCommandEvent&)
{
    // Not skipped: that tells WebView::RecreateAll() not to reload the old URL itself.
    if (IsShownOnScreen())
        navigate();
}

void FilamentManagerPanel::on_script_message(wxWebViewEvent& evt)
{
    handle_raw(to_utf8(evt.GetString()), to_utf8(evt.GetURL()), true);
}

void FilamentManagerPanel::on_navigating(wxWebViewEvent& evt)
{
    const wxString url = evt.GetURL();
    if (url.StartsWith("http://") || url.StartsWith("https://")) {
        wxLaunchDefaultBrowser(url);
        evt.Veto();
        return;
    }
    if (url.StartsWith("app://")) {
        // The page's fallback transport: the packet is the rest of the URL.
        handle_raw(to_utf8(wxURI::Unescape(url.Mid(6))), std::string(), false);
        evt.Veto();
        return;
    }
    evt.Skip();
}

void FilamentManagerPanel::handle_raw(const std::string& raw, const std::string& url, bool use_url_generation)
{
    if (!m_vm)
        return;
    if (use_url_generation && !url.empty()) {
        // Only our page, and only the current document, may talk to the bridge.
        const std::string plain = to_utf8(wxURI::Unescape(wxString::FromUTF8(url)));
        if (!Fm::is_page_url(plain))
            return;
        const std::optional<std::string> gen = Fm::generation_of_url(plain);
        if (!gen || *gen != std::to_string(m_generation))
            return; // a message of a document that is gone
    }
    const std::optional<Fm::Request> request = Fm::parse_request(raw);
    if (!request || request->module == "device_host")
        return;
    push("response", request->seq, m_vm->handle(*request));
}

void FilamentManagerPanel::push(const std::string& type, std::uint64_t seq, const json& body)
{
    if (!m_vm || m_browser == nullptr)
        return;
    // A report goes only to a page that is on screen and loaded; the next show loads a fresh page that asks for everything.
    if (type == "report" && !(IsShown() && m_loaded))
        return;
    const std::uint64_t now_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    // RunScript logs the whole script in developer mode, and packets carry spool lists: use the silent runner.
    const std::string script = "(function(){" + Fm::push_script(type, seq, now_ms, body, std::to_string(m_generation)) + "return \"\";})()";
    WebView::RunScriptAsync(m_browser, wxString::FromUTF8(script), [](bool, const std::string&) {});
}

void FilamentManagerPanel::on_timer(wxTimerEvent&)
{
    Fm::Runtime& runtime = Fm::Runtime::get();
    if (runtime.service().refresh_session() && runtime.session().signed_in)
        runtime.service().request_pull();
}

Fm::Host FilamentManagerPanel::make_host()
{
    Fm::Host host;
    host.dark = [] { return wxGetApp().dark_mode(); };
    host.presets = [] {
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (bundle == nullptr)
            return json{{"vendors", json::array()}};
        return Fm::preset_tree(bundle->filaments, &BBLPrinterAgent::bambu_filament_id);
    };
    host.machines = [] { return Fm::machine_list(wxGetApp().getDeviceManager()); };
    host.ams = [](const std::string& dev_id, bool switch_selected) {
        DeviceManager*  mgr = wxGetApp().getDeviceManager();
        Fm::TrayContext ctx;
        ctx.colors   = &Fm::Runtime::get().colors();
        ctx.lang     = Fm::Runtime::get().lang();
        ctx.to_bambu = &BBLPrinterAgent::bambu_filament_id;
        if (switch_selected)
            Fm::select_machine(mgr, dev_id);
        return Fm::ams_data(mgr, ctx);
    };
    host.request_pushall = [](const std::string& dev_id, std::string& used) { return Fm::request_pushall(wxGetApp().getDeviceManager(), dev_id, used); };
    host.colors = [](const std::string& fila_id) {
        return Fm::colors_response(Fm::Runtime::get().colors(), fila_id, Fm::Runtime::get().lang());
    };
    host.decorate_spool = [](json& row) {
        Fm::decorate_color_name(row, Fm::Runtime::get().colors(), Fm::Runtime::get().lang());
        Fm::decorate_tray_label(row);
    };
    host.now_ms = [] {
        return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count());
    };
    host.fail_text = [](fil::FailKind kind, const std::string& server_message) {
        std::string text;
        switch (kind) {
        case fil::FailKind::NotSignedIn: text = _u8L("Sign in to your Bambu account to use the Filament Manager."); break;
        case fil::FailKind::Unavailable: text = _u8L("The Bambu network plug-in is not available."); break;
        case fil::FailKind::BadRequest: text = _u8L("The request was not valid."); break;
        case fil::FailKind::NotFound: text = _u8L("This spool no longer exists in the cloud."); break;
        case fil::FailKind::Rejected: text = _u8L("The cloud refused the change."); break;
        default: text = _u8L("No answer from the cloud. The change may or may not have been applied, so the list was refreshed."); break;
        }
        if (!server_message.empty())
            text += " (" + server_message + ")";
        return text;
    };
    host.create_custom = [this](const std::string& vendor, const std::string& client_request_id) { create_custom(vendor, client_request_id); };
    return host;
}

void FilamentManagerPanel::create_custom(const std::string&, const std::string& client_request_id)
{
    // Deferred, so the response to the page goes out before the modal dialog opens.
    std::weak_ptr<int> alive = m_alive;
    wxTheApp->CallAfter([this, alive, client_request_id] {
        if (alive.expired() || !m_vm)
            return;
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (bundle == nullptr) {
            m_vm->custom_created(client_request_id, false, {});
            return;
        }
        const std::set<std::string> before = user_filament_names();
        CreateFilamentPresetDialog  dlg(wxGetApp().mainframe);
        const int                   result = dlg.ShowModal();
        if (alive.expired() || !m_vm)
            return;
        if (result != wxID_OK) {
            m_vm->custom_created(client_request_id, false, {});
            return;
        }
        // What Plater::priv::on_create_filament does after the dialog, without its success dialog.
        wxGetApp().mainframe->update_side_preset_ui();
        wxGetApp().plater()->update_ui_from_settings();
        wxGetApp().plater()->sidebar().update_all_preset_comboboxes();

        PresetCollection& filaments = bundle->filaments;
        for (Preset& preset : filaments) {
            if (preset.is_system || preset.is_default || before.count(preset.name) != 0)
                continue;
            json created = {{"vendor", preset.config.get_filament_vendor()},
                            {"type", preset.config.get_filament_type()},
                            {"name", filaments.get_preset_alias(preset, true)},
                            {"filament_id", BBLPrinterAgent::bambu_filament_id(preset.filament_id)}};
            m_vm->custom_created(client_request_id, true, created);
            return;
        }
        m_vm->custom_created(client_request_id, false, {});
    });
}

} // namespace Slic3r::GUI
