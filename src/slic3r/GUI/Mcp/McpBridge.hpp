#pragma once

#include "McpInstances.hpp"
#include "McpProtocol.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// The outcome of one POST to a window. `sent` is true once the request started going out (a partial write
// counts), so a caller knows whether the window may have acted on it; `status` is empty when no reply came.
struct HttpResult
{
    bool                               sent = false;
    std::optional<int>                 status;
    std::string                        body;
    std::map<std::string, std::string> headers;
};

struct LaunchResult
{
    bool started        = false; // the process was created and stayed up
    bool exited_quickly = false; // it ended within a moment: a running OrcaSlicer without the MCP server took the launch over
};

// Held while one bridge decides whether to launch OrcaSlicer; released by destruction.
class LaunchLock
{
public:
    virtual ~LaunchLock() = default;
};

// Everything BridgeCore needs from the outside world, injected so its decisions run without processes,
// sockets or time.
struct BridgeDeps
{
    // Live windows' records (production: read_instance_records(default_datadir())).
    std::function<std::vector<InstanceRecord>()> instances;
    // POSTs `body` to the window. The last argument is the read timeout in seconds.
    std::function<HttpResult(const InstanceRecord&, const std::string& body, const std::map<std::string, std::string>& headers, int read_timeout_s)> post;
    // Starts OrcaSlicer with `extra_args` after its path.
    std::function<LaunchResult(const std::vector<std::string>& extra_args)> launch;
    // The interprocess launch lock; null return means proceed without it.
    std::function<std::unique_ptr<LaunchLock>()> launch_lock;
    // resources/mcp/catalog.json; null when it could not be loaded.
    nlohmann::json seed;
    std::function<void(const std::string&)> log;      // stderr
    std::function<int64_t()>                now_ms;   // monotonic milliseconds
    std::function<void(int)>                sleep_ms;
    std::string                             bridge_id;   // 16 hex characters
    std::string                             datadir_arg; // --datadir, passed on to launched windows; may be empty
};

// All decisions of the stdio bridge: which messages it answers itself, which window gets a call, when it
// launches OrcaSlicer, how job ids and window ids map. handle_line may be called from several threads.
class BridgeCore
{
public:
    explicit BridgeCore(BridgeDeps deps);

    // One line of the client's stdin. `write` receives whole JSON lines (replies); it must be callable from any thread.
    void handle_line(const std::string& line, const std::function<void(const std::string&)>& write);

    // The tools/list `tools` array: the seed's tools with `orca_instance` added, then the bridge's own.
    const nlohmann::json& tools() const { return m_tools; }
    const ServerInfo&     server_info() const { return m_info; }

private:
    struct Window
    {
        std::string id;
        int         pid = 0;
        std::string incarnation;
        bool        gone = false;
    };
    struct Live
    {
        std::string    id;
        InstanceRecord rec;
    };
    struct Resolution
    {
        std::optional<Live> target;
        std::string         error;
        bool                named    = false; // the caller chose the window
        bool                multiple = false; // several windows were live
    };
    struct Dispatch
    {
        enum Kind { Replied, NotReachable, NoReply, Gone } kind = NotReachable;
        HttpResult http;
    };

    std::optional<nlohmann::json> handle_message(const nlohmann::json& msg);
    std::optional<nlohmann::json> forward(const nlohmann::json& msg, bool allow_launch);
    nlohmann::json                bridge_tool(const nlohmann::json& id, const std::string& name, const nlohmann::json& args, bool modern);

    std::vector<Live>    live_windows();
    Resolution           resolve(const std::string& explicit_id, const std::string& job_window);
    std::string          describe_live(const std::vector<Live>& live) const;
    std::string          missing_window_message(const std::string& id, const std::vector<Live>& live) const;
    Dispatch             dispatch(const Live& target, const std::string& body, std::map<std::string, std::string> headers, int read_timeout_s = 120);
    void                 mark_gone(const std::string& id);
    std::optional<Live>  launch_window(bool fresh_window, const std::vector<std::string>& files, std::string& err);
    std::optional<Live>  launch_locked(bool fresh_window, const std::vector<std::string>& files, std::string& err);
    bool                 ping(const Live& window);
    std::optional<nlohmann::json> window_state(const Live& window, int read_timeout_s);
    std::string          negotiated() const;
    int64_t              now() const;
    void                 sleep(int ms) const;
    void                 log(const std::string& s) const;

    BridgeDeps     m_d;
    ServerInfo     m_info;
    nlohmann::json m_tools;

    mutable std::mutex  m_mu; // windows, selection, negotiated version, last launch error
    std::vector<Window> m_windows;
    int                 m_next_id = 1;
    std::string         m_selected;
    std::string         m_negotiated;
    std::string         m_launch_error;

    std::mutex            m_launch_mu; // one launch at a time in this bridge
    std::atomic<uint64_t> m_launch_gen{0};
};

}}} // namespace Slic3r::GUI::Mcp
