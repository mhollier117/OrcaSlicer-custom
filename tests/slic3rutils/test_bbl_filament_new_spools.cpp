#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "slic3r/Utils/BblFilament/Linker.hpp"
#include "slic3r/Utils/BblFilament/NewSpools.hpp"
#include "slic3r/Utils/BblFilament/PrinterTap.hpp"
#include "slic3r/Utils/BblFilament/Spool.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"

#include <string>
#include <utility>
#include <vector>

namespace fil = Slic3r::BblFilament;
using Catch::Matchers::WithinAbs;

namespace {

const std::string k_rfid = "A1B2C3D4E5F60718293A4B5C6D7E8F90";

fil::TrayFacts tray(int ams_id, const std::string& slot, const std::string& uuid)
{
    fil::TrayFacts t;
    t.ams_id   = ams_id;
    t.slot_id  = slot;
    t.occupied = true;
    t.uuid     = uuid;
    return t;
}

fil::PrinterView printer(const std::string& dev, std::vector<fil::TrayFacts> trays)
{
    fil::PrinterView p;
    p.dev_id = dev;
    p.name   = "Printer " + dev;
    p.bound  = true;
    p.fresh  = true;
    p.trays  = std::move(trays);
    return p;
}

const fil::SlotKey k_slot{"dev1", 0, "1"};

} // namespace

TEST_CASE("A new spool's marker is added, read and dismissed", "[BblFilament]")
{
    fil::NewSpools spools;
    CHECK_FALSE(spools.has(k_slot));
    CHECK(spools.rfid(k_slot).empty());

    spools.add(k_slot, k_rfid);
    CHECK(spools.has(k_slot));
    CHECK(spools.rfid(k_slot) == k_rfid);
    CHECK_FALSE(spools.has(fil::SlotKey{"dev1", 0, "2"}));

    spools.dismiss(k_slot);
    CHECK_FALSE(spools.has(k_slot));

    spools.add(k_slot, k_rfid);
    spools.clear();
    CHECK_FALSE(spools.has(k_slot));
}

TEST_CASE("The markers of a printer are listed by printer", "[BblFilament]")
{
    fil::NewSpools spools;
    spools.add(fil::SlotKey{"dev1", 0, "1"}, k_rfid);
    spools.add(fil::SlotKey{"dev1", 128, "0"}, "B");
    spools.add(fil::SlotKey{"dev2", 0, "0"}, "C");

    const std::vector<fil::SlotKey> one = spools.of_printer("dev1");
    REQUIRE(one.size() == 2);
    CHECK(one[0] == fil::SlotKey{"dev1", 0, "1"});
    CHECK(one[1] == fil::SlotKey{"dev1", 128, "0"});
    CHECK(spools.of_printer("dev2").size() == 1);
    CHECK(spools.of_printer("dev3").empty());
}

TEST_CASE("A marker is dropped when its slot is empty", "[BblFilament]")
{
    fil::NewSpools spools;
    spools.add(k_slot, k_rfid);
    fil::TrayFacts t = tray(0, "1", "");
    t.occupied       = false;
    spools.reconcile({printer("dev1", {t})});
    CHECK_FALSE(spools.has(k_slot));
}

TEST_CASE("A marker is dropped when the slot reports another spool", "[BblFilament]")
{
    fil::NewSpools spools;
    spools.add(k_slot, k_rfid);
    spools.reconcile({printer("dev1", {tray(0, "1", "00000000000000000000000000000000")})});
    CHECK_FALSE(spools.has(k_slot));
}

TEST_CASE("A marker is dropped when its AMS unit or tray is gone", "[BblFilament]")
{
    fil::NewSpools spools;
    spools.add(k_slot, k_rfid);
    spools.add(fil::SlotKey{"dev1", 1, "0"}, "B");
    spools.reconcile({printer("dev1", {tray(0, "0", ""), tray(0, "2", "")})});
    CHECK_FALSE(spools.has(k_slot));
    CHECK_FALSE(spools.has(fil::SlotKey{"dev1", 1, "0"}));
}

TEST_CASE("A marker stays while the slot reports the same spool or one being read", "[BblFilament]")
{
    fil::NewSpools spools;
    spools.add(k_slot, k_rfid);
    spools.add(fil::SlotKey{"dev1", 0, "2"}, "B");
    spools.reconcile({printer("dev1", {tray(0, "1", k_rfid), tray(0, "2", "")})});
    CHECK(spools.has(k_slot));
    CHECK(spools.has(fil::SlotKey{"dev1", 0, "2"}));
}

TEST_CASE("A printer without fresh data keeps its markers", "[BblFilament]")
{
    fil::NewSpools spools;
    spools.add(k_slot, k_rfid);
    spools.add(fil::SlotKey{"dev2", 0, "0"}, "B");
    fil::PrinterView stale = printer("dev1", {});
    stale.fresh            = false;
    spools.reconcile({stale, printer("dev2", {})});
    CHECK(spools.has(k_slot));
    CHECK_FALSE(spools.has(fil::SlotKey{"dev2", 0, "0"}));
}

TEST_CASE("Reconciling leaves the markers of printers it was not given", "[BblFilament]")
{
    fil::NewSpools spools;
    spools.add(k_slot, k_rfid);
    spools.reconcile({printer("dev2", {})});
    CHECK(spools.has(k_slot));
}

TEST_CASE("The queue entry of a spool is found by its RFID", "[BblFilament]")
{
    fil::SoftMatchQueue queue;
    fil::SoftMatchEntry a;
    a.hit.id   = "11";
    a.hit.rfid = "OTHER";
    fil::SoftMatchEntry b;
    b.hit.id   = "12";
    b.hit.rfid = k_rfid;
    queue.entries = {a, b};

    const fil::SoftMatchEntry* found = fil::entry_for_rfid(queue, k_rfid);
    REQUIRE(found != nullptr);
    CHECK(found->hit.id == "12");
    CHECK(fil::entry_for_rfid(queue, "NONE") == nullptr);
    CHECK(fil::entry_for_rfid(queue, "") == nullptr);
    CHECK(fil::entry_for_rfid(fil::SoftMatchQueue{}, k_rfid) == nullptr);
}

TEST_CASE("An entry without an RFID never matches an empty one", "[BblFilament]")
{
    fil::SoftMatchQueue queue;
    fil::SoftMatchEntry e;
    e.hit.id = "11";
    queue.entries = {e};
    CHECK(fil::entry_for_rfid(queue, "") == nullptr);
}

TEST_CASE("Adding the spool as new sends create_new for the hit", "[BblFilament]")
{
    const auto act = fil::soft_match_act_for(fil::NewSpoolChoice::AddNew, "12", "34");
    REQUIRE(act.has_value());
    CHECK(act->action == "create_new");
    CHECK(act->spool_id == "12");
    CHECK(act->target_spool_id.empty());
}

TEST_CASE("Confirming another candidate sends link_other to it", "[BblFilament]")
{
    const auto act = fil::soft_match_act_for(fil::NewSpoolChoice::Confirm, "12", "34");
    REQUIRE(act.has_value());
    CHECK(act->action == "link_other");
    CHECK(act->spool_id == "12");
    CHECK(act->target_spool_id == "34");
}

TEST_CASE("Confirming without a candidate, or with the hit itself, accepts the hit", "[BblFilament]")
{
    const std::string candidate = GENERATE(std::string(), std::string("12"));
    const auto        act       = fil::soft_match_act_for(fil::NewSpoolChoice::Confirm, "12", candidate);
    REQUIRE(act.has_value());
    CHECK(act->action == "accept");
    CHECK(act->spool_id == "12");
    CHECK(act->target_spool_id.empty());
}

TEST_CASE("Nothing is sent without a hit", "[BblFilament]")
{
    CHECK_FALSE(fil::soft_match_act_for(fil::NewSpoolChoice::AddNew, "", "34").has_value());
    CHECK_FALSE(fil::soft_match_act_for(fil::NewSpoolChoice::Confirm, "", "34").has_value());
}

TEST_CASE("A spool shown from a tray takes its filament, colours and grams", "[BblFilament]")
{
    fil::TrayFacts t = tray(0, "1", k_rfid);
    t.filament_id    = "GFA00";
    t.type           = "PLA";
    t.sub_brands     = "PLA Basic";
    t.color          = "#FF0000FF";
    t.colors         = {"#FF0000FF"};
    t.color_type     = 2;
    t.total_g        = 1000;
    t.remain         = 40;

    t.remain_g = 250;
    fil::Spool s = fil::spool_from_tray(t);
    CHECK(s.rfid == k_rfid);
    CHECK(s.filament_id == "GFA00");
    CHECK(s.name == "PLA Basic");
    CHECK(s.type == "PLA");
    CHECK(s.color == "#FF0000FF");
    CHECK(s.colors.size() == 1);
    CHECK_THAT(s.total_net_weight, WithinAbs(1000.0, 1e-9));
    CHECK_THAT(s.net_weight, WithinAbs(250.0, 1e-9));

    t.remain_g = -1;
    CHECK_THAT(fil::spool_from_tray(t).net_weight, WithinAbs(400.0, 1e-9));

    t.remain = -1;
    CHECK_THAT(fil::spool_from_tray(t).net_weight, WithinAbs(0.0, 1e-9));

    t.uuid = "00000000000000000000000000000000";
    CHECK(fil::spool_from_tray(t).rfid.empty());
}
