// Printer MCP tools (fork): printer_camera, printer_files.
// Design: C:/claude-code/orca_mcp/docs/specs/2026-11-02-printer-mcp-design.md
//
// Both tools talk to the printer over the BambuSource tunnel on the LAN, as the Device tab's live view
// (MediaPlayCtrl / wxMediaCtrl3) and its SD-card browser (MediaFilePanel / PrinterFileSystem) do.
// The tunnel URLs embed the printer's access code: they are built here, handed to the tunnel and never
// returned, logged or journaled; every text that may echo them goes through scrub_secrets().

// PrinterFileSystem.h defines BAMBU_DYNAMIC before BambuTunnel.h, so it comes before the decoder's include.
#include "slic3r/GUI/Printer/PrinterFileSystem.h"

#include "McpPaths.hpp"
#include "McpPrinterCommon.hpp"
#include "McpPrinterMedia.hpp"
#include "McpService.hpp"
#include "McpTools.hpp"
#include "McpTypes.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r_version.h"
#include "slic3r/GUI/AVVideoDecoder.hpp"
#include "slic3r/GUI/DeviceCore/DevStorage.h"
#include "slic3r/GUI/DeviceManager.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/Utils/NetworkAgent.hpp"
#include "slic3r/Utils/PrinterNetworkTypes.hpp"

#include <boost/beast/core/detail/base64.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>
#include <boost/smart_ptr/weak_ptr.hpp>

#include <wx/event.h>
#include <wx/image.h>
#include <wx/mstream.h>
#include <wx/string.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// The Bambu tunnel function table, as the Device tab's file browser and player use it (PrinterFileSystem.cpp:1909).
extern "C" BambuLib* bambulib_get();

namespace Slic3r { namespace GUI { namespace Mcp {

using nlohmann::json;
namespace bfs = boost::filesystem;
using namespace std::chrono_literals;

// ---- pure helpers (McpPrinterMedia.hpp) ------------------------------------------------------

namespace {

const char* const kRedacted = "<redacted>";

void require_endpoint(const MediaEndpoint& e)
{
    if (e.ip.empty())
        throw ToolFailure("The printer has no known LAN IP address; set it with printer_list {action: \"connect\", ip: ...}",
                          {{"dev_id", e.dev_id}});
    if (e.access_code.empty())
        throw ToolFailure("The printer has no LAN access code; set it with printer_list {action: \"connect\", access_code: ...}",
                          {{"dev_id", e.dev_id}});
}

std::string tunnel_suffix(const MediaEndpoint& e)
{
    std::string s;
    s += "&device=" + e.dev_id;
    s += "&net_ver=" + e.net_ver;
    s += "&dev_ver=" + e.dev_ver;
    s += "&cli_id=" + e.cli_id;
    s += "&cli_ver=" + e.cli_ver;
    return s;
}

} // namespace

std::string camera_lan_url(int liveview_local, const MediaEndpoint& e)
{
    require_endpoint(e);
    std::string url;
    // mirrors MediaPlayCtrl.cpp:474-489 Play()
    if (liveview_local == LVL_Local)
        url = "bambu:///local/" + e.ip + ".?port=6000&user=" + e.user + "&passwd=" + e.access_code;
    else if (liveview_local == LVL_Rtsps)
        url = "bambu:///rtsps___" + e.user + ":" + e.access_code + "@" + e.ip + "/streaming/live/1?proto=rtsps";
    else if (liveview_local == LVL_Rtsp)
        url = "bambu:///rtsp___" + e.user + ":" + e.access_code + "@" + e.ip + "/streaming/live/1?proto=rtsp";
    else if (liveview_local == LVL_Disable)
        throw ToolFailure("LAN Only Liveview is off on the printer; turn Liveview on in the printer's screen settings", {{"dev_id", e.dev_id}});
    else
        throw ToolFailure("The printer reports no LAN camera stream (firmware without LAN liveview, or it has not reported one yet); "
                          "update the printer firmware or wait for its status",
                          {{"dev_id", e.dev_id}});
    return url + tunnel_suffix(e);
}

std::string files_lan_url(const MediaEndpoint& e)
{
    require_endpoint(e);
    // mirrors MediaFilePanel.cpp:518 fetchUrl()
    return "bambu:///local/" + e.ip + ".?port=6000&user=" + e.user + "&passwd=" + e.access_code + tunnel_suffix(e);
}

std::string scrub_secrets(std::string text, const std::vector<std::string>& secrets)
{
    for (const std::string& secret : secrets) {
        if (secret.empty())
            continue;
        for (std::size_t at = text.find(secret); at != std::string::npos; at = text.find(secret, at + std::strlen(kRedacted))) {
            text.replace(at, secret.size(), kRedacted);
        }
    }
    return text;
}

FrameSize fit_frame_width(int native_width, int native_height, int max_width)
{
    if (native_width <= 0 || native_height <= 0)
        return {};
    max_width = std::max(max_width, 16);
    if (native_width <= max_width && (native_width & 0x0f) == 0)
        return {native_width, native_height};
    int width = std::min(native_width, max_width) & ~0x0f;
    width     = std::max(width, 16);
    int height = int((std::int64_t(native_height) * width + native_width / 2) / native_width);
    height     = std::max(2, height & ~1);
    return {width, height};
}

namespace {

std::string iso_utc(std::time_t t)
{
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

} // namespace

json media_file_json(const MediaFileEntry& entry, const std::string& type, const std::string& storage, bool has_thumbnail)
{
    return {{"name", entry.name},
            {"path", entry.path.empty() ? entry.name : entry.path},
            {"size", entry.size},
            {"time", std::int64_t(entry.time)},
            {"time_utc", iso_utc(entry.time)},
            {"type", type},
            {"storage", storage},
            {"has_thumbnail", has_thumbnail}};
}

std::size_t find_media_file(const std::vector<MediaFileEntry>& entries, const std::string& key)
{
    if (key.empty())
        throw ToolFailure("path is required: the file's path (or name) from printer_files {action: \"list\"}");
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (!entries[i].path.empty() && entries[i].path == key)
            return i;
    std::size_t found = entries.size();
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].name != key && (entries[i].path.empty() || entries[i].path != key))
            continue;
        if (found != entries.size())
            throw ToolFailure("More than one file is named \"" + key + "\"; pass its full path from printer_files {action: \"list\"}");
        found = i;
    }
    if (found == entries.size())
        throw ToolFailure("No file \"" + key + "\" on the printer's storage; printer_files {action: \"list\"} lists them", {{"path", key}});
    return found;
}

std::string media_storage_arg(const std::string& storage)
{
    if (storage.empty() || storage == "sd" || storage == "external")
        return "";
    if (storage.size() <= 32 && std::all_of(storage.begin(), storage.end(), [](unsigned char c) { return std::islower(c) || std::isdigit(c) || c == '_'; }))
        return storage;
    throw ToolFailure("storage must be \"sd\", \"internal\" or a storage name the printer reports");
}

bool is_plain_file_name(const std::string& name)
{
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\") == std::string::npos && name.find('\0') == std::string::npos;
}

std::string media_error_text(int code)
{
    switch (code) {
    case 2: return "unexpected reply from the printer";
    case 3: return "the connection to the printer was lost";
    case 4: return "cancelled";
    case 5: return "the printer is busy with another client (close the Device tab's live view / file browser, or other viewers) and try again";
    case 6: return "the printer did not answer in time";
    case 10: return "the file does not exist on the printer";
    case 11: return "invalid file name";
    case 12: return "file size mismatch";
    case 13: return "the file could not be opened";
    case 14: return "reading or writing the file failed";
    case 15: return "the downloaded file failed its checksum";
    case 16: return "the printer does not support listing this file type";
    case 17: return "the storage is not available; check that the SD card is inserted (or try storage \"internal\")";
    case 18: return "the printer firmware is too old for this";
    case 19: return "the file already exists";
    case 20: return "not enough space on the storage";
    case 21: return "the file could not be created";
    case 22: return "writing the file failed";
    case 23: return "the file failed its MD5 check";
    case 24: return "renaming the file failed";
    case 25: return "sending the file failed";
    case -2: return "the BambuSource library is not loaded (no network plugin)";
    default: return "tunnel error code " + std::to_string(code) + " (check the IP, the access code and that the printer is reachable on the LAN)";
    }
}

// ---- shared tool plumbing --------------------------------------------------------------------

namespace {

std::string base64(const std::string& bytes)
{
    namespace b64 = boost::beast::detail::base64;
    std::string out(b64::encoded_size(bytes.size()), '\0');
    out.resize(b64::encode(out.data(), bytes.data(), bytes.size()));
    return out;
}

std::string encode_image(const wxImage& image, bool png)
{
    wxImage         copy(image);
    wxMemoryOutputStream out;
    if (!png)
        copy.SetOption(wxIMAGE_OPTION_QUALITY, 85);
    if (!copy.SaveFile(out, png ? wxBITMAP_TYPE_PNG : wxBITMAP_TYPE_JPEG))
        throw ToolFailure("Could not encode the image");
    std::string bytes(out.GetSize(), '\0');
    out.CopyTo(bytes.data(), bytes.size());
    return bytes;
}

// What the call needs from the printer, read on the UI thread.
struct PrinterAccess
{
    MediaEndpoint endpoint;
    int           liveview_local = LVL_None;
    int           file_local     = FL_None;
    bool          has_camera     = true;
    bool          camera_busy    = false;
    int           sdcard_state   = 0;
    std::string   name;
};

// UI thread. Refuses printers the tunnels cannot reach.
PrinterAccess ui_printer_access(const std::string& dev_id)
{
    MachineObject& p = ui_printer(dev_id);
    if (!ui_supports_dry_run(p)) // true for the bbl printer agent
        throw ToolFailure("printer_camera and printer_files work with Bambu Lab printers (the bbl printer agent) only");
    PrinterAccess a;
    a.name                 = p.get_dev_name();
    a.endpoint.ip          = p.get_dev_ip();
    a.endpoint.access_code = p.get_access_code();
    a.endpoint.dev_id      = p.get_dev_id();
    a.endpoint.dev_ver     = p.get_ota_version();
    NetworkAgent* agent    = wxGetApp().getAgent();
    a.endpoint.net_ver     = agent ? agent->get_version() : std::string();
    a.endpoint.cli_id      = wxGetApp().app_config->get("slicer_uuid");
    a.endpoint.cli_ver     = SLIC3R_VERSION;
    a.liveview_local       = int(p.liveview_local);
    a.file_local           = int(p.file_local);
    a.has_camera           = p.has_ipcam;
    a.camera_busy          = p.is_camera_busy_off();
    a.sdcard_state         = int(p.GetStorage()->get_sdcard_state());
    return a;
}

// ---- printer_camera --------------------------------------------------------------------------

struct GrabResult
{
    wxImage     image;
    int         native_width  = 0;
    int         native_height = 0;
    int         code          = 0;
    std::string error;
};

struct GrabState
{
    std::atomic_bool stop{false};
    std::atomic_int  log_code{0};
};

void grab_log(void* context, int level, tchar const* msg)
{
    auto* state = static_cast<GrabState*>(context);
#ifdef _WIN32
    wxString text(msg);
#else
    wxString text = wxString::FromUTF8(msg);
#endif
    // mirrors wxMediaCtrl3::bambu_log: an error message ends with "[code]"
    if (level == 1 && text.EndsWith("]")) {
        const int n = int(text.find_last_of('['));
        long      val = 0;
        if (n >= 0 && text.SubString(size_t(n) + 1, text.Length() - 2).ToLong(&val))
            state->log_code = int(val);
    }
    if (BambuLib* lib = bambulib_get(); lib != nullptr && lib->Bambu_FreeLogMsg != nullptr) // as PrinterFileSystem::DumpLog
        lib->Bambu_FreeLogMsg(msg);
}

// One decoded frame over the Bambu tunnel, as wxMediaCtrl3::PlayThread plays it (wxMediaCtrl3.cpp:480-560):
// Bambu_Create / Bambu_Open / Bambu_StartStream until it stops blocking, Bambu_GetStreamInfo, then
// Bambu_ReadSample into AVVideoDecoder until a frame decodes. Runs on its own thread.
GrabResult grab_frame(const BambuLib& lib, const std::string& url, int max_width, GrabState& state,
                      std::chrono::steady_clock::time_point deadline)
{
    GrabResult  r;
    auto        expired = [&] { return state.stop.load() || std::chrono::steady_clock::now() >= deadline; };
    auto        fail    = [&](int code, const std::string& message) {
        r.code  = state.log_code != 0 ? state.log_code.load() : code;
        r.error = message;
        return r;
    };
    if (!lib.Bambu_Create || !lib.Bambu_Open || !lib.Bambu_StartStream || !lib.Bambu_GetStreamInfo || !lib.Bambu_ReadSample ||
        !lib.Bambu_Close || !lib.Bambu_Destroy)
        return fail(-2, "the BambuSource library is not loaded (no network plugin)");

    Bambu_Tunnel tunnel = nullptr;
    struct Closer
    {
        const BambuLib& lib;
        Bambu_Tunnel&   tunnel;
        ~Closer()
        {
            if (tunnel) {
                lib.Bambu_Close(tunnel);
                lib.Bambu_Destroy(tunnel);
            }
        }
    } closer{lib, tunnel};

    int error = lib.Bambu_Create(&tunnel, url.c_str());
    if (error == 0) {
        if (lib.Bambu_SetLogger)
            lib.Bambu_SetLogger(tunnel, &grab_log, &state);
        error = lib.Bambu_Open(tunnel);
        if (error == 0)
            error = Bambu_would_block;
    }
    while (error == int(Bambu_would_block)) {
        std::this_thread::sleep_for(100ms);
        if (expired())
            return fail(6, "timed out connecting to the camera");
        error = lib.Bambu_StartStream(tunnel, true);
    }
    if (error != 0)
        return fail(error, "could not open the camera stream");

    Bambu_StreamInfo info{};
    int              count = lib.Bambu_GetStreamCount ? std::max(1, lib.Bambu_GetStreamCount(tunnel)) : 1;
    bool             have  = false;
    for (int i = 0; i < count && !have; ++i) {
        error = lib.Bambu_GetStreamInfo(tunnel, i, &info);
        have  = error == 0 && info.type == VIDE;
    }
    if (!have)
        return fail(error, "the camera stream has no video");

    AVVideoDecoder decoder;
    if (decoder.open(info) < 0)
        return fail(-1, "the camera's video format cannot be decoded");

    const bool     keyframes_only = info.sub_type == MJPG; // every JPEG frame stands alone; H.264 starts at a sync sample
    bool           seen_sync      = keyframes_only;
    Bambu_Sample   sample{};
    for (;;) {
        if (expired())
            return fail(6, "no video frame arrived in time");
        error = lib.Bambu_ReadSample(tunnel, &sample);
        if (error == int(Bambu_would_block)) {
            std::this_thread::sleep_for(50ms);
            continue;
        }
        if (error != 0)
            return fail(error, "the camera stream ended before a frame arrived");
        if ((sample.flags & f_sync) != 0)
            seen_sync = true;
        if (!seen_sync)
            continue;
        decoder.decode(sample);
        const wxSize native = decoder.decoded_frame_size();
        if (!native.IsFullySpecified() || native.x <= 0 || native.y <= 0)
            continue;
        const FrameSize out = fit_frame_width(native.x, native.y, max_width);
        wxImage         image;
        if (!decoder.toWxImage(image, wxSize(out.width, out.height)) || !image.IsOk())
            continue;
        r.image         = image;
        r.native_width  = native.x;
        r.native_height = native.y;
        return r;
    }
}

ToolResult camera_snapshot(const json& args, ToolContext& ctx)
{
    const std::string format = arg_string(args, "format", "jpeg");
    if (format != "jpeg" && format != "png")
        throw ToolFailure("format must be \"jpeg\" or \"png\"");
    const bool png       = format == "png";
    const int  max_width = std::clamp(arg_int(args, "max_width", 1280), 64, 4096);

    bfs::path save_to;
    if (args.contains("save_path")) {
        std::string err;
        auto        p = resolve_output_path(arg_string(args, "save_path"), arg_bool(args, "overwrite", false), err);
        if (!p)
            throw ToolFailure(err);
        save_to = *p;
    }

    const PrinterTarget target = resolve_printer(ctx, args, /*select*/ true, /*wait_ready*/ true);

    std::string url;
    BambuLib    lib{};
    int         liveview = LVL_None;
    std::vector<std::string> secrets;
    ctx.run_ui([&]() -> json {
        const PrinterAccess a = ui_printer_access(target.dev_id);
        if (!a.has_camera)
            throw ToolFailure("The printer reports no camera (ipcam_dev off); nothing to snapshot", {{"dev_id", target.dev_id}});
        if (a.camera_busy)
            throw ToolFailure("The printer's camera is unavailable while it prepares or upgrades; try again shortly", {{"dev_id", target.dev_id}});
        url      = camera_lan_url(a.liveview_local, a.endpoint);
        liveview = a.liveview_local;
        secrets  = {a.endpoint.access_code};
        BambuLib* table = bambulib_get();
        if (table == nullptr)
            throw ToolFailure("The BambuSource library is not available (no network plugin loaded)");
        lib = *table;
        return json();
    });

    // The tunnel blocks, so it runs on its own thread; the call waits for it, and on a timeout or a cancel leaves
    // it to finish by itself (it owns everything it uses).
    auto state   = std::make_shared<GrabState>();
    auto promise = std::make_shared<std::promise<GrabResult>>();
    auto future  = promise->get_future();
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    std::thread([lib, url, max_width, state, promise, deadline]() mutable {
        try {
            promise->set_value(grab_frame(lib, url, max_width, *state, deadline));
        } catch (...) {
            GrabResult failed;
            failed.code  = -1;
            failed.error = "the camera grab failed unexpectedly";
            promise->set_value(std::move(failed));
        }
    }).detach();

    while (future.wait_for(100ms) != std::future_status::ready) {
        if (ctx.cancelled()) {
            state->stop = true;
            throw ToolFailure("Cancelled while waiting for a camera frame");
        }
        if (std::chrono::steady_clock::now() >= deadline + 5s) { // the thread is stuck inside the tunnel
            state->stop = true;
            throw ToolFailure("The camera did not answer within 15 s", {{"dev_id", target.dev_id}});
        }
    }
    GrabResult grab = future.get();
    if (grab.error.empty() == false || !grab.image.IsOk()) {
        std::string why = scrub_secrets(grab.error.empty() ? "no frame" : grab.error, secrets);
        if (grab.code == 1) // the player's "device cannot handle more conversations"
            why += ": the printer cannot serve another viewer; close other live views and retry";
        else if (grab.code != 0)
            why += " (" + scrub_secrets(media_error_text(grab.code), secrets) + ")";
        throw ToolFailure("Could not get a camera frame: " + why, {{"dev_id", target.dev_id}, {"code", grab.code}});
    }

    const std::string bytes = encode_image(grab.image, png);
    json              data{{"dev_id", target.dev_id},
                           {"source", "lan_tunnel"},
                           {"transport", liveview == LVL_Local ? "mjpeg" : liveview == LVL_Rtsps ? "rtsps" : "rtsp"},
                           {"format", format},
                           {"width", grab.image.GetWidth()},
                           {"height", grab.image.GetHeight()},
                           {"native_width", grab.native_width},
                           {"native_height", grab.native_height},
                           {"bytes", bytes.size()}};
    if (target.selected_now)
        data["selected_now"] = true;
    if (!save_to.empty()) {
        bfs::ofstream out(save_to, std::ios::binary);
        out.write(bytes.data(), std::streamsize(bytes.size()));
        out.close();
        if (!out)
            throw ToolFailure("Could not write " + path_to_utf8(save_to));
        data["path"] = path_to_utf8(save_to);
    }
    ToolResult result = ToolResult::ok(redact(std::move(data)));
    result.content.push_back({{"type", "image"}, {"mimeType", png ? "image/png" : "image/jpeg"}, {"data", base64(bytes)}});
    return result;
}

// ---- printer_files ---------------------------------------------------------------------------

constexpr std::chrono::seconds kListTimeout{30}; // connecting included
constexpr std::chrono::seconds kShortTimeout{30};

const char* const kTypeNames[] = {"timelapse", "video", "model"};

// One PrinterFileSystem for one call, owned by the UI thread: events from its thread arrive on the UI
// thread and update this record; the call reads it through observe_ui.
struct FilesSession
{
    boost::shared_ptr<PrinterFileSystem> fs;
    int         status      = -1; // PrinterFileSystem::Status as last reported
    int         ready_count = 0;  // times a file list became ready
    bool        failed      = false;
    int         error       = 0;
    bool        download_failed = false;
    std::string download_error;
    wxBitmap    thumbnail_before;
    std::vector<std::string> secrets;
};

struct SessionGuard
{
    ToolContext&                  ctx;
    std::shared_ptr<FilesSession> session;
    ~SessionGuard()
    {
        try {
            ctx.run_ui([&]() -> json {
                if (session->fs) {
                    session->fs->Stop(true);
                    session->fs.reset();
                }
                return json();
            });
        } catch (...) { // the app is closing; the session dies with it
        }
    }
};

std::vector<MediaFileEntry> ui_entries(PrinterFileSystem& fs)
{
    std::vector<MediaFileEntry> entries;
    for (std::size_t i = 0; i < fs.GetCount(); ++i) {
        const PrinterFileSystem::File& f = fs.GetFile(i);
        entries.push_back({f.name, f.path, f.size, std::time_t(f.time)});
    }
    return entries;
}

// Waits (polling the UI thread) until `done` says so. `done` runs on the UI thread and may throw ToolFailure.
void wait_session(ToolContext& ctx, const std::shared_ptr<FilesSession>& s, std::chrono::seconds timeout, const std::string& what,
                  const std::function<bool(FilesSession&)>& done)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        const json r = ctx.observe_ui([&]() -> json {
            if (s->failed)
                throw ToolFailure("Could not " + what + ": " + scrub_secrets(media_error_text(s->error), s->secrets),
                                  {{"code", s->error}});
            if (s->download_failed)
                throw ToolFailure("Could not " + what + ": " + scrub_secrets(s->download_error, s->secrets));
            return done(*s) ? json(true) : json(false);
        });
        if (r.get<bool>())
            return;
        if (ctx.cancelled())
            throw ToolFailure("Cancelled while waiting for the printer");
        if (std::chrono::steady_clock::now() >= deadline)
            throw ToolFailure("Timed out after " + std::to_string(timeout.count()) + " s trying to " + what +
                              "; the printer may be busy or unreachable");
        std::this_thread::sleep_for(150ms);
    }
}

struct FilesRequest
{
    std::string                    dev_id;
    bool                           selected_now = false;
    std::string                    storage_label; // "sd" | "internal" | other
    std::string                    storage_arg;   // as the tunnel takes it
    PrinterFileSystem::FileType    type = PrinterFileSystem::F_MODEL;
    std::string                    url;
    std::vector<std::string>       secrets;
};

FilesRequest files_request(const json& args, ToolContext& ctx)
{
    FilesRequest req;
    const PrinterTarget target = resolve_printer(ctx, args, /*select*/ true, /*wait_ready*/ true);
    req.dev_id       = target.dev_id;
    req.selected_now = target.selected_now;

    const std::string type = arg_string(args, "type", "model");
    if (type == "timelapse")
        req.type = PrinterFileSystem::F_TIMELAPSE;
    else if (type == "video")
        req.type = PrinterFileSystem::F_VIDEO;
    else if (type == "model")
        req.type = PrinterFileSystem::F_MODEL;
    else
        throw ToolFailure("type must be \"model\", \"timelapse\" or \"video\"");

    const std::string storage_in = arg_string(args, "storage");
    json              probe      = ctx.run_ui([&]() -> json {
        const PrinterAccess a = ui_printer_access(target.dev_id);
        if (a.file_local == FL_None)
            throw ToolFailure("The printer does not offer LAN file browsing (firmware too old, or it has not reported yet)",
                              {{"dev_id", target.dev_id}});
        if (a.camera_busy)
            throw ToolFailure("The printer is busy (preparing or upgrading); try again shortly", {{"dev_id", target.dev_id}});
        req.url     = files_lan_url(a.endpoint);
        req.secrets = {a.endpoint.access_code};
        return {{"sdcard", a.sdcard_state}};
    });
    // Like the Device tab: the SD card when one is inserted, else the printer's own storage.
    std::string label = storage_in;
    if (label.empty())
        label = probe["sdcard"].get<int>() == 1 ? "sd" : "internal";
    req.storage_label = (label == "external" || label.empty()) ? "sd" : label;
    req.storage_arg   = media_storage_arg(label);
    if (req.storage_label == "sd" && probe["sdcard"].get<int>() == 0)
        throw ToolFailure("The printer reports no SD card; use storage \"internal\" (models only)", {{"dev_id", target.dev_id}});
    if (req.storage_arg == "internal" && req.type != PrinterFileSystem::F_MODEL)
        throw ToolFailure("The printer's internal storage holds models only; use type \"model\" or storage \"sd\"");
    return req;
}

// Connects a PrinterFileSystem to the printer and waits for the first file list, as MediaFilePanel does
// (MediaFilePanel.cpp:319-410): the tunnel asks for its URL when it reaches Initializing and lists when it
// reaches ListSyncing. Returns the listing.
std::vector<MediaFileEntry> start_and_list(ToolContext& ctx, const std::shared_ptr<FilesSession>& s, const FilesRequest& req)
{
    s->secrets = req.secrets;
    ctx.run_ui([&]() -> json {
        boost::shared_ptr<PrinterFileSystem> fs(new PrinterFileSystem);
        s->fs = fs;
        fs->Attached();
        fs->SetFileType(req.type, req.storage_arg);
        boost::weak_ptr<PrinterFileSystem> wfs(fs);
        std::weak_ptr<FilesSession>        ws(s);
        const std::string                  url = req.url;
        fs->Bind(EVT_STATUS_CHANGED, [wfs, ws, url](wxCommandEvent& e) {
            e.Skip();
            auto f  = wfs.lock();
            auto st = ws.lock();
            if (!f || !st)
                return;
            st->status = e.GetInt();
            switch (st->status) {
            case PrinterFileSystem::Initializing: f->SetUrl(url); break;
            case PrinterFileSystem::ListSyncing: f->ListAllFiles(); break;
            case PrinterFileSystem::ListReady: ++st->ready_count; break;
            case PrinterFileSystem::Failed:
                st->failed = true;
                st->error  = int(e.GetExtraLong());
                break;
            default: break;
            }
        });
        fs->Bind(EVT_DOWNLOAD, [ws](wxCommandEvent& e) {
            e.Skip();
            auto st = ws.lock();
            if (!st)
                return;
            const long result = e.GetExtraLong();
            if (result > PrinterFileSystem::CONTINUE) {
                st->download_failed = true;
                st->download_error  = media_error_text(int(result));
                if (!e.GetString().IsEmpty())
                    st->download_error += ": " + std::string(e.GetString().ToUTF8().data());
            }
        });
        fs->Start();
        return json();
    });
    wait_session(ctx, s, kListTimeout, "list the printer's files", [](FilesSession& st) { return st.ready_count >= 1; });
    std::vector<MediaFileEntry> entries;
    ctx.observe_ui([&]() -> json {
        entries = ui_entries(*s->fs);
        return json();
    });
    return entries;
}

json file_list_json(const std::vector<MediaFileEntry>& entries, const FilesRequest& req)
{
    json items = json::array();
    const char* type = kTypeNames[req.type];
    for (const MediaFileEntry& e : entries)
        items.push_back(media_file_json(e, type, req.storage_label, true));
    return items;
}

ToolResult files_list(const json& args, ToolContext& ctx, const FilesRequest& req, const std::shared_ptr<FilesSession>& s)
{
    const auto entries = start_and_list(ctx, s, req);
    json       data    = page(file_list_json(entries, req), args, 100);
    data["dev_id"]     = req.dev_id;
    data["storage"]    = req.storage_label;
    data["type"]       = kTypeNames[req.type];
    if (req.selected_now)
        data["selected_now"] = true;
    return ToolResult::ok(redact(std::move(data)));
}

ToolResult files_thumbnail(const json& args, ToolContext& ctx, const FilesRequest& req, const std::shared_ptr<FilesSession>& s)
{
    const auto        entries = start_and_list(ctx, s, req);
    const std::size_t index   = find_media_file(entries, arg_string(args, "path"));
    // mirrors the file grid asking for the thumbnails of the files in view: SetFocusRange
    ctx.run_ui([&]() -> json {
        s->thumbnail_before = s->fs->GetFile(index).thumbnail;
        s->fs->SetFocusRange(index, 1);
        return json();
    });
    wait_session(ctx, s, kShortTimeout, "fetch the thumbnail", [index](FilesSession& st) {
        return (st.fs->GetFile(index).flags & PrinterFileSystem::FF_THUMNAIL) != 0;
    });
    wxImage image;
    ctx.run_ui([&]() -> json {
        const wxBitmap& bmp = s->fs->GetFile(index).thumbnail;
        if (bmp.IsOk() && !bmp.IsSameAs(s->thumbnail_before))
            image = bmp.ConvertToImage();
        return json();
    });
    if (!image.IsOk())
        throw ToolFailure("The printer returned no thumbnail for \"" + entries[index].name + "\"");
    const std::string bytes = encode_image(image, true);
    json              data{{"dev_id", req.dev_id},
                           {"name", entries[index].name},
                           {"path", entries[index].path.empty() ? entries[index].name : entries[index].path},
                           {"width", image.GetWidth()},
                           {"height", image.GetHeight()}};
    ToolResult result = ToolResult::ok(redact(std::move(data)));
    result.content.push_back({{"type", "image"}, {"mimeType", "image/png"}, {"data", base64(bytes)}});
    return result;
}

ToolResult files_download(const json& args, ToolContext& ctx, const FilesRequest& req, const std::shared_ptr<FilesSession>& s)
{
    std::string err;
    auto        dest_dir = resolve_output_dir(arg_string(args, "dest_dir"), err);
    if (!dest_dir)
        throw ToolFailure(err.empty() ? "dest_dir is required: an existing absolute folder" : err);
    const bool overwrite = arg_bool(args, "overwrite", false);

    const auto        entries = start_and_list(ctx, s, req);
    const std::size_t index   = find_media_file(entries, arg_string(args, "path"));
    const MediaFileEntry& entry = entries[index];
    if (!is_plain_file_name(entry.name))
        throw ToolFailure("The printer's file name \"" + entry.name + "\" is not usable as a local file name");
    const bfs::path target = *dest_dir / path_from_utf8(entry.name);
    boost::system::error_code ec;
    if (bfs::exists(target, ec) && !overwrite)
        throw ToolFailure(path_to_utf8(target) + " exists; pass overwrite: true to replace it", {{"path", path_to_utf8(target)}});

    json data{{"dev_id", req.dev_id},
              {"name", entry.name},
              {"path", entry.path.empty() ? entry.name : entry.path},
              {"size", entry.size},
              {"storage", req.storage_label},
              {"type", kTypeNames[req.type]},
              {"dest", path_to_utf8(target)}};
    if (arg_bool(args, "dry_run", false)) {
        data["dry_run"]        = true;
        data["would_download"] = true;
        return ToolResult::ok(redact(std::move(data)));
    }

    // PrinterFileSystem writes through narrow paths (ANSI on Windows), so the file is received in a plain
    // temp folder and moved to dest_dir, which also leaves nothing half-written there.
    const bfs::path stage = bfs::temp_directory_path(ec) / bfs::unique_path("orca-mcp-dl-%%%%-%%%%-%%%%");
    const std::string stage_str = stage.string();
#ifdef _WIN32
    if (!std::all_of(stage_str.begin(), stage_str.end(), [](unsigned char c) { return c < 0x80; }))
        throw ToolFailure("The temp folder's path has non-ASCII characters, which the printer file transfer cannot use");
#endif
    bfs::create_directories(stage, ec);
    if (ec)
        throw ToolFailure("Could not create a temp folder: " + ec.message());
    struct StageCleaner
    {
        bfs::path path;
        ~StageCleaner()
        {
            boost::system::error_code ec;
            bfs::remove_all(path, ec);
        }
    } cleaner{stage};

    ctx.run_ui([&]() -> json {
        s->fs->DownloadFiles(index, stage_str);
        return json();
    });
    const auto seconds = std::chrono::seconds(std::clamp<std::int64_t>(30 + std::int64_t(entry.size / (256 * 1024)), 60, 600));
    std::string local;
    try {
        wait_session(ctx, s, seconds, "download \"" + entry.name + "\"", [&](FilesSession& st) {
            const PrinterFileSystem::File& f = st.fs->GetFile(index);
            if (f.local_path.empty())
                return false;
            local = f.local_path;
            return true;
        });
    } catch (...) {
        try {
            ctx.run_ui([&]() -> json {
                s->fs->DownloadCancel(index);
                return json();
            });
        } catch (...) {
        }
        throw;
    }

    bfs::path src(local); // narrow, as PrinterFileSystem wrote it
    if (bfs::exists(target, ec) && overwrite)
        bfs::remove(target, ec);
    bfs::rename(src, target, ec);
    if (ec) { // another volume
        bfs::copy_file(src, target, bfs::copy_options::overwrite_existing, ec);
        if (ec)
            throw ToolFailure("Could not save " + path_to_utf8(target) + ": " + ec.message());
    }
    const auto written = bfs::file_size(target, ec);
    data["saved_path"] = path_to_utf8(target);
    data["bytes"]      = ec ? json(nullptr) : json(std::uint64_t(written));
    return ToolResult::ok(redact(std::move(data)));
}

ToolResult files_delete(const json& args, ToolContext& ctx, const FilesRequest& req, const std::shared_ptr<FilesSession>& s)
{
    const auto        entries = start_and_list(ctx, s, req);
    const std::size_t index   = find_media_file(entries, arg_string(args, "path")); // an unknown path is refused, also for a dry run
    const MediaFileEntry& entry = entries[index];
    json data{{"dev_id", req.dev_id},
              {"name", entry.name},
              {"path", entry.path.empty() ? entry.name : entry.path},
              {"size", entry.size},
              {"storage", req.storage_label},
              {"type", kTypeNames[req.type]}};
    if (arg_bool(args, "dry_run", false)) {
        data["dry_run"]      = true;
        data["would_delete"] = true;
        return ToolResult::ok(redact(std::move(data)));
    }

    const std::string key = entry.path.empty() ? entry.name : entry.path;
    ctx.run_ui([&]() -> json {
        s->fs->DeleteFiles(index);
        return json();
    });
    wait_session(ctx, s, kShortTimeout, "delete \"" + entry.name + "\"", [&](FilesSession& st) {
        for (const MediaFileEntry& e : ui_entries(*st.fs))
            if ((e.path.empty() ? e.name : e.path) == key)
                return false;
        return true;
    });
    // The delete's reply is not reported by PrinterFileSystem (it drops the file from its list either way), so the
    // printer is asked for its list again.
    int before = 0;
    ctx.run_ui([&]() -> json {
        before = s->ready_count;
        s->fs->ListAllFiles();
        return json();
    });
    wait_session(ctx, s, kShortTimeout, "confirm the delete", [before](FilesSession& st) { return st.ready_count > before; });
    bool still_there = false;
    ctx.observe_ui([&]() -> json {
        for (const MediaFileEntry& e : ui_entries(*s->fs))
            if ((e.path.empty() ? e.name : e.path) == key)
                still_there = true;
        return json();
    });
    if (still_there)
        throw ToolFailure("The printer still lists \"" + entry.name + "\" after the delete; it was not deleted (is the SD card write-protected?)",
                          {{"dev_id", req.dev_id}, {"path", key}});
    data["deleted"] = true;
    return ToolResult::ok(redact(std::move(data)));
}

ToolResult printer_files_call(const json& args, ToolContext& ctx)
{
    const std::string action = arg_string(args, "action");
    if (action != "list" && action != "thumbnail" && action != "download" && action != "delete")
        throw ToolFailure("action must be list, thumbnail, download or delete");
    if (action != "list" && arg_string(args, "path").empty())
        throw ToolFailure("path is required: the file's path (or name) from printer_files {action: \"list\"}");
    if (action == "download" && arg_string(args, "dest_dir").empty())
        throw ToolFailure("dest_dir is required: an existing absolute folder");

    const FilesRequest req = files_request(args, ctx);
    auto               session = std::make_shared<FilesSession>();
    SessionGuard       guard{ctx, session};
    if (action == "list")
        return files_list(args, ctx, req, session);
    if (action == "thumbnail")
        return files_thumbnail(args, ctx, req, session);
    if (action == "download")
        return files_download(args, ctx, req, session);
    return files_delete(args, ctx, req, session);
}

} // namespace

void register_printer_media_tools(McpService& service)
{
    ToolRegistry& reg = service.registry();

    reg.add(inspect_tool(
        "printer_camera", L("Printer camera"),
        "One current frame from the printer's camera, returned as an image (action \"snapshot\"). It connects to the printer "
        "over the LAN (the same tunnel the Device tab's live view uses), so the printer needs its IP and access code "
        "(printer_list) and LAN live view on; cloud-only access is not supported. A printer that is not the selected one is "
        "selected first. The printer serves few viewers at once: close the Device tab's live view if it is refused as busy. "
        "format jpeg (default) or png, scaled down to max_width (default 1280); save_path writes the image to an absolute "
        "path (overwrite replaces). The camera address and access code are never returned.",
        object_schema({{"action", enum_prop({"snapshot"}, "snapshot: one frame.")},
                       {"dev_id", dev_id_prop()},
                       {"format", enum_prop({"jpeg", "png"}, "Image format (default jpeg).")},
                       {"max_width", {{"type", "integer"}, {"minimum", 64}, {"maximum", 4096}, {"description", "Widest the image may be (default 1280)."}}},
                       {"save_path", prop("string", "Absolute path to also write the image to.")},
                       {"overwrite", prop("boolean", "Replace an existing save_path.")}},
                      {"action"}),
        [](const json& args, ToolContext& ctx) {
            if (arg_string(args, "action") != "snapshot")
                throw ToolFailure("action must be \"snapshot\"");
            return camera_snapshot(args, ctx);
        }));

    reg.add(mutate_tool(
        "printer_files", L("Printer files"),
        "Browse and fetch files on the printer's storage over the LAN (the tunnel the Device tab's file browser uses). "
        "action list: files with name, path (the id for the other actions), size, time, paged with limit/offset. thumbnail: "
        "the file's preview image. download: saves the file to dest_dir (an existing absolute folder; overwrite replaces; "
        "large files take minutes). delete: removes the file from the printer (destructive; dry_run reports what would be "
        "deleted and removes nothing; the printer is asked for its list again to confirm). storage: \"sd\" (default when a "
        "card is inserted) or \"internal\" (models only); type: model (default), timelapse or video. A printer that is not "
        "the selected one is selected first. Needs the printer's IP and access code. Access codes and tunnel URLs are never returned.",
        object_schema({{"action", enum_prop({"list", "thumbnail", "download", "delete"}, "What to do.")},
                       {"dev_id", dev_id_prop()},
                       {"storage", prop("string", "sd, internal, or a storage name the printer reports.")},
                       {"type", enum_prop({"model", "timelapse", "video"}, "Kind of file (default model).")},
                       {"path", prop("string", "The file's path (or name) from list; for thumbnail, download and delete.")},
                       {"dest_dir", prop("string", "Download: absolute path of an existing folder.")},
                       {"overwrite", prop("boolean", "Download: replace an existing local file.")},
                       {"dry_run", dry_run_prop()},
                       {"limit", {{"type", "integer"}, {"minimum", 1}, {"description", "List: page size (default 100)."}}},
                       {"offset", {{"type", "integer"}, {"minimum", 0}, {"description", "List: first entry."}}}},
                      {"action"}),
        printer_files_call, /*destructive*/ true));
}

}}} // namespace Slic3r::GUI::Mcp
