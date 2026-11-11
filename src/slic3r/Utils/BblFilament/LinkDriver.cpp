#include "LinkDriver.hpp"

#include "PrinterTap.hpp"
#include "SpoolCloud.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r::BblFilament {

namespace {

using LinkRequest = SpoolService::LinkRequest;
using LinkOutcome = SpoolService::LinkOutcome;

const Spool* find_row(const std::vector<Spool>& rows, const std::string& id)
{
    for (const Spool& r : rows)
        if (r.id == id) return &r;
    return nullptr;
}

bool claims_slot(const Spool& r, const SlotKey& k)
{
    return r.in_printer && r.dev_id == k.dev_id && r.ams_id == k.ams_id && r.slot_id == k.slot_id;
}

// An unbind names the slot, not the spool: it is sent only while the row still has exactly the mount that was planned
// (printer, AMS serial, AMS id, AMS type and slot) and no other row claims that slot.
bool unbind_still_wanted(const std::vector<Spool>& rows, const std::string& spool_id, const SlotKey& slot, const std::string& ams_sn, int ams_type)
{
    const Spool* row = find_row(rows, spool_id);
    if (row == nullptr || !claims_slot(*row, slot) || row->ams_sn != ams_sn || row->ams_type != ams_type) return false;
    for (const Spool& r : rows)
        if (r.id != spool_id && claims_slot(r, slot)) return false;
    return true;
}

const char* mode_text(LinkMode m)
{
    switch (m) {
    case LinkMode::Off: return "off";
    case LinkMode::DryRun: return "dry_run";
    default: return "on";
    }
}

const char* fail_text(FailKind k)
{
    switch (k) {
    case FailKind::NotSignedIn: return "not_signed_in";
    case FailKind::Unavailable: return "unavailable";
    case FailKind::BadRequest: return "bad_request";
    case FailKind::NotFound: return "not_found";
    case FailKind::Rejected: return "rejected";
    default: return "unknown";
    }
}

// One item of an ams sync of a round: an RFID mount or a weight.
struct AmsEntry
{
    std::string              spool_id, rfid, ams_sn, slot_id;
    int                      ams_id{0}, ams_type{0};
    std::optional<long long> grams;
    bool                     mount{false};
};

// One item of a slot sync of a round: a manual bind or an unbind.
struct SlotEntry
{
    bool        bind{false};
    std::string spool_id, ams_sn;
    SlotKey     slot;
    int         ams_type{0};
};

} // namespace

std::string write_refusal(const LinkConfig& config, const SlotKey* slot, bool dry_run_answers)
{
    if (config.mode == LinkMode::Off) return "linking is off";
    if (config.mode == LinkMode::DryRun && !dry_run_answers) return "linking is a dry run";
    if (slot != nullptr && !config.only_slots.empty() && config.only_slots.count(slot->text()) == 0) return "the slot is not one of the slots linking may touch";
    return {};
}

LinkDriver::LinkDriver(LinkDriverDeps deps) : deps_(std::move(deps)) {}

long long LinkDriver::now() const { return deps_.now ? deps_.now() : 0; }

void LinkDriver::drop_queued()
{
    ++epoch_;
    ++round_;
    round_in_flight_ = false;
    round_pending_   = 0;
    round_ops_.clear();
}

bool LinkDriver::waiting_for_list() const
{
    return have_round_ && deps_.service != nullptr && deps_.service->list_serial() <= round_end_serial_;
}

void LinkDriver::tick(const std::vector<PrinterView>& printers)
{
    // What was queued under another mode or allow-list is not sent.
    if (last_config_ && (last_config_->mode != config.mode || last_config_->only_slots != config.only_slots)) drop_queued();
    last_config_ = config;
    if (config.mode == LinkMode::Off) return;
    SpoolService* svc = deps_.service;
    if (svc == nullptr || !svc->state().signed_in) return;
    resolve_creations();
    if (round_in_flight_) return;
    const long long t = now();
    if (waiting_for_list()) {
        // The pull the last round asked for has not brought a list (it failed, or was dropped): ask again now and then.
        if (!svc->state().is_syncing && t - pull_asked_at_ >= k_pull_retry_seconds) {
            pull_asked_at_ = t;
            svc->request_pull();
        }
        return;
    }
    if (have_round_ && !skip_spacing_ && t - round_ended_at_ < k_round_spacing_seconds) return;
    skip_spacing_ = false;

    last_plan_ = plan_links(printers, svc->confirmed(), svc->busy_ids(), svc->list_serial(), state_, config, t);
    if (last_plan_.want_pull) {
        std::weak_ptr<int> weak = alive_;
        svc->request_pull();
        if (weak.expired()) return;
    }
    if (config.mode != LinkMode::On || !last_plan_.has_actions()) return;
    start_round(printers);
}

void LinkDriver::start_round(const std::vector<PrinterView>& printers)
{
    SpoolService&             svc  = *deps_.service;
    const std::vector<Spool>& rows = svc.confirmed();

    std::map<std::string, std::vector<AmsEntry>>        ams;
    std::map<std::string, std::vector<SlotEntry>>       slots;
    std::map<std::string, std::vector<PlannedRegister>> regs;
    std::set<std::string>                               devs;
    for (const PlannedMount& m : last_plan_.mounts) {
        const Spool* row = find_row(rows, m.spool_id);
        if (row == nullptr) continue;
        devs.insert(m.slot.dev_id);
        if (m.rfid) ams[m.slot.dev_id].push_back(AmsEntry{m.spool_id, row->rfid, m.ams_sn, m.slot.slot_id, m.slot.ams_id, m.ams_type, m.grams, true});
        else slots[m.slot.dev_id].push_back(SlotEntry{true, m.spool_id, m.ams_sn, m.slot, m.ams_type});
    }
    for (const PlannedWeight& w : last_plan_.weights) {
        const Spool* row = find_row(rows, w.spool_id);
        if (row == nullptr) continue;
        devs.insert(w.slot.dev_id);
        ams[w.slot.dev_id].push_back(AmsEntry{w.spool_id, row->rfid, w.ams_sn, w.slot.slot_id, w.slot.ams_id, w.ams_type, w.grams, false});
    }
    for (const PlannedUnbind& u : last_plan_.unbinds) {
        devs.insert(u.slot.dev_id);
        slots[u.slot.dev_id].push_back(SlotEntry{false, u.spool_id, u.ams_sn, u.slot, u.ams_type});
    }
    for (const PlannedRegister& r : last_plan_.registrations) {
        devs.insert(r.slot.dev_id);
        regs[r.slot.dev_id].push_back(r);
    }

    std::map<std::string, bool> printer_busy;
    for (const PrinterView& p : printers) printer_busy[p.dev_id] = p.busy;

    ++round_;
    const unsigned long long round = round_;
    const unsigned long long gen   = generation_;
    const unsigned long long epoch = epoch_;
    std::weak_ptr<int>       weak  = alive_;

    std::vector<std::pair<LinkRequest, std::string>> requests;
    for (const std::string& dev : devs) {
        if (auto it = ams.find(dev); it != ams.end()) {
            const std::vector<AmsEntry> entries  = it->second;
            const bool                  busy     = printer_busy[dev];
            const auto                  skip     = last_plan_.skipped_cooldown.find(dev);
            const int                   skipped  = skip == last_plan_.skipped_cooldown.end() ? 0 : skip->second;
            auto                        measured = std::make_shared<std::map<std::string, long long>>(); // RFID -> grams sent
            auto                        keys = std::make_shared<std::map<std::string, std::vector<std::string>>>(); // RFID -> sent keys
            LinkRequest                 req;
            req.kind      = LinkRequest::Kind::AmsSync;
            req.dev_id    = dev;
            req.build_ams = [this, weak, epoch, entries, measured, keys, dev](const std::vector<Spool>& now_rows, const std::set<std::string>& busy_ids) {
                std::vector<AmsItem> items;
                measured->clear();
                keys->clear();
                if (weak.expired() || epoch != epoch_) return items; // queued before a reset or a config change: dropped without a call
                for (const AmsEntry& e : entries) {
                    const Spool* row = find_row(now_rows, e.spool_id);
                    if (row == nullptr || busy_ids.count(e.spool_id) > 0 || row->rfid != e.rfid) continue;
                    items.push_back(ams_item_from_row(*row, e.ams_sn, e.ams_id, e.ams_type, e.slot_id, e.grams));
                    if (e.grams) (*measured)[e.rfid] = *e.grams;
                    if (e.mount) (*keys)[e.rfid].push_back(sent_key_mount(SlotKey{dev, e.ams_id, e.slot_id}, e.spool_id));
                    if (e.grams) (*keys)[e.rfid].push_back(sent_key_weight(e.rfid, *e.grams));
                }
                return items;
            };
            req.done = [this, weak, gen, round, measured, keys, busy, skipped](const LinkOutcome& out) {
                if (weak.expired() || gen != generation_) return;
                int pushed = 0;
                if (out.ok) {
                    for (const AmsItem& i : out.ams_sent) {
                        if (auto k = keys->find(i.rfid); k != keys->end())
                            for (const std::string& key : k->second) state_.sent_until[key] = now() + k_refusal_seconds;
                        auto m = measured->find(i.rfid);
                        if (m == measured->end()) continue;
                        state_.pushed[i.rfid] = LinkerState::Push{now(), m->second};
                        ++pushed;
                    }
                }
                op_finished(round, OpRecord{"ams", out.ok, out.kind, static_cast<int>(out.ams_sent.size())});
                if (weak.expired() || pushed == 0 || !on_auto_push) return;
                const auto callback = on_auto_push;
                callback(AutoPushSummary{pushed, skipped, busy});
            };
            requests.emplace_back(std::move(req), "ams");
        }
        if (auto it = regs.find(dev); it != regs.end()) {
            for (const PlannedRegister& r : it->second) {
                const TrayFacts      tray   = r.tray;
                const std::string    ams_sn = r.ams_sn;
                const SlotKey        slot   = r.slot;
                const FilamentLookup lookup = deps_.lookup;
                LinkRequest          req;
                req.kind      = LinkRequest::Kind::AmsSync;
                req.dev_id    = dev;
                req.build_ams = [this, weak, epoch, tray, ams_sn, lookup](const std::vector<Spool>& now_rows, const std::set<std::string>&) {
                    if (weak.expired() || epoch != epoch_) return std::vector<AmsItem>();
                    for (const Spool& row : now_rows)
                        if (row.rfid == tray.uuid) return std::vector<AmsItem>(); // the account has it by now
                    return std::vector<AmsItem>{ams_item_from_tray(tray, ams_sn, lookup)};
                };
                const std::string rfid = tray.uuid;
                req.done = [this, weak, gen, round, slot, rfid](const LinkOutcome& out) {
                    if (weak.expired() || gen != generation_) return;
                    // Dropped by the builder because the account has the spool by now: no attempt, the next plan matches it.
                    const bool dropped = !out.sent && out.kind == FailKind::BadRequest && out.ams_sent.empty();
                    if (out.ok) {
                        state_.registrations[rfid].done = true;
                    } else if (!dropped) {
                        LinkerState::Registration& reg = state_.registrations[rfid];
                        ++reg.attempts;
                        reg.next_at = now() + std::min<long long>(60LL << std::min(reg.attempts, 6), 3600);
                    }
                    op_finished(round, OpRecord{"register", out.ok, out.kind, static_cast<int>(out.ams_sent.size())});
                    if (weak.expired() || !out.ok || !on_registered) return;
                    const auto callback = on_registered;
                    callback(slot, rfid);
                };
                requests.emplace_back(std::move(req), "register");
            }
        }
        if (auto it = slots.find(dev); it != slots.end()) {
            const std::vector<SlotEntry> entries = it->second;
            LinkRequest                  req;
            req.kind        = LinkRequest::Kind::SlotSync;
            req.dev_id      = dev;
            req.build_slots = [this, weak, epoch, entries](const std::vector<Spool>& now_rows, const std::set<std::string>& busy_ids) {
                std::vector<SlotItem> items;
                if (weak.expired() || epoch != epoch_) return items;
                for (const SlotEntry& e : entries) {
                    const Spool* row = find_row(now_rows, e.spool_id);
                    if (row == nullptr || busy_ids.count(e.spool_id) > 0) continue;
                    if (e.bind) {
                        if (!row->rfid.empty()) continue; // an RFID spool is mounted by an ams sync
                        items.push_back(slot_bind(e.ams_sn, e.slot.ams_id, e.ams_type, e.slot.slot_id, *row));
                    } else if (unbind_still_wanted(now_rows, e.spool_id, e.slot, e.ams_sn, e.ams_type)) {
                        items.push_back(slot_unbind(e.ams_sn, e.slot.ams_id, e.ams_type, e.slot.slot_id));
                    }
                }
                return items;
            };
            req.done = [this, weak, gen, round, dev](const LinkOutcome& out) {
                if (weak.expired() || gen != generation_) return;
                if (out.ok) record_slot_results(dev, out);
                op_finished(round, OpRecord{"slots", out.ok, out.kind, static_cast<int>(out.slots_sent.size())});
            };
            requests.emplace_back(std::move(req), "slots");
        }
    }
    if (requests.empty()) return;

    round_in_flight_ = true;
    round_pending_   = static_cast<int>(requests.size());
    round_ops_.clear();
    for (auto& [req, kind] : requests) {
        const Accept a = svc.link(std::move(req));
        if (weak.expired() || round != round_) return; // destroyed or reset meanwhile
        if (!a.ok) op_finished(round, OpRecord{kind, false, a.kind, 0}); // refused: `done` is not called
    }
}

void LinkDriver::op_finished(unsigned long long round, const OpRecord& record)
{
    if (round != round_ || !round_in_flight_) return;
    round_ops_.push_back(record);
    if (--round_pending_ > 0) return;
    round_in_flight_ = false;
    have_round_      = true;
    round_ended_at_  = now();
    pull_asked_at_   = round_ended_at_;
    last_round_ops_  = std::move(round_ops_);
    round_ops_.clear();
    round_end_serial_ = deps_.service->list_serial();
    deps_.service->request_pull(); // a no-op when a successful op already wants one
}

void LinkDriver::record_slot_results(const std::string& dev_id, const LinkOutcome& out)
{
    const long long       t = now();
    std::set<std::size_t> refused_items;
    for (const SlotResult& r : parse_slot_results(out.body)) {
        if (r.code == 0) continue;
        for (std::size_t k = 0; k < out.slots_sent.size(); ++k) {
            const SlotItem& i = out.slots_sent[k];
            if (i.ams_sn != r.ams_sn || i.slot_id != r.slot_id) continue;
            refused_items.insert(k);
            state_.refused[SlotKey{dev_id, i.ams_id, i.slot_id}.text() + "|" + i.spool_id] = LinkerState::Refusal{t + k_refusal_seconds, r.code};
        }
    }
    // The items the cloud took: not planned again while the list may not show them yet.
    for (std::size_t k = 0; k < out.slots_sent.size(); ++k) {
        if (refused_items.count(k)) continue;
        const SlotItem& i    = out.slots_sent[k];
        const SlotKey   slot{dev_id, i.ams_id, i.slot_id};
        state_.sent_until[i.spool_id.empty() ? sent_key_unbind(slot) : sent_key_mount(slot, i.spool_id)] = t + k_refusal_seconds;
    }
}

Accept LinkDriver::assign(const Assignment& a)
{
    if (!write_refusal(config, &a.slot).empty()) return {false, FailKind::Rejected};
    store_assignment(a);
    return {};
}

void LinkDriver::store_assignment(const Assignment& a)
{
    Assignment copy = a;
    if (copy.created_at == 0) copy.created_at = now();
    state_.assignments[copy.slot] = copy;
}

Accept LinkDriver::create_and_assign(const json& page_spool, const SlotKey& slot)
{
    SpoolService* svc = deps_.service;
    if (svc == nullptr) return {false, FailKind::Unavailable};
    if (!page_spool.is_object()) return {false, FailKind::BadRequest};
    const auto text = [&page_spool](const char* key) {
        const auto it = page_spool.find(key);
        return it != page_spool.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    const std::string setting_id = text("setting_id");
    const std::string color_code = text("color_code");
    if (setting_id.empty() || color_code.empty()) return {false, FailKind::BadRequest};

    Creation c;
    c.slot        = slot;
    c.filament_id = setting_id;
    if (auto it = page_spool.find("colors"); it != page_spool.end() && it->is_array())
        for (const json& v : *it)
            if (v.is_string() && !v.get<std::string>().empty()) c.colors.push_back(v.get<std::string>());
    if (c.colors.empty()) c.colors.push_back(color_code);
    c.created_at  = now();
    c.list_serial = svc->list_serial();
    for (const Spool& r : svc->confirmed()) c.baseline.insert(r.id);
    // Without a list there is no baseline to tell the new spool from an older identical one: it is created, not assigned.
    // It is not assigned either while linking may not write to that slot.
    const bool remember = c.list_serial > 0 && write_refusal(config, &slot).empty();

    const unsigned long long gen  = generation_;
    std::weak_ptr<int>       weak = alive_;
    const Accept             a    = svc->add(page_spool, 1);
    if (!a.ok || weak.expired() || gen != generation_ || !remember) return a; // a reset meanwhile forgets it too
    creations_.push_back(std::move(c));
    return a;
}

void LinkDriver::resolve_creations()
{
    if (creations_.empty()) return;
    SpoolService&            svc    = *deps_.service;
    const long long          t      = now();
    const unsigned long long serial = svc.list_serial();
    std::vector<Creation>    keep;
    for (Creation& c : creations_) {
        if (serial > c.list_serial) {
            std::vector<const Spool*> found;
            for (const Spool& r : svc.confirmed()) {
                if (c.baseline.count(r.id) > 0 || !r.rfid.empty() || r.in_printer || r.filament_id != c.filament_id) continue;
                if (same_rgb(r.colors.empty() ? r.color : r.colors.front(), c.colors.front())) found.push_back(&r);
            }
            if (found.size() == 1) {
                Assignment a;
                a.slot        = c.slot;
                a.spool_id    = found.front()->id;
                a.filament_id = found.front()->filament_id;
                a.colors      = found.front()->colors.empty() ? std::vector<std::string>{found.front()->color} : found.front()->colors;
                store_assignment(a);
                continue;
            }
            if (found.size() > 1) {
                creation_notes_.push_back(CreationNote{c.slot, "could not tell which new spool belongs to the slot"});
                if (creation_notes_.size() > 5) creation_notes_.erase(creation_notes_.begin());
                continue;
            }
        }
        if (t - c.created_at > k_assignment_seconds) continue; // nothing new showed up in time
        keep.push_back(std::move(c));
    }
    creations_ = std::move(keep);
}

Accept LinkDriver::unassign(const SlotKey& slot, const std::vector<PrinterView>& printers, std::string* would_unbind)
{
    SpoolService* svc = deps_.service;
    if (svc == nullptr) return {false, FailKind::Unavailable};
    if (!write_refusal(config, &slot, true).empty()) return {false, FailKind::Rejected};
    const Spool* row = nullptr;
    for (const Spool& r : svc->confirmed())
        if (claims_slot(r, slot)) {
            row = &r;
            break;
        }
    if (row == nullptr || !row->rfid.empty()) return {false, FailKind::BadRequest}; // the printer decides where an RFID spool is

    std::string ams_sn   = row->ams_sn;
    int         ams_type = row->ams_type;
    for (const PrinterView& p : printers) {
        if (p.dev_id != slot.dev_id) continue;
        if (ams_sn.empty())
            if (auto it = p.ams_sn.find(slot.ams_id); it != p.ams_sn.end()) ams_sn = it->second;
        if (ams_type < 0)
            for (const TrayFacts& t : p.trays)
                if (t.ams_id == slot.ams_id) {
                    ams_type = t.ams_type;
                    break;
                }
    }
    if (ams_sn.empty() || ams_type < 0) return {false, FailKind::BadRequest};
    if (config.mode == LinkMode::DryRun) { // what it would send, and nothing more
        if (would_unbind != nullptr) *would_unbind = row->id;
        return {};
    }

    state_.assignments.erase(slot);
    const std::string        spool_id     = row->id;
    const std::string        row_ams_sn   = row->ams_sn; // the mount as the row showed it, checked again when the op starts
    const int                row_ams_type = row->ams_type;
    std::weak_ptr<int>       weak         = alive_;
    const unsigned long long gen          = generation_;
    const unsigned long long epoch        = epoch_;
    const std::string        dev          = slot.dev_id;
    LinkRequest              req;
    req.kind        = LinkRequest::Kind::SlotSync;
    req.dev_id      = slot.dev_id;
    req.build_slots = [this, weak, epoch, slot, spool_id, row_ams_sn, row_ams_type, ams_sn, ams_type](const std::vector<Spool>& now_rows,
                                                                                                      const std::set<std::string>&) {
        if (weak.expired() || epoch != epoch_) return std::vector<SlotItem>();
        if (!unbind_still_wanted(now_rows, spool_id, slot, row_ams_sn, row_ams_type)) return std::vector<SlotItem>();
        return std::vector<SlotItem>{slot_unbind(ams_sn, slot.ams_id, ams_type, slot.slot_id)};
    };
    req.done = [this, weak, gen, dev](const LinkOutcome& out) {
        if (weak.expired() || gen != generation_) return;
        if (out.ok) record_slot_results(dev, out);
    };
    return svc->link(std::move(req));
}

void LinkDriver::sync_now()
{
    skip_spacing_ = true;
    state_.pushed.clear();
    state_.sent_until.clear();
}

void LinkDriver::reset()
{
    ++generation_;
    ++epoch_;
    ++round_;
    state_           = LinkerState{};
    last_plan_       = LinkPlan{};
    round_in_flight_ = false;
    round_pending_   = 0;
    have_round_      = false;
    round_ended_at_  = 0;
    round_end_serial_ = 0;
    pull_asked_at_   = 0;
    skip_spacing_    = false;
    round_ops_.clear();
    last_round_ops_.clear();
    creations_.clear();
    creation_notes_.clear();
}

json LinkDriver::status_json(const std::vector<PrinterView>& printers) const
{
    // A dev id is the printer's serial number: printers are named, never identified by it.
    std::map<std::string, std::string> names;
    for (const PrinterView& p : printers) names[p.dev_id] = p.name;
    const auto label = [&](const std::string& dev) {
        auto it = names.find(dev);
        return it == names.end() ? std::string("?") : it->second;
    };
    const auto where = [&](const SlotKey& k) { return label(k.dev_id) + " AMS " + std::to_string(k.ams_id) + " slot " + k.slot_id; };

    json j                = json::object();
    j["mode"]             = mode_text(config.mode);
    json only             = json::array();
    for (const std::string& text : config.only_slots) {
        const auto b = text.rfind(':');
        const auto a = b == std::string::npos || b == 0 ? std::string::npos : text.rfind(':', b - 1);
        if (a == std::string::npos) continue;
        only.push_back(label(text.substr(0, a)) + text.substr(a));
    }
    j["only_slots"]       = only;
    j["round_in_flight"]  = round_in_flight_;
    j["waiting_for_list"] = waiting_for_list();

    json ps = json::array();
    for (const PrinterView& p : printers) {
        json trays = json::array();
        for (const TrayReport& t : last_plan_.trays) {
            if (t.slot.dev_id != p.dev_id) continue;
            auto sn = p.ams_sn.find(t.slot.ams_id);
            trays.push_back(json{{"ams_id", t.slot.ams_id},
                                 {"slot_id", t.slot.slot_id},
                                 {"occupied", t.occupied},
                                 {"rule", t.rule},
                                 {"spool_id", t.spool_id},
                                 {"note", t.note},
                                 {"has_serial", sn != p.ams_sn.end() && !sn->second.empty()}});
        }
        ps.push_back(json{{"name", p.name},
                          {"takes_part", takes_part(p)},
                          {"fresh", p.fresh},
                          {"busy", p.busy},
                          {"selected", p.selected},
                          {"lan_only", p.lan_only},
                          {"observations", p.observations},
                          {"trays", trays}});
    }
    j["printers"] = ps;
    j["planned"]  = json{{"mounts", last_plan_.mounts.size()},
                        {"weights", last_plan_.weights.size()},
                        {"unbinds", last_plan_.unbinds.size()},
                        {"registrations", last_plan_.registrations.size()},
                        {"skipped_recently_sent", last_plan_.skipped_sent}};

    json lines = json::array();
    for (const PlannedMount& m : last_plan_.mounts)
        lines.push_back("mount " + where(m.slot) + " spool " + m.spool_id + (m.grams ? " " + std::to_string(*m.grams) + " g" : std::string()));
    for (const PlannedWeight& w : last_plan_.weights)
        lines.push_back("weight " + where(w.slot) + " spool " + w.spool_id + " " + std::to_string(w.grams) + " g");
    for (const PlannedUnbind& u : last_plan_.unbinds) lines.push_back("unbind " + where(u.slot) + " spool " + u.spool_id);
    for (const PlannedRegister& r : last_plan_.registrations) {
        const auto g = measured_grams(r.tray, Spool{});
        lines.push_back("register " + where(r.slot) + " new spool" + (g ? " " + std::to_string(*g) + " g" : std::string()));
    }
    j["plan"]  = lines;
    json notes = last_plan_.notes;
    for (const CreationNote& n : creation_notes_) notes.push_back(where(n.slot) + ": " + n.text);
    j["notes"] = notes;

    const long long t       = now();
    json            assigns = json::array();
    for (const auto& [key, a] : state_.assignments)
        assigns.push_back(json{{"ams_id", key.ams_id}, {"slot_id", key.slot_id}, {"spool_id", a.spool_id}, {"age_s", t - a.created_at}});
    j["assignments"] = assigns;
    json creations   = json::array();
    for (const Creation& c : creations_)
        creations.push_back(json{{"ams_id", c.slot.ams_id}, {"slot_id", c.slot.slot_id}, {"age_s", t - c.created_at}});
    j["creations"] = creations;

    if (have_round_) {
        json ops = json::array();
        for (const OpRecord& o : last_round_ops_)
            ops.push_back(json{{"kind", o.kind}, {"ok", o.ok}, {"fail_kind", o.ok ? "" : fail_text(o.fail_kind)}, {"items", o.items}});
        j["last_round"] = json{{"at", round_ended_at_}, {"ops", ops}};
    } else {
        j["last_round"] = nullptr;
    }
    return j;
}

} // namespace Slic3r::BblFilament
