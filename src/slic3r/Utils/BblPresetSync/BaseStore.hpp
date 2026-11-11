#pragma once

#include "Types.hpp"

#include <boost/filesystem/path.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace Slic3r::BblPresetSync {

// The two bases of a linked preset, one per side.
struct Bases
{
    ValueMap remote; // the body on Bambu at the last sync
    ValueMap local;  // OrcaSlicer's upload map of the preset at the last sync
};

// The bases per Bambu setting id. One JSON file each: {"remote": {...}, "local": {...}}.
class BaseStore
{
public:
    // data_dir/bbl_sync/base_<orca id>_<bambu uid>
    static boost::filesystem::path dir(const boost::filesystem::path& data_dir, const std::string& orca_user_id, const std::string& bambu_uid);

    explicit BaseStore(boost::filesystem::path dir);

    std::optional<Bases>    get(const std::string& setting_id) const;
    void                    put(const std::string& setting_id, const Bases& bases);
    void                    erase(const std::string& setting_id);
    bool                    has(const std::string& setting_id) const;

private:
    boost::filesystem::path file_of(const std::string& setting_id) const;
    // Reads into the cache when not cached yet. Caller holds the mutex.
    const std::optional<Bases>& cached(const std::string& setting_id) const;

    boost::filesystem::path                                  m_dir;
    mutable std::mutex                                       m_mutex;
    mutable std::map<std::string, std::optional<Bases>>        m_cache;
};

} // namespace Slic3r::BblPresetSync
