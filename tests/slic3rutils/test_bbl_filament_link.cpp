#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "slic3r/Utils/BblFilament/Spool.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"
#include "slic3r/Utils/BblFilament/SpoolService.hpp"

#include "bbl_filament_test_utils.hpp"

#include <nlohmann/json.hpp>

#include <functional>
#include <memory>
#include <new>
#include <set>
#include <string>
#include <vector>

namespace fil = Slic3r::BblFilament;
using fil::json;
using namespace BblFilamentTest;

namespace {

using Request = fil::SpoolService::LinkRequest;
using Outcome = fil::SpoolService::LinkOutcome;
using Done    = std::function<void(const Outcome&)>;

Request ams_request(Done done = {})
{
    Request r;
    r.kind       = Request::Kind::AmsSync;
    r.dev_id     = "dev1";
    r.build_ams  = [](const std::vector<fil::Spool>&, const std::set<std::string>&) {
        fil::AmsItem i;
        i.rfid    = "RFID1";
        i.ams_sn  = "sn1";
        i.slot_id = "0";
        return std::vector<fil::AmsItem>{i};
    };
    r.done = std::move(done);
    return r;
}

Request slots_request(Done done = {})
{
    Request r;
    r.kind        = Request::Kind::SlotSync;
    r.dev_id      = "dev1";
    r.build_slots = [](const std::vector<fil::Spool>&, const std::set<std::string>&) {
        fil::SlotItem i;
        i.ams_sn   = "sn1";
        i.slot_id  = "1";
        i.spool_id = "1000001";
        return std::vector<fil::SlotItem>{i};
    };
    r.done = std::move(done);
    return r;
}

Request soft_read_request(Done done = {})
{
    Request r;
    r.kind   = Request::Kind::SoftMatchRead;
    r.dev_id = "dev1";
    r.ams_sn = "sn1";
    r.done   = std::move(done);
    return r;
}

Request soft_act_request(Done done = {})
{
    Request r;
    r.kind     = Request::Kind::SoftMatchAct;
    r.action   = "accept";
    r.spool_id = "1000001";
    r.done     = std::move(done);
    return r;
}

int index_of(const std::vector<std::string>& log, const std::string& prefix)
{
    for (std::size_t i = 0; i < log.size(); ++i)
        if (log[i].rfind(prefix, 0) == 0) return static_cast<int>(i);
    return -1;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// pure parts

TEST_CASE("A spool id fits an int only when it is positive and at most INT_MAX", "[BblFilament]")
{
    CHECK(fil::spool_id_as_int("1000001") == std::optional<int>(1000001));
    CHECK(fil::spool_id_as_int("2147483647") == std::optional<int>(2147483647));
    CHECK_FALSE(fil::spool_id_as_int("2147483648").has_value());
    CHECK_FALSE(fil::spool_id_as_int("0").has_value());
    CHECK_FALSE(fil::spool_id_as_int("").has_value());
    CHECK_FALSE(fil::spool_id_as_int("abc").has_value());
    CHECK_FALSE(fil::spool_id_as_int("-5").has_value());
}

TEST_CASE("An ams sync reply is read into one result per row", "[BblFilament]")
{
    const json full = json::parse(R"({"createdRFIDs":["A1","B2"],"results":[
        {"amsSn":"s1","slotId":"1","spoolId":10004295},{"amsSn":"s1","slotId":2,"spoolId":null},{"amsSn":"s2","slotId":"0"}]})");
    const auto results = fil::parse_ams_sync_results(full);
    REQUIRE(results.size() == 3);
    CHECK(results[0].spool_id == "10004295");
    CHECK(results[0].ams_sn == "s1");
    CHECK(results[0].slot_id == "1");
    CHECK(results[1].spool_id.empty());
    CHECK(results[1].slot_id == "2");
    CHECK(results[2].spool_id.empty());
    const auto rfids = fil::parse_created_rfids(full);
    REQUIRE(rfids.size() == 2);
    CHECK(rfids[1] == "B2");
}

TEST_CASE("The ams sync parsers tolerate empty, null and malformed bodies", "[BblFilament]")
{
    const std::vector<json> bodies = {json(), json::object(), json::array(), json("text"), json::parse(R"({"results":null,"createdRFIDs":null})"),
                                      json::parse(R"({"results":"x","createdRFIDs":5})"), json::parse(R"({"results":[1,"a",null,{}]})"),
                                      json::parse(R"({"createdRFIDs":[1,null,""]})")};
    for (const json& b : bodies) {
        CHECK(fil::parse_created_rfids(b).empty());
        for (const auto& r : fil::parse_ams_sync_results(b)) CHECK(r.spool_id.empty());
    }
    CHECK(fil::parse_ams_sync_results(json::parse(R"({"results":[1,"a",null,{}]})")).size() == 1);
}

TEST_CASE("A slot mapping reply is read into codes, and a missing code counts as refused", "[BblFilament]")
{
    const auto results = fil::parse_slot_results(json::parse(R"({"results":[{"amsSn":"s1","slotId":"1","code":0},
        {"amsSn":"s1","slotId":"2","code":3},{"amsSn":"s1","slotId":"3"},{"amsSn":"s1","slotId":"4","code":"0"},7]})"));
    REQUIRE(results.size() == 4);
    CHECK(results[0].code == 0);
    CHECK(results[1].code == 3);
    CHECK(results[2].code == -1);
    CHECK(results[3].code == -1);
    CHECK(fil::parse_slot_results(json()).empty());
    CHECK(fil::parse_slot_results(json::object()).empty());
    CHECK(fil::parse_slot_results(json::parse(R"({"results":null})")).empty());
    CHECK(fil::parse_slot_results(json::parse(R"([1])")).empty());
}

TEST_CASE("A soft match reply ties each candidate list to its hit", "[BblFilament]")
{
    json body = {{"hits", json::array({row_with(1000001, "New A"), row_with(1000002, "New B")})},
                 {"candidates", json::array({json{{"pendingId", 1000002}, {"candidates", json::array({row_with(1000007, "Old"), row_with(1000008, "Older")})}},
                                             json{{"pendingId", 999}, {"candidates", json::array({row_with(1000009)})}}})}};
    const auto q = fil::parse_soft_match(body);
    REQUIRE(q.entries.size() == 2);
    CHECK(q.entries[0].hit.id == "1000001");
    CHECK(q.entries[0].candidates.empty());
    CHECK(q.entries[1].hit.name == "New B");
    REQUIRE(q.entries[1].candidates.size() == 2);
    CHECK(q.entries[1].candidates[1].id == "1000008");
}

TEST_CASE("The soft match parser tolerates empty, null and malformed bodies", "[BblFilament]")
{
    CHECK(fil::parse_soft_match(json()).entries.empty());
    CHECK(fil::parse_soft_match(json::object()).entries.empty());
    CHECK(fil::parse_soft_match(json::parse(R"({"hits":[],"candidates":[]})")).entries.empty());
    CHECK(fil::parse_soft_match(json::parse(R"({"hits":null,"candidates":null})")).entries.empty());
    CHECK(fil::parse_soft_match(json::parse(R"({"hits":"x","candidates":3})")).entries.empty());
    CHECK(fil::parse_soft_match(json::parse(R"({"hits":[1,null,{"id":"no"}],"candidates":[1,{"pendingId":"x"}]})")).entries.empty());
    const auto q = fil::parse_soft_match(json{{"hits", json::array({row_with(1000001)})}, {"candidates", json::array({json{{"pendingId", 1000001}, {"candidates", nullptr}}})}});
    REQUIRE(q.entries.size() == 1);
    CHECK(q.entries[0].candidates.empty());
}

// ---------------------------------------------------------------------------------------------------------------------
// PluginSpoolCloud

TEST_CASE("The plugin spool cloud reports the link calls unavailable when no plugin is loaded", "[BblFilament]")
{
    fil::PluginSpoolCloud cloud;
    fil::AmsItem          ai;
    ai.rfid = "R";
    fil::SlotItem si;
    si.ams_sn   = "s";
    si.slot_id  = "0";
    si.spool_id = "1000001";
    CHECK(cloud.sync_ams("dev", {ai}).error == fil::CloudError::Unavailable);
    CHECK(cloud.sync_slots("dev", {si}).error == fil::CloudError::Unavailable);
    CHECK(cloud.soft_match_pending("dev", "").error == fil::CloudError::Unavailable);
    CHECK(cloud.soft_match_action("accept", "1000001", "").error == fil::CloudError::Unavailable);
}

TEST_CASE("The plugin spool cloud rejects invalid link input before it looks for the plugin", "[BblFilament]")
{
    fil::PluginSpoolCloud cloud;
    fil::AmsItem          ai;
    CHECK(cloud.sync_ams("", {ai}).error == fil::CloudError::Rejected);
    CHECK(cloud.sync_ams("dev", {}).error == fil::CloudError::Rejected);

    fil::SlotItem good;
    good.ams_sn   = "s";
    good.slot_id  = "0";
    good.spool_id = "1000001";
    SECTION("empty serial")
    {
        fil::SlotItem bad = good;
        bad.ams_sn.clear();
        CHECK(cloud.sync_slots("dev", {good, bad}).error == fil::CloudError::Rejected);
    }
    SECTION("empty slot")
    {
        fil::SlotItem bad = good;
        bad.slot_id.clear();
        CHECK(cloud.sync_slots("dev", {bad}).error == fil::CloudError::Rejected);
    }
    SECTION("negative ids")
    {
        fil::SlotItem bad = good;
        bad.ams_id = -1;
        CHECK(cloud.sync_slots("dev", {bad}).error == fil::CloudError::Rejected);
        bad         = good;
        bad.ams_type = -1;
        CHECK(cloud.sync_slots("dev", {bad}).error == fil::CloudError::Rejected);
    }
    SECTION("a spool id that does not fit would unbind, so it is refused")
    {
        fil::SlotItem bad = good;
        bad.spool_id = "2147483648";
        CHECK(cloud.sync_slots("dev", {bad}).error == fil::CloudError::Rejected);
        bad.spool_id = "x";
        CHECK(cloud.sync_slots("dev", {bad}).error == fil::CloudError::Rejected);
    }
    SECTION("an unbind is valid input")
    {
        fil::SlotItem unbind = good;
        unbind.spool_id.clear();
        CHECK(cloud.sync_slots("dev", {unbind}).error == fil::CloudError::Unavailable);
    }

    CHECK(cloud.soft_match_action("merge", "1000001", "").error == fil::CloudError::Rejected);
    CHECK(cloud.soft_match_action("accept", "0", "").error == fil::CloudError::Rejected);
    CHECK(cloud.soft_match_action("accept", "2147483648", "").error == fil::CloudError::Rejected);
    CHECK(cloud.soft_match_action("create_new", "", "").error == fil::CloudError::Rejected);
    CHECK(cloud.soft_match_action("link_other", "1000001", "").error == fil::CloudError::Rejected);
    CHECK(cloud.soft_match_action("link_other", "1000001", "2147483648").error == fil::CloudError::Rejected);
    CHECK(cloud.soft_match_action("link_other", "1000001", "1000002").error == fil::CloudError::Unavailable);
    CHECK(cloud.soft_match_action("accept", "1000001", "garbage").error == fil::CloudError::Unavailable);
}

// ---------------------------------------------------------------------------------------------------------------------
// service: confirmed rows, busy ids, list serial

TEST_CASE("The confirmed rows ignore a waiting edit that the view shows", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    CHECK(rig.svc->confirmed().empty());
    CHECK(rig.svc->list_serial() == 0);
    rig.svc->request_pull();
    rig.settle();
    REQUIRE(rig.svc->confirmed().size() == 2);
    CHECK(rig.svc->confirmed()[0].note.empty());

    rig.svc->update(json{{"spool_id", "1000001"}, {"note", "waiting"}});
    CHECK(rig.note_of("1000001") == "waiting");
    CHECK(rig.svc->confirmed()[0].note.empty());

    rig.settle();
    CHECK(rig.svc->confirmed()[0].note == "waiting");
}

TEST_CASE("The confirmed rows are empty when signed out or after a reset", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.settle();
    REQUIRE(rig.svc->confirmed().size() == 2);
    rig.session.signed_in = false;
    CHECK(rig.svc->confirmed().empty());
    rig.session.signed_in = true;
    rig.svc->reset();
    CHECK(rig.svc->confirmed().empty());
}

TEST_CASE("Busy ids cover queued, running and settled edits and deletes until the pull", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(3);
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.svc->busy_ids().empty());

    rig.svc->update(json{{"spool_id", "1000001"}, {"note", "a"}});   // running
    rig.svc->remove({"1000002"});                                   // queued
    CHECK(rig.svc->busy_ids() == std::set<std::string>{"1000001", "1000002"});

    rig.step_worker();
    rig.run_ui(); // the update is settled, the delete runs
    CHECK(rig.svc->busy_ids() == std::set<std::string>{"1000001", "1000002"});
    rig.step_worker();
    rig.run_ui(); // the delete is settled, the pull runs
    CHECK(rig.svc->busy_ids() == std::set<std::string>{"1000001", "1000002"});
    rig.settle();
    CHECK(rig.svc->busy_ids().empty());
}

TEST_CASE("The list serial grows with every complete list and not with a failed one", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.svc->list_serial() == 1);
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.svc->list_serial() == 2);
    rig.cloud->forced["list"].push_back(forced_error(fil::CloudError::Unknown));
    rig.svc->request_pull();
    rig.settle();
    CHECK(rig.svc->list_serial() == 2);
}

// ---------------------------------------------------------------------------------------------------------------------
// service: link ops

TEST_CASE("A link op runs after earlier writes and before the pull they wanted", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();

    rig.svc->update(json{{"spool_id", "1000001"}, {"note", "x"}});
    int calls = 0;
    CHECK(rig.svc->link(ams_request([&](const Outcome&) { ++calls; })).ok);
    rig.settle();
    CHECK(calls == 1);
    REQUIRE(rig.cloud->log.size() == 3);
    CHECK(rig.cloud->log[0] == "update 1000001");
    CHECK(rig.cloud->log[1] == "sync_ams dev1");
    CHECK(rig.cloud->log[2] == "list 0");
}

TEST_CASE("Exactly one pull follows a round of two link ops", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    rig.svc->request_pull();
    rig.settle();
    rig.cloud->log.clear();

    rig.svc->link(ams_request());
    rig.svc->link(slots_request());
    rig.settle();
    REQUIRE(rig.cloud->log.size() == 3);
    CHECK(rig.cloud->log[0] == "sync_ams dev1");
    CHECK(rig.cloud->log[1] == "sync_slots dev1");
    CHECK(rig.cloud->count("list") == 1);
}

TEST_CASE("A link builder sees the rows as they are when the op starts", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    rig.svc->request_pull();
    rig.settle();

    rig.svc->update(json{{"spool_id", "1000001"}, {"note", "edited"}});
    rig.step_worker();
    rig.run_ui(); // the edit is done; the pull it wanted is running

    std::string          note_seen = "<none>";
    std::set<std::string> busy_seen{"<none>"};
    Request              r = slots_request();
    r.build_slots = [&](const std::vector<fil::Spool>& rows, const std::set<std::string>& busy) {
        note_seen = rows.empty() ? "<empty>" : rows.front().note;
        busy_seen = busy;
        fil::SlotItem i;
        i.ams_sn = "sn1";
        i.slot_id = "0";
        return std::vector<fil::SlotItem>{i};
    };
    rig.svc->link(std::move(r));
    rig.settle();
    CHECK(note_seen == "edited");
    CHECK(busy_seen.empty());
    CHECK(rig.cloud->count("sync_slots") == 1);
}

TEST_CASE("A link builder is told which spools have an edit in flight", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.svc->request_pull();
    rig.settle();

    rig.svc->update(json{{"spool_id", "1000002"}, {"note", "e"}});
    std::set<std::string> busy_seen;
    Request               r = ams_request();
    r.build_ams = [&](const std::vector<fil::Spool>&, const std::set<std::string>& busy) {
        busy_seen = busy;
        return std::vector<fil::AmsItem>{fil::AmsItem{}};
    };
    rig.svc->link(std::move(r));
    rig.settle();
    CHECK(busy_seen == std::set<std::string>{"1000002"});
}

TEST_CASE("A link op with an empty build is dropped without a cloud call", "[BblFilament]")
{
    Rig rig;
    int calls = 0;
    Outcome got;
    Request r = ams_request([&](const Outcome& o) {
        ++calls;
        got = o;
    });
    r.build_ams = [](const std::vector<fil::Spool>&, const std::set<std::string>&) { return std::vector<fil::AmsItem>(); };
    CHECK(rig.svc->link(std::move(r)).ok);
    rig.settle();
    CHECK(calls == 1);
    CHECK_FALSE(got.sent);
    CHECK_FALSE(got.ok);
    CHECK(got.kind == fil::FailKind::BadRequest);
    CHECK(rig.cloud->count("sync_ams") == 0);
    CHECK(rig.cloud->count("list") == 0);
    CHECK_FALSE(rig.has_event("push_done sync_ams "));
    CHECK_FALSE(rig.svc->state().is_syncing);
}

TEST_CASE("A link op without its builder is refused", "[BblFilament]")
{
    Rig rig;
    Request r;
    r.kind = Request::Kind::AmsSync;
    CHECK(rig.svc->link(r).kind == fil::FailKind::BadRequest);
    r.kind = Request::Kind::SlotSync;
    CHECK_FALSE(rig.svc->link(r).ok);
    CHECK(rig.worker.empty());
}

TEST_CASE("The done callback of a link op gets the reply and what was sent, once", "[BblFilament]")
{
    Rig rig;
    rig.cloud->rows.push_back(row_with(1000001));
    rig.cloud->rows[0]["RFID"] = "RFID1";
    int     calls = 0;
    Outcome got;
    rig.svc->link(ams_request([&](const Outcome& o) {
        ++calls;
        got = o;
    }));
    rig.settle();
    CHECK(calls == 1);
    CHECK(got.sent);
    CHECK(got.ok);
    REQUIRE(got.ams_sent.size() == 1);
    CHECK(got.ams_sent[0].rfid == "RFID1");
    CHECK(got.slots_sent.empty());
    const auto results = fil::parse_ams_sync_results(got.body);
    REQUIRE(results.size() == 1);
    CHECK(results[0].spool_id == "1000001");
    CHECK(rig.has_event("push_done sync_ams "));
    CHECK(rig.svc->state().last_synced_at > 0);
    CHECK_FALSE(rig.svc->state().has_error);
}

TEST_CASE("A slot sync and a soft match action fire their events and want a pull", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    rig.svc->link(slots_request());
    rig.settle();
    CHECK(rig.has_event("push_done slot_mappings "));
    CHECK(rig.cloud->count("list") == 1);

    rig.cloud->log.clear();
    Outcome got;
    rig.svc->link(soft_act_request([&](const Outcome& o) { got = o; }));
    rig.settle();
    CHECK(got.ok);
    CHECK(rig.has_event("push_done soft_match "));
    CHECK(rig.cloud->count("soft_match_action accept") == 1);
    CHECK(rig.cloud->actions_got.back() == "accept 1000001 ");
    CHECK(rig.cloud->count("list") == 1);
}

TEST_CASE("A mount the cloud applied shows in the list that follows the link op", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(2);
    rig.cloud->apply_mounts = true;
    rig.svc->link(slots_request());
    rig.settle();
    bool found = false;
    for (const auto& s : rig.svc->confirmed())
        if (s.id == "1000001") {
            found = true;
            CHECK(s.in_printer);
            CHECK(s.ams_sn == "sn1");
            CHECK(s.slot_id == "1");
        }
    CHECK(found);
}

TEST_CASE("A refused link op reports the failure and wants no pull", "[BblFilament]")
{
    Rig rig;
    rig.cloud->forced["sync_ams"].push_back(forced_error(fil::CloudError::Rejected, "no"));
    Outcome got;
    int     calls = 0;
    rig.svc->link(ams_request([&](const Outcome& o) {
        ++calls;
        got = o;
    }));
    rig.settle();
    CHECK(calls == 1);
    CHECK_FALSE(got.ok);
    CHECK(got.kind == fil::FailKind::Rejected);
    CHECK(got.message == "no");
    CHECK(got.sent);
    CHECK(rig.has_event("push_failed sync_ams  Rejected"));
    CHECK(rig.svc->state().has_error);
    CHECK(rig.cloud->count("list") == 0);
}

TEST_CASE("A link op with an unknown outcome wants a pull", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    rig.cloud->forced["sync_slots"].push_back(forced_error(fil::CloudError::Unknown));
    Outcome got;
    rig.svc->link(slots_request([&](const Outcome& o) { got = o; }));
    rig.settle();
    CHECK_FALSE(got.ok);
    CHECK(got.kind == fil::FailKind::Unknown);
    CHECK(rig.has_event("push_failed slot_mappings  Unknown"));
    CHECK(rig.cloud->count("list") == 1);
}

TEST_CASE("An unavailable link call is reported as a call that was not sent", "[BblFilament]")
{
    Rig rig;
    rig.cloud->forced["sync_ams"].push_back(forced_error(fil::CloudError::Unavailable));
    Outcome got;
    rig.svc->link(ams_request([&](const Outcome& o) { got = o; }));
    rig.settle();
    CHECK_FALSE(got.ok);
    CHECK_FALSE(got.sent);
    CHECK(got.kind == fil::FailKind::Unavailable);
}

TEST_CASE("A link request while signed out is refused and its callback never runs", "[BblFilament]")
{
    Rig rig;
    rig.session.signed_in = false;
    int calls             = 0;
    const auto a          = rig.svc->link(ams_request([&](const Outcome&) { ++calls; }));
    CHECK_FALSE(a.ok);
    CHECK(a.kind == fil::FailKind::NotSignedIn);
    CHECK(rig.worker.empty());
    rig.settle();
    CHECK(calls == 0);
}

TEST_CASE("A reset calls the done callback of every link op still waiting", "[BblFilament]")
{
    Rig rig;
    rig.svc->add(page_spool()); // running
    std::vector<Outcome> got;
    rig.svc->link(ams_request([&](const Outcome& o) { got.push_back(o); }));
    rig.svc->link(slots_request([&](const Outcome& o) { got.push_back(o); }));
    rig.svc->reset();
    REQUIRE(got.size() == 2);
    for (const Outcome& o : got) {
        CHECK_FALSE(o.sent);
        CHECK_FALSE(o.ok);
        CHECK(o.kind == fil::FailKind::NotSignedIn);
    }
    rig.settle();
    CHECK(got.size() == 2);
    CHECK(rig.cloud->count("sync_ams") == 0);
}

TEST_CASE("A changed account calls the done callback of a waiting link op", "[BblFilament]")
{
    Rig rig;
    rig.svc->add(page_spool());
    int  calls = 0;
    auto kind  = fil::FailKind::Unknown;
    rig.svc->link(ams_request([&](const Outcome& o) {
        ++calls;
        kind = o.kind;
    }));
    rig.session.uid = "u2";
    rig.settle();
    CHECK(calls == 1);
    CHECK(kind == fil::FailKind::NotSignedIn);
    CHECK(rig.cloud->count("sync_ams") == 0);
}

TEST_CASE("A done callback may destroy the service", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    rig.svc->link(ams_request([&](const Outcome&) { rig.svc.reset(); }));
    rig.settle();
    CHECK(rig.svc.get() == nullptr);
    CHECK(rig.cloud->count("list") == 0);
}

namespace {

// A service in storage the test owns, so that a callback can end it and put a fresh one at the same address. Code that
// went on using the ended service would reach the fresh one (the same type at the same address), which counts every use.
struct ReplaceableService
{
    alignas(fil::SpoolService) unsigned char bytes[sizeof(fil::SpoolService)];
    bool live{false};

    fil::SpoolService* get() { return std::launder(reinterpret_cast<fil::SpoolService*>(bytes)); }
    void               emplace(fil::ServiceDeps deps)
    {
        ::new (static_cast<void*>(bytes)) fil::SpoolService(std::move(deps));
        live = true;
    }
    void destroy()
    {
        if (!live) return;
        live = false;
        get()->~SpoolService();
    }
    ~ReplaceableService() { destroy(); }
};

} // namespace

TEST_CASE("A request that finds a changed session returns at once when a link callback ended the service", "[BblFilament]")
{
    const bool via_link = GENERATE(false, true);
    Rig        rig; // the cloud, the runner and the session
    rig.svc.reset();
    int                sentinel_uses = 0;
    ReplaceableService slot;
    fil::ServiceDeps   deps;
    deps.cloud      = rig.cloud;
    deps.run_async  = [&rig](std::function<void()> job) {
        rig.worker.push_back(std::move(job));
        return true;
    };
    deps.post_to_ui = [&rig](std::function<void()> f) { rig.ui.push_back(std::move(f)); };
    deps.session    = [&rig] { return rig.session; };
    deps.now        = [] { return 1000LL; };
    slot.emplace(std::move(deps));

    slot.get()->request_pull(); // running: the link op waits behind it
    slot.get()->link(ams_request([&](const Outcome&) {
        slot.destroy();
        fil::ServiceDeps sentinel;
        sentinel.session   = [&sentinel_uses] {
            ++sentinel_uses;
            return fil::Session{true, "u2", 1, false};
        };
        sentinel.run_async = [&sentinel_uses](std::function<void()>) {
            ++sentinel_uses;
            return false;
        };
        slot.emplace(std::move(sentinel));
    }));
    REQUIRE(rig.worker.size() == 1);

    rig.session.uid = "u2";
    if (via_link) {
        const fil::Accept a = slot.get()->link(ams_request());
        CHECK_FALSE(a.ok);
    } else {
        slot.get()->request_pull();
    }
    CHECK(sentinel_uses == 0);
    rig.settle();
    CHECK(rig.cloud->count("list") == 0);
    CHECK(rig.cloud->count("sync_ams") == 0);
}

TEST_CASE("A done callback may queue another link op", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    int calls = 0;
    rig.svc->link(ams_request([&](const Outcome&) {
        if (++calls == 1) rig.svc->link(slots_request([&](const Outcome&) { ++calls; }));
    }));
    rig.settle();
    CHECK(calls == 2);
    CHECK(rig.cloud->count("sync_slots") == 1);
    CHECK(rig.cloud->count("list") == 1);
}

TEST_CASE("A builder that destroys the service does not crash", "[BblFilament]")
{
    Rig rig;
    Request r   = ams_request();
    r.build_ams = [&](const std::vector<fil::Spool>&, const std::set<std::string>&) {
        rig.svc.reset();
        return std::vector<fil::AmsItem>{fil::AmsItem{}};
    };
    rig.svc->link(std::move(r));
    CHECK(rig.svc.get() == nullptr);
    rig.settle();
    CHECK(rig.cloud->count("sync_ams") == 0);
}

TEST_CASE("A link op the worker threw away fails as unavailable", "[BblFilament]")
{
    Rig rig;
    Outcome got;
    rig.svc->link(ams_request([&](const Outcome& o) { got = o; }));
    REQUIRE(rig.worker.size() == 1);
    rig.worker.clear(); // drain: destroying the job completes it as unavailable
    rig.run_ui();
    CHECK_FALSE(got.ok);
    CHECK_FALSE(got.sent);
    CHECK(got.kind == fil::FailKind::Unavailable);
}

TEST_CASE("A soft match read fires no event and wants no pull", "[BblFilament]")
{
    Rig rig;
    rig.cloud->soft_queue = json{{"hits", json::array({row_with(1000001, "Fresh")})}, {"candidates", json::array()}};
    Outcome got;
    int     calls = 0;
    rig.svc->link(soft_read_request([&](const Outcome& o) {
        ++calls;
        got = o;
    }));
    CHECK_FALSE(rig.svc->state().is_syncing);
    rig.events.clear();
    rig.settle();
    CHECK(calls == 1);
    CHECK(got.ok);
    CHECK(fil::parse_soft_match(got.body).entries.size() == 1);
    CHECK(rig.events.empty());
    CHECK(rig.cloud->count("soft_match_pending") == 1);
    CHECK(rig.cloud->count("list") == 0);
    CHECK_FALSE(rig.svc->state().is_syncing);
    CHECK(rig.svc->state().last_synced_at == 0);
}

TEST_CASE("A failed soft match read leaves the error state alone", "[BblFilament]")
{
    Rig rig;
    rig.cloud->forced["soft_match_pending"].push_back(forced_error(fil::CloudError::Unknown));
    Outcome got;
    rig.svc->link(soft_read_request([&](const Outcome& o) { got = o; }));
    rig.settle();
    CHECK_FALSE(got.ok);
    CHECK(got.kind == fil::FailKind::Unknown);
    CHECK_FALSE(rig.svc->state().has_error);
    CHECK(rig.cloud->count("list") == 0);
}

TEST_CASE("A link op counts as syncing, and link ops are not part of the view", "[BblFilament]")
{
    Rig rig;
    rig.cloud->fill(1);
    rig.svc->request_pull();
    rig.settle();
    const auto before = rig.svc->view().size();
    rig.svc->link(ams_request());
    CHECK(rig.svc->state().is_syncing);
    CHECK(rig.svc->view().size() == before);
    CHECK(rig.svc->status().pending_writes == 1);
    rig.settle();
    CHECK_FALSE(rig.svc->state().is_syncing);
    CHECK(index_of(rig.cloud->log, "sync_ams") >= 0);
}
