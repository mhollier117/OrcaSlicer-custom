#include "McpStdioBridge.hpp"
#include "McpBridge.hpp"
#include "McpInstances.hpp"
#include "McpPaths.hpp"
#include "McpToken.hpp"

#include "McpOptions.hpp"
#include "libslic3r/Utils.hpp"
#include "libslic3r_version.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core/error.hpp>
#include <boost/beast/core/flat_buffer.hpp>
#include <boost/beast/core/tcp_stream.hpp>
#include <boost/beast/http/field.hpp>
#include <boost/beast/http/message.hpp>
#include <boost/beast/http/read.hpp>
#include <boost/beast/http/string_body.hpp>
#include <boost/beast/http/verb.hpp>
#include <boost/beast/http/write.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/nowide/cstdlib.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/nowide/iostream.hpp>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>
#include <vector>
#include <exception>
#include <optional>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace fs   = boost::filesystem;
namespace net  = boost::asio;
namespace http = boost::beast::http;
using tcp      = net::ip::tcp;

namespace {

std::string base64(const std::string& in)
{
    static const char* table = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string        out;
    int                val = 0, bits = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(table[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6)
        out.push_back(table[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4)
        out.push_back('=');
    return out;
}

bool header_safe(const std::string& v)
{
    if (v.empty() || v.front() == ' ' || v.back() == ' ' || (v.rfind("=?base64?", 0) == 0))
        return false;
    for (unsigned char c : v)
        if (c < 0x20 || c > 0x7E)
            return false;
    return true;
}

std::string self_executable(char** argv)
{
    // An AppImage is relaunched through its runtime, not the extracted binary.
    if (const char* appimage = boost::nowide::getenv("APPIMAGE"); appimage && *appimage)
        return appimage;
#ifdef _WIN32
    wchar_t exe[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    return path_to_utf8(fs::path(exe));
#elif defined(__APPLE__)
    char     exe[4096] = {0};
    uint32_t size      = sizeof(exe);
    return _NSGetExecutablePath(exe, &size) == 0 ? std::string(exe) : std::string(argv[0]);
#else
    char    exe[4096] = {0};
    ssize_t n         = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    return n > 0 ? std::string(exe, std::size_t(n)) : std::string(argv[0]);
#endif
}

// POSTs one message to a window's endpoint; reports whether it left this process (a partial write counts as sent,
// so the caller never resends a call the window may have run) and the reply, when one came.
HttpResult post_to_window(const InstanceRecord& rec, const std::string& body, const std::map<std::string, std::string>& headers,
                          int read_timeout_s)
{
    HttpResult out;
    // Deadlines only apply to Beast's asynchronous operations, so each step runs as one on a
    // private io_context. A tool replies within its budget (45 s) and `wait` within 50 s.
    try {
        net::io_context                   ioc;
        boost::beast::tcp_stream          stream(ioc);
        http::request<http::string_body>  req{http::verb::post, "/mcp", 11};
        boost::beast::flat_buffer         buffer;
        http::response<http::string_body> res;
        bool                              ok = false;
        req.set(http::field::host, "127.0.0.1:" + std::to_string(rec.port));
        req.set(http::field::content_type, "application/json");
        req.set(http::field::accept, "application/json, text/event-stream");
        req.set(http::field::authorization, "Bearer " + rec.token);
        for (const auto& [k, v] : headers)
            req.set(k, v);
        req.body() = body;
        req.prepare_payload();
        stream.expires_after(std::chrono::seconds(5));
        stream.async_connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), uint16_t(rec.port)), [&](boost::beast::error_code ec) {
            if (ec)
                return;
            stream.expires_after(std::chrono::seconds(30));
            out.sent = true;
            http::async_write(stream, req, [&](boost::beast::error_code ec, std::size_t) {
                if (ec)
                    return;
                stream.expires_after(std::chrono::seconds(read_timeout_s));
                http::async_read(stream, buffer, res, [&](boost::beast::error_code ec, std::size_t) { ok = !ec; });
            });
        });
        ioc.run();
        if (ok) {
            out.status = int(res.result_int());
            out.body   = res.body();
            for (const auto& f : res)
                out.headers[std::string(f.name_string())] = std::string(f.value());
        }
    } catch (const std::exception&) {
    }
    return out;
}

#ifdef _WIN32
// One argument as CommandLineToArgvW reads it back.
std::wstring quote_arg(const std::wstring& a)
{
    std::wstring out = L"\"";
    std::size_t  backslashes = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}
#endif

// Starts `exe <extra_args>` detached from this process. exited_quickly: it ended within a moment (a running
// instance without MCP took the launch over).
LaunchResult launch_app(const std::string& exe, const std::vector<std::string>& extra_args)
{
    LaunchResult result;
#ifdef _WIN32
    std::wstring cmd = quote_arg(boost::nowide::widen(exe));
    for (const std::string& a : extra_args)
        cmd += L" " + quote_arg(boost::nowide::widen(a));
    STARTUPINFOW        si{};
    PROCESS_INFORMATION pi{};
    si.cb = sizeof(si);
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &si, &pi))
        return result;
    const DWORD wait = WaitForSingleObject(pi.hProcess, 5000);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    result.started        = wait == WAIT_TIMEOUT;
    result.exited_quickly = wait != WAIT_TIMEOUT;
    return result;
#else
    const pid_t pid = fork();
    if (pid < 0)
        return result;
    if (pid == 0) {
        setsid();
        // The GUI must not inherit the MCP stdio pipes.
        std::freopen("/dev/null", "r", stdin);
        std::freopen("/dev/null", "w", stdout);
        std::vector<std::string> args{exe};
        args.insert(args.end(), extra_args.begin(), extra_args.end());
        std::vector<char*> cargs;
        for (std::string& a : args)
            cargs.push_back(a.data());
        cargs.push_back(nullptr);
        execv(exe.c_str(), cargs.data());
        _exit(127);
    }
    for (int i = 0; i < 50; ++i) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            result.exited_quickly = true;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    // The app stays this process's child, so reap it whenever it exits: an app quit while the bridge
    // still runs would otherwise stay a zombie until the bridge exits.
    std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
    result.started = true;
    return result;
#endif
}

// The interprocess lock that keeps two bridges (two Claude sessions) from launching OrcaSlicer at once. The
// thread that takes it releases it.
class ProcessLaunchLock : public LaunchLock
{
public:
    ProcessLaunchLock()
    {
#ifdef _WIN32
        m_mutex = CreateMutexW(nullptr, FALSE, L"Local\\OrcaSlicerMcpLaunch");
        if (m_mutex != nullptr) {
            const DWORD r = WaitForSingleObject(m_mutex, 120000);
            m_held        = r == WAIT_OBJECT_0 || r == WAIT_ABANDONED;
        }
#else
        const fs::path dir = mcp_dir(default_datadir());
        boost::system::error_code ec;
        fs::create_directories(dir, ec);
        m_fd = ::open(path_to_utf8(dir / "launch.lock").c_str(), O_RDWR | O_CREAT, 0600);
        if (m_fd >= 0) {
            for (int i = 0; i < 1200 && !m_held; ++i) {
                m_held = ::flock(m_fd, LOCK_EX | LOCK_NB) == 0;
                if (!m_held)
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
#endif
    }
    ~ProcessLaunchLock() override
    {
#ifdef _WIN32
        if (m_mutex != nullptr) {
            if (m_held)
                ReleaseMutex(m_mutex);
            CloseHandle(m_mutex);
        }
#else
        if (m_fd >= 0) {
            if (m_held)
                ::flock(m_fd, LOCK_UN);
            ::close(m_fd);
        }
#endif
    }

private:
#ifdef _WIN32
    HANDLE m_mutex = nullptr;
#else
    int m_fd = -1;
#endif
    bool m_held = false;
};

// resources/mcp/catalog.json next to the program, wherever the platform keeps resources.
json load_seed(const std::string& exe)
{
    const fs::path dir = path_from_utf8(exe).parent_path();
    for (const fs::path& candidate : {dir / "resources", dir.parent_path() / "resources", dir.parent_path().parent_path() / "resources",
                                      dir.parent_path() / "Resources"}) {
        boost::nowide::ifstream in(path_to_utf8(candidate / "mcp" / "catalog.json").c_str(), std::ios::binary);
        if (!in)
            continue;
        json seed = json::parse(in, nullptr, false);
        if (seed.is_object())
            return seed;
    }
    return json();
}

void write_line(const std::string& s)
{
    boost::nowide::cout << s << "\n";
    boost::nowide::cout.flush();
}

} // namespace

std::map<std::string, std::string> bridge_headers(const json& message, const std::string& negotiated_version)
{
    std::map<std::string, std::string> h;
    if (!message.is_object())
        return h;
    const std::string                  method = message.value("method", "");
    const json                         params = message.contains("params") && message["params"].is_object() ? message["params"] : json::object();
    std::string                        version = negotiated_version;
    if (params.contains("_meta") && params["_meta"].is_object()) {
        const json& meta = params["_meta"];
        if (auto it = meta.find("io.modelcontextprotocol/protocolVersion"); it != meta.end() && it->is_string())
            version = it->get<std::string>();
    }
    if (!version.empty())
        h["MCP-Protocol-Version"] = version;
    if (!method.empty())
        h["Mcp-Method"] = method;
    std::string name;
    if (method == "tools/call" || method == "prompts/get")
        name = params.value("name", "");
    else if (method == "resources/read")
        name = params.value("uri", "");
    if (!name.empty())
        h["Mcp-Name"] = header_safe(name) ? name : "=?base64?" + base64(name) + "?=";
    return h;
}


int run_stdio_bridge(int argc, char** argv)
{
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#else
    std::signal(SIGPIPE, SIG_IGN);
#endif
    // --datadir: launch OrcaSlicer with this configuration folder. --mcp-port is accepted and ignored: the bridge finds
    // every window through its instance record, whatever port it serves on.
    std::string datadir_arg;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--datadir" && i + 1 < argc)
            datadir_arg = argv[++i];
        else if (a.rfind("--datadir=", 0) == 0)
            datadir_arg = a.substr(10);
        else if (a == "--mcp-port" && i + 1 < argc)
            ++i;
    }

    const std::string exe  = self_executable(argv);
    const fs::path    base = default_datadir();

    BridgeDeps deps;
    deps.instances   = [base] { return read_instance_records(base); };
    deps.post        = post_to_window;
    deps.launch      = [exe](const std::vector<std::string>& args) { return launch_app(exe, args); };
    deps.launch_lock = [] { return std::unique_ptr<LaunchLock>(new ProcessLaunchLock); };
    deps.seed        = load_seed(exe);
    deps.bridge_id   = random_token().substr(0, 16);
    deps.datadir_arg = datadir_arg;
    auto core        = std::make_shared<BridgeCore>(std::move(deps));

    // Every reply is one whole line, written under one lock; each line runs on its own worker (16 at most, the rest wait).
    struct Shared
    {
        std::mutex              write_mu;
        std::mutex              mu;
        std::condition_variable cv;
        std::deque<std::string> queue;
        int                     busy = 0;
        bool                    stop = false;
    };
    auto shared = std::make_shared<Shared>();
    auto write  = [shared](const std::string& s) {
        std::lock_guard<std::mutex> lock(shared->write_mu);
        write_line(s);
    };
    for (int i = 0; i < 16; ++i)
        std::thread([shared, core, write] {
            for (;;) {
                std::string line;
                {
                    std::unique_lock<std::mutex> lock(shared->mu);
                    shared->cv.wait(lock, [&] { return shared->stop || !shared->queue.empty(); });
                    if (shared->stop)
                        return;
                    line = std::move(shared->queue.front());
                    shared->queue.pop_front();
                    ++shared->busy;
                }
                core->handle_line(line, write);
                {
                    std::lock_guard<std::mutex> lock(shared->mu);
                    --shared->busy;
                }
                shared->cv.notify_all();
            }
        }).detach();

    std::string line;
    while (std::getline(boost::nowide::cin, line)) {
        {
            std::lock_guard<std::mutex> lock(shared->mu);
            shared->queue.push_back(line);
        }
        shared->cv.notify_all();
    }
    // stdin closed: the client is gone. Give calls in flight a moment, then end; nothing else ends the bridge.
    {
        std::unique_lock<std::mutex> lock(shared->mu);
        shared->cv.wait_for(lock, std::chrono::seconds(2), [&] { return shared->queue.empty() && shared->busy == 0; });
        shared->stop = true;
    }
    shared->cv.notify_all();
    return 0;
}

}}} // namespace Slic3r::GUI::Mcp
