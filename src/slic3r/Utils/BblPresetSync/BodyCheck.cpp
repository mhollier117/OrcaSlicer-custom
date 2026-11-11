#include "BodyCheck.hpp"

namespace Slic3r::BblPresetSync {

const std::set<std::string>& metadata_keys()
{
    static const std::set<std::string> keys = {"type",    "name",     "setting_id",  "updated_time", "update_time",
                                               "version", "base_id",  "inherits",    "filament_id",  "user_id"};
    return keys;
}

BodyStatus check_body(const ValueMap& body, PresetType row_type, const std::string& row_setting_id, bool row_is_root)
{
    auto value_of = [&body](const char* key) -> const std::string* {
        auto it = body.find(key);
        return it == body.end() ? nullptr : &it->second;
    };

    const std::string* type       = value_of("type");
    const std::string* setting_id = value_of("setting_id");
    if (type == nullptr || *type != type_string(row_type) || setting_id == nullptr || *setting_id != row_setting_id)
        return BodyStatus::WrongIdentity;

    bool has_content = false;
    for (const auto& kv : body)
        if (!metadata_keys().count(kv.first)) {
            has_content = true;
            break;
        }
    if (!has_content) return BodyStatus::MetadataOnly;

    if (row_is_root && row_type == PresetType::Filament && body.find("filament_type") == body.end())
        return BodyStatus::RootMissingType;

    return BodyStatus::Valid;
}

} // namespace Slic3r::BblPresetSync
