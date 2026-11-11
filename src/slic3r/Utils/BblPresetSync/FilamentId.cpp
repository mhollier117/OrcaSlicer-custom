#include "FilamentId.hpp"

#include "Hash.hpp"

namespace Slic3r::BblPresetSync {

std::string filament_base_name(const std::string& preset_name)
{
    const size_t pos = preset_name.find(" @");
    if (pos == std::string::npos || pos == 0) return preset_name;
    return preset_name.substr(0, pos);
}

std::string make_filament_id(const std::string& base_name, const std::string& bambu_uid, const std::string& salt)
{
    return "P" + md5_hex(base_name + "@" + bambu_uid + salt).substr(0, 7);
}

std::string choose_filament_id(const std::string&                  preset_name,
                               const std::string&                  bambu_uid,
                               const std::vector<ExistingRoot>&    roots,
                               const std::function<std::string()>& next_salt)
{
    const std::string base = filament_base_name(preset_name);

    for (const ExistingRoot& root : roots)
        if (root.base_name == base && root.filament_id.size() == 8 && root.filament_id[0] == 'P') return root.filament_id;

    auto clashes = [&](const std::string& id) {
        for (const ExistingRoot& root : roots)
            if (root.base_name != base && root.filament_id == id) return true;
        return false;
    };

    std::string id = make_filament_id(base, bambu_uid);
    for (int tries = 0; clashes(id); ++tries) {
        if (tries >= 16) return std::string();
        id = make_filament_id(base, bambu_uid, next_salt());
    }
    return id;
}

} // namespace Slic3r::BblPresetSync
