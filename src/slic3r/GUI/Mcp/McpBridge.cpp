#include "McpBridge.hpp"
#include "McpPaths.hpp"
#include "McpStdioBridge.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <set>
#include <thread>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;

namespace {

constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams  = -32602;
constexpr int kInternalError  = -32603;
constexpr int kReplaced       = -32001; // McpHttpServer: Mcp-Orca-Incarnation did not match; the call did not run

constexpr const char* kInstanceKey = "orca_instance";
constexpr const char* kIncarnation = "Mcp-Orca-Incarnation";

constexpr int kStartWaitMs = 60000; // for the new window's endpoint
constexpr int kReadyWaitMs = 45000; // for get_state.ready
constexpr int kPollMs      = 500;
constexpr int kStateBudgetS = 5;    // orca_list_instances: per window
constexpr std::size_t kMaxInstructions = 4096;

const char* kInstructionsAddendum =
    "Several OrcaSlicer windows can run at once. orca_list_instances lists them (ids w1, w2...); "
    "orca_open_instance opens a new one; orca_use_instance picks the default. Any tool takes orca_instance to choose "
    "the window for that call. OrcaSlicer opens itself on the first tool call when none is running.";

const char* kQuickExitMessage = "OrcaSlicer is running with the MCP server off. Turn on \"MCP server\" under Preferences > General > "
                                "AI features, then retry.";

const char* kAnotherWindowWarning = "Another OrcaSlicer window is running. If both are signed in to the same Bambu account, cloud printer "
                                    "status and X1C camera video may stop working in one of them.";

std::string key_of(int pid, const std::string& incarnation) { return std::to_string(pid) + "-" + incarnation; }

json tool_result(const json& data)
{
    return {{"content", json::array({{{"type", "text"}, {"text", data.dump()}}})}};
}

json tool_error(const std::string& message)
{
    return {{"content", json::array({{{"type", "text"}, {"text", message}}})}, {"isError", true}};
}

bool starts_with(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

// "w2.j7" -> {"w2", "j7"}.
bool split_job(const std::string& s, std::string& window, std::string& job)
{
    if (s.size() < 4 || s[0] != 'w')
        return false;
    std::size_t i = 1;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i])))
        ++i;
    if (i == 1 || i >= s.size() - 1 || s[i] != '.')
        return false;
    window = s.substr(0, i);
    job    = s.substr(i + 1);
    return true;
}

// Prefixes every job id of a tool result with its window: strings under a key named "job", and the id and
// parent of the entries of a "jobs" array (get_state). True when something changed.
bool prefix_jobs(json& v, const std::string& window)
{
    bool changed = false;
    if (v.is_object()) {
        // A running call's note names its job (`wait {for:"job", job:"j3"}`); keep it in step with the field.
        if (auto job = v.find("job"); job != v.end() && job->is_string())
            if (auto note = v.find("note"); note != v.end() && note->is_string()) {
                const std::string from = "job:\"" + job->get<std::string>() + "\"";
                std::string       text = note->get<std::string>();
                for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + 1))
                    text.replace(at, from.size(), "job:\"" + window + "." + job->get<std::string>() + "\"");
                *note = text;
            }
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (it.key() == "job" && it->is_string()) {
                *it     = window + "." + it->get<std::string>();
                changed = true;
            } else {
                if (it.key() == "jobs" && it->is_array())
                    for (json& entry : *it)
                        if (entry.is_object())
                            for (const char* field : {"id", "parent"})
                                if (auto f = entry.find(field); f != entry.end() && f->is_string()) {
                                    *f      = window + "." + f->get<std::string>();
                                    changed = true;
                                }
                changed = prefix_jobs(*it, window) || changed;
            }
        }
    } else if (v.is_array()) {
        for (json& e : v)
            changed = prefix_jobs(e, window) || changed;
    }
    return changed;
}

// Strips the window prefix from every string under a key named "job" (arguments).
void restore_jobs(json& v)
{
    if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it) {
            std::string w, j;
            if (it.key() == "job" && it->is_string() && split_job(it->get<std::string>(), w, j))
                *it = j;
            else
                restore_jobs(*it);
        }
    } else if (v.is_array()) {
        for (json& e : v)
            restore_jobs(e);
    }
}

// The first string under a key named "job".
const json* find_job(const json& v)
{
    if (v.is_object()) {
        for (auto it = v.begin(); it != v.end(); ++it) {
            if (it.key() == "job" && it->is_string())
                return &*it;
            if (const json* f = find_job(*it))
                return f;
        }
    } else if (v.is_array()) {
        for (const json& e : v)
            if (const json* f = find_job(e))
                return f;
    }
    return nullptr;
}

// A tool result's text is one JSON document (ok results) or "message\n<JSON data>" (errors).
void rewrite_tool_result(json& result, const std::string& window)
{
    if (!result.is_object() || !result.contains("content") || !result["content"].is_array() || result["content"].empty())
        return;
    json& first = result["content"][0];
    if (!first.is_object() || !first.contains("text") || !first["text"].is_string())
        return;
    const std::string text = first["text"].get<std::string>();
    auto              try_from = [&](std::size_t from) {
        json parsed = json::parse(text.begin() + from, text.end(), nullptr, false);
        if (parsed.is_discarded() || !(parsed.is_object() || parsed.is_array()))
            return false;
        if (prefix_jobs(parsed, window))
            first["text"] = text.substr(0, from) + parsed.dump();
        return true;
    };
    if (try_from(0))
        return;
    for (std::size_t nl = text.find('\n'); nl != std::string::npos; nl = text.find('\n', nl + 1))
        if (try_from(nl + 1))
            return;
}

} // namespace

BridgeCore::BridgeCore(BridgeDeps deps) : m_d(std::move(deps))
{
    if (!m_d.now_ms)
        m_d.now_ms = [] {
            return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        };
    if (!m_d.sleep_ms)
        m_d.sleep_ms = [](int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); };

    const bool have_seed = m_d.seed.is_object() && m_d.seed.contains("tools") && m_d.seed["tools"].is_array();
    m_info.version       = have_seed ? m_d.seed.value("version", "") : std::string();
    std::string seed_instructions = have_seed ? m_d.seed.value("instructions", "") : std::string();
    if (!have_seed)
        seed_instructions = "OrcaSlicer's tool catalog (resources/mcp/catalog.json) was not found next to this program, so only the window "
                            "tools are listed.";
    std::string addendum = kInstructionsAddendum;
    if (seed_instructions.size() + 2 >= kMaxInstructions)
        addendum.clear();
    else if (seed_instructions.size() + 2 + addendum.size() > kMaxInstructions)
        addendum.resize(kMaxInstructions - seed_instructions.size() - 2);
    m_info.instructions = addendum.empty() ? seed_instructions : seed_instructions + "\n\n" + addendum;

    m_tools = json::array();
    if (have_seed)
        for (json tool : m_d.seed["tools"]) {
            const std::string name = tool.value("name", "");
            if (starts_with(name, "orca_")) {
                log("MCP bridge: the catalog's tool '" + name + "' uses the reserved orca_ prefix; leaving it out");
                continue;
            }
            if (!tool.contains("inputSchema") || !tool["inputSchema"].is_object())
                tool["inputSchema"] = {{"type", "object"}};
            json& schema = tool["inputSchema"];
            if (!schema.contains("properties") || !schema["properties"].is_object())
                schema["properties"] = json::object();
            if (schema["properties"].contains(kInstanceKey)) {
                log("MCP bridge: the catalog's tool '" + name + "' already has an orca_instance argument; leaving it out");
                continue;
            }
            schema["properties"][kInstanceKey] = {
                {"type", "string"},
                {"description", "Which OrcaSlicer window runs this call: an id from orca_list_instances, such as w2. Omit to use the default window."}};
            m_tools.push_back(std::move(tool));
        }

    auto bridge_def = [](const char* name, const char* title, const char* description, json properties, bool read_only, bool idempotent) {
        return json{{"name", name},
                    {"title", title},
                    {"description", description},
                    {"inputSchema", {{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}}},
                    {"annotations",
                     {{"title", title},
                      {"readOnlyHint", read_only},
                      {"destructiveHint", false},
                      {"idempotentHint", idempotent},
                      {"openWorldHint", false}}}};
    };
    m_tools.push_back(bridge_def("orca_list_instances", "List OrcaSlicer windows",
                                 "Lists the running OrcaSlicer windows with their ids (w1, w2...), process ids, versions, whether each is "
                                 "ready, its open project, which one is the default, and which this session opened.",
                                 json::object(), true, true));
    m_tools.push_back(bridge_def(
        "orca_open_instance", "Open an OrcaSlicer window",
        "Opens a new OrcaSlicer window (always a new one, even when OrcaSlicer is set to a single instance), optionally with files "
        "loaded, waits until it is ready, and makes it the default window unless select is false.",
        {{"files", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "Absolute paths of files to open in the new window."}}},
         {"select", {{"type", "boolean"}, {"description", "Make the new window the default for later calls. Default true."}}}},
        false, false));
    m_tools.push_back(bridge_def("orca_use_instance", "Pick the default OrcaSlicer window",
                                 "Sets which window calls without orca_instance go to, or clears the choice with null.",
                                 {{"id", {{"type", json::array({"string", "null"})}, {"description", "A window id from orca_list_instances, or null to clear."}}}},
                                 false, true));
}

void BridgeCore::log(const std::string& s) const
{
    if (m_d.log)
        m_d.log(s);
    else
        std::cerr << s << std::endl;
}

int64_t BridgeCore::now() const { return m_d.now_ms(); }
void    BridgeCore::sleep(int ms) const { m_d.sleep_ms(ms); }

std::string BridgeCore::negotiated() const
{
    std::lock_guard<std::mutex> lock(m_mu);
    return m_negotiated;
}

// ---- windows ------------------------------------------------------------------

std::vector<BridgeCore::Live> BridgeCore::live_windows()
{
    std::vector<InstanceRecord> records;
    try {
        records = m_d.instances ? m_d.instances() : std::vector<InstanceRecord>();
    } catch (const std::exception&) {
    }
    std::lock_guard<std::mutex> lock(m_mu);
    std::set<std::string>       known;
    for (const Window& w : m_windows)
        known.insert(key_of(w.pid, w.incarnation));
    std::vector<const InstanceRecord*> fresh;
    for (const InstanceRecord& r : records)
        if (!known.count(key_of(r.pid, r.incarnation)))
            fresh.push_back(&r);
    std::sort(fresh.begin(), fresh.end(), [](const InstanceRecord* a, const InstanceRecord* b) {
        return std::make_pair(a->started_at, a->pid) < std::make_pair(b->started_at, b->pid);
    });
    for (const InstanceRecord* r : fresh)
        m_windows.push_back({"w" + std::to_string(m_next_id++), r->pid, r->incarnation, false});
    std::vector<Live> live;
    for (const Window& w : m_windows) {
        if (w.gone)
            continue;
        for (const InstanceRecord& r : records)
            if (r.pid == w.pid && r.incarnation == w.incarnation) {
                live.push_back({w.id, r});
                break;
            }
    }
    return live;
}

void BridgeCore::mark_gone(const std::string& id)
{
    std::lock_guard<std::mutex> lock(m_mu);
    for (Window& w : m_windows)
        if (w.id == id)
            w.gone = true;
}

std::string BridgeCore::describe_live(const std::vector<Live>& live) const
{
    std::string out;
    for (const Live& l : live)
        out += (out.empty() ? "" : ", ") + l.id + " (pid " + std::to_string(l.rec.pid) + ")";
    return out;
}

std::string BridgeCore::missing_window_message(const std::string& id, const std::vector<Live>& live) const
{
    return "OrcaSlicer window " + id + " is not running (it was closed or replaced). " +
           (live.empty() ? std::string("No window is running; any tool call opens one.") : "Running windows: " + describe_live(live) + ".");
}

BridgeCore::Resolution BridgeCore::resolve(const std::string& explicit_id, const std::string& job_window)
{
    Resolution        r;
    std::vector<Live> live = live_windows();
    r.multiple             = live.size() > 1;
    auto by_id             = [&](const std::string& id) -> std::optional<Live> {
        for (const Live& l : live)
            if (l.id == id)
                return l;
        return std::nullopt;
    };
    for (const std::string* named : {&explicit_id, &job_window})
        if (!named->empty()) {
            r.named = true;
            if (auto l = by_id(*named))
                r.target = l;
            else
                r.error = missing_window_message(*named, live);
            return r;
        }
    std::string selected;
    {
        std::lock_guard<std::mutex> lock(m_mu);
        selected = m_selected;
    }
    if (!selected.empty())
        if (auto l = by_id(selected)) {
            r.target = l;
            return r;
        }
    for (const Live& l : live)
        if (l.rec.launched_by == m_d.bridge_id) {
            r.target = l;
            return r;
        }
    if (live.size() == 1) {
        r.target = live.front();
        return r;
    }
    if (live.empty())
        return r;
    // Several: the one on the configured port, else the oldest.
    const Live* best = &live.front();
    for (const Live& l : live) {
        const bool better = (l.rec.primary && !best->rec.primary) ||
                            (l.rec.primary == best->rec.primary && std::make_pair(l.rec.started_at, l.rec.pid) < std::make_pair(best->rec.started_at, best->rec.pid));
        if (better)
            best = &l;
    }
    r.target = *best;
    return r;
}

// ---- sending ------------------------------------------------------------------

BridgeCore::Dispatch BridgeCore::dispatch(const Live& target, const std::string& body, std::map<std::string, std::string> headers, int read_timeout_s)
{
    auto current = [&]() -> std::optional<InstanceRecord> {
        try {
            for (const InstanceRecord& r : m_d.instances())
                if (r.pid == target.rec.pid && r.incarnation == target.rec.incarnation)
                    return r;
        } catch (const std::exception&) {
        }
        return std::nullopt;
    };
    auto send = [&](const InstanceRecord& rec) {
        headers[kIncarnation] = rec.incarnation;
        return m_d.post(rec, body, headers, read_timeout_s);
    };

    Dispatch       out;
    InstanceRecord rec = target.rec;
    HttpResult     r   = send(rec);
    if (!r.sent && !r.status) {
        // Not sent: the server cannot have acted. Its record may have changed (a restart on the same pid cannot, but a
        // moved port can); send once more to the same window.
        auto cur = current();
        if (!cur) {
            out.kind = Dispatch::Gone;
            return out;
        }
        rec = *cur;
        r   = send(rec);
    } else if (r.status && *r.status == 401) {
        // The token rotated: the same window's record has the new one. Never reroute.
        if (auto cur = current()) {
            rec = *cur;
            r   = send(rec);
        }
    }
    if (!r.status) {
        out.kind = r.sent ? Dispatch::NoReply : Dispatch::NotReachable;
        return out;
    }
    if (*r.status == 409) {
        const json body_json = json::parse(r.body, nullptr, false);
        if (body_json.is_object() && body_json.contains("error") && body_json["error"].is_object() &&
            body_json["error"].value("code", 0) == kReplaced) {
            mark_gone(target.id);
            out.kind = Dispatch::Gone;
            return out;
        }
    }
    out.kind = Dispatch::Replied;
    out.http = std::move(r);
    return out;
}

bool BridgeCore::ping(const Live& window)
{
    const json  body{{"jsonrpc", "2.0"}, {"id", "bridge-probe"}, {"method", "ping"}};
    const auto  r = m_d.post(window.rec, body.dump(), {{"Mcp-Method", "ping"}, {kIncarnation, window.rec.incarnation}}, kStateBudgetS);
    return r.status && *r.status == 200;
}

std::optional<json> BridgeCore::window_state(const Live& window, int read_timeout_s)
{
    const json body{{"jsonrpc", "2.0"},
                    {"id", "bridge-state"},
                    {"method", "tools/call"},
                    {"params", {{"name", "get_state"}, {"arguments", json::object()}}}};
    const Dispatch d = dispatch(window, body.dump(), bridge_headers(body, negotiated()), read_timeout_s);
    if (d.kind != Dispatch::Replied || *d.http.status != 200)
        return std::nullopt;
    const json reply = json::parse(d.http.body, nullptr, false);
    if (!reply.is_object() || !reply.contains("result") || !reply["result"].is_object() || reply["result"].value("isError", false))
        return std::nullopt;
    json content = reply["result"].value("content", json::array());
    if (!content.is_array() || content.empty() || !content[0].is_object() || !content[0].contains("text") || !content[0]["text"].is_string())
        return std::nullopt;
    json state = json::parse(content[0]["text"].get<std::string>(), nullptr, false);
    if (!state.is_object())
        return std::nullopt;
    return state;
}

// ---- launching ----------------------------------------------------------------

std::optional<BridgeCore::Live> BridgeCore::launch_window(bool fresh_window, const std::vector<std::string>& files, std::string& err)
{
    const uint64_t             seen = m_launch_gen.load();
    std::lock_guard<std::mutex> serial(m_launch_mu);
    if (!fresh_window && m_launch_gen.load() != seen) {
        // A launch finished while this call waited for its turn: share its outcome.
        if (Resolution r = resolve("", ""); r.target)
            return r.target;
        std::lock_guard<std::mutex> lock(m_mu);
        if (!m_launch_error.empty()) {
            err = m_launch_error;
            return std::nullopt;
        }
    }
    auto result = launch_locked(fresh_window, files, err);
    {
        std::lock_guard<std::mutex> lock(m_mu);
        m_launch_error = result ? std::string() : err;
    }
    ++m_launch_gen;
    return result;
}

std::optional<BridgeCore::Live> BridgeCore::launch_locked(bool fresh_window, const std::vector<std::string>& files, std::string& err)
{
    std::unique_ptr<LaunchLock> lock = m_d.launch_lock ? m_d.launch_lock() : nullptr;
    if (!fresh_window) {
        // Another bridge (another Claude session) may have launched while this one waited for the lock.
        if (Resolution r = resolve("", ""); r.target)
            return r.target;
    }
    std::set<std::string> before;
    live_windows();
    {
        std::lock_guard<std::mutex> lock_state(m_mu);
        for (const Window& w : m_windows)
            before.insert(key_of(w.pid, w.incarnation));
    }
    std::vector<std::string> args{"--mcp", "--mcp-launched-by", m_d.bridge_id};
    if (!m_d.datadir_arg.empty()) {
        args.push_back("--datadir");
        args.push_back(m_d.datadir_arg);
    }
    if (fresh_window)
        args.push_back("--no-single-instance");
    for (const std::string& f : files)
        args.push_back(f);

    const LaunchResult launched = m_d.launch(args);
    if (launched.exited_quickly) {
        err = kQuickExitMessage;
        return std::nullopt;
    }
    if (!launched.started) {
        err = "OrcaSlicer could not be started.";
        return std::nullopt;
    }

    std::optional<Live> found;
    const int64_t       start_deadline = now() + kStartWaitMs;
    for (;;) {
        for (const Live& l : live_windows())
            if (l.rec.launched_by == m_d.bridge_id && !before.count(key_of(l.rec.pid, l.rec.incarnation)) && ping(l)) {
                found = l;
                break;
            }
        if (found)
            break;
        if (now() >= start_deadline) {
            err = "OrcaSlicer was started but its MCP server did not answer within 60 seconds.";
            return std::nullopt;
        }
        sleep(kPollMs);
    }
    lock.reset();

    // A window held by the first-run wizard or a prompt answers get_state with ready:false; the call is forwarded anyway
    // after the wait, and the window's own reply tells what it needs.
    const int64_t ready_deadline = now() + kReadyWaitMs;
    for (;;) {
        if (auto state = window_state(*found, kStateBudgetS); state && state->value("ready", false))
            break;
        if (now() >= ready_deadline)
            break;
        sleep(kPollMs);
    }
    return found;
}

// ---- messages -----------------------------------------------------------------

void BridgeCore::handle_line(const std::string& line_in, const std::function<void(const std::string&)>& write)
{
    std::string line = line_in;
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    if (line.find_first_not_of(" \t") == std::string::npos)
        return;
    const json msg = json::parse(line, nullptr, false);
    if (msg.is_discarded()) {
        write(jsonrpc_error_body(nullptr, -32700, "Parse error").dump());
        return;
    }
    if (!msg.is_object()) {
        write(jsonrpc_error_body(nullptr, -32600, "Each line must be one JSON-RPC message object (batches are not supported)").dump());
        return;
    }
    std::optional<json> reply;
    try {
        reply = handle_message(msg);
    } catch (const std::exception& e) {
        if (msg.contains("id") && msg.contains("method"))
            reply = jsonrpc_error_body(msg["id"], kInternalError, std::string("Internal error: ") + e.what());
    }
    if (reply)
        write(reply->dump());
}

std::optional<json> BridgeCore::handle_message(const json& msg)
{
    // Notifications and responses get no reply.
    if (!msg.contains("id") || !msg.contains("method") || !msg["method"].is_string())
        return std::nullopt;
    const json        id     = msg["id"];
    const std::string method = msg["method"].get<std::string>();
    if (msg.contains("params") && !msg["params"].is_object())
        return jsonrpc_error_body(id, kInvalidParams, "params must be an object");
    const json params = msg.contains("params") ? msg["params"] : json::object();
    const auto meta   = meta_protocol_version(params);
    const bool modern = meta.has_value();

    if (method == "initialize") {
        json result = initialize_result(params, m_info);
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_negotiated = result.value("protocolVersion", "");
        }
        return jsonrpc_result_body(id, std::move(result));
    }
    const bool local = method == "server/discover" || method == "ping" || method == "tools/list";
    if (local && modern && *meta != kModernVersion)
        return unsupported_version_body(id, *meta);
    if (method == "server/discover")
        return jsonrpc_result_body(id, discover_result(m_info));
    if (method == "ping")
        return jsonrpc_result_body(id, modern ? complete_result(json::object()) : json::object());
    if (method == "tools/list")
        return jsonrpc_result_body(id, tools_list_result(m_tools, modern));
    if (method == "tools/call") {
        const std::string name = params.contains("name") && params["name"].is_string() ? params["name"].get<std::string>() : std::string();
        if (starts_with(name, "orca_")) {
            json args = params.contains("arguments") && params["arguments"].is_object() ? params["arguments"] : json::object();
            const bool known = name == "orca_list_instances" || name == "orca_open_instance" || name == "orca_use_instance";
            if (!known)
                return jsonrpc_error_body(id, kInvalidParams, "Unknown tool: " + name, {{"name", name}});
            return bridge_tool(id, name, args, modern);
        }
        return forward(msg, true);
    }
    return forward(msg, false);
}

std::optional<json> BridgeCore::forward(const json& msg, bool allow_launch)
{
    const json        id     = msg["id"];
    const std::string method = msg["method"].get<std::string>();
    json              fwd    = msg;
    std::string       explicit_id, job_window;

    if (method == "tools/call" && fwd.contains("params") && fwd["params"].is_object() && fwd["params"].contains("arguments") &&
        fwd["params"]["arguments"].is_object()) {
        json& args = fwd["params"]["arguments"];
        if (auto it = args.find(kInstanceKey); it != args.end()) {
            if (!it->is_string())
                return jsonrpc_error_body(id, kInvalidParams, "orca_instance must be a string such as \"w2\"");
            explicit_id = it->get<std::string>();
            args.erase(it);
        }
        if (const json* job = find_job(args)) {
            std::string w, j;
            if (split_job(job->get<std::string>(), w, j)) {
                if (!explicit_id.empty() && explicit_id != w)
                    return jsonrpc_error_body(id, kInvalidParams, "The job belongs to window " + w + " but orca_instance names " + explicit_id);
                job_window = w;
                restore_jobs(args);
            } else if (explicit_id.empty()) {
                // A job id without its window prefix: only unambiguous with one window.
                const std::vector<Live> live = live_windows();
                if (live.size() != 1)
                    return jsonrpc_error_body(id, kInvalidParams,
                                              std::string("The job id has no window prefix, and ") +
                                                  (live.empty() ? "no OrcaSlicer window is running." : "several windows are running (" + describe_live(live) + ").") +
                                                  " Use the prefixed id the call returned, such as w1.j7.");
                job_window = live.front().id;
            }
        }
    }
    const std::string body = fwd.dump();

    for (int attempt = 0; attempt < 3; ++attempt) {
        Resolution r = resolve(explicit_id, job_window);
        if (!r.target) {
            if (!r.error.empty())
                return jsonrpc_error_body(id, kInternalError, r.error);
            if (!allow_launch)
                return jsonrpc_error_body(id, kMethodNotFound, "Method not found: " + method);
            std::string err;
            auto        launched = launch_window(false, {}, err);
            if (!launched)
                return jsonrpc_error_body(id, kInternalError, err);
            r.target = launched;
            r.multiple = live_windows().size() > 1;
        }
        const Live target = *r.target;
        Dispatch   d      = dispatch(target, body, bridge_headers(fwd, negotiated()));
        if (d.kind == Dispatch::Gone) {
            if (r.named) {
                std::vector<Live> live = live_windows();
                return jsonrpc_error_body(id, kInternalError, missing_window_message(target.id, live) + " The call did not run.");
            }
            continue; // chosen implicitly and it is gone: decide again (may open one)
        }
        if (d.kind == Dispatch::NotReachable)
            return jsonrpc_error_body(id, kInternalError, "OrcaSlicer window " + target.id + "'s MCP server is not reachable.");
        if (d.kind == Dispatch::NoReply)
            return jsonrpc_error_body(id, kInternalError, "OrcaSlicer did not reply; the call may have run. Check with get_state before repeating it.");

        const int status = *d.http.status;
        if (status == 202 || d.http.body.empty())
            return std::nullopt;
        json reply = json::parse(d.http.body, nullptr, false);
        if (reply.is_discarded() || !reply.is_object() || !reply.contains("jsonrpc"))
            return jsonrpc_error_body(id, kInternalError, "HTTP " + std::to_string(status) + ": " + d.http.body);
        if (reply.contains("result") && method == "tools/call") {
            rewrite_tool_result(reply["result"], target.id);
            if (r.multiple && !r.named && reply["result"].is_object() && reply["result"].contains("content") && reply["result"]["content"].is_array())
                reply["result"]["content"].push_back({{"type", "text"}, {"text", "(ran in OrcaSlicer window " + target.id + ")"}});
        } else if (reply.contains("error") && reply["error"].is_object() && reply["error"].contains("data")) {
            prefix_jobs(reply["error"]["data"], target.id);
        }
        return reply;
    }
    return jsonrpc_error_body(id, kInternalError, "No OrcaSlicer window could be reached; the call did not run.");
}

json BridgeCore::bridge_tool(const json& id, const std::string& name, const json& args, bool modern)
{
    auto wrap = [&](json result) { return jsonrpc_result_body(id, modern ? complete_result(std::move(result)) : std::move(result)); };

    if (name == "orca_list_instances") {
        const std::vector<Live> live = live_windows();
        std::string             selected;
        {
            std::lock_guard<std::mutex> lock(m_mu);
            selected = m_selected;
        }
        const bool selected_live = std::any_of(live.begin(), live.end(), [&](const Live& l) { return l.id == selected; });
        json       windows       = json::array();
        for (const Live& l : live) {
            const auto state = window_state(l, kStateBudgetS);
            json       w{{"id", l.id},
                         {"pid", l.rec.pid},
                         {"port", l.rec.port},
                         {"version", l.rec.version},
                         {"features", l.rec.features},
                         {"datadir", l.rec.datadir},
                         {"primary", l.rec.primary},
                         {"launched_by_me", l.rec.launched_by == m_d.bridge_id},
                         {"selected", selected_live && l.id == selected},
                         {"ready", state && state->value("ready", false)},
                         {"project", json()}};
            if (state && state->contains("project") && (*state)["project"].is_object())
                w["project"] = (*state)["project"].value("name", "");
            if (m_d.seed.is_object() && m_d.seed.contains("version") &&
                (l.rec.version != m_d.seed.value("version", "") || l.rec.features != m_d.seed.value("features", "")))
                w["differs_from_this_bridge_build"] = true;
            windows.push_back(std::move(w));
        }
        json out{{"windows", std::move(windows)}, {"default", json()}};
        if (selected_live)
            out["default"] = selected;
        else if (Resolution r = resolve("", ""); r.target)
            out["default"] = r.target->id;
        return wrap(tool_result(out));
    }

    if (name == "orca_use_instance") {
        if (!args.contains("id") || !(args["id"].is_string() || args["id"].is_null()))
            return wrap(tool_error("id must be a window id such as \"w2\", or null to clear the choice."));
        if (args["id"].is_null()) {
            std::lock_guard<std::mutex> lock(m_mu);
            m_selected.clear();
            return wrap(tool_result({{"default", nullptr}}));
        }
        const std::string       want = args["id"].get<std::string>();
        const std::vector<Live> live = live_windows();
        if (std::none_of(live.begin(), live.end(), [&](const Live& l) { return l.id == want; }))
            return wrap(tool_error(missing_window_message(want, live)));
        {
            std::lock_guard<std::mutex> lock(m_mu);
            m_selected = want;
        }
        return wrap(tool_result({{"default", want}}));
    }

    // orca_open_instance
    std::vector<std::string> files;
    if (args.contains("files")) {
        if (!args["files"].is_array())
            return wrap(tool_error("files must be an array of absolute paths."));
        for (const json& f : args["files"]) {
            if (!f.is_string() || !path_from_utf8(f.get<std::string>()).is_absolute())
                return wrap(tool_error("Every file must be an absolute path: " + f.dump()));
            files.push_back(f.get<std::string>());
        }
    }
    const bool select = !args.contains("select") || !args["select"].is_boolean() || args["select"].get<bool>();
    const bool others = !live_windows().empty();
    std::string err;
    auto        opened = launch_window(true, files, err);
    if (!opened)
        return wrap(tool_error(err));
    if (select) {
        std::lock_guard<std::mutex> lock(m_mu);
        m_selected = opened->id;
    }
    json out{{"id", opened->id}, {"pid", opened->rec.pid}, {"port", opened->rec.port}};
    if (others)
        out["warning"] = kAnotherWindowWarning;
    return wrap(tool_result(out));
}

}}} // namespace Slic3r::GUI::Mcp
