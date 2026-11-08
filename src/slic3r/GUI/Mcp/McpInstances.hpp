#pragma once

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r { namespace GUI { namespace Mcp {

// What one serving OrcaSlicer window publishes about itself, so a client can find every window: one file
// <base>/mcp/instances/<pid>-<incarnation>.json per window. `incarnation` identifies one run of one window
// (128 random bits): a pid or a port can be reused, a token outlives the process, an incarnation does not.
struct InstanceRecord
{
    int         pid = 0;
    std::string incarnation;
    int         port = 0;
    std::string token;
    std::string version;
    std::string features;       // "cad" when built with SLIC3R_CAD, else ""
    std::string datadir;        // the window's own configuration folder (UTF-8)
    std::string exe;            // the executable (UTF-8)
    std::string launched_by;    // --mcp-launched-by, or ""
    int64_t     started_at = 0; // unix seconds
    bool        primary    = false; // serving on the configured port

    nlohmann::json to_json() const;
    // False when `j` is not an object with at least a positive pid, an incarnation and a port.
    static bool from_json(const nlohmann::json& j, InstanceRecord& out);
};

// <base>/mcp/instances; created when a record is written. Production callers pass default_datadir().
boost::filesystem::path instances_dir(const boost::filesystem::path& base);

// The record's file name: "<pid>-<incarnation>.json".
std::string instance_record_name(int pid, const std::string& incarnation);

// Writes the record owner-only (write_private_file). False with `err` set on failure.
bool write_instance_record(const boost::filesystem::path& base, const InstanceRecord& record, std::string& err);
void remove_instance_record(const boost::filesystem::path& base, int pid, const std::string& incarnation);

// Every parsable record whose process is running. Never deletes anything.
std::vector<InstanceRecord> read_instance_records(const boost::filesystem::path& base);

// Deletes the records whose process is not running. A file whose content changed between the check and
// the delete is left alone.
void prune_dead_instance_records(const boost::filesystem::path& base);

// 128 random bits as 32 lowercase hex characters.
std::string new_incarnation();

}}} // namespace Slic3r::GUI::Mcp
