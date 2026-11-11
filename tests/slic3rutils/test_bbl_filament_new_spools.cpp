#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BblFilament/Linker.hpp"
#include "slic3r/Utils/BblFilament/NewSpools.hpp"
#include "slic3r/Utils/BblFilament/PrinterTap.hpp"

#include <string>
#include <utility>
#include <vector>

namespace fil = Slic3r::BblFilament;

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
