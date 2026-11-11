#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::BblFilament {

// What the filament linker needs to know about one tray (an AMS slot or an external spool), as the printer reports it.
struct TrayFacts
{
    int         ams_id{0};            // 0..3, 128.. for single-slot units, 255 / 254 for the external spools
    int         ams_type{1};          // 0 external, 1 AMS, 2 AMS Lite, 3 AMS 2 Pro, 4 AMS HT, 5 AMS Lite mixed
    std::string slot_id;              // "0".."3" ("0" for external)
    bool        external{false};
    bool        occupied{false};
    bool        reading{false};
    std::string tag_uid;              // the chip id
    std::string uuid;                 // the spool's RFID (tray_uuid) as received; empty or all zeros = none
    bool        official{false};      // tag_uid has 16 characters with "01" at offset 12
    std::string filament_id;          // tray_info_idx as received
    std::string type, sub_brands, tray_id_name;
    std::string color;                // "#RRGGBBAA", empty when unknown
    std::vector<std::string> colors;  // cols, same form; empty when the tray reports none
    int         color_type{2};        // ctype as received; 2 when the tray reports none
    long long   total_g{0};           // tray_weight
    int         remain{-1};           // percent, -1 unknown
    int         remain_g{-1};         // grams left as the printer reports them (remain_g), -1 unknown
};

struct PrinterFacts
{
    bool               has_full{false};      // a full state arrived since the tap last forgot this printer
    long long          last_message_at{0};   // `now` of the last status message
    unsigned long long observations{0};      // status messages seen (full states included) since the tap last forgot this printer
    std::vector<TrayFacts> trays;            // AMS trays in unit and slot order, then external spools
    double             nozzle_diameter{0};   // print.nozzle_diameter when reported, else 0
    std::optional<bool> pa_supported;        // from home_flag when reported
};

// Receives every status message of every printer and keeps the parts of it the linker needs. Pure: JSON and the
// standard library only. Not thread-safe; use it on one thread.
class PrinterTap
{
public:
    // Ignores anything that is not a push_status message. Never throws.
    void on_message(const std::string& dev_id, const std::string& text, long long now);
    // Null when the printer is unknown. The pointer is valid until the next call that changes the tap.
    const PrinterFacts*      facts(const std::string& dev_id) const;
    std::vector<std::string> printers() const;
    void                     forget(const std::string& dev_id); // the connection to that printer was lost or replaced
    void                     clear();

private:
    struct Entry
    {
        nlohmann::json       parts = nlohmann::json::object(); // print.ams, vir_slot, vt_tray, nozzle_diameter, home_flag
        mutable PrinterFacts facts;                            // counters kept up to date; the parsed members filled on demand
        mutable bool         dirty{true};
    };
    std::map<std::string, Entry> m_printers;
};

} // namespace Slic3r::BblFilament
