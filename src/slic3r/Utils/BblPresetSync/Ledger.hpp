#pragma once

#include "Types.hpp"

#include <boost/filesystem/path.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r::BblPresetSync {

struct LedgerEntry
{
    std::string setting_id;     // Bambu id, "PFUS..."
    long long   update_time{0}; // Bambu server time (unix seconds) at the last sync
    std::string synced_hash;
    std::string state{"linked"}; // linked | declined | unresolvable | too_large
    std::string reason;          // for unresolvable
    int         absent_passes{0};
    long long   absent_since{0}; // local clock, unix seconds, when the row was first seen absent; 0 = not absent
    long long   retry_after{0}; // unix seconds, local clock; 0 = now
    int         fail_count{0};
    std::string last_error;
    std::string converted_from; // old filament id of a converted filament
    std::vector<std::string> k_copied_printers;
};

struct Conversion
{
    std::string              from_filament_id, to_filament_id;
    std::vector<std::string> k_copied_printers;
};

class Ledger
{
public:
    static boost::filesystem::path file_path(const boost::filesystem::path& data_dir,
                                             const std::string&             orca_user_id,
                                             const std::string&             bambu_uid);
    static boost::filesystem::path backup_dir(const boost::filesystem::path& data_dir); // data_dir/"bbl_sync"/"backup"

    explicit Ledger(boost::filesystem::path file);

    bool load(std::string* error = nullptr);
    bool save(std::string* error = nullptr) const;

    std::optional<LedgerEntry> get(const Key& key) const;
    void                       put(const Key& key, const LedgerEntry& entry);
    void                       erase(const Key& key);
    std::map<Key, LedgerEntry> entries() const;

    std::optional<Conversion> get_conversion(const Key& key) const;
    void                      put_conversion(const Key& key, const Conversion& conversion);
    std::map<Key, Conversion> conversions() const;

    bool first_run_confirmed() const;
    void set_first_run_confirmed(bool v);
    std::string halted() const;
    void        set_halted(const std::string& v);
    long long   clock_offset() const;
    void        set_clock_offset(long long v);

    const boost::filesystem::path& file() const;

private:
    boost::filesystem::path    m_file;
    mutable std::mutex         m_mutex;
    mutable std::mutex         m_save_mutex; // serializes whole saves (one .tmp writer at a time)
    std::map<Key, LedgerEntry> m_entries;
    std::map<Key, Conversion>  m_conversions;
    bool                       m_first_run_confirmed{false};
    std::string                m_halted;
    long long                  m_clock_offset{0};
};

} // namespace Slic3r::BblPresetSync
