#include "Ledger.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <iterator>
#include <utility>

namespace Slic3r::BblPresetSync {

namespace fs = boost::filesystem;
using json   = nlohmann::json;

namespace {

std::string safe_component(const std::string& s)
{
    std::string out = s;
    for (char& c : out) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) c = '_';
    }
    return out;
}

long long unix_now()
{
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string get_string(const json& j, const char* key, const std::string& def)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : def;
}

long long get_int(const json& j, const char* key, long long def)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_number_integer()) ? it->get<long long>() : def;
}

LedgerEntry entry_from_json(const json& j)
{
    LedgerEntry e;
    e.setting_id     = get_string(j, "setting_id", e.setting_id);
    e.update_time    = get_int(j, "update_time", e.update_time);
    e.synced_hash    = get_string(j, "synced_hash", e.synced_hash);
    e.state          = get_string(j, "state", e.state);
    e.reason         = get_string(j, "reason", e.reason);
    e.absent_passes  = static_cast<int>(get_int(j, "absent_passes", e.absent_passes));
    e.retry_after    = get_int(j, "retry_after", e.retry_after);
    e.fail_count     = static_cast<int>(get_int(j, "fail_count", e.fail_count));
    e.last_error     = get_string(j, "last_error", e.last_error);
    e.converted_from = get_string(j, "converted_from", e.converted_from);
    auto it          = j.find("k_copied_printers");
    if (it != j.end() && it->is_array())
        for (const json& p : *it)
            if (p.is_string()) e.k_copied_printers.push_back(p.get<std::string>());
    return e;
}

json entry_to_json(const LedgerEntry& e)
{
    json j;
    j["setting_id"]        = e.setting_id;
    j["update_time"]       = e.update_time;
    j["synced_hash"]       = e.synced_hash;
    j["state"]             = e.state;
    j["reason"]            = e.reason;
    j["absent_passes"]     = e.absent_passes;
    j["retry_after"]       = e.retry_after;
    j["fail_count"]        = e.fail_count;
    j["last_error"]        = e.last_error;
    j["converted_from"]    = e.converted_from;
    j["k_copied_printers"] = e.k_copied_printers;
    return j;
}

} // namespace

fs::path Ledger::file_path(const fs::path& data_dir, const std::string& orca_user_id, const std::string& bambu_uid)
{
    return data_dir / "bbl_sync" / ("ledger_" + safe_component(orca_user_id) + "_" + safe_component(bambu_uid) + ".json");
}

fs::path Ledger::backup_dir(const fs::path& data_dir)
{
    return data_dir / "bbl_sync" / "backup";
}

Ledger::Ledger(fs::path file) : m_file(std::move(file)) {}

bool Ledger::load(std::string* error)
{
    boost::system::error_code ec;
    if (!fs::exists(m_file, ec)) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_entries.clear();
        m_first_run_confirmed = false;
        m_halted.clear();
        m_clock_offset = 0;
        return true;
    }

    std::string problem;
    json        root;
    {
        boost::nowide::ifstream in(m_file.string(), std::ios::binary);
        if (!in.is_open()) {
            problem = "cannot open ledger file";
        } else {
            const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            root = json::parse(text, nullptr, false);
            if (root.is_discarded() || !root.is_object())
                problem = "ledger file is not valid JSON";
            else if (root.contains("version") && root["version"].is_number_integer() && root["version"].get<long long>() > 1)
                problem = "ledger file has a newer version";
        }
    }

    if (!problem.empty()) {
        fs::path corrupt = m_file;
        corrupt += ".corrupt-" + std::to_string(unix_now());
        boost::system::error_code rec;
        fs::rename(m_file, corrupt, rec);
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_entries.clear();
            m_first_run_confirmed = false;
            m_halted.clear();
            m_clock_offset = 0;
        }
        if (error) *error = problem;
        return false;
    }

    std::map<Key, LedgerEntry> entries;
    auto                       it = root.find("entries");
    if (it != root.end() && it->is_object())
        for (auto e = it->begin(); e != it->end(); ++e) {
            auto key = Key::parse(e.key());
            if (!key || !e.value().is_object()) continue;
            entries[*key] = entry_from_json(e.value());
        }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries             = std::move(entries);
    m_first_run_confirmed = root.contains("first_run_confirmed") && root["first_run_confirmed"].is_boolean() &&
                            root["first_run_confirmed"].get<bool>();
    m_halted       = get_string(root, "halted", "");
    m_clock_offset = get_int(root, "clock_offset", 0);
    return true;
}

bool Ledger::save(std::string* error) const
{
    json root;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        root["version"]             = 1;
        root["first_run_confirmed"] = m_first_run_confirmed;
        root["halted"]              = m_halted;
        root["clock_offset"]        = m_clock_offset;
        json entries                = json::object();
        for (const auto& [key, entry] : m_entries) entries[key.str()] = entry_to_json(entry);
        root["entries"] = std::move(entries);
    }

    auto fail = [error](const std::string& msg) {
        if (error) *error = msg;
        return false;
    };

    boost::system::error_code ec;
    fs::create_directories(m_file.parent_path(), ec);
    if (ec) return fail("cannot create ledger directory: " + ec.message());

    fs::path tmp = m_file;
    tmp += ".tmp";
    {
        boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return fail("cannot write " + tmp.string());
        out << root.dump(2);
        out.flush();
        if (!out.good()) {
            out.close();
            fs::remove(tmp, ec);
            return fail("write failed for " + tmp.string());
        }
    }

    fs::rename(tmp, m_file, ec);
    if (ec) {
        fs::remove(m_file, ec);
        boost::system::error_code ec2;
        fs::rename(tmp, m_file, ec2);
        if (ec2) {
            fs::remove(tmp, ec);
            return fail("cannot replace ledger file: " + ec2.message());
        }
    }
    return true;
}

std::optional<LedgerEntry> Ledger::get(const Key& key) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    auto                        it = m_entries.find(key);
    if (it == m_entries.end()) return std::nullopt;
    return it->second;
}

void Ledger::put(const Key& key, const LedgerEntry& entry)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries[key] = entry;
}

void Ledger::erase(const Key& key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_entries.erase(key);
}

std::map<Key, LedgerEntry> Ledger::entries() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_entries;
}

bool Ledger::first_run_confirmed() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_first_run_confirmed;
}
void Ledger::set_first_run_confirmed(bool v)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_first_run_confirmed = v;
}
std::string Ledger::halted() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_halted;
}
void Ledger::set_halted(const std::string& v)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_halted = v;
}
long long Ledger::clock_offset() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_clock_offset;
}
void Ledger::set_clock_offset(long long v)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_clock_offset = v;
}

const fs::path& Ledger::file() const
{
    return m_file;
}

} // namespace Slic3r::BblPresetSync
