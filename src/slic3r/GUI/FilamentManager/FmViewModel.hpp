#pragma once

#include "FmColorCodes.hpp"
#include "FmProtocol.hpp"
#include "slic3r/Utils/BblFilament/SpoolService.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace Slic3r::GUI::Fm {

using json = nlohmann::json;

// What the view-model needs from the app; any member may be unset.
struct Host
{
    std::function<bool()>                                                dark;
    std::function<json()>                                                presets;  // preset_tree(...)
    std::function<json()>                                                machines; // machine_list(...)
    std::function<json(const std::string& dev_id, bool switch_selected)> ams;      // select_machine + ams_data
    std::function<bool(const std::string& dev_id, std::string& used)>    request_pushall;
    std::function<json(const std::string& fila_id)>                      colors; // see colors_response
    std::function<void(const std::string& vendor, const std::string& client_request_id)> create_custom;
    std::function<void(json& page_spool)>                                decorate_spool; // fills color_name, tray_label
    std::function<std::string(BblFilament::FailKind, const std::string& server_message)> fail_text;
    std::function<std::uint64_t()>                                       now_ms;
};

// Answers the page's requests and turns the spool service's events into reports.
class ViewModel
{
public:
    ViewModel(BblFilament::SpoolService& service, Host host, std::function<void(const json& body)> send_report);
    ~ViewModel() = default;

    ViewModel(const ViewModel&)            = delete;
    ViewModel& operator=(const ViewModel&) = delete;

    // The handlers that turn service events into reports. The owner of service.events installs them (see Runtime).
    BblFilament::ServiceEvents event_handlers();

    json handle(const Request& request); // the response body; never throws
    void custom_created(const std::string& client_request_id, bool ok, const json& created);
    void machine_selected_changed();

    json spools_json() const;
    json state_json() const;

private:
    json  dispatch(const Request& request);
    json  refused(const Request& request, const BblFilament::Accept& accept) const;
    json  answer(const Request& request, int code, const std::string& message, json payload) const;
    int   fail_code(BblFilament::FailKind kind) const;
    std::string fail_message(BblFilament::FailKind kind, const std::string& server_message) const;
    json  presets_or_empty() const;
    void  report(const std::string& module, const std::string& submod, const std::string& action, json payload);
    void  note_list_state();

    BblFilament::SpoolService&         m_service;
    Host                               m_host;
    std::function<void(const json&)>   m_send;
    bool                               m_list_seen{false};
};

// {"fila_id","fila_type","candidates":[{"color_code","name","color_type","colors":[...]}]} for the page's colour picker.
json colors_response(const ColorCodes& colors, const std::string& fila_id, const std::string& lang);
// Fills an empty "color_name" of a page spool from the colour table.
void decorate_color_name(json& page_spool, const ColorCodes& colors, const std::string& lang);
// Fills "tray_label" of a page spool that is in a printer.
void decorate_tray_label(json& page_spool);

} // namespace Slic3r::GUI::Fm
