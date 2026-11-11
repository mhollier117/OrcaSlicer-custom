#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BblPresetSync/Forms.hpp"
#include "slic3r/Utils/BblPresetSync/Keys.hpp"
#include "slic3r/Utils/BblPresetSync/Merge.hpp"
#include "slic3r/Utils/BblPresetSync/Types.hpp"

#include "libslic3r/Config.hpp"

#include <boost/filesystem/path.hpp>

#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sync = Slic3r::BblPresetSync;
namespace fs   = boost::filesystem;
using sync::FormRule;
using sync::ValueMap;

namespace {

bool orca_k(const std::string& k) { return k != "studio_only"; }
bool studio_k(const std::string& k) { return k != "orca_only"; }

const std::optional<std::string> nothing;

std::optional<std::string> to_studio(FormRule rule, const std::string& v) { return sync::to_studio_form(rule, v); }
std::optional<std::string> to_orca(FormRule rule, const std::string& v, const std::string& now = "") { return sync::to_orca_form(rule, v, now); }

// OrcaSlicer's list form of one text.
std::string orca_note(const std::string& text) { return Slic3r::escape_strings_cstyle({text}); }

sync::Forms test_forms()
{
    return sync::Forms({{"filament_notes", FormRule::StringToStrings},
                        {"line_width", FormRule::FloatToFloatOrPercent},
                        {"enable_arc_fitting", FormRule::Never},
                        {"default_jerk", FormRule::ScalarToList},
                        {"layer_height", FormRule::Same}});
}

sync::MergeResult merge(const sync::Forms& forms, const ValueMap& l, const ValueMap& lb, const ValueMap& r, const ValueMap& rb,
                        bool local_newer = false)
{
    return sync::merge_bodies(sync::MergeInput{l, lb, r, rb, false, local_newer, orca_k, studio_k, &forms});
}

sync::MergeResult merge_pairing(const sync::Forms& forms, const ValueMap& l, const ValueMap& r, bool local_newer = false)
{
    const ValueMap none;
    return sync::merge_bodies(sync::MergeInput{l, none, r, none, true, local_newer, orca_k, studio_k, &forms});
}

} // namespace

// ---------------------------------------------------------------------------
// Rules
// ---------------------------------------------------------------------------

TEST_CASE("form_rule follows the table of type pairs", "[BblPresetSync]")
{
    const std::vector<std::pair<std::pair<std::string, std::string>, FormRule>> table = {
        {{"coFloat", "coFloats"}, FormRule::ScalarToList},
        {{"coInt", "coInts"}, FormRule::ScalarToList},
        {{"coPercent", "coPercents"}, FormRule::ScalarToList},
        {{"coBool", "coBools"}, FormRule::ScalarToList},
        {{"coFloatOrPercent", "coFloatsOrPercents"}, FormRule::ScalarToList},
        {{"coString", "coStrings"}, FormRule::StringToStrings},
        {{"coInts", "coFloats"}, FormRule::IntsToFloats},
        {{"coInt", "coFloat"}, FormRule::IntsToFloats},
        {{"coFloat", "coFloatOrPercent"}, FormRule::FloatToFloatOrPercent},
        {{"coFloats", "coFloatsOrPercents"}, FormRule::FloatToFloatOrPercent},
        {{"coPercent", "coFloatOrPercent"}, FormRule::PercentToFloatOrPercent},
        {{"coPercents", "coFloatsOrPercents"}, FormRule::PercentToFloatOrPercent},
    };
    for (const auto& [pair, rule] : table) {
        CAPTURE(pair.first, pair.second);
        CHECK(sync::form_rule(pair.first, pair.second) == rule);
    }
    CHECK(sync::form_rule("coBool", "coEnum") == FormRule::Never);
    CHECK(sync::form_rule("coFloats", "coFloat") == FormRule::Never); // the table is one-way
    CHECK(sync::form_rule("coFloat", "coFloat") == FormRule::Same);
    CHECK(sync::form_rule("", "coFloats") == FormRule::Same);
    CHECK(sync::form_rule("coFloat", "") == FormRule::Same);
}

TEST_CASE("Same and Never translate everything or nothing", "[BblPresetSync]")
{
    CHECK(to_studio(FormRule::Same, "a b") == std::optional<std::string>("a b"));
    CHECK(to_orca(FormRule::Same, "a b", "x") == std::optional<std::string>("a b"));
    CHECK(to_studio(FormRule::Never, "1") == nothing);
    CHECK(to_orca(FormRule::Never, "1", "1") == nothing);
}

TEST_CASE("ScalarToList sends the first item and repeats a value found on Bambu", "[BblPresetSync]")
{
    const FormRule rule = FormRule::ScalarToList;
    CHECK(to_studio(rule, "9,12") == std::optional<std::string>("9"));
    CHECK(to_studio(rule, " 9 , 12") == std::optional<std::string>("9"));
    CHECK(to_studio(rule, "9") == std::optional<std::string>("9"));
    CHECK(to_studio(rule, "") == nothing);
    CHECK(to_studio(rule, "nil") == nothing);
    CHECK(to_studio(rule, "nil,9") == nothing);
    CHECK(to_studio(rule, ",9") == nothing);

    CHECK(to_orca(rule, "10", "9,12") == std::optional<std::string>("10,10"));
    CHECK(to_orca(rule, "9,9", "9,12") == std::optional<std::string>("9,9")); // an older upload in OrcaSlicer's form
    CHECK(to_orca(rule, "10", "") == std::optional<std::string>("10"));
    CHECK(to_orca(rule, "10", "9") == std::optional<std::string>("10"));
    CHECK(to_orca(rule, "", "9,12") == nothing);
    CHECK(to_orca(rule, "nil", "9,12") == nothing);
}

TEST_CASE("StringToStrings carries one text between Studio's scalar form and OrcaSlicer's list form", "[BblPresetSync]")
{
    const FormRule rule = FormRule::StringToStrings;

    // An empty note: OrcaSlicer's list of one empty text is Studio's empty value, and back.
    CHECK(orca_note("") == "\"\"");
    CHECK(to_studio(rule, "\"\"") == std::optional<std::string>(""));
    CHECK(to_studio(rule, "") == std::optional<std::string>(""));
    CHECK(to_orca(rule, "") == std::optional<std::string>("\"\""));

    // A plain text is the same in both.
    CHECK(to_studio(rule, "plain") == std::optional<std::string>("plain"));
    CHECK(to_orca(rule, "plain") == std::optional<std::string>("plain"));

    // A note with a line break, a quote, a semicolon and a backslash survives OrcaSlicer -> Studio -> OrcaSlicer.
    const std::string text  = "line one\nsays \"hi\"; path C:\\temp";
    const std::string local = orca_note(text);
    const auto        cloud = to_studio(rule, local);
    REQUIRE(cloud.has_value());
    CHECK(*cloud == Slic3r::escape_string_cstyle(text));
    CHECK(cloud->front() != '"');
    CHECK(to_orca(rule, *cloud) == std::optional<std::string>(local));

    // A value from an older upload in OrcaSlicer's own form is taken unchanged.
    CHECK(to_orca(rule, "\"\"") == std::optional<std::string>("\"\""));
    CHECK(to_orca(rule, local) == std::optional<std::string>(local));
    // Studio's form of the two-character text "" becomes OrcaSlicer's list form of that text.
    CHECK(to_orca(rule, "\\\"\\\"") == std::optional<std::string>(orca_note("\"\"")));

    // Values neither side can read.
    CHECK(to_studio(rule, "\"unterminated") == nothing);
    CHECK(to_orca(rule, "\"unterminated") == nothing);
    CHECK(to_orca(rule, "ends in a backslash\\") == nothing);
}

TEST_CASE("IntsToFloats sends whole numbers only", "[BblPresetSync]")
{
    const FormRule rule = FormRule::IntsToFloats;
    CHECK(to_studio(rule, "40,40.0,nil") == std::optional<std::string>("40,40,nil"));
    CHECK(to_studio(rule, "100,80") == std::optional<std::string>("100,80"));
    CHECK(to_studio(rule, "-3") == std::optional<std::string>("-3"));
    CHECK(to_studio(rule, "40.5") == nothing);
    CHECK(to_studio(rule, "40,40.5") == nothing);
    CHECK(to_studio(rule, "abc") == nothing);
    CHECK(to_studio(rule, "40x") == nothing);
    CHECK(to_studio(rule, "1e10") == nothing);
    CHECK(to_orca(rule, "40,nil") == std::optional<std::string>("40,nil"));
}

TEST_CASE("FloatToFloatOrPercent sends numbers and keeps a percent local", "[BblPresetSync]")
{
    const FormRule rule = FormRule::FloatToFloatOrPercent;
    CHECK(to_studio(rule, "0.45") == std::optional<std::string>("0.45"));
    CHECK(to_studio(rule, "110%") == nothing);
    CHECK(to_studio(rule, "10,50%") == nothing);
    CHECK(to_studio(rule, "10,nil") == std::optional<std::string>("10,nil"));
    CHECK(to_orca(rule, "0.45") == std::optional<std::string>("0.45"));
}

TEST_CASE("PercentToFloatOrPercent sends percents and marks a bare number found on Bambu as one", "[BblPresetSync]")
{
    const FormRule rule = FormRule::PercentToFloatOrPercent;
    CHECK(to_studio(rule, "10%") == std::optional<std::string>("10%"));
    CHECK(to_studio(rule, "nil") == std::optional<std::string>("nil"));
    CHECK(to_studio(rule, "0.5") == nothing);
    CHECK(to_orca(rule, "10") == std::optional<std::string>("10%"));
    CHECK(to_orca(rule, "10%") == std::optional<std::string>("10%"));
    CHECK(to_orca(rule, "10,nil,20%") == std::optional<std::string>("10%,nil,20%"));
}

TEST_CASE("Forms keeps the rules other than Same and translates by key", "[BblPresetSync]")
{
    const sync::Forms forms = test_forms();
    CHECK(forms.size() == 4);
    CHECK(forms.rule("layer_height") == FormRule::Same);
    CHECK(forms.rule("unknown") == FormRule::Same);
    CHECK(forms.rule("filament_notes") == FormRule::StringToStrings);
    CHECK(forms.to_studio("default_jerk", "9,12") == std::optional<std::string>("9"));
    CHECK(forms.to_studio("unknown", "9,12") == std::optional<std::string>("9,12"));
    CHECK(forms.to_orca("default_jerk", "10", "9,12") == std::optional<std::string>("10,10"));
    CHECK(sync::Forms().size() == 0);
}

TEST_CASE("studio_body translates a local map and leaves out what Studio cannot hold", "[BblPresetSync]")
{
    const sync::Forms forms = test_forms();
    const ValueMap    local{{"filament_notes", "\"\""}, {"line_width", "110%"}, {"layer_height", "0.2"},
                            {"default_jerk", "9,12"},  {"enable_arc_fitting", "1"}, {"type", "print"}};
    const ValueMap    expected{{"filament_notes", ""}, {"layer_height", "0.2"}, {"default_jerk", "9"}, {"type", "print"}};
    CHECK(forms.studio_body(local) == expected);
}

// ---------------------------------------------------------------------------
// The merge with forms
// ---------------------------------------------------------------------------

TEST_CASE("merge_bodies with forms compares and takes values in each side's own form", "[BblPresetSync]")
{
    const sync::Forms forms = test_forms();

    SECTION("Studio saves the preset again without a change: nothing is taken, nothing pending")
    {
        const ValueMap local{{"filament_notes", "\"\""}, {"default_jerk", "9,9"}};
        const ValueMap remote{{"filament_notes", ""}, {"default_jerk", "9"}};
        const auto     m = merge(forms, local, local, remote, remote);
        CHECK(m.merged == local);
        CHECK(m.taken_remote == 0);
        CHECK(m.pending_local.empty());
        CHECK(m.conflicts == 0);
    }
    SECTION("a note edited in Studio arrives in OrcaSlicer's list form")
    {
        const std::string text = "dry first\nthen print";
        const auto        m    = merge(forms, {{"filament_notes", "\"\""}}, {{"filament_notes", "\"\""}},
                                       {{"filament_notes", Slic3r::escape_string_cstyle(text)}}, {{"filament_notes", ""}});
        CHECK(m.merged.at("filament_notes") == orca_note(text));
        CHECK(m.taken_remote == 1);
        CHECK(m.pending_local.empty());
    }
    SECTION("a note edited locally is pending and goes up in Studio's form")
    {
        const ValueMap local{{"filament_notes", orca_note("new note")}, {"type", "filament"}};
        const ValueMap local_base{{"filament_notes", "\"\""}};
        const ValueMap remote{{"filament_notes", ""}, {"type", "filament"}};
        const auto     m = merge(forms, local, local_base, remote, remote);
        CHECK(m.pending_local == std::set<std::string>{"filament_notes"});
        CHECK(m.merged.at("filament_notes") == orca_note("new note"));
        const auto up = sync::apply_local_edits(remote, local, local_base, orca_k, &forms);
        CHECK(up.at("filament_notes") == "new note");
    }
    SECTION("a line width set to a percent locally is pending but leaves the remote number")
    {
        const ValueMap local{{"line_width", "110%"}};
        const ValueMap base{{"line_width", "0.42"}};
        const auto     m = merge(forms, local, base, base, base);
        CHECK(m.pending_local == std::set<std::string>{"line_width"});
        CHECK(m.merged.at("line_width") == "110%");
        CHECK(sync::apply_local_edits(base, local, base, orca_k, &forms).at("line_width") == "0.42");
    }
    SECTION("a Never key changed on Bambu keeps the local value and nothing is pending")
    {
        const auto m = merge(forms, {{"enable_arc_fitting", "1"}}, {{"enable_arc_fitting", "1"}}, {{"enable_arc_fitting", "0"}},
                             {{"enable_arc_fitting", "1"}});
        CHECK(m.merged == ValueMap{{"enable_arc_fitting", "1"}});
        CHECK(m.pending_local.empty());
        CHECK(m.taken_remote == 0);
        CHECK(m.kept_local == 0);
        CHECK(m.conflicts == 0);
    }
    SECTION("a Never key edited locally is not written into the body")
    {
        const ValueMap remote{{"enable_arc_fitting", "0"}};
        const auto     up = sync::apply_local_edits(remote, {{"enable_arc_fitting", "2"}}, {{"enable_arc_fitting", "1"}}, orca_k, &forms);
        CHECK(up.at("enable_arc_fitting") == "0");
    }
    SECTION("a remote value OrcaSlicer cannot take keeps the local value")
    {
        const auto m = merge(forms, {{"default_jerk", "9,12"}}, {{"default_jerk", "9,12"}}, {{"default_jerk", "nil"}}, {{"default_jerk", "9"}});
        CHECK(m.merged.at("default_jerk") == "9,12");
        CHECK(m.kept_local == 1);
        CHECK(m.taken_remote == 0);
    }
    SECTION("changed on both sides to the same value in each side's form")
    {
        const auto m = merge(forms, {{"default_jerk", "10,10"}}, {{"default_jerk", "9,9"}}, {{"default_jerk", "10"}}, {{"default_jerk", "9"}});
        CHECK(m.merged.at("default_jerk") == "10,10");
        CHECK(m.conflicts == 0);
        CHECK(m.pending_local.empty());
    }
    SECTION("pairing: a local jerk 9,9 and a remote 9 are no difference")
    {
        const auto m = merge_pairing(forms, {{"default_jerk", "9,9"}}, {{"default_jerk", "9"}});
        CHECK(m.conflicts == 0);
        CHECK(m.pending_local.empty());
        CHECK(m.merged.at("default_jerk") == "9,9");
    }
    SECTION("pairing: a local jerk 9,12 and a remote 9 keep the local list")
    {
        const auto m = merge_pairing(forms, {{"default_jerk", "9,12"}}, {{"default_jerk", "9"}});
        CHECK(m.merged.at("default_jerk") == "9,12");
        CHECK(m.pending_local.empty());
        CHECK(m.taken_remote == 0);
        CHECK(m.kept_local == 0);
        CHECK(m.conflicts == 0);
    }
    SECTION("Studio saved an older upload in OrcaSlicer's form again in its own form: the local list stays")
    {
        const ValueMap local{{"default_jerk", "9,12"}};
        const auto     m = merge(forms, local, local, {{"default_jerk", "9"}}, {{"default_jerk", "9,12"}});
        CHECK(m.merged.at("default_jerk") == "9,12");
        CHECK(m.pending_local.empty());
        CHECK(m.taken_remote == 0);
    }
    SECTION("a real edit in Studio of a list setting is still taken")
    {
        const ValueMap local{{"default_jerk", "9,12"}};
        const auto     m = merge(forms, local, local, {{"default_jerk", "10"}}, {{"default_jerk", "9"}});
        CHECK(m.merged.at("default_jerk") == "10,10");
        CHECK(m.taken_remote == 1);
        CHECK(m.pending_local.empty());
    }
    SECTION("pairing: a local value Studio cannot hold differs")
    {
        const auto m = merge_pairing(forms, {{"line_width", "110%"}}, {{"line_width", "0.42"}}, true);
        CHECK(m.conflicts == 1);
        CHECK(m.pending_local == std::set<std::string>{"line_width"});
    }
}

TEST_CASE("upload_with_foreign with forms starts from the local map in Studio's form", "[BblPresetSync]")
{
    const sync::Forms forms = test_forms();
    const auto        up    = sync::upload_with_foreign({{"filament_notes", "\"\""}, {"line_width", "110%"}}, {{"studio_only", "s"}}, orca_k, &forms);
    CHECK(up == ValueMap{{"filament_notes", ""}, {"studio_only", "s"}});
}

// ---------------------------------------------------------------------------
// The rules for the bundled Studio key list
// ---------------------------------------------------------------------------

TEST_CASE("build_forms finds the settings whose type differs in the bundled list", "[BblPresetSync]")
{
    const fs::path bundled = fs::path(std::string(PROFILES_DIR)).parent_path() / "printers" / "bambu_studio_option_keys.json";
    const auto     studio  = sync::StudioKeys::load(bundled);
    REQUIRE(studio.has_types());
    const sync::Forms forms = sync::build_forms(studio, sync::orca_type_name);

    CHECK(forms.rule("filament_notes") == FormRule::StringToStrings);
    CHECK(forms.rule("fan_max_speed") == FormRule::IntsToFloats);
    CHECK(forms.rule("line_width") == FormRule::FloatToFloatOrPercent);
    CHECK(forms.rule("seam_gap") == FormRule::PercentToFloatOrPercent);
    CHECK(forms.rule("default_jerk") == FormRule::ScalarToList);
    CHECK(forms.rule("enable_arc_fitting") == FormRule::Never);
    CHECK(forms.rule("layer_height") == FormRule::Same);
    CHECK(forms.size() == 30);

    // Without types there are no rules.
    CHECK(sync::build_forms(sync::StudioKeys({"filament_notes"}), sync::orca_type_name).size() == 0);
    // A key OrcaSlicer does not define has no rule.
    CHECK(sync::build_forms(sync::StudioKeys({"x"}, {{"x", "coFloat"}}), sync::orca_type_name).size() == 0);
}
