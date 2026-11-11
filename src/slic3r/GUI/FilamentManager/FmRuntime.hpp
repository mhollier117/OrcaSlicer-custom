#pragma once

#include "FmColorCodes.hpp"
#include "FmViewModel.hpp"
#include "slic3r/Utils/BblFilament/SpoolService.hpp"

#include <nlohmann/json.hpp>

#include <deque>
#include <memory>
#include <string>

namespace Slic3r::GUI::Fm {

using json = nlohmann::json;

// The app-side owner of the spool service. UI thread only.
class Runtime
{
public:
    static Runtime& get(); // created on first use and never destroyed (the spool worker outlives every static)

    BblFilament::SpoolService& service();
    const ColorCodes&          colors(); // loaded on first use from ColorCodes::default_path()
    BblFilament::Session       session() const;
    std::string                lang() const; // the app's language code, UTF-8

    // The view-model of the open panel, or none. Service events go to it and to the recent-events log.
    void attach(ViewModel* vm);
    void detach(ViewModel* vm);

    json status_json(); // for the MCP tool

private:
    Runtime();
    void log_event(json line);

    std::unique_ptr<BblFilament::SpoolService> m_service;
    std::unique_ptr<ColorCodes>                m_colors;
    ViewModel*                                 m_vm{nullptr};
    BblFilament::ServiceEvents                 m_vm_handlers;
    std::deque<json>                           m_log;
};

} // namespace Slic3r::GUI::Fm
