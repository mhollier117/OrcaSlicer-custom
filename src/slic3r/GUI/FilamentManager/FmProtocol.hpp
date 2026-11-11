#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <optional>
#include <string>

namespace Slic3r::GUI::Fm {

using json = nlohmann::json;

struct Request
{
    std::uint64_t seq{0};
    std::string   module, submod, action;
    json          payload; // always an object
};

// A request packet from the page, or nullopt. Never throws.
std::optional<Request> parse_request(const std::string& raw);

json make_body(const std::string& module, const std::string& submod, const std::string& action, int error_code, const std::string& message, json payload);

// The JavaScript that delivers one packet to the page, but only to the document of that generation.
// `type` is "response" or "report".
std::string push_script(const std::string& type, std::uint64_t seq, std::uint64_t ts_ms, const json& body, const std::string& generation);

// The value of the `gen` query parameter when it is 1 to 20 digits.
std::optional<std::string> generation_of_url(const std::string& url);
std::string                page_url(const std::string& index_url, const std::string& lang, bool dark, const std::string& generation);

} // namespace Slic3r::GUI::Fm
