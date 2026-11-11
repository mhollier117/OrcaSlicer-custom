#include "Linker.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <tuple>
#include <utility>

namespace Slic3r::BblFilament {

bool SlotKey::operator<(const SlotKey& o) const { return std::tie(dev_id, ams_id, slot_id) < std::tie(o.dev_id, o.ams_id, o.slot_id); }
bool SlotKey::operator==(const SlotKey& o) const { return dev_id == o.dev_id && ams_id == o.ams_id && slot_id == o.slot_id; }
std::string SlotKey::text() const { return dev_id + ":" + std::to_string(ams_id) + ":" + slot_id; }

bool takes_part(const PrinterView& p) { return p.bound && p.fresh && (!p.lan_only || p.selected); }

bool LinkPlan::has_actions() const { return !mounts.empty() || !weights.empty() || !unbinds.empty() || !registrations.empty(); }

// ---------------------------------------------------------------------------------------------------------------------
// helpers

namespace {

std::string rgb_of(const std::string& c)
{
    std::string s;
    for (char ch : c) {
        if (ch == '#') continue;
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    }
    if (s.size() > 6) s.resize(6);
    return s;
}

bool colours_agree(const std::vector<std::string>& a, const std::vector<std::string>& b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (!same_rgb(a[i], b[i])) return false;
    return true;
}

std::vector<std::string> tray_colours(const TrayFacts& t)
{
    if (!t.colors.empty()) return t.colors;
    if (!t.color.empty()) return {t.color};
    return {};
}

bool tray_agrees_with_assignment(const TrayFacts& t, const Assignment& a)
{
    if (t.filament_id != a.filament_id) return false;
    const std::vector<std::string> tc = tray_colours(t);
    if (tc.size() > 1 || a.colors.size() > 1) return colours_agree(tc, a.colors);
    return !a.colors.empty() && same_rgb(t.color, a.colors.front());
}

bool tray_agrees_with_row(const TrayFacts& t, const Spool& r)
{
    if (!t.filament_id.empty() && t.filament_id != r.filament_id) return false;
    if (!t.color.empty() && !same_rgb(t.color, r.color)) return false;
    return true;
}

bool claims(const Spool& r, const SlotKey& k)
{
    return r.in_printer && r.dev_id == k.dev_id && r.ams_id == k.ams_id && r.slot_id == k.slot_id;
}

std::string where(const PrinterView& p, const TrayFacts& t)
{
    return p.name + " AMS " + std::to_string(t.ams_id) + " slot " + t.slot_id;
}

// The total the percent rule of measured_grams uses.
long long percent_total(const TrayFacts& t, const Spool& r) { return t.total_g > 0 ? t.total_g : std::llround(r.total_net_weight); }

// The grams a weight push or an RFID mount sends; nothing when the row keeps its own weight: no valid measurement, or
// a measurement from the percent while the row's own weight rounds to that percent (a percent cannot overrule a finer
// number: a row with 304 of 1000 g and a tray at 30 % stay at 304).
std::optional<long long> grams_to_send(const TrayFacts& t, const Spool& r)
{
    const auto g = measured_grams(t, r);
    if (!g || t.remain_g >= 0) return g;
    const long long total = percent_total(t, r);
    if (total > 0 && std::llround(r.net_weight * 100.0 / static_cast<double>(total)) == t.remain) return std::nullopt;
    return g;
}

const TrayFacts* tray_at(const PrinterView& p, int ams_id, const std::string& slot_id)
{
    for (const TrayFacts& t : p.trays)
        if (t.ams_id == ams_id && t.slot_id == slot_id) return &t;
    return nullptr;
}

} // namespace

std::string sent_key_mount(const SlotKey& slot, const std::string& spool_id) { return "mount|" + slot.text() + "|" + spool_id; }
std::string sent_key_unbind(const SlotKey& slot) { return "unbind|" + slot.text(); }
std::string sent_key_weight(const std::string& rfid, long long grams) { return "weight|" + rfid + "|" + std::to_string(grams); }

bool same_rgb(const std::string& a, const std::string& b)
{
    const std::string x = rgb_of(a), y = rgb_of(b);
    if (x.empty() || y.empty()) return false;
    return x == y;
}

std::optional<long long> measured_grams(const TrayFacts& tray, const Spool& row)
{
    if (tray.reading) return std::nullopt;
    if (tray.remain_g >= 0) return static_cast<long long>(tray.remain_g);
    if (tray.remain < 0 || tray.remain > 100) return std::nullopt;
    const long long total = percent_total(tray, row);
    if (total <= 0) return std::nullopt;
    return std::llround(static_cast<double>(total) * tray.remain / 100.0);
}

AmsItem ams_item_from_row(const Spool& row, const std::string& ams_sn, int ams_id, int ams_type, const std::string& slot_id,
                          std::optional<long long> grams)
{
    AmsItem i;
    i.rfid        = row.rfid;
    i.vendor      = row.vendor;
    i.type        = row.type;
    i.name        = row.name;
    i.filament_id = row.filament_id;
    i.color       = row.color;
    i.colors      = row.colors;
    if (i.colors.empty()) i.colors = {row.color};
    i.color_type       = i.colors.size() <= 1 ? 2 : row.color_type;
    i.tray_id_name     = row.tray_id_name;
    i.note             = row.note;
    i.total_net_weight = std::llround(row.total_net_weight);
    i.net_weight       = grams ? *grams : std::llround(row.net_weight);
    i.ams_sn           = ams_sn;
    i.ams_id           = ams_id;
    i.ams_type         = ams_type;
    i.slot_id          = slot_id;
    i.create_new       = false;
    i.is_support       = false;
    return i;
}

AmsItem ams_item_from_tray(const TrayFacts& tray, const std::string& ams_sn, const FilamentLookup& lookup)
{
    AmsItem i;
    i.rfid        = tray.uuid;
    i.vendor      = "Bambu Lab";
    i.type        = tray.type;
    i.name        = tray.sub_brands;
    i.filament_id = tray.filament_id;
    i.color       = tray.color;
    i.colors      = tray.colors;
    if (i.colors.empty()) i.colors = {tray.color};
    i.color_type       = i.colors.size() <= 1 ? 2 : normalize_color_type(tray.color_type, i.colors.size());
    i.tray_id_name     = tray.tray_id_name;
    i.total_net_weight = tray.total_g;
    const auto grams   = measured_grams(tray, Spool{});
    i.net_weight       = grams ? *grams : tray.total_g;
    i.ams_sn           = ams_sn;
    i.ams_id           = tray.ams_id;
    i.ams_type         = tray.ams_type;
    i.slot_id          = tray.slot_id;
    if (lookup.is_support) {
        const auto s = lookup.is_support(tray.filament_id);
        if (s) i.is_support = *s;
    }
    i.create_new = false;
    return i;
}

SlotItem slot_bind(const std::string& ams_sn, int ams_id, int ams_type, const std::string& slot_id, const Spool& row)
{
    SlotItem s;
    s.ams_sn   = ams_sn;
    s.ams_id   = ams_id;
    s.ams_type = ams_type;
    s.slot_id  = slot_id;
    s.spool_id = row.id;
    s.rfid     = row.rfid;
    return s;
}

SlotItem slot_unbind(const std::string& ams_sn, int ams_id, int ams_type, const std::string& slot_id)
{
    SlotItem s;
    s.ams_sn   = ams_sn;
    s.ams_id   = ams_id;
    s.ams_type = ams_type;
    s.slot_id  = slot_id;
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// the plan

LinkPlan plan_links(const std::vector<PrinterView>& printers, const std::vector<Spool>& rows, const std::set<std::string>& busy,
                    unsigned long long list_serial, LinkerState& state, const LinkConfig& config, long long now)
{
    LinkPlan plan;
    if (config.mode == LinkMode::Off) {
        plan.notes.push_back("Linking is off.");
        return plan;
    }
    if (list_serial == 0) {
        plan.notes.push_back("The spool list has not been loaded yet.");
        return plan;
    }

    // Expiry.
    for (auto it = state.assignments.begin(); it != state.assignments.end();) {
        if (now - it->second.created_at > k_assignment_seconds) {
            plan.notes.push_back("The assignment of spool " + it->second.spool_id + " ran out.");
            it = state.assignments.erase(it);
        } else
            ++it;
    }
    for (auto it = state.refused.begin(); it != state.refused.end();) {
        if (it->second.until <= now) it = state.refused.erase(it);
        else ++it;
    }
    for (auto it = state.sent_until.begin(); it != state.sent_until.end();) {
        if (it->second <= now) it = state.sent_until.erase(it);
        else ++it;
    }

    const auto slot_allowed = [&](const SlotKey& k) { return config.only_slots.empty() || config.only_slots.count(k.text()) > 0; };
    const auto refused      = [&](const SlotKey& k, const std::string& spool_id) {
        auto it = state.refused.find(k.text() + "|" + spool_id);
        return it != state.refused.end() && it->second.until > now;
    };
    const auto sent_lately = [&](const std::string& key) {
        auto it = state.sent_until.find(key);
        return it != state.sent_until.end() && it->second > now;
    };

    struct Entry
    {
        const PrinterView* printer{nullptr};
        const TrayFacts*   tray{nullptr};
        SlotKey            key;
    };
    std::vector<Entry>                       entries;
    std::map<std::string, const PrinterView*> taking;
    for (const PrinterView& p : printers) {
        if (!takes_part(p)) continue;
        taking[p.dev_id] = &p;
        for (const TrayFacts& t : p.trays) entries.push_back({&p, &t, SlotKey{p.dev_id, t.ams_id, t.slot_id}});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.key < b.key; });

    std::map<std::string, std::size_t> row_by_id;
    for (std::size_t j = 0; j < rows.size(); ++j) row_by_id.emplace(rows[j].id, j);

    const std::size_t n = entries.size();
    std::vector<int>         tray_row(n, -1);
    std::vector<std::string> tray_rule(n);
    std::vector<bool>        row_used(rows.size(), false), rfid_blocked(n, false);
    std::vector<bool>        row_has_rfid_of_tray(n, false);

    const auto take = [&](std::size_t i, std::size_t j, const char* rule) {
        tray_row[i]  = static_cast<int>(j);
        tray_rule[i] = rule;
        row_used[j]  = true;
    };

    // Pass 1: RFID.
    for (std::size_t i = 0; i < n; ++i) {
        const TrayFacts& t = *entries[i].tray;
        if (!valid_rfid(t.uuid)) continue;
        rfid_blocked[i] = true;
        for (std::size_t j = 0; j < rows.size(); ++j) {
            if (rows[j].rfid != t.uuid) continue;
            row_has_rfid_of_tray[i] = true;
            if (!row_used[j]) {
                take(i, j, "rfid");
                break;
            }
        }
    }
    // Pass 2: assignments.
    for (std::size_t i = 0; i < n; ++i) {
        const TrayFacts& t = *entries[i].tray;
        if (tray_row[i] >= 0 || rfid_blocked[i] || !t.occupied) continue;
        auto a = state.assignments.find(entries[i].key);
        if (a == state.assignments.end()) continue;
        auto r = row_by_id.find(a->second.spool_id);
        if (r == row_by_id.end() || row_used[r->second]) continue;
        if (tray_agrees_with_assignment(t, a->second)) take(i, r->second, "assignment");
    }
    // Pass 3: the cloud's own pin.
    for (std::size_t i = 0; i < n; ++i) {
        const TrayFacts& t = *entries[i].tray;
        if (tray_row[i] >= 0 || rfid_blocked[i] || !t.occupied) continue;
        for (std::size_t j = 0; j < rows.size(); ++j) {
            if (row_used[j] || !rows[j].rfid.empty() || !claims(rows[j], entries[i].key)) continue;
            if (!tray_agrees_with_row(t, rows[j])) continue;
            take(i, j, "cloud");
            break;
        }
    }

    std::set<SlotKey> mounted_slots; // slots a mount of this plan names
    std::set<SlotKey> matched_slots; // slots whose tray matched a row

    // Matched and unmatched trays.
    std::vector<TrayReport> reports(n);
    for (std::size_t i = 0; i < n; ++i) {
        const PrinterView& p = *entries[i].printer;
        const TrayFacts&   t = *entries[i].tray;
        const SlotKey&     S = entries[i].key;
        TrayReport&        rep = reports[i];
        rep.slot     = S;
        rep.occupied = t.occupied;
        rep.rule     = tray_rule[i];
        const auto note = [&](const std::string& text) {
            plan.notes.push_back(text);
            if (!rep.note.empty()) rep.note += " ";
            rep.note += text;
        };
        const std::string sn = [&] {
            auto it = p.ams_sn.find(t.ams_id);
            return it == p.ams_sn.end() ? std::string() : it->second;
        }();

        if (tray_row[i] >= 0) {
            const Spool& R = rows[static_cast<std::size_t>(tray_row[i])];
            rep.spool_id   = R.id;
            matched_slots.insert(S);
            state.unmatched.erase(R.id);
            const bool mounted = R.in_printer && R.dev_id == S.dev_id && R.ams_sn == sn && R.ams_id == t.ams_id && R.ams_type == t.ams_type &&
                                 R.slot_id == t.slot_id;
            if (tray_rule[i] == "assignment" && mounted) state.assignments.erase(S);

            if (busy.count(R.id)) {
                note("Spool " + R.id + " has a change waiting: left alone at " + where(p, t) + ".");
                continue;
            }
            if (t.external || sn.empty()) {
                note("No AMS serial known for " + where(p, t) + ": nothing sent.");
                continue;
            }
            if (!slot_allowed(S)) {
                note("Slot " + where(p, t) + " is not in the chosen slots: nothing sent.");
                continue;
            }
            if (refused(S, R.id)) {
                note("The cloud refused spool " + R.id + " at " + where(p, t) + " lately: not tried again yet.");
                continue;
            }
            if (!mounted) {
                mounted_slots.insert(S);
                if (sent_lately(sent_key_mount(S, R.id))) {
                    ++plan.skipped_sent;
                    note("Spool " + R.id + " was sent to " + where(p, t) + " a short while ago and the list does not show it: not sent again yet.");
                    continue;
                }
                PlannedMount m;
                m.slot     = S;
                m.ams_sn   = sn;
                m.ams_type = t.ams_type;
                m.spool_id = R.id;
                m.rfid     = !R.rfid.empty();
                if (m.rfid) m.grams = grams_to_send(t, R);
                plan.mounts.push_back(m);
            } else if (!R.rfid.empty()) {
                const auto g = grams_to_send(t, R);
                if (g && *g != std::llround(R.net_weight)) {
                    auto pushed = state.pushed.find(R.rfid);
                    if (p.busy && pushed != state.pushed.end() && now - pushed->second.sent_at < k_busy_push_seconds) {
                        ++plan.skipped_cooldown[p.dev_id];
                        note("Weight of spool " + R.id + " was sent a short while ago and " + p.name + " is busy: waiting.");
                    } else if (sent_lately(sent_key_weight(R.rfid, *g))) {
                        ++plan.skipped_sent;
                        note("Weight " + std::to_string(*g) + " g of spool " + R.id + " was sent a short while ago and the list does not show it: not sent again yet.");
                    } else {
                        plan.weights.push_back({S, sn, t.ams_type, R.id, *g});
                    }
                }
            }
            continue;
        }

        // Unmatched.
        if (!valid_rfid(t.uuid) || !t.official || row_has_rfid_of_tray[i]) continue;
        if (p.lan_only || t.ams_type == 2 || t.external || sn.empty() || !slot_allowed(S)) continue;
        auto reg = state.registrations.find(t.uuid);
        if (reg != state.registrations.end() && (reg->second.done || reg->second.next_at > now)) continue;
        plan.registrations.push_back({S, sn, t.ams_type, t});
    }

    // Rows that claim a slot of a printer that takes part and that no tray matched.
    std::set<std::string> still_claiming;
    std::vector<PlannedUnbind> unbinds;
    for (std::size_t j = 0; j < rows.size(); ++j) {
        const Spool& R = rows[j];
        if (row_used[j] || !R.in_printer) continue;
        auto pit = taking.find(R.dev_id);
        if (pit == taking.end()) continue;
        const PrinterView& p = *pit->second;
        const SlotKey      S{R.dev_id, R.ams_id, R.slot_id};
        still_claiming.insert(R.id);

        const std::string place = p.name + " AMS " + std::to_string(R.ams_id) + " slot " + R.slot_id;
        if (busy.count(R.id)) {
            plan.notes.push_back("Spool " + R.id + " has a change waiting: its claim on " + place + " is left alone.");
            continue;
        }
        if (state.assignments.count(S)) {
            plan.notes.push_back("A spool is being assigned to " + place + ": the claim of spool " + R.id + " is left alone.");
            continue;
        }
        if (mounted_slots.count(S)) {
            plan.notes.push_back("Another spool is being mounted at " + place + ": the claim of spool " + R.id + " is left alone.");
            continue;
        }
        // An unbind names the slot, not the spool: it would also remove the binding of the spool that is there.
        if (matched_slots.count(S)) {
            plan.notes.push_back("Another spool is in " + place + ": the claim of spool " + R.id + " is left alone.");
            continue;
        }
        // An occupied slot whose spool cannot be identified right now (its tag is being read, or not read yet after
        // the AMS restarted) is not an empty slot: the streak neither starts nor grows.
        if (const TrayFacts* t = tray_at(p, R.ams_id, R.slot_id); t != nullptr && t->occupied && (t->reading || (!R.rfid.empty() && !valid_rfid(t->uuid)))) {
            plan.notes.push_back("The spool in " + place + " cannot be identified yet: the claim of spool " + R.id + " is left alone.");
            continue;
        }

        auto it = state.unmatched.find(R.id);
        if (it == state.unmatched.end()) {
            state.unmatched[R.id] = LinkerState::Streak{1, p.observations, now, list_serial};
            it                    = state.unmatched.find(R.id);
        } else if (p.observations != it->second.last_observation) {
            ++it->second.count;
            it->second.last_observation = p.observations;
        }
        const auto& streak = it->second;
        if (streak.count < k_unbind_observations || now - streak.first_at < k_unbind_seconds) continue;
        if (R.ams_sn.empty() || !slot_allowed(S) || refused(S, std::string())) continue;
        if (list_serial <= streak.list_serial) {
            plan.want_pull = true;
        } else if (sent_lately(sent_key_unbind(S))) {
            ++plan.skipped_sent;
            plan.notes.push_back("The slot " + place + " was cleared a short while ago and the list still shows spool " + R.id + ": not sent again yet.");
        } else {
            unbinds.push_back({S, R.ams_sn, R.ams_type, R.id});
        }
    }
    for (auto it = state.unmatched.begin(); it != state.unmatched.end();) {
        if (still_claiming.count(it->first) == 0) it = state.unmatched.erase(it);
        else ++it;
    }
    std::stable_sort(unbinds.begin(), unbinds.end(), [](const PlannedUnbind& a, const PlannedUnbind& b) { return a.slot < b.slot; });
    plan.unbinds = std::move(unbinds);
    plan.trays   = std::move(reports);
    return plan;
}

} // namespace Slic3r::BblFilament
