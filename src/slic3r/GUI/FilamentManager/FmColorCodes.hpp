#pragma once

#include <map>
#include <string>
#include <vector>

namespace Slic3r::GUI::Fm {

struct ColorEntry
{
    std::string                        fila_color_code; // "10300"
    std::string                        color_code;      // "A0"
    int                                color_type{2};   // 0 gradient, 1 multicolour, 2 single
    std::vector<std::string>           colors;          // normalised, file order
    std::map<std::string, std::string> names;           // language -> name
};

// Bambu's table of official filament colours (filaments_color_codes.json), keyed by Bambu filament id.
class ColorCodes
{
public:
    static ColorCodes  parse(const std::string& json_text); // never throws; unusable entries are skipped
    static ColorCodes  load_file(const std::string& path);  // empty table when unreadable
    static std::string default_path();
    static std::string normalize_hex(const std::string& s); // "#RRGGBBAA" upper case, or ""

    bool                           empty() const;
    std::string                    fila_type(const std::string& fila_id) const; // "" when unknown
    const std::vector<ColorEntry>* entries(const std::string& fila_id) const;   // null when unknown; file order
    // The entry of that filament with the same set of colours (order ignored) and the same colour type.
    const ColorEntry* find(const std::string& fila_id, const std::vector<std::string>& colors, int color_type) const;
    static std::string name_for(const ColorEntry& e, const std::string& lang);

private:
    struct Filament
    {
        std::string type;
        std::vector<ColorEntry> entries;
    };
    std::map<std::string, Filament> m_filaments;
};

} // namespace Slic3r::GUI::Fm
