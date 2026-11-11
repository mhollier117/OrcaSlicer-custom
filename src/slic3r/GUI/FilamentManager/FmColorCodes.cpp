#include "FmColorCodes.hpp"

#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <set>

namespace Slic3r::GUI::Fm {

using json = nlohmann::json;

namespace {

std::string str_of(const json& obj, const char* key)
{
    auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

std::set<std::string> normalized_set(const std::vector<std::string>& colors)
{
    std::set<std::string> out;
    for (const std::string& c : colors) {
        std::string n = ColorCodes::normalize_hex(c);
        if (!n.empty())
            out.insert(std::move(n));
    }
    return out;
}

} // namespace

std::string ColorCodes::normalize_hex(const std::string& s)
{
    std::string v = (!s.empty() && s[0] == '#') ? s.substr(1) : s;
    if (v.size() != 6 && v.size() != 8)
        return {};
    for (char& c : v) {
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return {};
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (v.size() == 6)
        v += "FF";
    return "#" + v;
}

ColorCodes ColorCodes::parse(const std::string& json_text)
{
    ColorCodes table;
    try {
        const json doc = json::parse(json_text, nullptr, false);
        if (!doc.is_object())
            return table;
        const auto data = doc.find("data");
        if (data == doc.end() || !data->is_array())
            return table;
        for (const json& item : *data) {
            if (!item.is_object())
                continue;
            const std::string fila_id = str_of(item, "fila_id");
            if (fila_id.empty())
                continue;
            ColorEntry e;
            const auto cols = item.find("fila_color");
            if (cols != item.end() && cols->is_array())
                for (const json& c : *cols)
                    if (c.is_string()) {
                        std::string n = normalize_hex(c.get<std::string>());
                        if (!n.empty())
                            e.colors.push_back(std::move(n));
                    }
            if (e.colors.empty())
                continue;
            e.fila_color_code = str_of(item, "fila_color_code");
            e.color_code      = str_of(item, "color_code");
            const std::string type = str_of(item, "fila_color_type");
            if (type == "\xE5\x8D\x95\xE8\x89\xB2") // single colour
                e.color_type = 2;
            else if (type == "\xE5\xA4\x9A\xE6\x8B\xBC\xE8\x89\xB2") // multi colour
                e.color_type = 1;
            else if (type == "\xE6\xB8\x90\xE5\x8F\x98\xE8\x89\xB2") // gradient
                e.color_type = 0;
            else
                e.color_type = e.colors.size() > 1 ? 1 : 2;
            const auto names = item.find("fila_color_name");
            if (names != item.end() && names->is_object())
                for (auto it = names->begin(); it != names->end(); ++it)
                    if (it.value().is_string())
                        e.names[it.key()] = it.value().get<std::string>();

            Filament& f = table.m_filaments[fila_id];
            if (f.entries.empty())
                f.type = str_of(item, "fila_type");
            f.entries.push_back(std::move(e));
        }
    } catch (...) {
        return ColorCodes();
    }
    return table;
}

ColorCodes ColorCodes::load_file(const std::string& path)
{
    try {
        boost::nowide::ifstream file(path, std::ios::binary);
        if (!file.is_open())
            return {};
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        return parse(text);
    } catch (...) {
        return {};
    }
}

std::string ColorCodes::default_path()
{
    const std::string relative  = "/BBL/filament/filaments_color_codes.json";
    const std::string installed = data_dir() + "/system" + relative;
    return boost::filesystem::exists(installed) ? installed : resources_dir() + "/profiles" + relative;
}

bool ColorCodes::empty() const { return m_filaments.empty(); }

std::string ColorCodes::fila_type(const std::string& fila_id) const
{
    auto it = m_filaments.find(fila_id);
    return it == m_filaments.end() ? std::string() : it->second.type;
}

const std::vector<ColorEntry>* ColorCodes::entries(const std::string& fila_id) const
{
    auto it = m_filaments.find(fila_id);
    return it == m_filaments.end() ? nullptr : &it->second.entries;
}

const ColorEntry* ColorCodes::find(const std::string& fila_id, const std::vector<std::string>& colors, int color_type) const
{
    const std::vector<ColorEntry>* list = entries(fila_id);
    if (list == nullptr)
        return nullptr;
    const std::set<std::string> wanted = normalized_set(colors);
    if (wanted.empty())
        return nullptr;
    const int wanted_type = wanted.size() <= 1 ? 2 : BblFilament::normalize_color_type(color_type, wanted.size());
    for (const ColorEntry& e : *list) {
        if (normalized_set(e.colors) != wanted)
            continue;
        if (BblFilament::normalize_color_type(e.color_type, e.colors.size()) == wanted_type)
            return &e;
    }
    return nullptr;
}

std::string ColorCodes::name_for(const ColorEntry& e, const std::string& lang)
{
    const std::string base = lang.substr(0, lang.find('_'));
    for (const std::string& key : {base, std::string("en")}) {
        auto it = e.names.find(key);
        if (it != e.names.end())
            return it->second;
    }
    return e.names.empty() ? std::string() : e.names.begin()->second;
}

} // namespace Slic3r::GUI::Fm
