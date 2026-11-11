#include "Forms.hpp"

#include "libslic3r/Config.hpp"

#include <algorithm>
#include <cmath>
#include <locale>
#include <sstream>
#include <utility>
#include <vector>

namespace Slic3r::BblPresetSync {

namespace {

// The parts of a value between commas, each trimmed of spaces; an empty value has no items.
std::vector<std::string> items_of(const std::string& value)
{
    std::vector<std::string> items;
    if (value.empty()) return items;
    size_t start = 0;
    for (;;) {
        const size_t comma = value.find(',', start);
        std::string  item  = value.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const size_t first = item.find_first_not_of(' ');
        const size_t last  = item.find_last_not_of(' ');
        items.push_back(first == std::string::npos ? std::string() : item.substr(first, last - first + 1));
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return items;
}

std::string join(const std::vector<std::string>& items)
{
    std::string out;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) out += ',';
        out += items[i];
    }
    return out;
}

bool is_nil(const std::string& item) { return item == "nil"; }
bool is_percent(const std::string& item) { return !item.empty() && item.back() == '%'; }

// A whole number written without a fraction: the whole item parses as a number, finite, magnitude below 1e9, equal to its rounding.
std::optional<std::string> whole_number(const std::string& item)
{
    if (item.empty()) return std::nullopt;
    std::istringstream in(item);
    in.imbue(std::locale::classic());
    double v = 0;
    in >> v;
    if (in.fail() || in.peek() != std::char_traits<char>::eof()) return std::nullopt;
    if (!std::isfinite(v) || std::fabs(v) >= 1e9 || v != std::round(v)) return std::nullopt;
    return std::to_string(std::llround(v));
}

} // namespace

FormRule form_rule(const std::string& studio_type, const std::string& orca_type)
{
    if (studio_type.empty() || orca_type.empty() || studio_type == orca_type) return FormRule::Same;
    static const std::map<std::pair<std::string, std::string>, FormRule> table = {
        {{"coFloat", "coFloats"}, FormRule::ScalarToList},
        {{"coInt", "coInts"}, FormRule::ScalarToList},
        {{"coPercent", "coPercents"}, FormRule::ScalarToList},
        {{"coBool", "coBools"}, FormRule::ScalarToList},
        {{"coFloatOrPercent", "coFloatsOrPercents"}, FormRule::ScalarToList},
        {{"coString", "coStrings"}, FormRule::StringToStrings},
        {{"coInts", "coFloats"}, FormRule::IntsToFloats},
        {{"coInt", "coFloat"}, FormRule::IntsToFloats},
        {{"coFloat", "coFloatOrPercent"}, FormRule::FloatToFloatOrPercent},
        {{"coFloats", "coFloatsOrPercents"}, FormRule::FloatToFloatOrPercent},
        {{"coPercent", "coFloatOrPercent"}, FormRule::PercentToFloatOrPercent},
        {{"coPercents", "coFloatsOrPercents"}, FormRule::PercentToFloatOrPercent},
    };
    const auto it = table.find({studio_type, orca_type});
    return it == table.end() ? FormRule::Never : it->second;
}

std::optional<std::string> to_studio_form(FormRule rule, const std::string& orca_value)
{
    switch (rule) {
    case FormRule::Same: return orca_value;
    case FormRule::Never: return std::nullopt;
    case FormRule::ScalarToList: {
        const auto items = items_of(orca_value);
        if (items.empty() || items.front().empty() || is_nil(items.front())) return std::nullopt;
        return items.front();
    }
    case FormRule::StringToStrings: {
        std::vector<std::string> texts;
        if (!unescape_strings_cstyle(orca_value, texts)) return std::nullopt;
        return escape_string_cstyle(texts.empty() ? std::string() : texts.front());
    }
    case FormRule::IntsToFloats: {
        std::vector<std::string> out;
        for (const std::string& item : items_of(orca_value)) {
            if (is_nil(item)) {
                out.push_back(item);
                continue;
            }
            const auto whole = whole_number(item);
            if (!whole) return std::nullopt;
            out.push_back(*whole);
        }
        return join(out);
    }
    case FormRule::FloatToFloatOrPercent:
        for (const std::string& item : items_of(orca_value))
            if (is_percent(item)) return std::nullopt;
        return orca_value;
    case FormRule::PercentToFloatOrPercent:
        for (const std::string& item : items_of(orca_value))
            if (!is_nil(item) && !is_percent(item)) return std::nullopt;
        return orca_value;
    }
    return std::nullopt;
}

std::optional<std::string> to_orca_form(FormRule rule, const std::string& cloud_value, const std::string& orca_now)
{
    switch (rule) {
    case FormRule::Same: return cloud_value;
    case FormRule::Never: return std::nullopt;
    case FormRule::ScalarToList: {
        // A list found on Bambu is an older upload in OrcaSlicer's form: its first item stands for the value.
        const auto items = items_of(cloud_value);
        if (items.empty() || items.front().empty() || is_nil(items.front())) return std::nullopt;
        const size_t count = std::max<size_t>(1, items_of(orca_now).size());
        return join(std::vector<std::string>(count, items.front()));
    }
    case FormRule::StringToStrings: {
        // Studio's serializer never writes an unescaped quote: a value starting with one is OrcaSlicer's list form from an older upload.
        if (!cloud_value.empty() && cloud_value.front() == '"') {
            std::vector<std::string> texts;
            if (!unescape_strings_cstyle(cloud_value, texts)) return std::nullopt;
            return cloud_value;
        }
        std::string text;
        if (!unescape_string_cstyle(cloud_value, text)) return std::nullopt;
        return escape_strings_cstyle({text});
    }
    case FormRule::IntsToFloats:
    case FormRule::FloatToFloatOrPercent: return cloud_value;
    case FormRule::PercentToFloatOrPercent: {
        std::vector<std::string> out = items_of(cloud_value);
        for (std::string& item : out)
            if (!is_nil(item) && !is_percent(item)) item += '%';
        return join(out);
    }
    }
    return std::nullopt;
}

Forms::Forms(std::map<std::string, FormRule> rules)
{
    for (auto& [key, rule] : rules)
        if (rule != FormRule::Same) m_rules.emplace(key, rule);
}

FormRule Forms::rule(const std::string& key) const
{
    const auto it = m_rules.find(key);
    return it == m_rules.end() ? FormRule::Same : it->second;
}

std::optional<std::string> Forms::to_studio(const std::string& key, const std::string& orca_value) const
{
    return to_studio_form(rule(key), orca_value);
}

std::optional<std::string> Forms::to_orca(const std::string& key, const std::string& cloud_value, const std::string& orca_now) const
{
    return to_orca_form(rule(key), cloud_value, orca_now);
}

ValueMap Forms::studio_body(const ValueMap& local) const
{
    ValueMap out;
    for (const auto& [key, value] : local)
        if (const auto v = to_studio(key, value)) out.emplace(key, *v);
    return out;
}

Forms build_forms(const StudioKeys& studio, const std::function<std::string(const std::string&)>& orca_type)
{
    std::map<std::string, FormRule> rules;
    for (const auto& [key, studio_type] : studio.types()) {
        const std::string orca = orca_type(key);
        if (orca.empty() || orca == studio_type) continue;
        rules.emplace(key, form_rule(studio_type, orca));
    }
    return Forms(std::move(rules));
}

} // namespace Slic3r::BblPresetSync
