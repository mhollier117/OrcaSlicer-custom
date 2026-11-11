#pragma once

#include "FmColorCodes.hpp"
#include "FmViewModel.hpp"
#include "slic3r/Utils/BblFilament/LinkDriver.hpp"
#include "slic3r/Utils/BblFilament/Linker.hpp"
#include "slic3r/Utils/BblFilament/NewSpools.hpp"
#include "slic3r/Utils/BblFilament/PrinterTap.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"
#include "slic3r/Utils/BblFilament/SpoolService.hpp"

#include <nlohmann/json.hpp>
#include <wx/timer.h>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::GUI::Fm {

using json = nlohmann::json;

// The entries of a soft-match queue as the MCP tool shows them: per entry the spool id (a cloud id, not a secret),
// brand, name, colour and grams left of the hit, and its candidates with the same fields. Never an RFID or a serial.
json soft_match_items(const BblFilament::SoftMatchQueue& queue);

// The app-side owner of the spool service, the printer tap and the linker. UI thread only.
class Runtime
{
public:
    static Runtime& get(); // created on first use and never destroyed (the spool worker outlives every static)

    BblFilament::SpoolService& service();
    BblFilament::PrinterTap&   tap();
    BblFilament::LinkDriver&   linker();
    const ColorCodes&          colors(); // loaded on first use from ColorCodes::default_path()
    BblFilament::Session       session() const;
    std::string                lang() const; // the app's language code, UTF-8

    // Starts the linker's two-second tick. Idempotent.
    void start();
    // printer_views() over the app's device manager and the tap, now.
    std::vector<BblFilament::PrinterView> views();

    // The view-model of the open panel, or none. Service events go to it and to the recent-events log.
    void attach(ViewModel* vm);
    void detach(ViewModel* vm);

    // Slots that hold an official spool registered in this session (for the slot marker).
    BblFilament::NewSpools& new_spools() { return m_new_spools; }

    // Reads the soft-match queue of one AMS; the result shows in soft_match_json().
    BblFilament::Accept request_soft_match(const BblFilament::PrinterView& printer, int ams_id);
    const json&         soft_match_json() const { return m_soft_match; }
    // Queues an answer to that queue: "accept", "link_other" (to target_spool_id) or "create_new" for spool_id. Refused
    // (Rejected) unless linking is on. The outcome shows in the recent events and is handed to `done`, if any.
    BblFilament::Accept soft_match_act(const std::string& action, const std::string& spool_id, const std::string& target_spool_id,
                                       const std::string& dev_id = {},
                                       std::function<void(const BblFilament::SpoolService::LinkOutcome&)> done = {});

    // The link mode and allow-list set through MCP. They last for this session and are never stored; no mode means
    // the environment variable or the app config decides.
    void                                        set_session_link_mode(std::optional<BblFilament::LinkMode> mode) { m_session_mode = mode; }
    void                                        set_session_link_only(std::set<std::string> slots) { m_session_only = std::move(slots); }
    const std::optional<BblFilament::LinkMode>& session_link_mode() const { return m_session_mode; }
    // The linker's settings now: link_config() over the app config, ORCA_FM_LINK_MODE and the session's override.
    BblFilament::LinkConfig link_settings() const;

    // Linking is on (not off, not a dry run), the user is signed in and the printer takes part (printer_views).
    bool link_available(const std::string& dev_id) const;
    // Assigns a confirmed spool without RFID that is not in a printer to the slot. Refused (Rejected) unless linking is
    // on and the slot is allowed (BblFilament::write_refusal).
    BblFilament::Accept assign(const std::string& spool_id, const BblFilament::SlotKey& slot);
    // Creates the spool and assigns it to the slot once the list shows it; a plain add when linking is not available
    // for that printer or that slot, or the spool has no filament id or colour.
    BblFilament::Accept create_and_assign(const json& page_spool, const BblFilament::SlotKey& slot);
    // LinkDriver::unassign with the current settings and printers.
    BblFilament::Accept unassign(const BblFilament::SlotKey& slot, std::string* would_unbind = nullptr);

    json status_json(); // for the MCP tool

private:
    Runtime();
    void log_event(json line);
    void tick();
    void note_list_state();

    std::unique_ptr<BblFilament::SpoolService> m_service;
    std::unique_ptr<BblFilament::PrinterTap>   m_tap;
    std::unique_ptr<BblFilament::LinkDriver>   m_linker; // after the service: it must not outlive it
    std::unique_ptr<wxTimer>                   m_timer;
    std::unique_ptr<ColorCodes>                m_colors;
    ViewModel*                                 m_vm{nullptr};
    BblFilament::ServiceEvents                 m_vm_handlers;
    std::deque<json>                           m_log;
    bool                                       m_had_list{false};
    long long                                  m_pull_asked_at{0};
    std::map<std::string, long long>           m_pushall_asked_at; // by dev id
    std::optional<BblFilament::Session>        m_tap_session;      // the session of the last tick
    BblFilament::NewSpools                     m_new_spools;
    json                                       m_soft_match = nullptr;
    std::optional<BblFilament::LinkMode>       m_session_mode;
    std::set<std::string>                      m_session_only;
};

} // namespace Slic3r::GUI::Fm
