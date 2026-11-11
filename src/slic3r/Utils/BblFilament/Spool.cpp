#include "Spool.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Slic3r::BblFilament {

namespace {

bool read_string(const json& j, const char* key, std::string& out)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return false;
    out = it->get<std::string>();
    return true;
}

bool read_number(const json& j, const char* key, double& out)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return false;
    const double v = it->get<double>();
    if (!std::isfinite(v)) return false; // a non-finite number counts as absent
    out = v;
    return true;
}

bool read_int(const json& j, const char* key, int& out)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return false;
    const long long v = it->get<long long>();
    if (v < -2147483647LL || v > 2147483647LL) return false;
    out = static_cast<int>(v);
    return true;
}

// An array whose entries are all strings; anything else is "absent".
bool read_string_array(const json& j, const char* key, std::vector<std::string>& out)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return false;
    std::vector<std::string> v;
    for (const json& e : *it) {
        if (!e.is_string()) return false;
        v.push_back(e.get<std::string>());
    }
    out = std::move(v);
    return true;
}

// Text of an integer or a string field.
bool read_text_or_int(const json& j, const char* key, std::string& out)
{
    auto it = j.find(key);
    if (it == j.end()) return false;
    if (it->is_string()) {
        out = it->get<std::string>();
        return true;
    }
    if (it->is_number_integer()) {
        out = it->dump();
        return true;
    }
    return false;
}

bool present(const json& j, const char* key)
{
    auto it = j.find(key);
    return it != j.end() && !it->is_null();
}

void apply_color_rule(Spool& s)
{
    if (!s.colors.empty() && s.color != s.colors.front()) s.color = s.colors.front();
}

constexpr double k_max_weight_g = 999999999.0;

} // namespace

long long bounded_grams(double v)
{
    if (!std::isfinite(v)) return 0;
    return std::llround(std::clamp(v, 0.0, k_max_weight_g));
}

bool valid_rfid(const std::string& rfid)
{
    return !rfid.empty() && rfid.find_first_not_of('0') != std::string::npos;
}

std::optional<long long> parse_spool_id(const std::string& id)
{
    if (id.empty() || id.size() > 18) return std::nullopt;
    for (char c : id)
        if (c < '0' || c > '9') return std::nullopt;
    const long long v = std::strtoll(id.c_str(), nullptr, 10);
    if (v <= 0) return std::nullopt;
    return v;
}

int normalize_color_type(int color_type, std::size_t color_count)
{
    if (color_type >= 0 && color_type <= 2) return color_type;
    return color_count > 1 ? 1 : 2;
}

std::optional<Spool> spool_from_cloud(const json& row)
{
    if (!row.is_object()) return std::nullopt;
    Spool s;

    std::string id_text;
    if (!read_text_or_int(row, "id", id_text)) return std::nullopt;
    const auto id = parse_spool_id(id_text);
    if (!id) return std::nullopt;
    s.id = std::to_string(*id);

    read_string(row, "filamentId", s.filament_id);
    read_string(row, "trayIdName", s.tray_id_name);
    read_string(row, "filamentVendor", s.vendor);
    read_string(row, "filamentType", s.type);
    read_string(row, "filamentName", s.name);
    read_string(row, "note", s.note);
    read_string(row, "devId", s.dev_id);
    read_string(row, "amsSn", s.ams_sn);
    read_text_or_int(row, "slotId", s.slot_id);
    read_string(row, "deviceName", s.device_name);

    std::string rfid;
    if (read_string(row, "RFID", rfid) && valid_rfid(rfid)) s.rfid = rfid;

    read_string(row, "color", s.color);
    read_string_array(row, "colors", s.colors);
    int color_type = -1;
    read_int(row, "colorType", color_type);
    s.color_type = normalize_color_type(color_type, s.colors.size());
    apply_color_rule(s);

    read_number(row, "totalNetWeight", s.total_net_weight);
    read_number(row, "netWeight", s.net_weight);

    int status = 0;
    read_int(row, "status", status);
    s.status = status == 1 ? 1 : 0;

    read_string(row, "createType", s.create_type);
    read_text_or_int(row, "createdAt", s.created_at);
    read_text_or_int(row, "updatedAt", s.updated_at);

    auto ip = row.find("inPrinter");
    if (ip != row.end() && ip->is_boolean()) s.in_printer = ip->get<bool>();

    read_int(row, "amsId", s.ams_id);
    read_int(row, "amsType", s.ams_type);
    return s;
}

json spool_to_page(const Spool& s)
{
    int remain = 100;
    if (s.total_net_weight > 0 && s.net_weight >= 0) {
        const double percent = std::clamp(s.net_weight / s.total_net_weight * 100.0, 0.0, 100.0);
        remain               = static_cast<int>(std::llround(percent));
    }

    json j = json::object();
    j["spool_id"]       = s.id;
    j["setting_id"]     = s.filament_id;
    j["tag_uid"]        = s.rfid;
    j["tray_id_name"]   = s.tray_id_name;
    j["brand"]          = s.vendor;
    j["material_type"]  = s.type;
    j["series"]         = s.name;
    j["color_name"]     = "";
    j["color_code"]     = s.color;
    j["colors"]         = s.colors;
    j["color_type"]     = s.color_type;
    j["diameter"]       = 1.75;
    j["initial_weight"] = s.total_net_weight;
    j["spool_weight"]   = 0;
    j["net_weight"]     = s.net_weight;
    j["remain_percent"] = remain;
    j["status"]         = s.status == 1 ? "info_needed" : "active";
    j["entry_method"]   = s.create_type == "ams" ? std::string("ams_sync") : s.create_type;
    j["created_at"]     = s.created_at;
    j["updated_at"]     = s.updated_at;
    j["bound_dev_id"]   = "";
    j["bound_ams_id"]   = "";
    j["note"]           = s.note;
    j["favorite"]       = false;
    j["cloud_synced"]   = true;
    j["in_printer"]     = s.in_printer;
    j["dev_id"]         = s.dev_id;
    j["ams_sn"]         = s.ams_sn;
    j["ams_id"]         = s.ams_id;
    j["ams_type"]       = s.ams_type;
    j["slot_id"]        = s.slot_id;
    j["device_name"]    = s.device_name;
    j["tray_label"]     = "";
    return j;
}

void apply_page_patch(Spool& s, const json& patch)
{
    if (!patch.is_object()) return;
    read_string(patch, "setting_id", s.filament_id);
    read_string(patch, "brand", s.vendor);
    read_string(patch, "material_type", s.type);
    read_string(patch, "series", s.name);
    read_number(patch, "initial_weight", s.total_net_weight);
    read_number(patch, "net_weight", s.net_weight);
    read_string(patch, "note", s.note);

    // The colours the cloud will have after update_body's rules (a colour edit makes `colors` follow the colour).
    std::vector<std::string> pc;
    bool                     has_pc = read_string_array(patch, "colors", pc);
    std::string              code;
    const bool               has_code = read_string(patch, "color_code", code);
    if (has_code) {
        if (!has_pc || pc.empty()) {
            pc     = {code};
            has_pc = true;
        } else if (pc.size() == 1 && pc.front() != code) {
            pc = {code};
        }
    }
    if (has_pc) s.colors = std::move(pc);
    if (has_code) s.color = code;
    apply_color_rule(s);
    int color_type = 0;
    if (read_int(patch, "color_type", color_type)) s.color_type = normalize_color_type(color_type, s.colors.size());
}

json create_body(const json& page_spool, const FilamentLookup& lookup)
{
    const json empty = json::object();
    const json& p    = page_spool.is_object() ? page_spool : empty;

    std::string tag_uid, brand, type, series, setting_id, color_code, tray_id_name, note;
    read_string(p, "tag_uid", tag_uid);
    read_string(p, "brand", brand);
    read_string(p, "material_type", type);
    read_string(p, "series", series);
    read_string(p, "setting_id", setting_id);
    read_string(p, "color_code", color_code);
    read_string(p, "tray_id_name", tray_id_name);
    read_string(p, "note", note);
    std::vector<std::string> colors;
    read_string_array(p, "colors", colors);
    int color_type = -1;
    read_int(p, "color_type", color_type);
    double initial_weight = 0;
    read_number(p, "initial_weight", initial_weight);
    double net_weight = 0;
    const bool has_net = read_number(p, "net_weight", net_weight);

    const bool rfid_ok = valid_rfid(tag_uid);
    const std::string color = colors.empty() ? color_code : colors.front();
    const int         ctype = normalize_color_type(color_type, colors.size());

    json b            = json::object();
    b["createType"]   = rfid_ok ? "ams" : "manual";
    b["filamentVendor"] = brand;
    b["filamentType"]   = type;
    b["filamentName"]   = series.empty() ? type : series;
    b["filamentId"]     = setting_id;
    bool is_support = false;
    if (lookup.is_support) {
        if (auto v = lookup.is_support(setting_id)) is_support = *v;
    }
    b["isSupport"] = is_support;
    if (rfid_ok) b["RFID"] = tag_uid;
    b["color"]     = color;
    b["colorType"] = ctype;
    if (!colors.empty()) b["colors"] = colors;

    std::string tray = tray_id_name;
    if (tray.empty() && lookup.tray_id_name) {
        std::vector<std::string> colours = colors;
        if (colours.empty() && !color_code.empty()) colours.push_back(color_code);
        tray = lookup.tray_id_name(setting_id, colours, ctype);
    }
    b["trayIdName"] = tray;
    b["rolls"]      = 1;

    const bool      has_total = initial_weight > 0;
    const long long total     = bounded_grams(initial_weight);
    if (has_total) b["totalNetWeight"] = total;
    if (has_net && net_weight >= 0) b["netWeight"] = bounded_grams(net_weight);
    else if (has_total) b["netWeight"] = total;
    if (!note.empty()) b["note"] = note;
    return b;
}

std::optional<json> update_body(const Spool& current, const json& patch, const FilamentLookup& lookup)
{
    const json empty = json::object();
    const json& p    = patch.is_object() ? patch : empty;

    json b = json::object();
    auto copy_string = [&](const char* from, const char* to) {
        std::string v;
        if (read_string(p, from, v)) b[to] = v;
    };
    copy_string("brand", "filamentVendor");
    copy_string("material_type", "filamentType");
    copy_string("series", "filamentName");
    copy_string("setting_id", "filamentId");
    copy_string("color_code", "color");
    copy_string("note", "note");

    std::vector<std::string> colors;
    if (read_string_array(p, "colors", colors)) b["colors"] = colors;

    if (b.contains("color")) {
        const std::string color = b["color"].get<std::string>();
        auto it = b.find("colors");
        if (it == b.end() || it->empty()) b["colors"] = std::vector<std::string>{color};
        else if (it->size() == 1 && (*it)[0] != color) b["colors"] = std::vector<std::string>{color};
    }

    double num = 0;
    int    ct  = 0;
    if (read_int(p, "color_type", ct)) {
        const std::size_t n = b.contains("colors") ? b["colors"].size() : 1;
        b["colorType"]      = normalize_color_type(ct, n);
    }
    if (read_number(p, "net_weight", num)) b["netWeight"] = std::max<long long>(0, bounded_grams(num));
    if (read_number(p, "initial_weight", num) || read_number(p, "total_net_weight", num))
        b["totalNetWeight"] = std::max<long long>(0, bounded_grams(num));

    if (b.empty()) return json::object();

    const auto id = parse_spool_id(current.id);
    if (!id) return std::nullopt;
    b["id"] = *id;

    if (!b.contains("filamentName")) b["filamentName"] = current.name.empty() ? current.type : current.name;

    if (present(p, "setting_id") || present(p, "color_code") || present(p, "colors") || present(p, "color_type")) {
        std::string fid = current.filament_id;
        std::string patch_fid;
        if (read_string(p, "setting_id", patch_fid) && !patch_fid.empty()) fid = patch_fid;

        if (present(p, "setting_id") && lookup.is_support) {
            if (auto v = lookup.is_support(fid)) b["isSupport"] = *v;
        }
        if (lookup.filament_type) {
            const std::string t = lookup.filament_type(fid);
            if (!t.empty()) b["filamentType"] = t;
        }
        if (lookup.tray_id_name) {
            Spool after = current;
            apply_page_patch(after, p);
            std::vector<std::string> colours = after.colors;
            if (colours.empty()) colours.push_back(after.color);
            b["trayIdName"] = lookup.tray_id_name(fid, colours, after.color_type);
        }
    }
    return b;
}

} // namespace Slic3r::BblFilament
