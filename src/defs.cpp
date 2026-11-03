#include "defs.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <set>
#include <span>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "text.h"

using nlohmann::json;

namespace {

Defs gDefs;

struct Bad : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr double kRequired = std::numeric_limits<double>::quiet_NaN();  // a number field's default: none
constexpr size_t kMaxCopyDepth = 16;

std::string oneOf(std::span<const char* const> ids) {
    std::string out = "must be one of";
    for (size_t i = 0; i < ids.size(); ++i) out += (i ? ", " : " ") + std::string(ids[i]);
    return out;
}

bool snakeCase(const std::string& s) {
    return !s.empty() && s.size() <= 48 && s[0] >= 'a' && s[0] <= 'z' &&
           std::ranges::all_of(s, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; });
}

// ---- Pass 1: a definition file's records in file order, indexed by id.

struct File {
    std::string path;
    std::vector<std::pair<std::string, json>> records;
    std::map<std::string, size_t, std::less<>> index;
};

File parseFile(const std::filesystem::path& path) {
    File f{path.string(), {}, {}};
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Bad(std::format("{}: cannot open it", f.path));
    std::vector<std::set<std::string>> open;  // keys so far of each object being parsed
    size_t records = 0, record = 0;
    std::string duplicate;  // nlohmann keeps the last of duplicate keys; we refuse them
    auto noDuplicates = [&](int, json::parse_event_t e, json& parsed) {
        if (e == json::parse_event_t::object_start) {
            if (open.empty()) record = records++;
            open.emplace_back();
        } else if (e == json::parse_event_t::object_end) open.pop_back();
        else if (e == json::parse_event_t::key && duplicate.empty() && !open.back().insert(parsed.get<std::string>()).second)
            duplicate = std::format("{}: record {}: field '{}': duplicate key", f.path, record, parsed.get<std::string>());
        return true;
    };
    json all = json::parse(in, noDuplicates, false);
    if (all.is_discarded()) throw Bad(std::format("{}: not valid JSON", f.path));
    if (!all.is_array()) throw Bad(std::format("{}: must be an array of records", f.path));
    if (!duplicate.empty()) throw Bad(duplicate);
    for (size_t i = 0; i < all.size(); ++i) {
        const json& r = all[i];
        if (!r.is_object()) throw Bad(std::format("{}: record {}: must be an object", f.path, i));
        if (!r.contains("id") || !r["id"].is_string() || !snakeCase(r["id"].get<std::string>()))
            throw Bad(std::format("{}: record {}: field 'id': must be a snake_case string of 1..48 bytes", f.path, i));
        std::string id = r["id"].get<std::string>();
        if (auto [it, fresh] = f.index.emplace(id, i); !fresh)
            throw Bad(std::format("{}: record {} '{}': field 'id': duplicate of record {}", f.path, i, id, it->second));
        f.records.emplace_back(id, r);
    }
    return f;
}

// ---- Pass 2: one record with copy_from resolved, and checked reads that name file, record and field.

struct Record {
    const File& file;
    size_t index;
    json fields;

    const std::string& id() const { return file.records[index].first; }
    Bad fail(std::string_view field, std::string_view msg) const {
        return Bad(std::format("{}: record {} '{}': field '{}': {}", file.path, index, id(), field, msg));
    }
    // Fields the record itself may have (inherited ones were checked in their own record).
    void allow(std::initializer_list<std::string_view> names) const {
        for (const auto& [k, v] : file.records[index].second.items())
            if (std::ranges::find(names, std::string_view(k)) == names.end()) throw fail(k, "unknown field");
    }
    // A number in lo..hi; `field` is "key" or "object.key" inside `obj`. kRequired: no default.
    double number(const json& obj, std::string_view field, double lo, double hi, double def = kRequired,
                  bool integer = true) const {
        std::string key(field.substr(field.rfind('.') + 1));
        if (!obj.contains(key)) {
            if (std::isnan(def)) throw fail(field, "missing");
            return def;
        }
        const json& v = obj[key];
        if (integer ? !v.is_number_integer() : !v.is_number()) throw fail(field, integer ? "must be an integer" : "must be a number");
        double n = v.get<double>();
        if (!(n >= lo && n <= hi)) throw fail(field, std::format("{} is out of range {}..{}", v.dump(), lo, hi));
        return n;
    }
    // Display text: 1..48 bytes of UTF-8 without control, bidi or zero-width characters.
    std::string text(const char* key) const {
        const json* v = fields.contains(key) ? &fields[key] : nullptr;
        if (!v || !v->is_string() || v->get_ref<const std::string&>().empty() || v->get_ref<const std::string&>().size() > 48 ||
            !validName(v->get_ref<const std::string&>()))
            throw fail(key, "must be 1..48 bytes of text without control, bidi or zero-width characters");
        return v->get<std::string>();
    }
};

// Copies down the copy_from chain: the parent's fields (but save_id) merge-patched with the record's
// own (RFC 7386: objects merge deeply, null removes a field).
json flatten(const File& f, size_t i, size_t top, size_t depth = 0) {  // top: the record being resolved
    const auto& [id, r] = f.records[i];
    if (!r.contains("copy_from")) return r;
    Record rec{f, i, {}};
    if (!r["copy_from"].is_string()) throw rec.fail("copy_from", "must be a string id");
    std::string from = r["copy_from"].get<std::string>();
    auto it = f.index.find(from);
    if (it == f.index.end()) throw rec.fail("copy_from", std::format("unknown id '{}'", from));
    if (depth >= kMaxCopyDepth) throw Record{f, top, {}}.fail("copy_from", std::format("a cycle, or a chain longer than {}", kMaxCopyDepth));
    json out = flatten(f, it->second, top, depth + 1), own = r;
    out.erase("save_id");
    own.erase("copy_from");
    out.merge_patch(own);
    return out;
}

// Save ids of one file: first..255 (they are saved as a byte), unique, and no gaps.
struct SaveIds {
    size_t first = 1;        // items start at 1 (0 is NoItem), creatures at 0 (EntityKind values)
    std::vector<int> owner;  // record index by save id; -1 = none

    size_t claim(const Record& r) {
        auto save = size_t(r.number(r.fields, "save_id", double(first), 255));
        if (save >= owner.size()) owner.resize(save + 1, -1);
        if (owner[save] >= 0)
            throw r.fail("save_id", std::format("{} is already '{}'", save, r.file.records[size_t(owner[save])].first));
        owner[save] = int(r.index);
        return save;
    }
    // Ids are append-only, so a gap is a deleted record (retire one by keeping it).
    void checkNoGaps(const File& f) const {
        for (size_t s = first; s < owner.size(); ++s)
            if (owner[s] < 0)
                throw Bad(std::format("{}: no record has save_id {} (save ids run {}..{} with no gaps)", f.path, s, first, owner.size() - 1));
    }
};

// ---- Items.

constexpr const char* kSlotIds[] = {"head", "chest", "hands", "legs", "feet", "main_hand", "off_hand", "trinket", "bag", "none"};  // EquipSlot order
static_assert(std::size(kSlotIds) == SlotNone + 1);

// The ids code names (ItemId constants) and the records they must be.
constexpr std::pair<ItemId, const char*> kNamedItems[] = {
    {Shortsword, "iron_shortsword"}, {Satchel, "canvas_satchel"},   {Knife, "rusty_knife"},       {BoarSkin, "boar_skin"},
    {IronHelm, "iron_helm"},         {IronCuirass, "iron_cuirass"}, {IronGauntlets, "iron_gauntlets"},
    {IronGreaves, "iron_greaves"},   {IronSabatons, "iron_sabatons"}, {WolfPelt, "wolf_pelt"},    {BoneCharm, "bone_charm"},
};
static_assert(std::size(kNamedItems) == ItemCount - 1);

void loadItems(const File& f, Defs& d) {
    SaveIds saves;
    for (size_t i = 0; i < f.records.size(); ++i) {
        Record r{f, i, flatten(f, i, i)};
        r.allow({"id", "save_id", "copy_from", "name", "slot", "attack", "bag_slots", "armor", "mods"});
        size_t save = saves.claim(r);
        ItemType t{nullptr, SlotNone, {0, 0, 0}, 0, 0, {}};
        std::string name = r.text("name");
        if (r.fields.contains("slot")) {
            const json& v = r.fields["slot"];
            auto s = std::ranges::find(kSlotIds, v.is_string() ? v.get<std::string>() : "");
            if (s == std::end(kSlotIds)) throw r.fail("slot", oneOf(kSlotIds));
            t.slot = EquipSlot(s - std::begin(kSlotIds));
        }
        if (r.fields.contains("attack")) {
            const json& a = r.fields["attack"];
            if (t.slot != SlotMainHand && t.slot != SlotOffHand) throw r.fail("attack", "only main_hand and off_hand items attack");
            if (!a.is_object()) throw r.fail("attack", "must be an object {min, max, speed}");
            for (const auto& [k, v] : a.items())
                if (k != "min" && k != "max" && k != "speed") throw r.fail("attack." + k, "unknown field");
            t.attack.minDamage = int(r.number(a, "attack.min", 0, 1000));
            t.attack.maxDamage = int(r.number(a, "attack.max", t.attack.minDamage, 1000));
            t.attack.speed = float(r.number(a, "attack.speed", 0.1, 10, kRequired, false));
        } else if (t.slot == SlotMainHand) throw r.fail("attack", "missing (a main-hand item is a weapon)");
        if (t.slot == SlotBag) t.bagSlots = int(r.number(r.fields, "bag_slots", 1, 32));
        else if (r.fields.contains("bag_slots")) throw r.fail("bag_slots", "only bag items have bag slots");
        t.armor = int(r.number(r.fields, "armor", 0, 1000, 0));
        if (r.fields.contains("mods")) {  // [{"stat": "spirit", "flat": 2}, ...]: what wearing it adds (ADR-022)
            const json& ms = r.fields["mods"];
            if (!ms.is_array() || ms.size() > 8) throw r.fail("mods", "must be an array of at most 8 {stat, flat|increased|more}");
            if (!ms.empty() && t.slot >= EquipCount) throw r.fail("mods", "only items worn in an equip slot have mods");
            for (size_t j = 0; j < ms.size(); ++j) {
                const json& m = ms[j];
                std::string field = std::format("mods[{}]", j);
                if (!m.is_object()) throw r.fail(field, "must be an object {stat, flat|increased|more}");
                for (const auto& [k, v] : m.items())
                    if (k != "stat" && k != "flat" && k != "increased" && k != "more") throw r.fail(field + "." + k, "unknown field");
                auto stat = std::ranges::find(kStatIds, m.contains("stat") && m["stat"].is_string() ? m["stat"].get<std::string>() : "");
                if (stat == std::end(kStatIds))
                    throw r.fail(field + ".stat", oneOf(kStatIds));
                if (m.contains("flat") + m.contains("increased") + m.contains("more") != 1)
                    throw r.fail(field, "needs exactly one of flat, increased, more");
                ModKind kind = m.contains("flat") ? ModKind::Flat : m.contains("increased") ? ModKind::Increased : ModKind::More;
                bool flat = kind == ModKind::Flat;  // flat: points; increased and more: fractions (0.1 = 10%)
                double value = r.number(m, field + (flat ? ".flat" : kind == ModKind::Increased ? ".increased" : ".more"),
                                        flat ? -1000 : -1, flat ? 1000 : 10, kRequired, false);
                t.mods.push_back({Stat(stat - std::begin(kStatIds)), kind, float(value)});
            }
        }
        if (d.items.size() <= save) d.items.resize(save + 1), d.itemIds.resize(save + 1), d.itemNames.resize(save + 1);
        d.items[save] = t;
        d.itemIds[save] = r.id();
        d.itemNames[save] = std::move(name);
        d.itemById.emplace(r.id(), ItemId(save));
    }
    saves.checkNoGaps(f);
    for (auto [item, id] : kNamedItems)
        if (item >= d.itemIds.size() || d.itemIds[item] != id)
            throw Bad(std::format("{}: save_id {} must be '{}' (code names it), not '{}'", f.path, int(item), id,
                                  item < d.itemIds.size() ? d.itemIds[item] : "missing"));
    for (size_t s = 0; s < d.items.size(); ++s) d.items[s].name = d.itemNames[s].c_str();
}

// ---- Creatures (after items: they name their weapon and loot).

// Model painters are code (model.cpp); creatures name the body models.
constexpr std::pair<Model, const char*> kModelIds[] = {
    {ModelPlayer, "player"}, {ModelVillager, "villager"}, {ModelDummy, "dummy"}, {ModelBoar, "boar"}, {ModelBandit, "bandit"},
    {ModelWolf, "wolf"},     {ModelGuard, "guard"},       {ModelChief, "chief"}, {ModelGhoul, "ghoul"},
};
// kEntityKindNames (story.h) are the ids code names (EntityKind constants) and the records they must be.
static_assert(std::size(kEntityKindNames) == size_t(EntityKind::Count));

void loadCreatures(const File& f, Defs& d) {
    SaveIds saves{.first = 0};
    for (size_t i = 0; i < f.records.size(); ++i) {
        Record r{f, i, flatten(f, i, i)};
        r.allow({"id", "save_id", "copy_from", "name", "level", "stats", "attackable", "wanders", "talks", "model", "temper",
                 "aggro_range", "wander_radius", "chase_speed", "weapon", "loot", "loot_chance", "respawn_seconds", "leash_radius",
                 "reaggro_radius", "reaggro_cooldown", "give_up_seconds", "attack_windup", "attack_recover", "attack_cooldown",
                 "flee_health"});
        size_t save = saves.claim(r);
        auto given = [&](const char* key) { return r.fields.contains(key) && !r.fields[key].is_null(); };
        auto flag = [&](const char* key) {
            if (!r.fields.contains(key)) return false;
            if (!r.fields[key].is_boolean()) throw r.fail(key, "must be true or false");
            return r.fields[key].get<bool>();
        };
        auto item = [&](const char* key) {
            if (!given(key)) return NoItem;
            const json& v = r.fields[key];
            auto it = v.is_string() ? d.itemById.find(v.get<std::string>()) : d.itemById.end();
            if (it == d.itemById.end()) throw r.fail(key, std::format("unknown item {}", v.dump()));
            return it->second;
        };
        EntityType t{};
        std::string name = given("name") ? r.text("name") : "";  // none: a random villager name
        t.level = int(r.number(r.fields, "level", 1, 100));
        if (!r.fields.contains("stats") || !r.fields["stats"].is_object())
            throw r.fail("stats", "must be an object {stamina, strength, agility, intellect, spirit}");
        const json& st = r.fields["stats"];
        for (const auto& [k, v] : st.items())
            if (k != "stamina" && k != "strength" && k != "agility" && k != "intellect" && k != "spirit") throw r.fail("stats." + k, "unknown field");
        t.stats = {int(r.number(st, "stats.stamina", 1, 1000)), int(r.number(st, "stats.strength", 0, 1000)),
                   int(r.number(st, "stats.agility", 0, 1000)), int(r.number(st, "stats.intellect", 0, 1000)),
                   int(r.number(st, "stats.spirit", 0, 1000))};
        t.attackable = flag("attackable"), t.wanders = flag("wanders"), t.talks = flag("talks");
        const json* m = r.fields.contains("model") && r.fields["model"].is_string() ? &r.fields["model"] : nullptr;
        auto model = std::ranges::find(kModelIds, m ? m->get<std::string>() : "", [](auto& p) { return std::string_view(p.second); });
        if (model == std::end(kModelIds)) throw r.fail("model", "must be one of player, villager, dummy, boar, bandit, wolf, guard, chief, ghoul");
        t.model = model->first;
        const json* tp = r.fields.contains("temper") && r.fields["temper"].is_string() ? &r.fields["temper"] : nullptr;
        auto temper = std::ranges::find(kTemperNames, tp ? tp->get<std::string>() : "");
        if (temper == std::end(kTemperNames)) throw r.fail("temper", "must be one of harmless, passive, aggressive");
        t.temper = Temper(temper - std::begin(kTemperNames));
        t.aggroRange = float(r.number(r.fields, "aggro_range", 0, 64, 0, false));
        if (t.aggroRange > 0 && t.temper != Temper::Aggressive) throw r.fail("aggro_range", "only aggressive creatures attack on sight");
        t.wanderRadius = float(r.number(r.fields, "wander_radius", 0, kMaxMarkerRadius, 0, false));
        t.chaseSpeed = float(r.number(r.fields, "chase_speed", 0, 30, 0, false));
        if (t.chaseSpeed <= 0 && t.temper != Temper::Harmless) throw r.fail("chase_speed", "missing (a creature that fights chases)");
        t.weapon = item("weapon");
        if (t.weapon && d.items[t.weapon].slot != SlotMainHand) throw r.fail("weapon", std::format("'{}' is not a main_hand item", d.itemIds[t.weapon]));
        t.loot = item("loot");
        t.lootChance = float(r.number(r.fields, "loot_chance", 0, 1, 0, false));
        if (t.lootChance > 0 && !t.loot) throw r.fail("loot_chance", "no 'loot' to drop");
        t.respawnSeconds = float(r.number(r.fields, "respawn_seconds", 0, 3600, kRequired, false));
        // The AI's numbers (ADR-020): only for creatures that fight; the defaults are the AI before them.
        for (const char* key : {"leash_radius", "reaggro_radius", "reaggro_cooldown", "give_up_seconds", "attack_windup",
                                "attack_recover", "attack_cooldown", "flee_health"})
            if (given(key) && t.temper == Temper::Harmless) throw r.fail(key, "harmless creatures don't fight");
        if (t.temper != Temper::Harmless && !t.wanders) throw r.fail("wanders", "must be true for a creature that fights (the AI moves it)");
        t.leashRadius = float(r.number(r.fields, "leash_radius", 2, 64, 20, false));
        t.reaggroRadius = float(r.number(r.fields, "reaggro_radius", 0, 64, 0.8 * t.leashRadius, false));
        if (t.reaggroRadius >= t.leashRadius) throw r.fail("reaggro_radius", "must be less than leash_radius (or it flaps at the edge)");
        t.reaggroCooldown = float(r.number(r.fields, "reaggro_cooldown", 0, 60, 3, false));
        t.giveUpSeconds = float(r.number(r.fields, "give_up_seconds", 0.5, 60, 5, false));
        t.windup = float(r.number(r.fields, "attack_windup", 0.05, 2, kSwingWindup, false));
        // The swing's impact comes at most kPunchImpact into it (startPunch), so a longer windup would be cut short.
        float swing = swingTime(compute(t.stats, d.items[t.weapon].mods), t.weapon ? d.items[t.weapon].attack : kPunch);
        if (t.temper != Temper::Harmless && t.windup >= kPunchImpact * swing)
            throw r.fail("attack_windup", std::format("{} s must be less than {} x its swing time ({:.3f} s)", t.windup, kPunchImpact, swing));
        t.recover = float(r.number(r.fields, "attack_recover", 0, 2, 0.2, false));
        t.cooldown = float(r.number(r.fields, "attack_cooldown", 0, 10, 0, false));
        t.fleeHealth = float(r.number(r.fields, "flee_health", 0, 0.95, 0, false));
        if (d.creatures.size() <= save) d.creatures.resize(save + 1), d.creatureIds.resize(save + 1), d.creatureNames.resize(save + 1);
        d.creatures[save] = t;
        d.creatureIds[save] = r.id();
        d.creatureNames[save] = std::move(name);
        d.creatureById.emplace(r.id(), EntityKind(save));
    }
    saves.checkNoGaps(f);
    for (size_t k = 0; k < size_t(EntityKind::Count); ++k)
        if (k >= d.creatureIds.size() || d.creatureIds[k] != kEntityKindNames[k])
            throw Bad(std::format("{}: save_id {} must be '{}' (code names it), not '{}'", f.path, k, kEntityKindNames[k],
                                  k < d.creatureIds.size() ? d.creatureIds[k] : "missing"));
    for (size_t s = 0; s < d.creatures.size(); ++s) d.creatures[s].name = d.creatureNames[s].empty() ? nullptr : d.creatureNames[s].c_str();
}

}  // namespace

std::filesystem::path dataDir() {
    std::error_code ec;
    return std::filesystem::is_directory("data", ec) ? std::filesystem::path("data")
                                                     : std::filesystem::path(FLYCOOS_SOURCE_DIR) / "data";
}

bool loadDefs(const std::filesystem::path& dir, Defs& out, std::string& err) {
    try {
        File items = parseFile(dir / "items.json");  // pass 1: every file before any reference is resolved
        File creatures = parseFile(dir / "creatures.json");
        Defs d;
        loadItems(items, d);  // pass 2, in reference order: items first
        loadCreatures(creatures, d);
        out = std::move(d);
        err.clear();
        return true;
    } catch (const std::exception& e) {  // Bad, or a json error the checks above missed
        err = e.what();
        return false;
    }
}

const Defs& defs() { return gDefs; }
void setDefs(Defs&& d) { gDefs = std::move(d); }

bool reloadDefs(const std::filesystem::path& dir, std::string& err) {
    Defs d;
    if (!loadDefs(dir, d, err)) return false;
    // Inventories hold item save ids, spawn markers creature ones.
    auto kept = [&](const std::vector<std::string>& now, const std::vector<std::string>& was, size_t first, const char* what,
                    const char* holders) {
        if (now.size() < was.size()) {
            err = std::format("{} {}s, fewer than the {} loaded: {} may hold the missing ones", now.size() - first, what,
                              was.size() - first, holders);
            return false;
        }
        for (size_t s = first; s < was.size(); ++s)
            if (now[s] != was[s]) {
                err = std::format("{} save_id {} is now '{}', was '{}': save ids are never reused", what, s, now[s], was[s]);
                return false;
            }
        return true;
    };
    if (!kept(d.itemIds, gDefs.itemIds, 1, "item", "inventories") ||
        !kept(d.creatureIds, gDefs.creatureIds, 0, "creature", "spawn markers"))
        return false;
    setDefs(std::move(d));
    return true;
}
