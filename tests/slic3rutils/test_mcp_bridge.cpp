#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/McpBridge.hpp"
#include "slic3r/GUI/Mcp/McpProtocol.hpp"
#include "slic3r/GUI/Mcp/McpTypes.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Slic3r::GUI::Mcp;
using nlohmann::json;
namespace fs = boost::filesystem;

namespace {

const char* kBridgeId = "bridge0000000001";

struct FakeWindow
{
    int         pid = 0;
    std::string incarnation;
    int         port = 0;
    std::string token = "tok";
    int64_t     ready_at = 0;
    bool        never_ready = false;
};

struct PostLog
{
    int         pid = 0;
    std::string method;
    std::string tool;
    std::string incarnation_header;
    json        body;
};

// A world of fake windows, a virtual clock and the bridge's injected functions. Nothing touches a process,
// a socket or the file system.
struct World
{
    std::mutex                  mu;
    std::vector<InstanceRecord> records;
    std::vector<FakeWindow>     windows; // what actually listens, by port
    std::vector<PostLog>        posts;
    std::vector<std::vector<std::string>> launches;
    std::atomic<int64_t>        clock{1000};
    std::atomic<int>            instance_reads{0};
    std::atomic<int>            lock_takes{0};
    int                         next_pid  = 4000;
    int                         next_port = 20000;
    int                         launch_ready_delay = 1000;
    bool                        launch_registers   = true;
    bool                        launch_exits_quickly = false;
    bool                        launch_never_ready = false;
    std::function<void()>       on_lock;                       // runs when the launch lock is taken
    std::function<std::optional<HttpResult>(const InstanceRecord&, const json&)> override_post;
    std::function<void(const InstanceRecord&)> on_not_sent;
    std::function<void(const InstanceRecord&)> on_unauthorized;
    std::vector<std::string>    log_lines;

    static std::string inc_for(int n) { return std::string(31, 'a') + char('0' + n % 10); }

    // Adds a record and the window behind it.
    InstanceRecord add(int pid, int started_at, bool primary = false, const std::string& launched_by = "", const std::string& incarnation = "")
    {
        std::lock_guard<std::mutex> lock(mu);
        return add_locked(pid, started_at, primary, launched_by, incarnation);
    }
    InstanceRecord add_locked(int pid, int64_t started_at, bool primary, const std::string& launched_by, const std::string& incarnation)
    {
        InstanceRecord r;
        r.pid         = pid;
        r.incarnation = incarnation.empty() ? inc_for(pid) : incarnation;
        r.port        = next_port++;
        r.token       = "tok" + std::to_string(r.port);
        r.version     = "2.5.0";
        r.features    = "";
        r.datadir     = "D";
        r.launched_by = launched_by;
        r.started_at  = started_at;
        r.primary     = primary;
        records.push_back(r);
        FakeWindow w;
        w.pid = pid; w.incarnation = r.incarnation; w.port = r.port; w.token = r.token;
        windows.push_back(w);
        return r;
    }
    void remove_record(int pid)
    {
        std::lock_guard<std::mutex> lock(mu);
        records.erase(std::remove_if(records.begin(), records.end(), [&](const InstanceRecord& r) { return r.pid == pid; }), records.end());
    }
    size_t posts_to(int pid, const std::string& method = "", const std::string& tool = "")
    {
        std::lock_guard<std::mutex> lock(mu);
        return size_t(std::count_if(posts.begin(), posts.end(), [&](const PostLog& p) {
            return p.pid == pid && (method.empty() || p.method == method) && (tool.empty() || p.tool == tool);
        }));
    }
    std::optional<PostLog> last_call(const std::string& tool)
    {
        std::lock_guard<std::mutex> lock(mu);
        for (auto it = posts.rbegin(); it != posts.rend(); ++it)
            if (it->tool == tool && it->method == "tools/call")
                return *it;
        return std::nullopt;
    }

    HttpResult reply(const json& id, const json& result, int status = 200)
    {
        HttpResult r;
        r.sent   = true;
        r.status = status;
        r.body   = json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump();
        return r;
    }
    HttpResult text_reply(const json& id, const json& data)
    {
        return reply(id, {{"content", json::array({{{"type", "text"}, {"text", data.dump()}}})}});
    }

    HttpResult post(const InstanceRecord& rec, const std::string& body_text, const std::map<std::string, std::string>& headers, int)
    {
        const json         body = json::parse(body_text);
        std::unique_lock<std::mutex> lock(mu);
        const FakeWindow*  win = nullptr;
        for (const FakeWindow& w : windows)
            if (w.port == rec.port)
                win = &w;
        PostLog entry;
        entry.pid    = win ? win->pid : -1;
        entry.method = body.value("method", "");
        entry.body   = body;
        if (entry.method == "tools/call")
            entry.tool = body["params"].value("name", "");
        if (auto it = headers.find("Mcp-Orca-Incarnation"); it != headers.end())
            entry.incarnation_header = it->second;
        if (win == nullptr) {
            posts.push_back(entry);
            auto cb = on_not_sent;
            lock.unlock();
            if (cb)
                cb(rec);
            return HttpResult{};
        }
        posts.push_back(entry);
        const FakeWindow w  = *win;
        const json       id = body.value("id", json());
        if (w.token != rec.token) {
            auto cb = on_unauthorized;
            lock.unlock();
            if (cb)
                cb(rec);
            HttpResult r;
            r.sent = true;
            r.status = 401;
            r.body   = json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32000}, {"message", "unauthorized"}}}}.dump();
            return r;
        }
        if (entry.incarnation_header != w.incarnation) {
            HttpResult r;
            r.sent   = true;
            r.status = 409;
            r.body   = json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32001}, {"message", "replaced"}}}}.dump();
            return r;
        }
        auto ov = override_post;
        lock.unlock();
        if (ov)
            if (auto r = ov(rec, body))
                return *r;
        if (entry.method == "ping")
            return reply(id, json::object());
        if (entry.method == "tools/call") {
            if (entry.tool == "get_state")
                return text_reply(id, {{"ready", !w.never_ready && clock.load() >= w.ready_at},
                                       {"project", {{"name", "P" + std::to_string(w.pid)}}},
                                       {"jobs", json::array({{{"id", "j1"}, {"tool", "slice"}, {"state", "running"}, {"parent", "j0"}}})}});
            json data{{"ran", entry.tool}, {"pid", w.pid}, {"args", body["params"].value("arguments", json::object())}};
            if (entry.tool == "slice") {
                data["job"]  = "j7";
                data["note"] = "Use wait {for:\"job\", job:\"j7\"} for its result.";
            }
            return text_reply(id, data);
        }
        return reply(id, json::object());
    }

    BridgeDeps deps(json seed = json())
    {
        BridgeDeps d;
        d.instances = [this] {
            ++instance_reads;
            std::lock_guard<std::mutex> lock(mu);
            return records;
        };
        d.post = [this](const InstanceRecord& r, const std::string& b, const std::map<std::string, std::string>& h, int t) { return post(r, b, h, t); };
        d.launch = [this](const std::vector<std::string>& args) {
            std::lock_guard<std::mutex> lock(mu);
            launches.push_back(args);
            LaunchResult res;
            if (launch_exits_quickly) {
                res.exited_quickly = true;
                return res;
            }
            res.started = true;
            if (launch_registers) {
                std::string by;
                for (size_t i = 0; i + 1 < args.size(); ++i)
                    if (args[i] == "--mcp-launched-by")
                        by = args[i + 1];
                InstanceRecord r = add_locked(next_pid++, clock.load() / 1000, false, by, "");
                windows.back().ready_at    = clock.load() + launch_ready_delay;
                windows.back().never_ready = launch_never_ready;
            }
            return res;
        };
        d.launch_lock = [this]() -> std::unique_ptr<LaunchLock> {
            ++lock_takes;
            if (on_lock)
                on_lock();
            return std::unique_ptr<LaunchLock>(new LaunchLock);
        };
        d.seed      = seed;
        d.log       = [this](const std::string& s) {
            std::lock_guard<std::mutex> lock(mu);
            log_lines.push_back(s);
        };
        d.now_ms    = [this] { return clock.load(); };
        d.sleep_ms  = [this](int ms) {
            clock += ms;
            std::this_thread::yield();
        };
        d.bridge_id = kBridgeId;
        return d;
    }
};

json small_seed()
{
    return {{"version", "2.5.0"},
            {"features", ""},
            {"instructions", "Use get_state first."},
            {"tools",
             json::array({{{"name", "get_state"}, {"description", "State."}, {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}},
                          {{"name", "wait"},
                           {"description", "Wait."},
                           {"inputSchema", {{"type", "object"}, {"properties", {{"job", {{"type", "string"}}}}}}}},
                          {{"name", "slice"}, {"description", "Slice."}, {"inputSchema", {{"type", "object"}, {"properties", json::object()}}}},
                          {{"name", "transform"},
                           {"description", "Move."},
                           {"inputSchema", {{"type", "object"}, {"properties", {{"instance", {{"type", "integer"}}}}}}}}})}};
}

fs::path real_seed_path() { return fs::path(std::string(PROFILES_DIR)).parent_path() / "mcp" / "catalog.json"; }

json real_seed()
{
    boost::nowide::ifstream in(real_seed_path().string().c_str(), std::ios::binary);
    return json::parse(in, nullptr, false);
}

// A bridge over a world; every reply line is collected.
struct Harness
{
    World                      world;
    std::unique_ptr<BridgeCore> core;
    std::mutex                 out_mu;
    std::vector<std::string>   out;
    int                        next_id = 1;

    explicit Harness(json seed = small_seed(), std::function<void(World&)> setup = nullptr)
    {
        if (setup)
            setup(world);
        core.reset(new BridgeCore(world.deps(std::move(seed))));
    }

    void write(const std::string& s)
    {
        std::lock_guard<std::mutex> lock(out_mu);
        out.push_back(s);
    }

    // Sends one request; the reply (null for none).
    json send(json msg)
    {
        if (!msg.contains("id") && msg.contains("method") && msg.value("_expect_reply", true))
            msg["id"] = next_id++;
        msg.erase("_expect_reply");
        msg["jsonrpc"] = "2.0";
        const size_t before = [&] { std::lock_guard<std::mutex> lock(out_mu); return out.size(); }();
        core->handle_line(msg.dump(), [this](const std::string& s) { write(s); });
        std::lock_guard<std::mutex> lock(out_mu);
        return out.size() > before ? json::parse(out.back()) : json();
    }

    json call(const std::string& tool, json args = json::object())
    {
        return send({{"method", "tools/call"}, {"params", {{"name", tool}, {"arguments", std::move(args)}}}});
    }
};

// The text of a tool result, parsed.
json result_data(const json& reply)
{
    REQUIRE(reply.contains("result"));
    return json::parse(reply["result"]["content"][0]["text"].get<std::string>());
}

std::string error_message(const json& reply)
{
    if (reply.contains("error"))
        return reply["error"]["message"].get<std::string>();
    if (reply.contains("result") && reply["result"].value("isError", false))
        return reply["result"]["content"][0]["text"].get<std::string>();
    return {};
}

json modern_meta() { return {{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}}; }

class NoopCaller final : public ToolCaller
{
public:
    ToolResult call(const ToolDef&, const json&) override { return ToolResult::ok(json::object()); }
};

} // namespace

// ---- phase 3: the local protocol ------------------------------------------------

TEST_CASE("the bridge answers initialize without any window", "[Mcp][McpBridge]")
{
    Harness h;
    for (const char* v : {"2025-11-25", "2025-06-18", "1999-01-01"}) {
        const json r = h.send({{"method", "initialize"}, {"params", {{"protocolVersion", v}}}});
        REQUIRE(r.contains("result"));
        CHECK(r["result"]["protocolVersion"] == (std::string(v) == "1999-01-01" ? "2025-11-25" : v));
        CHECK(r["result"]["capabilities"]["tools"]["listChanged"] == false);
        CHECK(r["result"]["serverInfo"]["name"] == "orcaslicer");
        const std::string instructions = r["result"]["instructions"];
        CHECK(instructions.rfind("Use get_state first.", 0) == 0);
        CHECK(instructions.find("orca_list_instances") != std::string::npos);
        CHECK(instructions.size() < 4096);
    }
    CHECK(h.world.instance_reads == 0);
    CHECK(h.world.launches.empty());
    CHECK(h.world.posts.empty());
}

TEST_CASE("the bridge keeps the seed's instructions whole and trims only its own paragraph", "[Mcp][McpBridge]")
{
    json seed             = small_seed();
    seed["instructions"]  = std::string(4000, 'x');
    Harness h(seed);
    const std::string got = h.send({{"method", "initialize"}, {"params", {{"protocolVersion", "2025-11-25"}}}})["result"]["instructions"];
    CHECK(got.size() <= 4096);
    CHECK(got.rfind(std::string(4000, 'x'), 0) == 0);
    CHECK(got.size() > 4000);
}

TEST_CASE("the bridge's server/discover, tools/list and ping match the server's", "[Mcp][McpBridge]")
{
    // A server registry, its serialization as the seed, and McpProtocol answering for the same info.
    ToolRegistry registry;
    ToolDef      echo;
    echo.name         = "echo";
    echo.description  = "Echo text back.";
    echo.input_schema = {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}};
    registry.add(echo);
    json seed{{"version", "9.9.9"}, {"features", ""}, {"instructions", "Hello."}, {"tools", tools_list_json(registry)}};
    Harness h(seed);
    NoopCaller caller;
    McpProtocol protocol(registry, caller, h.core->server_info());

    auto server = [&](const json& msg, const std::string& version, const std::string& method) {
        return json::parse(protocol.handle_post(PostRequest{msg.dump(), version, method, ""}).body);
    };
    const json discover_msg{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "server/discover"}, {"params", {{"_meta", modern_meta()}}}};
    CHECK(h.send({{"id", 1}, {"method", "server/discover"}, {"params", {{"_meta", modern_meta()}}}}) == server(discover_msg, "2026-07-28", "server/discover"));

    const json list_modern{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}, {"params", {{"_meta", modern_meta()}}}};
    json       bridge_list = h.send({{"id", 2}, {"method", "tools/list"}, {"params", {{"_meta", modern_meta()}}}});
    json       server_list = server(list_modern, "2026-07-28", "tools/list");
    json       bridge_tools = bridge_list["result"]["tools"], server_tools = server_list["result"]["tools"];
    bridge_list["result"].erase("tools");
    server_list["result"].erase("tools");
    CHECK(bridge_list == server_list);
    CHECK(bridge_tools.size() == server_tools.size() + 3);
    json first = bridge_tools[0];
    first["inputSchema"]["properties"].erase("orca_instance");
    CHECK(first == server_tools[0]);

    const json list_legacy{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "tools/list"}};
    json       bl = h.send({{"id", 3}, {"method", "tools/list"}});
    json       sl = server(list_legacy, "", "tools/list");
    CHECK(bl["result"].contains("tools"));
    CHECK_FALSE(bl["result"].contains("resultType"));
    CHECK(bl["result"].size() == sl["result"].size());

    CHECK(h.send({{"id", 4}, {"method", "ping"}}) == server({{"jsonrpc", "2.0"}, {"id", 4}, {"method", "ping"}}, "", "ping"));
    CHECK(h.send({{"id", 5}, {"method", "ping"}, {"params", {{"_meta", modern_meta()}}}}) ==
          server({{"jsonrpc", "2.0"}, {"id", 5}, {"method", "ping"}, {"params", {{"_meta", modern_meta()}}}}, "2026-07-28", "ping"));

    // A modern request for a revision the server does not speak.
    const json bad = h.send({{"id", 6}, {"method", "tools/list"}, {"params", {{"_meta", {{"io.modelcontextprotocol/protocolVersion", "2030-01-01"}}}}}});
    CHECK(bad["error"]["code"] == -32022);

    // Notifications get no reply.
    CHECK(h.send({{"method", "notifications/initialized"}, {"_expect_reply", false}}).is_null());
    CHECK(h.world.instance_reads == 0);
    CHECK(h.world.launches.empty());
    CHECK(h.world.posts.empty());
}

TEST_CASE("every shipped tool gets orca_instance and transform keeps its integer instance", "[Mcp][McpBridge]")
{
    const json seed = real_seed();
    REQUIRE(seed.is_object());
    Harness    h(seed);
    const json tools = h.send({{"method", "tools/list"}})["result"]["tools"];
    REQUIRE(tools.size() == seed["tools"].size() + 3);
    bool saw_transform = false;
    for (size_t i = 0; i < seed["tools"].size(); ++i) {
        const json& tool = tools[i];
        INFO(tool["name"]);
        CHECK(tool["name"] == seed["tools"][i]["name"]);
        const json& prop = tool["inputSchema"]["properties"]["orca_instance"];
        CHECK(prop["type"] == "string");
        if (tool["name"] == "transform") {
            saw_transform = true;
            CHECK(tool["inputSchema"]["properties"]["instance"]["type"] == "integer");
        }
        // Nothing else of the tool changed.
        json copy = tool;
        copy["inputSchema"]["properties"].erase("orca_instance");
        CHECK(copy == seed["tools"][i]);
    }
    CHECK(saw_transform);
    std::vector<std::string> tail;
    for (size_t i = seed["tools"].size(); i < tools.size(); ++i)
        tail.push_back(tools[i]["name"]);
    CHECK(tail == std::vector<std::string>{"orca_list_instances", "orca_open_instance", "orca_use_instance"});
}

TEST_CASE("a catalog tool that collides with the bridge's names is left out and logged", "[Mcp][McpBridge]")
{
    json seed = small_seed();
    seed["tools"].push_back({{"name", "orca_thing"}, {"description", "x"}, {"inputSchema", {{"type", "object"}}}});
    seed["tools"].push_back({{"name", "owns_it"},
                             {"description", "x"},
                             {"inputSchema", {{"type", "object"}, {"properties", {{"orca_instance", {{"type", "integer"}}}}}}}});
    Harness    h(seed);
    const json tools = h.send({{"method", "tools/list"}})["result"]["tools"];
    std::set<std::string> names;
    for (const json& t : tools)
        names.insert(t["name"].get<std::string>());
    CHECK_FALSE(names.count("orca_thing"));
    CHECK_FALSE(names.count("owns_it"));
    CHECK(names.count("get_state"));
    CHECK(h.world.log_lines.size() == 2);
}

TEST_CASE("without a catalog the bridge lists only its own tools and says so", "[Mcp][McpBridge]")
{
    Harness h{json()};
    const json init = h.send({{"method", "initialize"}, {"params", {{"protocolVersion", "2025-11-25"}}}});
    CHECK(init["result"]["instructions"].get<std::string>().find("catalog") != std::string::npos);
    CHECK(h.send({{"method", "tools/list"}})["result"]["tools"].size() == 3);
}

TEST_CASE("other methods without a window are not found, and nothing launches", "[Mcp][McpBridge]")
{
    Harness    h;
    const json r = h.send({{"method", "resources/list"}});
    CHECK(r["error"]["code"] == -32601);
    CHECK(h.world.launches.empty());
}

// ---- phase 4: routing -----------------------------------------------------------

TEST_CASE("window ids follow first sight, ordered by start time, and are never reused", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(300, 50);
    h.world.add(200, 10); // older although listed second
    const json list = result_data(h.call("orca_list_instances"));
    REQUIRE(list["windows"].size() == 2);
    CHECK(list["windows"][0]["id"] == "w1");
    CHECK(list["windows"][0]["pid"] == 200);
    CHECK(list["windows"][1]["id"] == "w2");
    CHECK(list["windows"][1]["pid"] == 300);
    // w1 closes, a new window appears: it is w3.
    h.world.remove_record(200);
    h.world.add(400, 60);
    const json again = result_data(h.call("orca_list_instances"));
    REQUIRE(again["windows"].size() == 2);
    CHECK(again["windows"][0]["id"] == "w2");
    CHECK(again["windows"][1]["id"] == "w3");
}

TEST_CASE("the resolution order of a tool call", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10, /*primary*/ true);
    h.world.add(102, 20);
    h.world.add(103, 30);
    auto ran_in = [&](const json& reply) { return result_data(reply)["pid"].get<int>(); };

    SECTION("several windows, no choice: the primary one, and a note says where it ran")
    {
        const json r = h.call("slice");
        CHECK(ran_in(r) == 101);
        CHECK(r["result"]["content"].back()["text"] == "(ran in OrcaSlicer window w1)");
    }
    SECTION("several windows, none primary: the oldest")
    {
        h.world.records[0].primary = false;
        CHECK(ran_in(h.call("slice")) == 101);
        h.world.records[0].started_at = 99;
        h.call("orca_list_instances"); // ids are fixed already
        CHECK(ran_in(h.call("slice")) == 102);
    }
    SECTION("the window this bridge launched beats the primary and the oldest")
    {
        h.world.add(104, 40, false, kBridgeId);
        CHECK(ran_in(h.call("slice")) == 104);
    }
    SECTION("the selected window beats the one this bridge launched")
    {
        h.world.add(104, 40, false, kBridgeId);
        h.call("orca_list_instances"); // w1..w4
        CHECK_FALSE(h.call("orca_use_instance", {{"id", "w3"}}).contains("error"));
        CHECK(ran_in(h.call("slice")) == 103);
        CHECK(h.call("slice")["result"]["content"].back()["text"] == "(ran in OrcaSlicer window w3)");
    }
    SECTION("a window named by orca_instance beats everything, and the argument is not forwarded")
    {
        h.call("orca_use_instance", {{"id", "w3"}});
        const json r = h.call("slice", {{"orca_instance", "w2"}});
        CHECK(ran_in(r) == 102);
        CHECK(h.world.last_call("slice")->body["params"]["arguments"].is_object());
        CHECK_FALSE(h.world.last_call("slice")->body["params"]["arguments"].contains("orca_instance"));
        // No note when the caller named the window.
        CHECK(r["result"]["content"].size() == 1);
    }
    SECTION("a job id with a window prefix goes to that window, with the id restored")
    {
        const json r = h.call("wait", {{"for", "job"}, {"job", "w3.j9"}});
        CHECK(ran_in(r) == 103);
        CHECK(h.world.last_call("wait")->body["params"]["arguments"]["job"] == "j9");
    }
    SECTION("an unknown or closed window is an error naming the live ones, and nothing runs")
    {
        const json r = h.call("slice", {{"orca_instance", "w9"}});
        const std::string m = error_message(r);
        CHECK(m.find("w9") != std::string::npos);
        CHECK(m.find("w1 (pid 101)") != std::string::npos);
        CHECK(m.find("w3 (pid 103)") != std::string::npos);
        CHECK_FALSE(h.world.last_call("slice").has_value());
        CHECK(h.world.launches.empty());
    }
    SECTION("a non-string orca_instance is refused")
    {
        CHECK(h.call("slice", {{"orca_instance", 2}})["error"]["code"] == -32602);
    }
}

TEST_CASE("with one window there is no note and no launch", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10);
    const json r = h.call("slice");
    CHECK(r["result"]["content"].size() == 1);
    CHECK(h.world.launches.empty());
    CHECK(h.world.lock_takes == 0);
}

TEST_CASE("a reused pid with a new incarnation is never reached through the old id", "[Mcp][McpBridge]")
{
    Harness h;
    const InstanceRecord old = h.world.add(500, 10, false, "", std::string(32, '1'));
    h.world.add(600, 20);
    h.call("orca_list_instances"); // w1 = (500, 1111), w2 = (600)
    // The window on pid 500 died and its pid and port went to a new window; the dead one's record is still listed.
    {
        std::lock_guard<std::mutex> lock(h.world.mu);
        h.world.windows[0].incarnation = std::string(32, '2');
    }
    const json r = h.call("slice", {{"orca_instance", "w1"}});
    CHECK(error_message(r).find("w1") != std::string::npos);
    CHECK(error_message(r).find("did not run") != std::string::npos);
    CHECK(h.world.posts_to(600, "tools/call", "slice") == 0);
    // The server checked the incarnation before running anything; the id stays gone.
    const json again = h.call("slice", {{"orca_instance", "w1"}});
    CHECK(error_message(again).find("not running") != std::string::npos);
    // An implicit call is decided again and lands in a live window instead.
    CHECK(result_data(h.call("slice"))["pid"] == 600);
    (void) old;
}

// ---- job ids ----------------------------------------------------------------------

TEST_CASE("job ids in results get their window prefix", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10);
    h.world.add(102, 20);

    const json started = h.call("slice", {{"orca_instance", "w2"}});
    CHECK(result_data(started)["job"] == "w2.j7");
    CHECK(result_data(started)["note"] == "Use wait {for:\"job\", job:\"w2.j7\"} for its result.");

    const json state = result_data(h.call("get_state", {{"orca_instance", "w1"}}));
    CHECK(state["jobs"][0]["id"] == "w1.j1");
    CHECK(state["jobs"][0]["parent"] == "w1.j0");
    CHECK(state["jobs"][0]["tool"] == "slice"); // other fields untouched
}

TEST_CASE("job ids in errors get their window prefix", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10);
    h.world.override_post = [&](const InstanceRecord&, const json& body) -> std::optional<HttpResult> {
        if (body.value("method", "") != "tools/call" || body["params"].value("name", "") != "slice")
            return std::nullopt;
        HttpResult r;
        r.sent   = true;
        r.status = 200;
        const json id = body["id"];
        if (body["params"]["arguments"].contains("rpc"))
            r.body = json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", -32602}, {"message", "busy"}, {"data", {{"job", "j3"}}}}}}.dump();
        else
            r.body = json{{"jsonrpc", "2.0"},
                          {"id", id},
                          {"result", {{"isError", true},
                                      {"content", json::array({{{"type", "text"}, {"text", "Busy with slice\n" + json{{"job", "j3"}}.dump()}}})}}}}
                         .dump();
        return r;
    };
    const json tool_error = h.call("slice");
    CHECK(tool_error["result"]["content"][0]["text"] == "Busy with slice\n{\"job\":\"w1.j3\"}");
    CHECK(tool_error["result"]["isError"] == true);
    const json rpc_error = h.call("slice", {{"rpc", true}});
    CHECK(rpc_error["error"]["data"]["job"] == "w1.j3");
}

TEST_CASE("an unprefixed job id goes to the only window, and fails with several", "[Mcp][McpBridge]")
{
    SECTION("one window")
    {
        Harness h;
        h.world.add(101, 10);
        const json r = h.call("wait", {{"for", "job"}, {"job", "j4"}});
        CHECK(result_data(r)["pid"] == 101);
        CHECK(h.world.last_call("wait")->body["params"]["arguments"]["job"] == "j4");
    }
    SECTION("two windows")
    {
        Harness h;
        h.world.add(101, 10);
        h.world.add(102, 20);
        const json        r = h.call("wait", {{"for", "job"}, {"job", "j4"}});
        const std::string m = error_message(r);
        CHECK(m.find("no window prefix") != std::string::npos);
        CHECK(m.find("w1") != std::string::npos);
        CHECK(m.find("w2") != std::string::npos);
        CHECK_FALSE(h.world.last_call("wait").has_value());
    }
}

TEST_CASE("a job prefix that disagrees with orca_instance is refused", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10);
    h.world.add(102, 20);
    const json r = h.call("wait", {{"job", "w1.j2"}, {"orca_instance", "w2"}});
    CHECK(r["error"]["code"] == -32602);
    CHECK_FALSE(h.world.last_call("wait").has_value());
}

// ---- auto-launch ------------------------------------------------------------------

TEST_CASE("the first call launches OrcaSlicer, waits for ready and forwards", "[Mcp][McpBridge]")
{
    Harness h;
    const json r = h.call("slice");
    REQUIRE(h.world.launches.size() == 1);
    const auto& args = h.world.launches[0];
    CHECK(args == std::vector<std::string>{"--mcp", "--mcp-launched-by", kBridgeId});
    CHECK(result_data(r)["ran"] == "slice");
    CHECK(h.world.lock_takes == 1);
    // It waited for get_state to say ready before forwarding.
    CHECK(h.world.posts_to(4000, "tools/call", "get_state") >= 2);
    CHECK(h.world.clock >= 2000);
    // The next call reuses the window.
    h.call("slice");
    CHECK(h.world.launches.size() == 1);
}

TEST_CASE("--datadir is passed on to launched windows", "[Mcp][McpBridge]")
{
    World       world;
    BridgeDeps  d = world.deps(small_seed());
    d.datadir_arg = "D:\\data";
    BridgeCore core(d);
    core.handle_line(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/call"}, {"params", {{"name", "slice"}}}}.dump(), [](const std::string&) {});
    REQUIRE(world.launches.size() == 1);
    CHECK(world.launches[0] == std::vector<std::string>{"--mcp", "--mcp-launched-by", kBridgeId, "--datadir", "D:\\data"});
}

TEST_CASE("concurrent first calls share one launch", "[Mcp][McpBridge]")
{
    Harness h;
    std::vector<std::thread> threads;
    std::mutex               m;
    std::vector<json>        replies;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&, i] {
            json out;
            h.core->handle_line(json{{"jsonrpc", "2.0"}, {"id", i}, {"method", "tools/call"}, {"params", {{"name", "slice"}}}}.dump(),
                                [&](const std::string& s) { out = json::parse(s); });
            std::lock_guard<std::mutex> lock(m);
            replies.push_back(out);
        });
    for (auto& t : threads)
        t.join();
    CHECK(h.world.launches.size() == 1);
    REQUIRE(replies.size() == 8);
    for (const json& r : replies)
        CHECK(result_data(r)["ran"] == "slice");
}

TEST_CASE("a window launched by another bridge while waiting for the lock is used, not duplicated", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.on_lock = [&] {
        // Another bridge got there first (the lock's holder launched, then released).
        h.world.add_locked(7000, 1, false, "otherbridge0000000", "");
    };
    const json r = h.call("slice");
    CHECK(h.world.launches.empty());
    CHECK(h.world.lock_takes == 1);
    CHECK(result_data(r)["pid"] == 7000);
}

TEST_CASE("a launch that a non-serving OrcaSlicer takes over fails at once with the preference to turn on", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.launch_exits_quickly = true;
    const int64_t before = h.world.clock;
    const json    r      = h.call("slice");
    CHECK(error_message(r).find("MCP server off") != std::string::npos);
    CHECK(error_message(r).find("Preferences > General > AI features") != std::string::npos);
    CHECK(h.world.clock - before < 1000);
    // The next call tries again.
    h.call("slice");
    CHECK(h.world.launches.size() == 2);
}

TEST_CASE("a window that never answers times out after 60 s and the next call launches again", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.launch_registers = false;
    const int64_t before = h.world.clock;
    const json    r      = h.call("slice");
    CHECK(error_message(r).find("60 seconds") != std::string::npos);
    CHECK(h.world.clock - before >= 60000);
    CHECK(h.world.launches.size() == 1);
    CHECK(h.world.lock_takes == 1);

    h.world.launch_registers = true;
    const json again = h.call("slice");
    CHECK(h.world.launches.size() == 2);
    CHECK(result_data(again)["ran"] == "slice");
}

TEST_CASE("a window that is not ready after 45 s still gets the call", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.launch_never_ready = true;
    const int64_t before       = h.world.clock;
    const json    r            = h.call("slice");
    CHECK(h.world.clock - before >= 45000);
    CHECK(result_data(r)["ran"] == "slice");
}

// ---- bridge tools ---------------------------------------------------------------------

TEST_CASE("orca_list_instances reports each window", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10, true);
    h.world.add(102, 20, false, kBridgeId);
    h.world.records[1].version = "2.4.0";
    h.call("orca_use_instance", {{"id", "w1"}});
    const json list = result_data(h.call("orca_list_instances"));
    REQUIRE(list["windows"].size() == 2);
    const json& w1 = list["windows"][0];
    const json& w2 = list["windows"][1];
    CHECK(w1["primary"] == true);
    CHECK(w1["selected"] == true);
    CHECK(w1["launched_by_me"] == false);
    CHECK(w1["ready"] == true);
    CHECK(w1["project"] == "P101");
    CHECK(w1["port"].is_number());
    CHECK_FALSE(w1.contains("token"));
    CHECK(w2["launched_by_me"] == true);
    CHECK(w2["selected"] == false);
    CHECK(w2["differs_from_this_bridge_build"] == true);
    CHECK_FALSE(w1.contains("differs_from_this_bridge_build"));
    CHECK(list["default"] == "w1");
    // The listing never launches anything.
    CHECK(h.world.launches.empty());
}

TEST_CASE("orca_list_instances with no window lists none and does not launch", "[Mcp][McpBridge]")
{
    Harness    h;
    const json list = result_data(h.call("orca_list_instances"));
    CHECK(list["windows"].empty());
    CHECK(list["default"].is_null());
    CHECK(h.world.launches.empty());
}

TEST_CASE("orca_open_instance always opens a new window and warns about the Bambu account", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10);
    const std::string file = (fs::temp_directory_path() / "a.3mf").string();
    const json        r    = result_data(h.call("orca_open_instance", {{"files", json::array({file})}}));
    REQUIRE(h.world.launches.size() == 1);
    const auto& args = h.world.launches[0];
    CHECK(std::find(args.begin(), args.end(), "--no-single-instance") != args.end());
    CHECK(args.back() == file);
    CHECK(std::find(args.begin(), args.end(), "--mcp-launched-by") != args.end());
    CHECK(r["id"] == "w2");
    CHECK(r["pid"] == 4000);
    CHECK(r["port"].is_number());
    CHECK(r["warning"].get<std::string>().find("Bambu account") != std::string::npos);
    // It became the default window.
    CHECK(result_data(h.call("slice"))["pid"] == 4000);
}

TEST_CASE("orca_open_instance with select:false leaves the default alone; the first window has no warning", "[Mcp][McpBridge]")
{
    Harness    h;
    const json first = result_data(h.call("orca_open_instance"));
    CHECK_FALSE(first.contains("warning"));
    CHECK(first["id"] == "w1");
    const json second = result_data(h.call("orca_open_instance", {{"select", false}}));
    CHECK(second["id"] == "w2");
    CHECK(second.contains("warning"));
    CHECK(h.world.launches.size() == 2);
    // The first, selected window is still the default.
    CHECK(result_data(h.call("slice"))["pid"] == 4000);
}

TEST_CASE("orca_open_instance refuses relative file paths", "[Mcp][McpBridge]")
{
    Harness    h;
    const json r = h.call("orca_open_instance", {{"files", json::array({"relative.3mf"})}});
    CHECK(r["result"]["isError"] == true);
    CHECK(h.world.launches.empty());
}

TEST_CASE("orca_use_instance sets and clears the default, and rejects unknown ids", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10);
    h.world.add(102, 20);
    CHECK(result_data(h.call("orca_use_instance", {{"id", "w2"}}))["default"] == "w2");
    CHECK(result_data(h.call("slice"))["pid"] == 102);
    CHECK(result_data(h.call("orca_use_instance", {{"id", nullptr}}))["default"].is_null());
    CHECK(result_data(h.call("slice"))["pid"] == 101);
    const json bad = h.call("orca_use_instance", {{"id", "w7"}});
    CHECK(bad["result"]["isError"] == true);
    CHECK(error_message(bad).find("w1 (pid 101)") != std::string::npos);
    // The selected window closing falls back to the normal order.
    h.call("orca_use_instance", {{"id", "w2"}});
    h.world.remove_record(102);
    CHECK(result_data(h.call("slice"))["pid"] == 101);
}

TEST_CASE("modern-era bridge tool results carry resultType", "[Mcp][McpBridge]")
{
    Harness    h;
    const json r = h.send({{"method", "tools/call"},
                           {"params", {{"name", "orca_list_instances"}, {"arguments", json::object()}, {"_meta", modern_meta()}}}});
    CHECK(r["result"]["resultType"] == "complete");
}

// ---- retry rules ------------------------------------------------------------------------

TEST_CASE("a request that was not sent is sent again to the same window with its current record", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10);
    h.world.add(102, 20);
    h.call("orca_list_instances");
    // Window w1's record points at a dead port; its real port is in the file by the time of the retry.
    int real_port = 0;
    {
        std::lock_guard<std::mutex> lock(h.world.mu);
        real_port                 = h.world.records[0].port;
        h.world.records[0].port   = 1;
    }
    h.world.on_not_sent = [&](const InstanceRecord&) {
        std::lock_guard<std::mutex> lock(h.world.mu);
        h.world.records[0].port = real_port;
    };
    const json r = h.call("slice", {{"orca_instance", "w1"}});
    CHECK(result_data(r)["pid"] == 101);
    CHECK(h.world.posts_to(101, "tools/call", "slice") == 1);
    CHECK(h.world.posts_to(102, "tools/call", "slice") == 0);
}

TEST_CASE("an unreachable window is an error after one more try", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10);
    {
        std::lock_guard<std::mutex> lock(h.world.mu);
        h.world.records[0].port = 1;
    }
    const json r = h.call("slice");
    CHECK(error_message(r).find("not reachable") != std::string::npos);
    CHECK(h.world.posts_to(-1, "tools/call", "slice") == 2);
}

TEST_CASE("an implicitly chosen window that vanished is decided again", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10);
    // Its record points at a dead port, and by the retry the window's record is gone and another window exists.
    {
        std::lock_guard<std::mutex> lock(h.world.mu);
        h.world.records[0].port = 1;
    }
    h.world.on_not_sent = [&](const InstanceRecord&) {
        h.world.remove_record(101);
        h.world.add(102, 20);
    };
    const json r = h.call("slice");
    CHECK(result_data(r)["pid"] == 102);
    CHECK(h.world.launches.empty());
}

TEST_CASE("a refused token is retried once with the same window's new record and never rerouted", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10);
    h.world.add(102, 20);
    h.call("orca_list_instances");
    {
        std::lock_guard<std::mutex> lock(h.world.mu);
        h.world.records[0].token = "stale";
    }
    h.world.on_unauthorized = [&](const InstanceRecord&) {
        std::lock_guard<std::mutex> lock(h.world.mu);
        h.world.records[0].token = h.world.windows[0].token; // the rotated token, now in the file
    };
    const json r = h.call("slice", {{"orca_instance", "w1"}});
    CHECK(result_data(r)["pid"] == 101);
    CHECK(h.world.posts_to(101, "tools/call", "slice") == 2);
    CHECK(h.world.posts_to(102, "tools/call", "slice") == 0);

    // A token that stays wrong: the second 401 is returned, not retried further and not rerouted.
    {
        std::lock_guard<std::mutex> lock(h.world.mu);
        h.world.records[0].token = "stale";
    }
    h.world.on_unauthorized = nullptr;
    const json bad = h.call("slice", {{"orca_instance", "w1"}});
    CHECK(bad["error"]["message"] == "unauthorized");
    CHECK(h.world.posts_to(101, "tools/call", "slice") == 4);
    CHECK(h.world.posts_to(102, "tools/call", "slice") == 0);
}

TEST_CASE("a request that was sent but not answered is never sent again", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10);
    h.world.override_post = [](const InstanceRecord&, const json& body) -> std::optional<HttpResult> {
        if (body.value("method", "") == "tools/call" && body["params"].value("name", "") == "slice") {
            HttpResult r;
            r.sent = true; // the request went out; no reply came
            return r;
        }
        return std::nullopt;
    };
    const json r = h.call("slice");
    CHECK(error_message(r) == "OrcaSlicer did not reply; the call may have run. Check with get_state before repeating it.");
    CHECK(h.world.posts_to(101, "tools/call", "slice") == 1);
    CHECK(h.world.launches.empty());
}

// ---- concurrency ------------------------------------------------------------------------

TEST_CASE("concurrent calls write whole, parseable lines", "[Mcp][McpBridge]")
{
    Harness h;
    h.world.add(101, 10);
    h.world.add(102, 20);
    std::mutex               out_mu;
    std::vector<std::string> lines;
    std::string              interleaved;
    // The write callback must be called with one whole line per call; a writer that is not itself locked would see
    // calls overlap only if the bridge split a reply, so the check is that every call is one parseable object.
    auto write = [&](const std::string& s) {
        std::lock_guard<std::mutex> lock(out_mu);
        lines.push_back(s);
    };
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
        threads.emplace_back([&, t] {
            for (int i = 0; i < 25; ++i) {
                const int id = t * 100 + i;
                const json msg = i % 3 == 0 ? json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/list"}}
                                 : i % 3 == 1 ? json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "tools/call"}, {"params", {{"name", "slice"}, {"arguments", {{"orca_instance", t % 2 ? "w1" : "w2"}}}}}}
                                              : json{{"jsonrpc", "2.0"}, {"id", id}, {"method", "ping"}};
                h.core->handle_line(msg.dump(), write);
            }
        });
    for (auto& t : threads)
        t.join();
    REQUIRE(lines.size() == 200);
    std::set<int> ids;
    for (const std::string& l : lines) {
        const json j = json::parse(l, nullptr, false);
        REQUIRE(j.is_object());
        REQUIRE(j.contains("id"));
        ids.insert(j["id"].get<int>());
        CHECK((j.contains("result") || j.contains("error")));
    }
    CHECK(ids.size() == 200);
}
