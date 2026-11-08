#include "McpInstances.hpp"
#include "McpPaths.hpp"
#include "McpToken.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/system/error_code.hpp>

#include <iterator>
#include <string>
#include <utility>

namespace Slic3r { namespace GUI { namespace Mcp {

namespace fs = boost::filesystem;
using nlohmann::json;

json InstanceRecord::to_json() const
{
    return {{"pid", pid},
            {"incarnation", incarnation},
            {"port", port},
            {"token", token},
            {"version", version},
            {"features", features},
            {"datadir", datadir},
            {"exe", exe},
            {"launched_by", launched_by},
            {"started_at", started_at},
            {"primary", primary}};
}

bool InstanceRecord::from_json(const json& j, InstanceRecord& out)
{
    if (!j.is_object())
        return false;
    try {
        InstanceRecord r;
        r.pid         = j.value("pid", 0);
        r.incarnation = j.value("incarnation", "");
        r.port        = j.value("port", 0);
        r.token       = j.value("token", "");
        r.version     = j.value("version", "");
        r.features    = j.value("features", "");
        r.datadir     = j.value("datadir", "");
        r.exe         = j.value("exe", "");
        r.launched_by = j.value("launched_by", "");
        r.started_at  = j.value("started_at", int64_t(0));
        r.primary     = j.value("primary", false);
        if (r.pid <= 0 || r.incarnation.empty() || r.port <= 0)
            return false;
        out = std::move(r);
        return true;
    } catch (const json::exception&) {
        return false;
    }
}

fs::path instances_dir(const fs::path& base) { return base / "mcp" / "instances"; }

std::string instance_record_name(int pid, const std::string& incarnation) { return std::to_string(pid) + "-" + incarnation + ".json"; }

std::string new_incarnation() { return random_token().substr(0, 32); }

bool write_instance_record(const fs::path& base, const InstanceRecord& record, std::string& err)
{
    mcp_dir(base);
    const fs::path            dir = instances_dir(base);
    boost::system::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        err = "cannot create " + path_to_utf8(dir);
        return false;
    }
    return write_private_file(dir / instance_record_name(record.pid, record.incarnation), record.to_json().dump(2) + "\n", err);
}

void remove_instance_record(const fs::path& base, int pid, const std::string& incarnation)
{
    boost::system::error_code ec;
    fs::remove(instances_dir(base) / instance_record_name(pid, incarnation), ec);
}

namespace {

std::string read_file(const fs::path& path)
{
    boost::nowide::ifstream in(path_to_utf8(path).c_str(), std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Temporary files of write_private_file end in ".tmp-xxxxxxxx", not ".json".
bool is_record_file(const fs::path& path) { return path.extension() == ".json"; }

} // namespace

std::vector<InstanceRecord> read_instance_records(const fs::path& base)
{
    std::vector<InstanceRecord> out;
    boost::system::error_code   ec;
    for (fs::directory_iterator it(instances_dir(base), ec), end; !ec && it != end; it.increment(ec)) {
        if (!is_record_file(it->path()))
            continue;
        InstanceRecord r;
        if (InstanceRecord::from_json(json::parse(read_file(it->path()), nullptr, false), r) && process_running(r.pid))
            out.push_back(std::move(r));
    }
    return out;
}

void prune_dead_instance_records(const fs::path& base)
{
    boost::system::error_code ec;
    for (fs::directory_iterator it(instances_dir(base), ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path path = it->path();
        if (!is_record_file(path))
            continue;
        const std::string content = read_file(path);
        InstanceRecord    r;
        if (!InstanceRecord::from_json(json::parse(content, nullptr, false), r) || process_running(r.pid))
            continue;
        // Only a file unchanged since it was judged dead: a window that took the name meanwhile rewrote it.
        if (read_file(path) != content)
            continue;
        boost::system::error_code rm;
        fs::remove(path, rm);
    }
}

}}} // namespace Slic3r::GUI::Mcp
