#include "FmProtocol.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace Slic3r::GUI::Fm {

namespace {

bool is_generation(const std::string& s)
{
    return !s.empty() && s.size() <= 20 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string string_field(const json& obj, const char* key)
{
    auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

} // namespace

std::optional<Request> parse_request(const std::string& raw)
{
    try {
        const json doc = json::parse(raw, nullptr, false);
        if (!doc.is_object())
            return std::nullopt;
        const auto head = doc.find("head");
        const auto body = doc.find("body");
        if (head == doc.end() || !head->is_object() || body == doc.end() || !body->is_object())
            return std::nullopt;
        if (string_field(*head, "version") != "1.0" || string_field(*head, "type") != "request")
            return std::nullopt;
        const auto seq = head->find("seq");
        if (seq == head->end() || !seq->is_number_unsigned())
            return std::nullopt;

        Request r;
        r.seq              = seq->get<std::uint64_t>();
        r.module           = string_field(*body, "module");
        r.submod           = string_field(*body, "submod");
        r.action           = string_field(*body, "action");
        const auto payload = body->find("payload");
        r.payload          = (payload != body->end() && payload->is_object()) ? *payload : json::object();
        return r;
    } catch (...) {
        return std::nullopt;
    }
}

json make_body(const std::string& module, const std::string& submod, const std::string& action, int error_code, const std::string& message, json payload)
{
    if (payload.is_null())
        payload = json::object();
    return json{{"module", module}, {"submod", submod}, {"action", action}, {"error_code", error_code}, {"message", message}, {"payload", std::move(payload)}};
}

std::string push_script(const std::string& type, std::uint64_t seq, std::uint64_t ts_ms, const json& body, const std::string& generation)
{
    json packet = json::object();
    packet["event"] = "device";
    packet["head"]  = json{{"version", "1.0"}, {"type", type}, {"seq", seq}, {"ts", ts_ms}};
    packet["body"]  = body;

    const std::string gen = is_generation(generation) ? generation : std::string("0");
    std::string       out = "(function(p,g){try{if(new URLSearchParams(window.location.search).get(\"gen\")!==g)return;"
                            "document.dispatchEvent(new CustomEvent(\"cpp:\"+p.event,{detail:p}));}catch(e){}})(";
    out += packet.dump(-1, ' ', true, json::error_handler_t::replace);
    out += ",\"" + gen + "\");";
    return out;
}

std::optional<std::string> generation_of_url(const std::string& url)
{
    const size_t q = url.find('?');
    if (q == std::string::npos || url.find('#') < q)
        return std::nullopt;
    const size_t      hash  = url.find('#', q);
    const std::string query = url.substr(q + 1, hash == std::string::npos ? std::string::npos : hash - q - 1);
    size_t            pos   = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos)
            amp = query.size();
        const std::string part = query.substr(pos, amp - pos);
        if (part.rfind("gen=", 0) == 0) {
            const std::string value = part.substr(4);
            if (is_generation(value))
                return value;
            return std::nullopt;
        }
        pos = amp + 1;
    }
    return std::nullopt;
}

std::string page_url(const std::string& index_url, const std::string& lang, bool dark, const std::string& generation)
{
    std::string url = index_url;
    url += lang.empty() ? std::string("?") : "?lang=" + lang + "&";
    url += std::string("theme=") + (dark ? "dark" : "light") + "&gen=" + generation + "#/filament_manager";
    return url;
}

} // namespace Slic3r::GUI::Fm
