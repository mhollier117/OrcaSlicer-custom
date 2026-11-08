#include <catch2/catch_test_macros.hpp>

#include "slic3r/GUI/Mcp/McpInstances.hpp"
#include "slic3r/GUI/Mcp/McpPaths.hpp"
#include "slic3r/GUI/Mcp/McpToken.hpp"
#include "test_utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <string>

using namespace Slic3r::GUI::Mcp;
namespace fs = boost::filesystem;

namespace {

// Above every platform's pid range, so no process has it.
constexpr int kNoSuchPid = 0x7FFFFFF0;

InstanceRecord make_record(int pid, const std::string& incarnation)
{
    InstanceRecord r;
    r.pid         = pid;
    r.incarnation = incarnation;
    r.port        = 13619;
    r.token       = "tok";
    r.version     = "2.5.0";
    r.features    = "cad";
    r.datadir     = "C:\\data";
    r.exe         = "C:\\orca\\orca-slicer.exe";
    r.launched_by = "bridge-1";
    r.started_at  = 1700000000;
    r.primary     = true;
    return r;
}

void write_text(const fs::path& path, const std::string& text)
{
    boost::nowide::ofstream out(path_to_utf8(path).c_str(), std::ios::binary | std::ios::trunc);
    out << text;
}

} // namespace

TEST_CASE("an instance record survives a write and a read", "[Mcp][McpInstances]")
{
    ScopedTemporaryDir base("orca-mcp");
    const InstanceRecord written = make_record(current_pid(), "0123456789abcdef0123456789abcdef");
    std::string          err;
    REQUIRE(write_instance_record(base.path(), written, err));

    CHECK(fs::exists(instances_dir(base.path()) / (std::to_string(current_pid()) + "-0123456789abcdef0123456789abcdef.json")));
    CHECK(instance_record_name(7, "abc") == "7-abc.json");

    const auto records = read_instance_records(base.path());
    REQUIRE(records.size() == 1);
    CHECK(records[0].to_json() == written.to_json());
    CHECK(records[0].pid == written.pid);
    CHECK(records[0].incarnation == written.incarnation);
    CHECK(records[0].port == 13619);
    CHECK(records[0].features == "cad");
    CHECK(records[0].launched_by == "bridge-1");
    CHECK(records[0].started_at == 1700000000);
    CHECK(records[0].primary);

    remove_instance_record(base.path(), written.pid, written.incarnation);
    CHECK(read_instance_records(base.path()).empty());
}

TEST_CASE("reading records skips dead pids and unparsable files and deletes nothing", "[Mcp][McpInstances]")
{
    ScopedTemporaryDir base("orca-mcp");
    std::string        err;
    REQUIRE(write_instance_record(base.path(), make_record(current_pid(), "live"), err));
    REQUIRE(write_instance_record(base.path(), make_record(kNoSuchPid, "dead"), err));
    const fs::path junk    = instances_dir(base.path()) / "junk.json";
    const fs::path partial = instances_dir(base.path()) / "partial.json";
    write_text(junk, "not json");
    write_text(partial, R"({"pid": 5})");

    const auto records = read_instance_records(base.path());
    REQUIRE(records.size() == 1);
    CHECK(records[0].incarnation == "live");

    CHECK(fs::exists(instances_dir(base.path()) / instance_record_name(kNoSuchPid, "dead")));
    CHECK(fs::exists(junk));
    CHECK(fs::exists(partial));
}

TEST_CASE("reading from a folder with no records gives none", "[Mcp][McpInstances]")
{
    ScopedTemporaryDir base("orca-mcp");
    CHECK(read_instance_records(base.path()).empty());
    prune_dead_instance_records(base.path()); // nothing to do, and no error
}

TEST_CASE("pruning deletes only the records of dead pids", "[Mcp][McpInstances]")
{
    ScopedTemporaryDir base("orca-mcp");
    std::string        err;
    REQUIRE(write_instance_record(base.path(), make_record(current_pid(), "live"), err));
    REQUIRE(write_instance_record(base.path(), make_record(kNoSuchPid, "dead"), err));
    const fs::path junk = instances_dir(base.path()) / "junk.json";
    write_text(junk, "not json");

    prune_dead_instance_records(base.path());

    CHECK(fs::exists(instances_dir(base.path()) / instance_record_name(current_pid(), "live")));
    CHECK_FALSE(fs::exists(instances_dir(base.path()) / instance_record_name(kNoSuchPid, "dead")));
    CHECK(fs::exists(junk)); // names no owner: left for a person to look at
}

TEST_CASE("a dead record is kept when the file is rewritten after the check", "[Mcp][McpInstances]")
{
    // The file's content is read again right before deleting. A window that took the same name between the
    // two reads writes a record of a live pid; modelled here by what the second read must see: pruning a
    // file that is live by content never removes it.
    ScopedTemporaryDir base("orca-mcp");
    std::string        err;
    InstanceRecord     r = make_record(kNoSuchPid, "reused");
    REQUIRE(write_instance_record(base.path(), r, err));
    const fs::path file = instances_dir(base.path()) / instance_record_name(kNoSuchPid, "reused");

    r.pid = current_pid(); // the same name cannot hold a live pid's record in practice, but the content rule is what protects it
    write_text(file, r.to_json().dump(2) + "\n");

    prune_dead_instance_records(base.path());
    CHECK(fs::exists(file));
}

TEST_CASE("incarnations are 128 random bits", "[Mcp][McpInstances]")
{
    const std::string a = new_incarnation();
    const std::string b = new_incarnation();
    CHECK(a.size() == 32);
    CHECK(a != b);
    CHECK(a.find_first_not_of("0123456789abcdef") == std::string::npos);
}

TEST_CASE("the default datadir is one shared function", "[Mcp][McpInstances]")
{
    // Declared in McpPaths.hpp and defined once there: this links only if the bridge's own copy is gone
    // from the same library, and it gives the folder both the bridge and the window records use.
    const fs::path dir = default_datadir();
    CHECK_FALSE(dir.empty());
}
