#pragma once

// printer_camera / printer_files helpers (McpToolsPrinterMedia.cpp). Testable pieces of the printer MCP tools (fork):
// the LAN tunnel URLs, secret scrubbing, file entries. No wx or printer state in here.
// Design: C:/claude-code/orca_mcp/docs/specs/2026-11-02-printer-mcp-design.md

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// What the BambuSource tunnel URLs are built from. The access code is a secret: URLs built from it never
// leave the process (not in replies, logs or the journal).
struct MediaEndpoint
{
    std::string ip;
    std::string access_code;
    std::string user = "bblp";
    std::string dev_id;
    std::string dev_ver; // MachineObject::get_ota_version()
    std::string net_ver; // NetworkAgent::get_version()
    std::string cli_id;  // app_config "slicer_uuid"
    std::string cli_ver; // SLIC3R_VERSION
};

// The camera's LAN URL for the printer's `liveview_local` (a Slic3r::LiveviewLocal value), as
// MediaPlayCtrl::Play builds it (MediaPlayCtrl.cpp:474): "bambu:///local/<ip>.?port=6000..." for the MJPEG
// tunnel (A1/P1 style), "bambu:///rtsps___user:code@ip/streaming/live/1?proto=rtsps" for RTSPS (X1/H2
// style), the same with rtsp. Throws ToolFailure when the printer has no IP or access code, or does not
// offer LAN live view (message says why).
std::string camera_lan_url(int liveview_local, const MediaEndpoint& endpoint);

// The file-browsing tunnel URL, as MediaFilePanel::fetchUrl builds it (MediaFilePanel.cpp:518). Throws
// ToolFailure without IP or access code.
std::string files_lan_url(const MediaEndpoint& endpoint);

// `text` with every non-empty secret replaced by "<redacted>" (error texts from the tunnel may echo the URL).
std::string scrub_secrets(std::string text, const std::vector<std::string>& secrets);

// Size of the returned frame: native size, or scaled down to at most `max_width` wide, the width a multiple
// of 16 (AVVideoDecoder pads other widths) and the height keeping the aspect ratio (even).
struct FrameSize
{
    int width  = 0;
    int height = 0;
};
FrameSize fit_frame_width(int native_width, int native_height, int max_width);

// A file on the printer's storage, as the listing returns it.
struct MediaFileEntry
{
    std::string   name;
    std::string   path; // empty on old firmware, which addresses files by name
    std::uint64_t size = 0;
    std::time_t   time = 0;
};

// {name, path, size, time, time_utc, type, storage, has_thumbnail}.
nlohmann::json media_file_json(const MediaFileEntry& entry, const std::string& type, const std::string& storage, bool has_thumbnail);

// Index of the file `key` names: its printer path, else its name when exactly one file has that name.
// Throws ToolFailure when there is none (a delete of an unknown path is refused) or the name is ambiguous.
std::size_t find_media_file(const std::vector<MediaFileEntry>& entries, const std::string& key);

// The storage argument as the tunnel takes it: "sd" (also "external", "") is the SD card, which the
// tunnel calls ""; "internal" the printer's own storage; any other short [a-z0-9_] name passes through.
// Throws ToolFailure otherwise.
std::string media_storage_arg(const std::string& storage);

// Which storage to browse, given the storage names the printer listed (REQUEST_MEDIA_ABILITY; empty when it
// did not answer). `requested`: "" chooses (the SD card when `sd_inserted` and listed, else the first listed
// storage that is not the SD card), "sd", "internal", or a name. Returns the name for the tunnel ("" is the
// SD card on printers that list no names) and sets `label` ("sd", or the storage's name). Throws ToolFailure
// for an invalid name.
std::string choose_media_storage(const std::vector<std::string>& listed, bool sd_inserted, const std::string& requested, std::string& label);

// A file name from the printer that is safe to use as a local file name (no separators or "..").
bool is_plain_file_name(const std::string& name);

// Text for a PrinterFileSystem / tunnel result code.
std::string media_error_text(int code);

}}} // namespace Slic3r::GUI::Mcp
