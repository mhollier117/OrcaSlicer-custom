#pragma once

#include "McpTypes.hpp"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// Revision served statelessly to requests that carry the per-request `_meta` version.
inline constexpr const char* kModernVersion = "2026-07-28";
// Revisions negotiated through the `initialize` handshake, newest first.
inline const std::vector<std::string>& legacy_versions()
{
    static const std::vector<std::string> versions{"2025-11-25", "2025-06-18"};
    return versions;
}

// `_meta` key of the per-request protocol version (modern era) and of the discovery server info.
inline constexpr const char* kMetaVersion = "io.modelcontextprotocol/protocolVersion";
inline constexpr const char* kMetaServer  = "io.modelcontextprotocol/serverInfo";
// How long a modern client may cache the tool list and discovery result. The catalog is fixed for
// the life of the process, so the value only bounds how long a stale client survives a restart into
// a different build.
inline constexpr int kListTtlMs = 300000;

struct ServerInfo
{
    std::string name = "orcaslicer";
    std::string title = "OrcaSlicer";
    std::string version;
    std::string instructions;
};

// One POSTed message and the transport headers the protocol checks. Header values are empty when
// the header was absent.
struct PostRequest
{
    std::string body;
    std::string protocol_version; // MCP-Protocol-Version
    std::string method_header;    // Mcp-Method
    std::string name_header;      // Mcp-Name
};

// Body is empty for 202 Accepted.
struct PostReply
{
    int         status = 200;
    std::string body;
};

// The MCP protocol layer: JSON-RPC 2.0 framing and the MCP methods of both eras, independent of
// the HTTP transport and of wx. It never throws; every failure becomes a JSON-RPC error or an
// isError tool result.
class McpProtocol
{
public:
    McpProtocol(const ToolRegistry& registry, ToolCaller& caller, ServerInfo info);

    PostReply handle_post(const PostRequest& request);

    static nlohmann::json tool_to_json(const ToolDef& def);
    static nlohmann::json result_to_json(const ToolResult& result);

private:
    PostReply handle_legacy(const nlohmann::json& id, const std::string& method, const nlohmann::json& params,
                            const PostRequest& request);
    PostReply handle_modern(const nlohmann::json& id, const std::string& method, const nlohmann::json& params,
                            const std::string& version, const PostRequest& request);
    nlohmann::json call_tool(const nlohmann::json& params, bool& unknown_tool, std::string& unknown_name);
    nlohmann::json tools_list() const;

    const ToolRegistry& m_registry;
    ToolCaller&         m_caller;
    ServerInfo          m_info;
};

// The `tools` array of a tools/list result: every tool of `registry`, as the server lists it.
nlohmann::json tools_list_json(const ToolRegistry& registry);

// Decodes the `=?base64?...?=` sentinel form of an Mcp-Name / Mcp-Param header value; plain values
// pass through unchanged.
std::string decode_header_value(const std::string& value);

}}} // namespace Slic3r::GUI::Mcp

namespace Slic3r { namespace GUI { namespace Mcp {

// Result builders and helpers shared by McpProtocol and the stdio bridge, so the two cannot drift.

// A JSON-RPC error body; `data` is left out when null.
nlohmann::json jsonrpc_error_body(const nlohmann::json& id, int code, const std::string& message, nlohmann::json data = nlohmann::json());
nlohmann::json jsonrpc_result_body(const nlohmann::json& id, nlohmann::json result);

// The version a request's params._meta carries (the modern, handshake-free era), if any.
std::optional<std::string> meta_protocol_version(const nlohmann::json& params);

// Every revision the server speaks, modern first.
nlohmann::json supported_versions_json();
// The -32022 body for a modern-era request naming a revision this server does not speak.
nlohmann::json unsupported_version_body(const nlohmann::json& id, const std::string& requested);

// Legacy-era `initialize` result: echoes a supported requested version, else the newest.
nlohmann::json initialize_result(const nlohmann::json& params, const ServerInfo& info);
// Modern-era `server/discover` result (with resultType, ttlMs and cacheScope).
nlohmann::json discover_result(const ServerInfo& info);
// Adds resultType:"complete", the modern era's marker on every result.
nlohmann::json complete_result(nlohmann::json result);
// A `tools/list` result: {tools} in the legacy era; in the modern era also resultType, ttlMs, cacheScope.
nlohmann::json tools_list_result(const nlohmann::json& tools, bool modern);

}}} // namespace Slic3r::GUI::Mcp
