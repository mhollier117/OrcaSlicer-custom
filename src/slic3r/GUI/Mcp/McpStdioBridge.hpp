#pragma once

#include <nlohmann/json.hpp>

#include <map>
#include <string>

namespace Slic3r { namespace GUI { namespace Mcp {

// `orca-slicer --mcp-stdio [--datadir DIR]`: an MCP server on stdin/stdout for clients that only speak stdio.
// It answers initialize, server/discover, tools/list and ping from the shipped catalog (resources/mcp/catalog.json)
// without OrcaSlicer, finds every running window through its instance record, routes each tool call to one of them
// and starts OrcaSlicer (--mcp) on the first call when none runs. --datadir is the configuration folder given to
// windows it starts; --mcp-port is accepted and ignored. Runs before any GUI setup; only stdin EOF ends it. The
// decisions live in BridgeCore (McpBridge.hpp).
int run_stdio_bridge(int argc, char** argv);

// The HTTP headers a Streamable HTTP client derives from a message: MCP-Protocol-Version (the
// message's _meta version, else the version `initialize` negotiated), Mcp-Method and, for
// tools/call, prompts/get and resources/read, Mcp-Name (base64-sentinel encoded when not plain
// ASCII).
std::map<std::string, std::string> bridge_headers(const nlohmann::json& message, const std::string& negotiated_version);

}}} // namespace Slic3r::GUI::Mcp
