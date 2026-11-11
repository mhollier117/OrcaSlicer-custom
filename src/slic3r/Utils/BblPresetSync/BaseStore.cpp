#include "BaseStore.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

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

bool read_map(const json& j, ValueMap& out)
{
    if (!j.is_object()) return false;
    for (auto e = j.begin(); e != j.end(); ++e) {
        if (!e.value().is_string()) return false;
        out[e.key()] = e.value().get<std::string>();
    }
    return true;
}

} // namespace

fs::path BaseStore::dir(const fs::path& data_dir, const std::string& orca_user_id, const std::string& bambu_uid)
{
    return data_dir / "bbl_sync" / ("base_" + safe_component(orca_user_id) + "_" + safe_component(bambu_uid));
}

BaseStore::BaseStore(fs::path dir) : m_dir(std::move(dir)) {}

fs::path BaseStore::file_of(const std::string& setting_id) const
{
    return m_dir / (safe_component(setting_id) + ".json");
}

const std::optional<Bases>& BaseStore::cached(const std::string& setting_id) const
{
    auto it = m_cache.find(setting_id);
    if (it != m_cache.end()) return it->second;

    std::optional<Bases> loaded;
    try {
        boost::system::error_code ec;
        const fs::path            file = file_of(setting_id);
        if (fs::exists(file, ec)) {
            boost::nowide::ifstream in(file.string(), std::ios::binary);
            if (in.is_open()) {
                const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                const json        root = json::parse(text, nullptr, false);
                if (!root.is_discarded() && root.is_object()) {
                    Bases bases;
                    bool  ok;
                    const auto remote = root.find("remote");
                    if (remote != root.end() && remote->is_object()) {
                        ok = read_map(*remote, bases.remote);
                        const auto local = root.find("local");
                        if (ok && local != root.end()) ok = read_map(*local, bases.local);
                    } else {
                        ok = read_map(root, bases.remote); // the old form: one map, the body on Bambu
                    }
                    if (ok) loaded = std::move(bases);
                }
            }
        }
    } catch (...) {
        loaded.reset();
    }
    return m_cache.emplace(setting_id, std::move(loaded)).first->second;
}

std::optional<Bases> BaseStore::get(const std::string& setting_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return cached(setting_id);
}

bool BaseStore::has(const std::string& setting_id) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return cached(setting_id).has_value();
}

void BaseStore::put(const std::string& setting_id, const Bases& bases)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache[setting_id] = bases;

    boost::system::error_code ec;
    fs::create_directories(m_dir, ec);
    if (ec) return;
    const fs::path file = file_of(setting_id);
    fs::path       tmp  = file;
    tmp += ".tmp";
    {
        boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return;
        out << json{{"remote", bases.remote}, {"local", bases.local}}.dump(2);
        out.flush();
        if (!out.good()) {
            out.close();
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) {
        fs::remove(file, ec);
        boost::system::error_code ec2;
        fs::rename(tmp, file, ec2);
        if (ec2) fs::remove(tmp, ec);
    }
}

void BaseStore::erase(const std::string& setting_id)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cache[setting_id].reset();
    boost::system::error_code ec;
    fs::remove(file_of(setting_id), ec);
}

} // namespace Slic3r::BblPresetSync
