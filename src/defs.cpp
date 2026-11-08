#include "defs.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
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

// [{"stat": "spirit", "flat": 2}, ...]: stat modifiers (ADR-022), at most 8, each a known stat and
// exactly one of flat (points), increased or more (fractions: 0.1 = 10%), and optionally "with" a
// weapon skill: only on the hits made with it (ADR-027).
std::vector<Modifier> readMods(const Record& r, const json& ms, const std::string& key) {
    if (!ms.is_array() || ms.size() > 8) throw r.fail(key, "must be an array of at most 8 {stat, flat|increased|more}");
    std::vector<Modifier> out;
    for (size_t j = 0; j < ms.size(); ++j) {
        const json& m = ms[j];
        std::string field = std::format("{}[{}]", key, j);
        if (!m.is_object()) throw r.fail(field, "must be an object {stat, flat|increased|more}");
        for (const auto& [k, v] : m.items())
            if (k != "stat" && k != "flat" && k != "increased" && k != "more" && k != "with") throw r.fail(field + "." + k, "unknown field");
        auto stat = std::ranges::find(kStatIds, m.contains("stat") && m["stat"].is_string() ? m["stat"].get<std::string>() : "");
        if (stat == std::end(kStatIds)) throw r.fail(field + ".stat", oneOf(kStatIds));
        if (m.contains("flat") + m.contains("increased") + m.contains("more") != 1)
            throw r.fail(field, "needs exactly one of flat, increased, more");
        ModKind kind = m.contains("flat") ? ModKind::Flat : m.contains("increased") ? ModKind::Increased : ModKind::More;
        bool flat = kind == ModKind::Flat;
        double value = r.number(m, field + (flat ? ".flat" : kind == ModKind::Increased ? ".increased" : ".more"), flat ? -1000 : -1,
                                flat ? 1000 : 10, kRequired, false);
        Modifier mod{Stat(stat - std::begin(kStatIds)), kind, float(value)};
        if (m.contains("with")) {
            auto with = std::ranges::find(kSkillIds, m["with"].is_string() ? m["with"].get<std::string>() : "");
            if (with == std::end(kSkillIds) || Skill(with - std::begin(kSkillIds)) == Skill::Armor)
                throw r.fail(field + ".with", "must be a weapon skill: one_handed, unarmed");
            mod.with = Skill(with - std::begin(kSkillIds));
        }
        out.push_back(mod);
    }
    return out;
}

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

// One of kDamageTypeIds.
DamageType damageType(const Record& r, const json& v, std::string_view field) {
    auto at = std::ranges::find(kDamageTypeIds, v.is_string() ? v.get<std::string>() : "");
    if (at == std::end(kDamageTypeIds)) throw r.fail(field, oneOf(kDamageTypeIds));
    return DamageType(at - std::begin(kDamageTypeIds));
}
// A number for every damage type: one for all, or an object with each ({"slash": 0.9, ...}). Absent:
// `def` for all (kRequired: it must be there).
PerType perType(const Record& r, const json& v, std::string_view field, double lo, double hi, double def) {
    PerType out;
    std::string f(field), key = f.substr(f.rfind('.') + 1);
    if (v.is_null() || v.is_number()) {
        out.fill(float(r.number(v.is_null() ? json::object() : json{{key, v}}, f, lo, hi, def, false)));
        return out;
    }
    if (!v.is_object()) throw r.fail(field, "must be a number or an object {slash, strike, pierce}");
    for (const auto& [k, x] : v.items())
        if (std::ranges::find(kDamageTypeIds, k) == std::end(kDamageTypeIds)) throw r.fail(f + "." + k, "unknown damage type");
    for (size_t i = 0; i < out.size(); ++i) out[i] = float(r.number(v, f + "." + kDamageTypeIds[i], lo, hi, kRequired, false));
    return out;
}

void loadItems(const File& f, Defs& d) {
    SaveIds saves;
    for (size_t i = 0; i < f.records.size(); ++i) {
        Record r{f, i, flatten(f, i, i)};
        r.allow({"id", "save_id", "copy_from", "name", "slot", "attack", "bag_slots", "armor", "mods", "shield", "weight", "damage", "parry"});
        size_t save = saves.claim(r);
        ItemType t{nullptr, SlotNone, {0, 0, 0}, 0, 0, {}, {}};
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
        // Weight (ADR-039), what a weapon's swings do (ADR-040), whether it parries (ADR-041).
        t.weight = float(r.number(r.fields, "weight", 0, 100, 0, false));
        if (t.weight > 0 && t.slot >= EquipCount) throw r.fail("weight", "only items worn in an equip slot weigh on the equip load");
        if (r.fields.contains("damage")) {
            if (!r.fields.contains("attack")) throw r.fail("damage", "only weapons (with an 'attack') do damage");
            t.damage = damageType(r, r.fields["damage"], "damage");
        }
        t.parry = float(r.number(r.fields, "parry", 0.25, 4, 0, false));
        if (t.parry > 0 && t.slot != SlotMainHand) throw r.fail("parry", "only main_hand weapons parry (shields: shield.parry)");
        if (t.slot == SlotBag) t.bagSlots = int(r.number(r.fields, "bag_slots", 1, 32));
        else if (r.fields.contains("bag_slots")) throw r.fail("bag_slots", "only bag items have bag slots");
        t.armor = int(r.number(r.fields, "armor", 0, 1000, 0));
        if (r.fields.contains("shield")) {  // ADR-036
            const json& sh = r.fields["shield"];
            if (t.slot != SlotOffHand) throw r.fail("shield", "only off_hand items are shields");
            if (!sh.is_object()) throw r.fail("shield", "must be an object {size, negation, boost, parry}");
            for (const auto& [k, v] : sh.items())
                if (k != "size" && k != "negation" && k != "boost" && k != "parry") throw r.fail("shield." + k, "unknown field");
            constexpr const char* kSizes[] = {"small", "medium", "great"};
            const json& size = sh.contains("size") ? sh["size"] : json();
            auto at = std::ranges::find(kSizes, size.is_string() ? size.get<std::string>() : "");
            if (at == std::end(kSizes)) throw r.fail("shield.size", oneOf(kSizes));
            t.shield.size = ShieldSize(1 + (at - std::begin(kSizes)));
            t.shield.negation = perType(r, sh.contains("negation") ? sh["negation"] : json(), "shield.negation", 0, 1, kRequired);
            t.shield.boost = float(r.number(sh, "shield.boost", 0, 1, kRequired, false));
            t.shield.parry = float(r.number(sh, "shield.parry", 0.25, 4, 0, false));
            if (t.shield.parry > 0 && t.shield.size == ShieldSize::Great) throw r.fail("shield.parry", "greatshields can't parry");
        }
        if (r.fields.contains("mods")) {  // what wearing it adds (ADR-022)
            t.mods = readMods(r, r.fields["mods"], "mods");
            if (!t.mods.empty() && t.slot >= EquipCount) throw r.fail("mods", "only items worn in an equip slot have mods");
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
                 "flee_health", "xp", "stance", "poise", "hit_stance", "parryable", "parries", "voice", "voice_pitch", "body", "damage",
                 "resist"});
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
        // A swing holds its windup pose until the hit is due (ADR-035): long enough to see coming.
        t.windup = float(r.number(r.fields, "attack_windup", 0.05, 2, kDefaultWindup, false));
        t.recover = float(r.number(r.fields, "attack_recover", 0, 2, kDefaultRecover, false));
        t.cooldown = float(r.number(r.fields, "attack_cooldown", 0, 10, 0, false));
        t.fleeHealth = float(r.number(r.fields, "flee_health", 0, 0.95, 0, false));
        // Stance and poise (ADR-035): what it takes to break its stance, and the smallest hit that makes it flinch.
        t.stance = float(r.number(r.fields, "stance", 0, 1000, 30, false));
        t.poise = float(r.number(r.fields, "poise", 0, 1000, 0, false));
        t.hitStance = float(r.number(r.fields, "hit_stance", 0, 1000, 15, false));
        if (given("hit_stance") && t.temper == Temper::Harmless) throw r.fail("hit_stance", "harmless creatures don't fight");
        if ((given("stance") || given("poise")) && !t.attackable) throw r.fail(given("stance") ? "stance" : "poise", "only attackable creatures are hit");
        // Parries (ADR-036): whether its attacks can be parried, and how many it takes to open it to a riposte.
        for (const char* key : {"parryable", "parries"})
            if (given(key) && t.temper == Temper::Harmless) throw r.fail(key, "harmless creatures don't fight");
        t.parryable = !r.fields.contains("parryable") || flag("parryable");
        t.parries = int(r.number(r.fields, "parries", 1, 10, 1));
        if (given("parries") && !t.parryable) throw r.fail("parries", "it can't be parried");
        // Character XP for killing one (ADR-027); by default only fighters that can be hurt give any.
        bool gives = t.attackable && t.temper != Temper::Harmless;
        t.xp = int(r.number(r.fields, "xp", 0, 100000, gives ? defaultKillXp(t.level) : 0));
        if (t.xp && !t.attackable) throw r.fail("xp", "a creature that can't be hurt is never killed");
        // Its sounds (ADR-037): a voice (the start of its sound names), its pitch, what its body sounds like hit.
        t.voice = given("voice") ? r.text("voice") : "";
        if (!std::ranges::all_of(t.voice, [](char c) { return (c >= 'a' && c <= 'z') || c == '_'; }))
            throw r.fail("voice", "must be lowercase letters and _ (it names sound files)");
        t.voicePitch = float(r.number(r.fields, "voice_pitch", 0.5, 2, 1, false));
        if (given("voice_pitch") && t.voice.empty()) throw r.fail("voice_pitch", "no 'voice' to pitch");
        std::string body = given("body") ? r.text("body") : "flesh";
        if (body != "flesh" && body != "wood") throw r.fail("body", "must be flesh or wood");
        t.wooden = body == "wood";
        // Damage types (ADR-040): what its own attacks do (with no weapon: teeth, tusks, claws, fists) and
        // what it resists (0.2: takes 20% less) or is weak to (-0.3: 30% more).
        if (given("damage") && t.temper == Temper::Harmless) throw r.fail("damage", "harmless creatures don't fight");
        if (given("damage") && t.weapon) throw r.fail("damage", "its weapon decides (only creatures without one have their own)");
        t.damage = given("damage") ? damageType(r, r.fields["damage"], "damage") : DamageType::Strike;
        if (given("resist") && !t.attackable) throw r.fail("resist", "only attackable creatures are hit");
        t.resist = perType(r, given("resist") ? r.fields["resist"] : json(), "resist", -1, 0.9, 0);
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

// ---- Skills and talents (ADR-027). Both files are optional: without skills.json skills never
// improve (no numbers), without talents.json there are no talents.

void loadSkills(const File* f, Defs& d) {
    for (size_t s = 0; s < size_t(Skill::Count); ++s) d.skills[s].name = kSkillIds[s];
    if (!f) return;
    std::array<bool, size_t(Skill::Count)> seen{};
    for (size_t i = 0; i < f->records.size(); ++i) {
        Record r{*f, i, flatten(*f, i, i)};
        r.allow({"id", "copy_from", "name", "use_mult", "use_offset", "improve_mult", "improve_offset", "per_level"});
        auto id = std::ranges::find(kSkillIds, r.id());
        if (id == std::end(kSkillIds)) throw r.fail("id", "must be one of one_handed, unarmed, armor (code awards their XP)");
        size_t s = size_t(id - std::begin(kSkillIds));
        seen[s] = true;
        SkillDef& k = d.skills[s];
        k.name = r.text("name");
        k.useMult = float(r.number(r.fields, "use_mult", 0, 1000, kRequired, false));
        k.useOffset = float(r.number(r.fields, "use_offset", 0, 1000, 0, false));
        k.improveMult = float(r.number(r.fields, "improve_mult", 0, 10000, kRequired, false));
        k.improveOffset = float(r.number(r.fields, "improve_offset", 1, 1e6, kRequired, false));  // >= 1: a level always costs XP
        if (r.fields.contains("per_level")) k.perLevel = readMods(r, r.fields["per_level"], "per_level");
    }
    for (size_t s = 0; s < seen.size(); ++s)
        if (!seen[s]) throw Bad(std::format("{}: no record for skill '{}' (code awards its XP)", f->path, kSkillIds[s]));
}

void loadTalents(const File* f, Defs& d) {
    if (!f) return;
    if (f->records.size() > 65535) throw Bad(std::format("{}: more than 65535 talents", f->path));
    for (size_t i = 0; i < f->records.size(); ++i) {
        Record r{*f, i, flatten(*f, i, i)};
        r.allow({"id", "copy_from", "name", "tree", "tier", "max_rank", "requires", "per_rank"});
        TalentDef t;
        t.id = r.id(), t.name = r.text("name");
        auto tree = std::ranges::find(kSkillIds, r.fields.contains("tree") && r.fields["tree"].is_string() ? r.fields["tree"].get<std::string>() : "");
        if (tree == std::end(kSkillIds)) throw r.fail("tree", oneOf(kSkillIds));
        t.tree = Skill(tree - std::begin(kSkillIds));
        t.tier = int(r.number(r.fields, "tier", 0, kMaxTier));
        t.maxRank = int(r.number(r.fields, "max_rank", 1, 10));
        if (r.fields.contains("requires")) {
            const json& v = r.fields["requires"];
            auto it = v.is_string() ? f->index.find(v.get<std::string>()) : f->index.end();
            if (it == f->index.end()) throw r.fail("requires", std::format("unknown talent {}", v.dump()));
            t.prerequisite = int(it->second);
        }
        if (!r.fields.contains("per_rank")) throw r.fail("per_rank", "missing (what each rank adds)");
        t.perRank = readMods(r, r.fields["per_rank"], "per_rank");
        if (t.perRank.empty()) throw r.fail("per_rank", "must add something");
        d.talentById.emplace(t.id, uint16_t(i));
        d.talents.push_back(std::move(t));
    }
    for (size_t i = 0; i < d.talents.size(); ++i)  // the prerequisites, now that every talent is read
        if (int pre = d.talents[i].prerequisite; pre >= 0) {
            const TalentDef &t = d.talents[i], &p = d.talents[size_t(pre)];
            if (p.tree != t.tree || p.tier >= t.tier)
                throw Record{*f, i, {}}.fail("requires", std::format("'{}' must be in the same tree and a lower tier", p.id));
        }
}

// ---- Quests (after items and creatures: they name both; ADR-026). Every file in data/quests/.

constexpr size_t kMaxQuestList = 64;  // entries of any one list: stages, objectives, triggers, actions, conditions
constexpr size_t kMaxQuestLines = 256;
constexpr size_t kMaxJournal = 600;   // bytes

// Pass 2 of one quest record. The ids, stage ids and objective ids of every quest are known (pass
// 2a), so references may point at later quests.
struct QuestReader {
    const Record& r;
    const Defs& d;
    uint16_t self;
    const std::vector<std::vector<std::string>>& objectiveIds;  // by quest handle

    template <class... Ks>
    static int keyCount(const json& j, Ks... keys) { return (int(j.contains(keys)) + ...); }
    void only(const json& j, const std::string& field, std::initializer_list<std::string_view> names) const {
        for (const auto& [k, v] : j.items())
            if (std::ranges::find(names, std::string_view(k)) == names.end()) throw r.fail(field + "." + k, "unknown field");
    }
    const json& object(const json& j, const std::string& field) const {
        if (!j.is_object()) throw r.fail(field, "must be an object");
        return j;
    }
    // An array of at most `most` entries; a missing one is empty unless required.
    json list(const json& obj, const char* key, const std::string& field, bool required, size_t most = kMaxQuestList) const {
        if (!obj.contains(key)) {
            if (required) throw r.fail(field, "missing");
            return json::array();
        }
        const json& v = obj[key];
        if (!v.is_array() || v.size() > most) throw r.fail(field, std::format("must be an array of at most {}", most));
        return v;
    }
    // Text of 1..`most` bytes without control, bidi or zero-width characters.
    std::string text(const json& v, const std::string& field, size_t most) const {
        if (!v.is_string() || v.get_ref<const std::string&>().empty() || v.get_ref<const std::string&>().size() > most ||
            !validName(v.get_ref<const std::string&>()))
            throw r.fail(field, std::format("must be 1..{} bytes of text without control, bidi or zero-width characters", most));
        return v.get<std::string>();
    }
    std::string id(const json& v, const std::string& field) const {
        if (!v.is_string() || !snakeCase(v.get<std::string>())) throw r.fail(field, "must be a snake_case id of 1..48 bytes");
        return v.get<std::string>();
    }
    uint16_t quest(const json& v, const std::string& field) const {
        auto it = d.questById.find(id(v, field));
        if (it == d.questById.end()) throw r.fail(field, std::format("unknown quest {}", v.dump()));
        return it->second;
    }
    uint16_t objective(uint16_t q, const json& v, const std::string& field) const {
        const std::vector<std::string>& ids = objectiveIds[q];
        auto it = std::ranges::find(ids, id(v, field));
        if (it == ids.end()) throw r.fail(field, std::format("quest '{}' has no objective {}", d.questIds[q], v.dump()));
        return uint16_t(it - ids.begin());
    }
    ItemId item(const json& v, const std::string& field) const {
        auto it = v.is_string() ? d.itemById.find(v.get<std::string>()) : d.itemById.end();
        if (it == d.itemById.end()) throw r.fail(field, std::format("unknown item {}", v.dump()));
        return it->second;
    }
    Cmp op(const json& c, const std::string& field, Cmp def) const {
        if (!c.contains("op")) return def;
        auto it = std::ranges::find(kCmpNames, c["op"].is_string() ? c["op"].get<std::string>() : "");
        if (it == std::end(kCmpNames)) throw r.fail(field + ".op", oneOf(kCmpNames));
        return Cmp(it - std::begin(kCmpNames));
    }

    Condition condition(const json& c, const std::string& field) const {
        object(c, field);
        static constexpr const char* kKinds[] = {"stage", "done", "failed", "objective", "flag", "has_item", "in_region", "npc_dead"};
        Condition out;
        int found = 0;
        for (size_t k = 0; k < std::size(kKinds); ++k)
            if (c.contains(kKinds[k])) out.kind = Condition::Kind(k), ++found;
        if (found != 1) throw r.fail(field, std::string("needs exactly one of stage, done, failed, objective, flag, has_item, in_region, npc_dead"));
        std::string key = kKinds[out.kind], at = field + "." + key;
        const json& v = c[key];
        bool compares = out.kind == Condition::Stage || out.kind == Condition::Flag || out.kind == Condition::HasItem;
        if (compares) only(c, field, {key, "op", "value", "not"});
        else if (out.kind == Condition::Objective) only(c, field, {key, "is", "not"});
        else only(c, field, {key, "not"});
        if (c.contains("not")) {
            if (!c["not"].is_boolean()) throw r.fail(field + ".not", "must be true or false");
            out.negate = c["not"].get<bool>();
        }
        switch (out.kind) {
        case Condition::Stage:
            out.quest = quest(v, at);
            out.op = op(c, field, Cmp::Eq);
            out.value = int(r.number(c, field + ".value", 0, kMaxStageId));
            break;
        case Condition::Done:
        case Condition::Failed: out.quest = quest(v, at); break;
        case Condition::Objective: {  // "quest_id.objective_id"
            std::string ref = v.is_string() ? v.get<std::string>() : "";
            size_t dot = ref.find('.');
            if (dot == std::string::npos) throw r.fail(at, "must be \"quest_id.objective_id\"");
            out.quest = quest(ref.substr(0, dot), at);
            out.objective = objective(out.quest, ref.substr(dot + 1), at);
            auto is = std::ranges::find(kObjectiveStateNames, c.contains("is") && c["is"].is_string() ? c["is"].get<std::string>() : "");
            if (is == std::end(kObjectiveStateNames)) throw r.fail(field + ".is", oneOf(kObjectiveStateNames));
            out.value = int(is - std::begin(kObjectiveStateNames));
            break;
        }
        case Condition::Flag:
            out.name = id(v, at);
            out.op = op(c, field, Cmp::Eq);
            out.value = int(r.number(c, field + ".value", std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
            break;
        case Condition::HasItem:
            out.item = item(v, at);
            out.op = op(c, field, Cmp::Ge);
            out.value = int(r.number(c, field + ".value", 0, 1000, 1));
            break;
        case Condition::InRegion:
        case Condition::NpcDead: out.name = text(v, at, kMaxMarkerName); break;
        }
        return out;
    }
    std::vector<Condition> conditions(const json& obj, const std::string& field) const {
        std::vector<Condition> out;
        json cs = list(obj, "if", field + ".if", false);
        for (size_t i = 0; i < cs.size(); ++i) out.push_back(condition(cs[i], std::format("{}.if[{}]", field, i)));
        return out;
    }

    Action action(const json& a, const std::string& field, const std::vector<int>& stages) const {
        object(a, field);
        static constexpr const char* kKinds[] = {"show_objective", "complete_objective", "fail_objective", "give_item", "take_item",
                                                 "set_flag", "set_stage", "start_quest", "complete_quest", "fail_quest"};
        Action out;
        int found = 0;
        for (size_t k = 0; k < std::size(kKinds); ++k)
            if (a.contains(kKinds[k])) out.kind = Action::Kind(k), ++found;
        if (found != 1) throw r.fail(field, "needs exactly one of " + oneOf(kKinds).substr(std::string_view("must be one of ").size()));
        std::string key = kKinds[out.kind], at = field + "." + key;
        const json& v = a[key];
        if (out.kind == Action::SetFlag) only(a, field, {key, "value"});
        else only(a, field, {key});
        out.quest = self;
        switch (out.kind) {
        case Action::ShowObjective:
        case Action::CompleteObjective:
        case Action::FailObjective: out.value = objective(self, v, at); break;
        case Action::GiveItem:
        case Action::TakeItem:
            out.item = item(v, at);
            if (out.kind == Action::TakeItem && d.items[out.item].slot == SlotBag) throw r.fail(at, "a bag can't be taken (it may hold items)");
            break;
        case Action::SetFlag:
            out.flag = id(v, at);
            out.value = int(r.number(a, field + ".value", std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
            break;
        case Action::SetStage:
            if (!v.is_number_integer() || std::ranges::find(stages, v.get<int64_t>()) == stages.end() || v.get<int64_t>() == 0)
                throw r.fail(at, std::format("{} is not one of this quest's stages (other than 0)", v.dump()));
            out.value = v.get<int>();
            break;
        case Action::StartQuest:
        case Action::CompleteQuest:
        case Action::FailQuest: out.quest = quest(v, at); break;
        }
        return out;
    }
    std::vector<Action> actions(const json& obj, const char* key, const std::string& field, const std::vector<int>& stages) const {
        std::vector<Action> out;
        json as = list(obj, key, field + "." + key, std::string_view(key) == "do");
        for (size_t i = 0; i < as.size(); ++i) out.push_back(action(as[i], std::format("{}.{}[{}]", field, key, i), stages));
        return out;
    }

    Trigger trigger(const json& t, const std::string& field, const std::vector<int>& stages) const {
        object(t, field);
        static constexpr const char* kKinds[] = {"talked_to", "killed", "killed_creature", "entered", "has_item"};
        Trigger out;
        int found = 0;
        for (size_t k = 0; k < std::size(kKinds); ++k)
            if (t.contains(kKinds[k])) out.kind = Trigger::Kind(k), ++found;
        if (found != 1) throw r.fail(field, "needs exactly one of talked_to, killed, killed_creature, entered, has_item");
        std::string key = kKinds[out.kind], at = field + "." + key;
        const json& v = t[key];
        if (out.kind == Trigger::HasItem) only(t, field, {key, "count", "if", "do"});
        else only(t, field, {key, "if", "do"});
        switch (out.kind) {
        case Trigger::TalkedTo:
        case Trigger::Killed:
        case Trigger::Entered: out.name = text(v, at, kMaxMarkerName); break;
        case Trigger::KilledCreature: {
            auto it = v.is_string() ? d.creatureById.find(v.get<std::string>()) : d.creatureById.end();
            if (it == d.creatureById.end()) throw r.fail(at, std::format("unknown creature {}", v.dump()));
            out.creature = it->second;
            break;
        }
        case Trigger::HasItem:
            out.item = item(v, at);
            out.count = int(r.number(t, field + ".count", 1, 1000, 1));
            break;
        }
        out.conditions = conditions(t, field);
        out.actions = actions(t, "do", field, stages);
        return out;
    }
};

void loadQuests(const std::vector<File>& files, Defs& d) {
    // Pass 2a: every quest's id, stage ids and objective ids.
    std::vector<std::pair<const File*, size_t>> records;
    std::vector<std::vector<int>> stageIds;
    std::vector<std::vector<std::string>> objectiveIds;
    for (const File& f : files)
        for (size_t i = 0; i < f.records.size(); ++i) {
            Record r{f, i, f.records[i].second};
            r.allow({"id", "title", "stages", "objectives", "lines"});
            if (d.questById.contains(r.id())) throw r.fail("id", "another quest file has this id");
            if (records.size() >= 65535) throw r.fail("id", "more than 65535 quests");
            d.questById.emplace(r.id(), uint16_t(records.size()));
            d.questIds.push_back(r.id());
            records.emplace_back(&f, i);
            QuestReader q{r, d, 0, objectiveIds};
            std::vector<int>& stages = stageIds.emplace_back();
            json ss = q.list(r.fields, "stages", "stages", true);
            for (size_t k = 0; k < ss.size(); ++k) {
                std::string field = std::format("stages[{}]", k);
                int id = int(r.number(q.object(ss[k], field), field + ".id", 0, kMaxStageId));
                if (std::ranges::find(stages, id) != stages.end()) throw r.fail(field + ".id", std::format("stage {} is listed twice", id));
                stages.push_back(id);
            }
            if (!std::ranges::any_of(stages, [](int s) { return s > 0; })) throw r.fail("stages", "needs a stage other than 0 (0 is: not started)");
            std::vector<std::string>& objectives = objectiveIds.emplace_back();
            json os = q.list(r.fields, "objectives", "objectives", false);
            for (size_t k = 0; k < os.size(); ++k) {
                std::string field = std::format("objectives[{}]", k);
                q.object(os[k], field);
                std::string id = q.id(os[k].contains("id") ? os[k]["id"] : json(), field + ".id");
                if (std::ranges::find(objectives, id) != objectives.end()) throw r.fail(field + ".id", std::format("objective '{}' is listed twice", id));
                objectives.push_back(id);
            }
        }
    // Pass 2b: everything else, with references resolved to handles.
    for (size_t h = 0; h < records.size(); ++h) {
        Record r{*records[h].first, records[h].second, records[h].first->records[records[h].second].second};
        QuestReader q{r, d, uint16_t(h), objectiveIds};
        QuestDef def{r.id(), r.text("title"), {}, {}};
        json ss = r.fields["stages"];
        for (size_t k = 0; k < ss.size(); ++k) {
            std::string field = std::format("stages[{}]", k);
            const json& st = ss[k];
            q.only(st, field, {"id", "journal", "actions", "on"});
            Stage stage{stageIds[h][k], {}, {}, {}};
            if (stage.id == 0 && (st.contains("journal") || st.contains("actions")))
                throw r.fail(field, "stage 0 (not started) has only triggers: no journal or actions");
            if (st.contains("journal")) stage.journal = q.text(st["journal"], field + ".journal", kMaxJournal);
            stage.actions = q.actions(st, "actions", field, stageIds[h]);
            json on = q.list(st, "on", field + ".on", false);
            for (size_t t = 0; t < on.size(); ++t) stage.on.push_back(q.trigger(on[t], std::format("{}.on[{}]", field, t), stageIds[h]));
            def.stages.push_back(std::move(stage));
        }
        json os = q.list(r.fields, "objectives", "objectives", false);
        for (size_t k = 0; k < os.size(); ++k) {
            std::string field = std::format("objectives[{}]", k);
            q.only(os[k], field, {"id", "text"});
            def.objectives.push_back({objectiveIds[h][k], q.text(os[k].contains("text") ? os[k]["text"] : json(), field + ".text", kMaxMarkerLine)});
        }
        json ls = q.list(r.fields, "lines", "lines", false, kMaxQuestLines);
        for (size_t k = 0; k < ls.size(); ++k) {
            std::string field = std::format("lines[{}]", k);
            q.object(ls[k], field);
            q.only(ls[k], field, {"spawn", "if", "say"});
            QuestLine line{q.text(ls[k].contains("spawn") ? ls[k]["spawn"] : json(), field + ".spawn", kMaxMarkerName),
                           q.text(ls[k].contains("say") ? ls[k]["say"] : json(), field + ".say", kMaxMarkerLine), q.conditions(ls[k], field),
                           uint16_t(h)};
            // The first passing line wins, so one without conditions hides every later line of its spawn.
            if (std::ranges::any_of(d.questLines, [&](const QuestLine& e) { return e.spawn == line.spawn && e.conditions.empty(); }))
                d.warnings.push_back(std::format("{}: record {} '{}': field '{}': never said: an earlier line for '{}' has no conditions",
                                                 r.file.path, r.index, r.id(), field, line.spawn));
            d.questLines.push_back(std::move(line));
        }
        d.quests.push_back(std::move(def));
    }
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
        std::vector<std::filesystem::path> questPaths;  // every file in quests/, in name order (none: no quests)
        if (std::error_code ec; std::filesystem::is_directory(dir / "quests", ec))
            for (const auto& entry : std::filesystem::directory_iterator(dir / "quests"))
                if (entry.path().extension() == ".json") questPaths.push_back(entry.path());
        std::ranges::sort(questPaths);
        std::vector<File> quests;
        for (const auto& path : questPaths) quests.push_back(parseFile(path));
        auto optional = [&](const char* name) {
            std::error_code ec;
            return std::filesystem::exists(dir / name, ec) ? std::optional<File>(parseFile(dir / name)) : std::nullopt;
        };
        std::optional<File> skills = optional("skills.json"), talents = optional("talents.json");
        Defs d;
        if (!skills) d.warnings.push_back((dir / "skills.json").string() + ": missing: skills never improve");
        if (!talents) d.warnings.push_back((dir / "talents.json").string() + ": missing: no talents to learn");
        loadItems(items, d);  // pass 2, in reference order: items, creatures, skills, talents, quests
        loadCreatures(creatures, d);
        loadSkills(skills ? &*skills : nullptr, d);
        loadTalents(talents ? &*talents : nullptr, d);
        loadQuests(quests, d);
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
    // Session::quests is by handle: a quest may be appended or dropped from the end (its state goes), not moved.
    // So are its objectives (QuestState::objectives is by index).
    for (size_t q = 0; q < std::min(d.questIds.size(), gDefs.questIds.size()); ++q) {
        if (d.questIds[q] != gDefs.questIds[q]) {
            err = std::format("quest {} is now '{}', was '{}': add quest files after the others (by name) or restart", q, d.questIds[q],
                              gDefs.questIds[q]);
            return false;
        }
        const auto &now = d.quests[q].objectives, &was = gDefs.quests[q].objectives;
        for (size_t o = 0; o < std::min(now.size(), was.size()); ++o)
            if (now[o].id != was[o].id) {
                err = std::format("quest '{}': objective {} is now '{}', was '{}': add objectives at the end or restart", d.questIds[q], o,
                                  now[o].id, was[o].id);
                return false;
            }
    }
    // Progress::talents is by handle too: talents may be appended or dropped from the end.
    for (size_t t = 0; t < std::min(d.talents.size(), gDefs.talents.size()); ++t)
        if (d.talents[t].id != gDefs.talents[t].id) {
            err = std::format("talent {} is now '{}', was '{}': add talents at the end or restart", t, d.talents[t].id, gDefs.talents[t].id);
            return false;
        }
    setDefs(std::move(d));
    return true;
}
