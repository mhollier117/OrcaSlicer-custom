#pragma once

#include "Keys.hpp"
#include "Types.hpp"

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace Slic3r::BblPresetSync {

// How a value of a setting whose type differs between Bambu Studio and OrcaSlicer is carried across. The Bambu copy of a
// preset always holds Studio's form.
enum class FormRule { Same, ScalarToList, StringToStrings, IntsToFloats, FloatToFloatOrPercent, PercentToFloatOrPercent, Never };

// From the two type tokens (Studio's, OrcaSlicer's). An empty token on either side gives Same.
FormRule form_rule(const std::string& studio_type, const std::string& orca_type);

// OrcaSlicer's upload form -> Studio's form. nullopt: Studio cannot hold this value.
std::optional<std::string> to_studio_form(FormRule rule, const std::string& orca_value);
// A value found on Bambu -> OrcaSlicer's form. `orca_now`: OrcaSlicer's current value ("" when it has none).
// nullopt: OrcaSlicer cannot take it.
std::optional<std::string> to_orca_form(FormRule rule, const std::string& cloud_value, const std::string& orca_now);

// The rule of every setting whose type differs.
class Forms
{
public:
    Forms() = default;
    explicit Forms(std::map<std::string, FormRule> rules); // only rules other than Same are kept

    FormRule    rule(const std::string& key) const; // Same for a key without a rule
    std::size_t size() const { return m_rules.size(); }

    std::optional<std::string> to_studio(const std::string& key, const std::string& orca_value) const;
    std::optional<std::string> to_orca(const std::string& key, const std::string& cloud_value, const std::string& orca_now) const;
    // A local upload map as it goes to Bambu when there is no Bambu copy to start from: every value translated,
    // a key whose value Studio cannot hold left out. Keys without a rule pass unchanged.
    ValueMap studio_body(const ValueMap& local) const;

private:
    std::map<std::string, FormRule> m_rules;
};

// One rule per key of `studio` that has a type, OrcaSlicer also defines, and whose two tokens differ.
Forms build_forms(const StudioKeys& studio, const std::function<std::string(const std::string&)>& orca_type);

} // namespace Slic3r::BblPresetSync
