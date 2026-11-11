#pragma once

#include <catch2/catch_test_macros.hpp>

#include "slic3r/Utils/BblFilament/Spool.hpp"
#include "slic3r/Utils/BblFilament/SpoolCloud.hpp"
#include "slic3r/Utils/BblFilament/SpoolService.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace BblFilamentTest {

namespace fil = Slic3r::BblFilament;
using fil::json;

inline json example_row(long long id = 1000001, const std::string& name = "PLA Basic")
{
    return json::parse(R"({"id":1,"createType":"manual","filamentVendor":"Bambu Lab","filamentType":"PLA","filamentName":"PLA Basic",
 "filamentId":"GFA00","RFID":"","color":"#FFFFFFFF","colorType":2,"colors":null,"netWeight":750,"totalNetWeight":1000,
 "note":"","createdAt":1700000000,"updatedAt":1700000000,"status":0,"isSupport":false,"trayIdName":"","category":"PLA",
 "inPrinter":false,"devId":"","amsSn":"","slotId":"","amsId":0,"amsType":0,"deviceName":""})");
}

inline json row_with(long long id, const std::string& name = "PLA Basic")
{
    json r         = example_row();
    r["id"]        = id;
    r["filamentName"] = name;
    return r;
}

// A cloud that holds rows in memory and logs every call.
class FakeCloud : public fil::ISpoolCloud
{
public:
    std::vector<json>        rows;
    std::vector<std::string> log;
    std::vector<json>        bodies; // create and update bodies, in order
    json                     config_body = json{{"filamentColors", json::array()}};
    long long                next_id     = 2000001;

    bool ignore_offset = false;
    bool ignore_limit  = false; // the server sends everything whatever page is asked for
    bool null_hits     = false;
    bool omit_hits     = false;
    bool with_total    = false;

    // Forced replies for the next calls of a kind; nullopt lets that call run normally.
    std::map<std::string, std::deque<std::optional<fil::CloudReply>>> forced;

    void fill(int count, long long first_id = 1000001)
    {
        for (int i = 0; i < count; ++i) rows.push_back(row_with(first_id + i, "Spool " + std::to_string(first_id + i)));
    }

    fil::CloudReply list_page(int offset, int limit) override
    {
        log.push_back("list " + std::to_string(offset));
        if (auto f = take("list")) return *f;
        if (ignore_offset) offset = 0;
        if (ignore_limit) {
            offset = 0;
            limit  = static_cast<int>(rows.size());
        }
        json hits = json::array();
        for (int i = offset; i < static_cast<int>(rows.size()) && i < offset + limit; ++i) hits.push_back(rows[i]);
        json body = json::object();
        if (hits.empty() && null_hits) body["hits"] = nullptr;
        else if (hits.empty() && omit_hits) {
        } else body["hits"] = hits;
        if (with_total) body["total"] = static_cast<int>(rows.size());
        return ok(body);
    }

    fil::CloudReply create(const json& body) override
    {
        log.push_back("create");
        bodies.push_back(body);
        if (auto f = take("create")) return *f;
        json row = body;
        row["id"] = next_id++;
        rows.push_back(row);
        return ok(json::object());
    }

    fil::CloudReply update(const std::string& id, const json& body) override
    {
        log.push_back("update " + id);
        bodies.push_back(body);
        if (auto f = take("update")) return *f;
        for (json& row : rows) {
            if (row["id"].dump() == id) {
                for (auto it = body.begin(); it != body.end(); ++it)
                    if (it.key() != "id") row[it.key()] = it.value();
                return ok(json{{"filamentV2", row}});
            }
        }
        return fil::classify_reply(-29, R"({"code":404,"error":"not found"})");
    }

    fil::CloudReply remove(const std::vector<std::string>& ids) override
    {
        std::string joined;
        for (const auto& id : ids) joined += (joined.empty() ? "" : ",") + id;
        log.push_back("remove " + joined);
        if (auto f = take("remove")) return *f;
        rows.erase(std::remove_if(rows.begin(), rows.end(),
                                  [&](const json& row) { return std::find(ids.begin(), ids.end(), row["id"].dump()) != ids.end(); }),
                   rows.end());
        return ok(json::object());
    }

    fil::CloudReply config() override
    {
        log.push_back("config");
        if (auto f = take("config")) return *f;
        return ok(config_body);
    }

    // The link calls. They record what they got; `apply_mounts` makes sync_slots / sync_ams change the rows (an ams sync
    // also sets the weight it sent), so that a following list shows the mount.
    std::vector<std::vector<fil::AmsItem>>  ams_got;
    std::vector<std::vector<fil::SlotItem>> slots_got;
    std::vector<std::string>                actions_got; // "action spool target"
    bool                                    apply_mounts = false;
    json                                    soft_queue   = json{{"hits", json::array()}, {"candidates", json::array()}};

    fil::CloudReply sync_ams(const std::string& dev_id, const std::vector<fil::AmsItem>& items) override
    {
        log.push_back("sync_ams " + dev_id);
        ams_got.push_back(items);
        if (auto f = take("sync_ams")) return *f;
        json results = json::array();
        for (const auto& it : items) {
            json r = {{"amsSn", it.ams_sn}, {"slotId", it.slot_id}, {"spoolId", nullptr}};
            for (json& row : rows) {
                if (!it.rfid.empty() && row.value("RFID", std::string()) == it.rfid) {
                    r["spoolId"] = row["id"];
                    if (apply_mounts) {
                        mount(row, dev_id, it.ams_sn, it.slot_id, it.ams_id, it.ams_type);
                        row["netWeight"] = it.net_weight;
                    }
                }
            }
            results.push_back(r);
        }
        return ok(json{{"createdRFIDs", nullptr}, {"results", results}});
    }

    fil::CloudReply sync_slots(const std::string& dev_id, const std::vector<fil::SlotItem>& items) override
    {
        log.push_back("sync_slots " + dev_id);
        slots_got.push_back(items);
        if (auto f = take("sync_slots")) return *f;
        json results = json::array();
        for (const auto& it : items) {
            if (apply_mounts) {
                for (json& row : rows) {
                    const bool here = row.value("amsSn", std::string()) == it.ams_sn && row.value("slotId", std::string()) == it.slot_id;
                    if (here || row["id"].dump() == it.spool_id) unmount(row);
                }
                for (json& row : rows)
                    if (!it.spool_id.empty() && row["id"].dump() == it.spool_id) mount(row, dev_id, it.ams_sn, it.slot_id, it.ams_id, it.ams_type);
            }
            results.push_back(json{{"amsSn", it.ams_sn}, {"slotId", it.slot_id}, {"code", 0}});
        }
        return ok(json{{"results", results}});
    }

    fil::CloudReply soft_match_pending(const std::string&, const std::string&) override
    {
        log.push_back("soft_match_pending");
        if (auto f = take("soft_match_pending")) return *f;
        return ok(soft_queue);
    }

    fil::CloudReply soft_match_action(const std::string& action, const std::string& spool_id, const std::string& target) override
    {
        log.push_back("soft_match_action " + action);
        actions_got.push_back(action + " " + spool_id + " " + target);
        if (auto f = take("soft_match_action")) return *f;
        return ok(json::object());
    }

    static void mount(json& row, const std::string& dev, const std::string& sn, const std::string& slot, int ams_id, int ams_type)
    {
        row["inPrinter"] = true;
        row["devId"]     = dev;
        row["amsSn"]     = sn;
        row["slotId"]    = slot;
        row["amsId"]     = ams_id;
        row["amsType"]   = ams_type;
    }
    static void unmount(json& row)
    {
        row["inPrinter"] = false;
        row["devId"]     = "";
        row["amsSn"]     = "";
        row["slotId"]    = "";
        row["amsId"]     = 0;
        row["amsType"]   = 0;
    }

    static fil::CloudReply ok(json body)
    {
        fil::CloudReply r;
        r.body = std::move(body);
        return r;
    }

    int count(const std::string& prefix) const
    {
        return static_cast<int>(std::count_if(log.begin(), log.end(), [&](const std::string& l) { return l.rfind(prefix, 0) == 0; }));
    }

private:
    std::optional<fil::CloudReply> take(const std::string& kind)
    {
        auto it = forced.find(kind);
        if (it == forced.end() || it->second.empty()) return std::nullopt;
        auto f = it->second.front();
        it->second.pop_front();
        return f;
    }
};

inline fil::CloudReply forced_error(fil::CloudError e, const std::string& message = "")
{
    fil::CloudReply r;
    r.error   = e;
    r.rc      = -28;
    r.message = message;
    return r;
}

inline const char* kind_text(fil::FailKind k)
{
    switch (k) {
    case fil::FailKind::NotSignedIn: return "NotSignedIn";
    case fil::FailKind::Unavailable: return "Unavailable";
    case fil::FailKind::BadRequest: return "BadRequest";
    case fil::FailKind::NotFound: return "NotFound";
    case fil::FailKind::Rejected: return "Rejected";
    default: return "Unknown";
    }
}

struct Rig
{
    std::shared_ptr<FakeCloud>         cloud = std::make_shared<FakeCloud>();
    std::deque<std::function<void()>>  worker;
    std::deque<std::function<void()>>  ui;
    fil::Session                       session{true, "u1", 1, false};
    long long                          clock{1000};
    long long                          clock_step{1};
    std::vector<std::string>           events;
    bool                               accept_async{true};
    std::unique_ptr<fil::SpoolService> svc;

    explicit Rig(fil::FilamentLookup lookup = {})
    {
        fil::ServiceDeps deps;
        deps.cloud      = cloud;
        deps.run_async  = [this](std::function<void()> job) {
            if (!accept_async) return false;
            worker.push_back(std::move(job));
            return true;
        };
        deps.post_to_ui = [this](std::function<void()> f) { ui.push_back(std::move(f)); };
        deps.session    = [this] { return session; };
        deps.lookup     = std::move(lookup);
        deps.now        = [this] { clock += clock_step; return clock; };
        svc             = std::make_unique<fil::SpoolService>(std::move(deps));
        svc->events.spools_changed = [this] { events.push_back("spools_changed"); };
        svc->events.state_changed  = [this] { events.push_back("state_changed"); };
        svc->events.pull_done      = [this](int a, int k) { events.push_back("pull_done " + std::to_string(a) + " " + std::to_string(k)); };
        svc->events.push_done      = [this](const std::string& op, const std::string& id) { events.push_back("push_done " + op + " " + id); };
        svc->events.push_failed    = [this](const std::string& op, const std::string& id, fil::FailKind k, const std::string&) {
            events.push_back("push_failed " + op + " " + id + " " + kind_text(k));
        };
        svc->events.config_fetched = [this](const json&) { events.push_back("config_fetched"); };
    }

    bool step_worker()
    {
        if (worker.empty()) return false;
        auto job = std::move(worker.front());
        worker.pop_front();
        job();
        return true;
    }
    void run_ui()
    {
        while (!ui.empty()) {
            auto f = std::move(ui.front());
            ui.pop_front();
            f();
        }
    }
    void settle()
    {
        for (int guard = 0; guard < 1000 && (!worker.empty() || !ui.empty()); ++guard) {
            step_worker();
            run_ui();
        }
        REQUIRE(worker.empty());
        REQUIRE(ui.empty());
    }
    int  count_events(const std::string& text) const { return static_cast<int>(std::count(events.begin(), events.end(), text)); }
    bool has_event(const std::string& text) const { return count_events(text) > 0; }
    std::string note_of(const std::string& id) const
    {
        for (const auto& s : svc->view())
            if (s.id == id) return s.note;
        return "<missing>";
    }
};

inline json page_spool(const std::string& name = "New")
{
    return json{{"brand", "Bambu Lab"}, {"material_type", "PLA"}, {"series", name}, {"setting_id", "GFA00"}, {"color_code", "#FFFFFFFF"},
                {"initial_weight", 1000}, {"net_weight", 1000}};
}

} // namespace BblFilamentTest
