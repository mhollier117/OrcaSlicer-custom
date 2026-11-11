#include "PrinterTap.hpp"

#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <limits>
#include <utility>

namespace Slic3r::BblFilament {

namespace {

using json = nlohmann::json;

constexpr int k_external_main   = 255; // DevDefs.h VIRTUAL_TRAY_MAIN_ID
constexpr int k_external_deputy = 254; // DevDefs.h VIRTUAL_TRAY_DEPUTY_ID
constexpr int k_lite_mixed      = 5;   // DevDefs.h DevAmsType::AMS_LITE_MIXED
constexpr int k_lite_mixed_tray_offset = 24; // DevDefs.h AMS_LITE_MIXED_TRAY_INDEX_OFFSET

// The part of a member the parsers read; a value of the wrong JSON type counts as absent.
const std::string* get_string(const json& j, const char* key)
{
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get_ptr<const std::string*>() : nullptr;
}

const json* get_array(const json& j, const char* key)
{
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    return (it != j.end() && it->is_array()) ? &*it : nullptr;
}

const json* get_object(const json& j, const char* key)
{
    if (!j.is_object()) return nullptr;
    auto it = j.find(key);
    return (it != j.end() && it->is_object()) ? &*it : nullptr;
}

std::optional<long long> get_integer(const json& j, const char* key)
{
    if (!j.is_object()) return std::nullopt;
    auto it = j.find(key);
    if (it == j.end() || !it->is_number_integer()) return std::nullopt;
    return it->get<long long>();
}

// A hex string such as the printer's bit strings and `info`; nullopt when it is not one.
std::optional<unsigned long long> parse_hex(const std::string& s)
{
    if (s.empty() || s.size() > 16) return std::nullopt;
    unsigned long long v = 0;
    for (char c : s) {
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else return std::nullopt;
        v = v * 16 + static_cast<unsigned>(d);
    }
    return v;
}

std::optional<unsigned long long> get_hex(const json& j, const char* key)
{
    const std::string* s = get_string(j, key);
    return s ? parse_hex(*s) : std::nullopt;
}

bool bit_set(const std::optional<unsigned long long>& bits, long long pos)
{
    if (!bits || pos < 0 || pos >= 64) return false;
    return ((*bits >> pos) & 1ULL) != 0;
}

std::optional<int> parse_decimal(const std::string& s)
{
    if (s.empty() || s.size() > 9) return std::nullopt;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return std::nullopt;
        v = v * 10 + (c - '0');
    }
    return v;
}

// A 6- or 8-digit hex colour becomes "#RRGGBBAA" in upper case; anything else is empty.
std::string normalize_color(const std::string& s)
{
    if (s.size() != 6 && s.size() != 8) return {};
    std::string out = "#";
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) return {};
        out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (s.size() == 6) out += "FF";
    return out;
}

bool all_zero_or_empty(const std::string& s) { return s.find_first_not_of('0') == std::string::npos; }

// The tray fields shared by the AMS slots and the external spools. Mirrors the per-tray key reads of
// DevFilaSystemParser::ParseV1_0 (DevFilaSystem.cpp:681-785) and MachineObject::parse_vt_tray
// (DeviceManager.cpp:5167-5290). `tray_id_name` is not read by OrcaSlicer's parser; it is read here when present.
void read_tray(const json& t, TrayFacts& out)
{
    if (const std::string* s = get_string(t, "tag_uid")) out.tag_uid = *s;                  // ParseV1_0:681, vt:5186
    if (const std::string* s = get_string(t, "tray_uuid")) out.uuid = *s;                   // ParseV1_0:762, vt:5264
    out.official = out.tag_uid.size() == 16 && out.tag_uid.compare(12, 2, "01") == 0;
    if (const std::string* s = get_string(t, "tray_info_idx")) out.filament_id = *s;        // ParseV1_0:685-687, vt:5190
    if (const std::string* s = get_string(t, "tray_type")) out.type = *s;                   // ParseV1_0:685, vt:5190
    if (const std::string* s = get_string(t, "tray_sub_brands")) out.sub_brands = *s;       // ParseV1_0:711
    if (const std::string* s = get_string(t, "tray_id_name")) out.tray_id_name = *s;
    if (const std::string* s = get_string(t, "tray_color")) out.color = normalize_color(*s); // ParseV1_0:739
    if (const json* cols = get_array(t, "cols")) {                                          // ParseV1_0:772, vt:5274
        for (const json& c : *cols)
            if (c.is_string()) {
                const std::string n = normalize_color(c.get<std::string>());
                if (!n.empty()) out.colors.push_back(n);
            }
    }
    if (const auto ct = get_integer(t, "ctype")) out.color_type = static_cast<int>(*ct);    // ParseV1_0:767, vt:5246
    if (const std::string* w = get_string(t, "tray_weight")) {                              // ParseV1_0:715, vt:5215 (a string there)
        char*        end = nullptr;
        const double v   = std::strtod(w->c_str(), &end);
        if (end != w->c_str() && v > 0 && v < 1e9) out.total_g = static_cast<long long>(v + 0.5);
    } else if (t.is_object()) {
        auto it = t.find("tray_weight");
        if (it != t.end() && it->is_number() && it->get<double>() > 0 && it->get<double>() < 1e9) out.total_g = static_cast<long long>(it->get<double>() + 0.5);
    }
    if (const auto r = get_integer(t, "remain")) {                                          // ParseV1_0:783, vt:5284
        if (*r >= 0 && *r <= 100) out.remain = static_cast<int>(*r);
    }
}

// Mirrors the unit loop of DevFilaSystemParser::ParseV1_0 (DevFilaSystem.cpp:466-840) for the slots.
void parse_units(const json& ams, std::vector<TrayFacts>& out)
{
    const json* units = get_array(ams, "ams");
    if (!units) return;
    const auto exist_bits   = get_hex(ams, "tray_exist_bits");   // :398 (in the "ams" object, hex string)
    const auto reading_bits = get_hex(ams, "tray_reading_bits"); // :420

    for (const json& unit : *units) {
        const std::string* id_text = get_string(unit, "id"); // :466 (a unit without an id is skipped)
        if (!id_text) continue;
        const auto ams_id = parse_decimal(*id_text);
        if (!ams_id) continue;

        // The unit type is the low 4 bits of the hex string `info` (:477 DevUtil::get_flag_bits(info, 0, 4));
        // without `info` it is 1 (an AMS; :500 sets AMS_LITE only for an "f1" printer, which the tap cannot know).
        int type_id = 1;
        if (const std::string* info = get_string(unit, "info")) {
            if (const auto v = parse_hex(*info)) type_id = static_cast<int>(*v & 0xF);
        }

        const json* trays = get_array(unit, "tray"); // :647
        if (!trays) continue;
        for (const json& tray : *trays) {
            const std::string* tid = get_string(tray, "id"); // :656
            if (!tid) continue;
            const auto slot = parse_decimal(*tid);
            if (!slot) continue;

            TrayFacts f;
            f.ams_id   = *ams_id;
            f.ams_type = type_id;
            f.slot_id  = std::to_string(*slot);
            read_tray(tray, f);

            // Existence bits (:803-818): normal units (type < 4) bit ams*4+slot; the mixed AMS Lite bit 24+slot;
            // single-slot units bit 16+(id-128).
            long long exist_pos;
            if (type_id < 4) exist_pos = static_cast<long long>(*ams_id) * 4 + *slot;
            else if (type_id == k_lite_mixed) exist_pos = k_lite_mixed_tray_offset + *slot;
            else exist_pos = 16 + (static_cast<long long>(*ams_id) - 128);
            if (exist_bits) f.occupied = bit_set(exist_bits, exist_pos);
            else f.occupied = !f.type.empty(); // no usable tray_exist_bits: a slot that reports a type holds a spool

            // Reading bits (StatusPanel.cpp:4215-4235; the branch is on the unit id, not the type): ids below 128 use
            // bit ams*4+slot, or 24+slot for the mixed AMS Lite; ids from 128 use bit 16+(id-128).
            long long read_pos;
            if (*ams_id < 128) read_pos = type_id == k_lite_mixed ? k_lite_mixed_tray_offset + *slot : static_cast<long long>(*ams_id) * 4 + *slot;
            else read_pos = 16 + (static_cast<long long>(*ams_id) - 128);
            f.reading = bit_set(reading_bits, read_pos);

            out.push_back(std::move(f));
        }
    }
}

// Mirrors the external-spool code of MachineObject::parse_json (DeviceManager.cpp:4135-4181) and parse_vt_tray (:5165).
void parse_external(const json& parts, std::vector<TrayFacts>& out)
{
    std::map<int, TrayFacts> found; // only ids 255 and 254 are kept, as there

    auto add = [&](const json& vtray, std::optional<int> forced_id) {
        if (!vtray.is_object()) return;
        int id = k_external_main; // the DevAmsTray constructed with the main id (:5166)
        if (forced_id) id = *forced_id;
        else if (const std::string* s = get_string(vtray, "id")) {
            const auto v = parse_decimal(*s);
            if (!v) return;
            id = *v;
        }
        if (id != k_external_main && id != k_external_deputy) return;
        TrayFacts f;
        f.ams_id   = id;
        f.ams_type = 0;
        f.slot_id  = "0";
        f.external = true;
        read_tray(vtray, f);
        if (!vtray.contains("cols") && !f.color.empty()) f.colors.push_back(f.color); // vt:5281
        f.occupied = !f.type.empty();
        found[id]  = std::move(f);
    };

    auto vir = parts.find("vir_slot");
    if (vir != parts.end() && vir->is_array()) { // :4135 (preferred when it is an array)
        for (const json& v : *vir) add(v, std::nullopt);
    } else {
        auto vt = parts.find("vt_tray"); // :4161 (the older form; its id is forced to the main one)
        if (vt != parts.end()) add(*vt, k_external_main);
    }
    for (int id : {k_external_main, k_external_deputy}) {
        auto it = found.find(id);
        if (it != found.end()) out.push_back(std::move(it->second));
    }
}

// The rules of json_diff::restore_objects / restore_append_objects (json_diff.cpp:132-230): objects are merged key by
// key, arrays and scalars (and a value whose type changed) are replaced, keys only in the base are kept.
void merge_into(json& base, const json& diff)
{
    for (auto it = diff.begin(); it != diff.end(); ++it) {
        auto b = base.find(it.key());
        if (b != base.end() && b->is_object() && it->is_object()) merge_into(*b, *it);
        else base[it.key()] = *it;
    }
}

// Valid UTF-8 or the bytes replaced by '?', so that a name with a stray byte does not lose the whole message.
std::string sanitize_utf8(const std::string& in)
{
    std::string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size();) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        std::size_t         n = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        bool                ok = n != 0 && i + n <= in.size();
        for (std::size_t k = 1; ok && k < n; ++k) ok = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
        if (ok) {
            out.append(in, i, n);
            i += n;
        } else {
            out += '?';
            ++i;
        }
    }
    return out;
}

const char* const k_parts[] = {"ams", "vir_slot", "vt_tray", "nozzle_diameter", "home_flag"};

} // namespace

void PrinterTap::on_message(const std::string& dev_id, const std::string& text, long long now)
{
    try {
        if (text.find("push_status") == std::string::npos) return;
        json j = json::parse(text, nullptr, false);
        if (j.is_discarded()) j = json::parse(sanitize_utf8(text), nullptr, false);
        if (!j.is_object()) return;
        const json* print = get_object(j, "print");
        if (!print) return;
        const std::string* command = get_string(*print, "command");
        if (!command || *command != "push_status") return;

        // MachineObject::parse_json (DeviceManager.cpp:2982-3016): msg 1 is a diff, msg 0 or no msg is a full state.
        bool full = true;
        auto msg  = print->find("msg");
        if (msg != print->end()) {
            if (!msg->is_number_integer()) return;
            const long long m = msg->get<long long>();
            if (m == 1) full = false;
            else if (m != 0) return; // :3006 "unsupported msg_type"
        }

        Entry& e = m_printers[dev_id];
        if (full) {
            e.parts = json::object();
            for (const char* key : k_parts) {
                auto it = print->find(key);
                if (it != print->end()) e.parts[key] = *it;
            }
            e.facts.has_full      = true;
            e.facts.observations = 0;
        } else {
            for (const char* key : k_parts) {
                auto it = print->find(key);
                if (it == print->end()) continue;
                auto b = e.parts.find(key);
                if (b != e.parts.end() && b->is_object() && it->is_object()) merge_into(*b, *it);
                else e.parts[key] = *it;
            }
            if (e.facts.has_full) ++e.facts.observations;
        }
        e.facts.last_message_at = now;
        e.dirty                 = true;
    } catch (...) {
    }
}

const PrinterFacts* PrinterTap::facts(const std::string& dev_id) const
{
    auto it = m_printers.find(dev_id);
    if (it == m_printers.end()) return nullptr;
    const Entry& e = it->second;
    if (e.dirty) {
        e.dirty = false;
        e.facts.trays.clear();
        e.facts.nozzle_diameter = 0;
        e.facts.pa_supported.reset();
        try {
            if (const json* ams = get_object(e.parts, "ams")) parse_units(*ams, e.facts.trays);
            parse_external(e.parts, e.facts.trays);

            auto nd = e.parts.find("nozzle_diameter");
            if (nd != e.parts.end()) {
                double v = 0;
                if (nd->is_number()) v = nd->get<double>();
                else if (nd->is_string()) v = std::strtod(nd->get<std::string>().c_str(), nullptr);
                if (v > 0 && v < 100) e.facts.nozzle_diameter = v;
            }
            // MachineObject::parse_home_flag (DeviceManager.cpp:1206): is_support_pa_calibration = (flag >> 16) & 1.
            if (const auto hf = get_integer(e.parts, "home_flag")) e.facts.pa_supported = ((*hf >> 16) & 1) != 0;
        } catch (...) {
        }
    }
    return &e.facts;
}

std::vector<std::string> PrinterTap::printers() const
{
    std::vector<std::string> out;
    for (const auto& kv : m_printers) out.push_back(kv.first);
    return out;
}

void PrinterTap::forget(const std::string& dev_id) { m_printers.erase(dev_id); }

void PrinterTap::clear() { m_printers.clear(); }

} // namespace Slic3r::BblFilament
