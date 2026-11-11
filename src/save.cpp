#include "save.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <span>

#include "defs.h"
#include "melee.h"
#include "text.h"

// Save file layout (little-endian), the .flyw container with another magic:
//   "FLYS" | u32 version | sections (char tag[4] | u64 bytes | payload; unknown tags are skipped,
//   missing ones keep startSession's state) | u32 FNV-1a of every byte before it.
// "HEAD" (required): u8 length | world file name | u32 world checksum | u32 world seed |
//   f64 simulated seconds | f32 time of day.
// "PLYR": f32 x, y, z, yaw | f32 health | f32 seconds knocked out | u32 level | u32 stamina,
//   strength, agility, intellect, spirit.
// "INVT": u8 count + item save ids of the equip slots (by EquipSlot), then of the bag bar, then
//   u8 container count, each u8 slot count + item save ids (container 0 is the backpack).
// "NPCS": u32 count, then per NPC: u32 spawn marker id | u8 slot | u8 kind | f32 health |
//   f32 seconds dead | u8 loot item | f32 x, y, z, yaw.
// "QUST" (ADR-026): u32 count, then per quest: u8 length | quest id | u32 stage | u8 bits (1 done,
//   2 failed) | u8 objective count, each u8 length | objective id | u8 ObjectiveState. By string id:
//   reordering or adding quests and objectives in the data doesn't scramble them.
// "FLAG": u32 count, then per flag: u8 length | name | i32 value.
// "PROG" (ADR-027): u32 character XP toward the next level (the level is PLYR's) | u8 skill count,
//   each u8 length | skill id | u32 level | f32 XP toward the next | u16 talent count, each u8 length |
//   talent id | u8 rank. By string id, like QUST.
// "WEAR" (ADR-027): u32 count, then per NPC whose XP wear isn't 0: u32 spawn marker id | u8 slot |
//   f32 the player's hits on it | f32 its hits on the player (Entity::xpWear, armorWear). Apart from
//   NPCS, so that layout stays; an entry without its NPC is dropped silently.
// "REST" (ADR-047): u8 flask charges left | u8 1 if the player has rested, then f32 x, y, z where.
//   Missing (older saves): full flasks, never rested.
// "GRIP" (ADR-048): u8 1 if the player two-hands its weapon. Missing (older saves): one-handed.
// "ARRW" (ADR-050): u8 arrows in the quiver (at most kQuiver). Missing (older saves): a full quiver.
// "THRW" (ADR-053): u8 the readied throwable's item save id (0: none) | u8 count, each u8 item save id | u8 how
//   many in the pouch. One the data doesn't know as a throwable is dropped; counts stop at its pouch. Missing
//   (older saves): a full pouch.
constexpr uint32_t kSaveFileVersion = 1;
constexpr size_t kMaxSaveBytes = size_t(1) << 20;  // the biggest valid one is a few KB
constexpr size_t kNpcBytes = 31;

namespace {

// Little-endian numbers, as in world.cpp's container.
void put(std::vector<uint8_t>& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void putF(std::vector<uint8_t>& out, float f) { put(out, std::bit_cast<uint32_t>(f), 4); }
void putStr(std::vector<uint8_t>& out, const std::string& s) {  // ids and flag names are at most 48 bytes
    size_t n = std::min<size_t>(s.size(), 255);
    put(out, n, 1);
    out.insert(out.end(), s.begin(), s.begin() + long(n));
}

uint64_t get(const uint8_t* p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}

// A section payload: numbers read past its end are 0 and mark it truncated.
struct Reader {
    const uint8_t *p, *end;
    bool truncated = false;
    uint64_t take(int bytes) {
        if (end - p < bytes) return truncated = true, p = end, 0;
        p += bytes;
        return get(p - bytes, bytes);
    }
    float f32() { return std::bit_cast<float>(uint32_t(take(4))); }
    std::string str() {  // u8 length, then the bytes
        uint64_t n = take(1);
        if (uint64_t(end - p) < n) return truncated = true, p = end, std::string();
        p += n;
        return std::string(p - n, p);
    }
    // Why it's malformed (truncated, or bytes left over), or "".
    std::string done(const char* what) const {
        return truncated ? std::format("truncated {}", what) : p != end ? std::format("{} section size mismatch", what) : "";
    }
};

struct SavedNpc {
    uint32_t marker;
    int slot;
    EntityKind kind;
    float health, deadTime;
    ItemId loot;
    glm::vec3 pos;
    float yaw;
};

struct SavedQuest {
    std::string id;
    int stage = 0;
    bool done = false, failed = false;
    std::vector<std::pair<std::string, ObjectiveState>> objectives;
};

// A parsed save, checked against the bounds (unknown ids already dropped), not yet applied.
struct SaveData {
    std::string world;
    uint32_t worldHash = 0, seed = 0;
    double elapsed = 0;
    float timeOfDay = 0;
    std::optional<Player> player;  // pos, yaw, health, deadTime, level, stats
    std::optional<Inventory> inv;  // as saved: containers may not match the bags (applyInventory fixes that)
    std::optional<std::vector<SavedNpc>> npcs;
    std::optional<std::vector<SavedQuest>> quests;
    std::optional<std::map<std::string, int32_t, std::less<>>> flags;
    std::optional<Progress> progress;  // unknown skills and talents already dropped
    std::map<std::pair<uint32_t, int>, std::pair<float, float>> wear;  // (marker id, slot) -> xpWear, armorWear
    std::optional<int> flasks;
    std::optional<glm::vec3> restPoint;
    bool twoHanded = false;
    std::optional<int> arrows;
    std::optional<std::vector<std::pair<int, int>>> pouch;  // (item save id, count), as saved
    int readied = 0;
    std::vector<std::string> notes;  // what parsing dropped (ids this build's data doesn't know)
};

bool finite(std::initializer_list<float> fs) { return std::ranges::all_of(fs, [](float f) { return std::isfinite(f); }); }
bool knownItem(uint64_t id) { return id < defs().items.size(); }

std::string readHead(Reader r, SaveData& d) {
    uint64_t n = r.take(1);
    if (uint64_t(r.end - r.p) < n) return "truncated header";
    d.world.assign(r.p, r.p + n);
    r.p += n;
    d.worldHash = uint32_t(r.take(4));
    d.seed = uint32_t(r.take(4));
    d.elapsed = std::bit_cast<double>(r.take(8));
    d.timeOfDay = r.f32();
    if (!r.truncated && !(std::isfinite(d.elapsed) && d.elapsed >= 0 && d.timeOfDay >= 0 && d.timeOfDay < 1)) return "bad header";
    return r.done("header");
}

std::string readPlayer(Reader r, SaveData& d) {
    Player& p = d.player.emplace();
    p.pos = {r.f32(), r.f32(), r.f32()};
    p.yaw = r.f32();
    p.health = r.f32();
    p.deadTime = r.f32();
    p.level = int(r.take(4));
    int* stats[] = {&p.stats.stamina, &p.stats.strength, &p.stats.agility, &p.stats.intellect, &p.stats.spirit};
    for (int* v : stats) *v = int(std::min<uint64_t>(r.take(4), 1u << 30));
    if (std::string error = r.done("player"); !error.empty()) return error;
    // The ranges data files allow (defs.cpp); positions are clamped into the world when applied.
    if (!finite({p.pos.x, p.pos.y, p.pos.z, p.yaw, p.health, p.deadTime}) || p.health < 0 || p.deadTime < 0 || p.level < 1 ||
        p.level > 100 || p.stats.stamina < 1 || std::ranges::any_of(stats, [](int* v) { return *v > 1000; }))
        return "bad player";
    return {};
}

std::string readInventory(Reader r, SaveData& d) {
    Inventory& inv = d.inv.emplace();
    auto ids = [&](auto& into, uint64_t n) {
        for (uint64_t i = 0; i < n; ++i) into[i] = ItemId(r.take(1));
    };
    uint64_t equip = r.take(1);
    if (equip > EquipCount) return "more equip slots than this build has";
    ids(inv.equipped, equip);
    uint64_t bags = r.take(1);
    if (bags > kBagBarSlots) return "more bags than the bag bar holds";
    ids(inv.bags, bags);
    uint64_t containers = r.take(1);
    if (containers > inv.containers.size()) return "more containers than bags";
    for (uint64_t c = 0; c < containers && !r.truncated; ++c) {
        inv.containers[c].resize(r.take(1));  // u8: at most 255
        ids(inv.containers[c], inv.containers[c].size());
    }
    if (std::string error = r.done("inventory"); !error.empty()) return error;
    auto known = [&](ItemId& id) {  // e.g. a save from a build with more items
        if (knownItem(id)) return;
        d.notes.push_back(std::format("unknown item {}: lost", int(id)));
        id = NoItem;
    };
    std::ranges::for_each(inv.equipped, known);
    std::ranges::for_each(inv.bags, known);
    for (std::vector<ItemId>& c : inv.containers) std::ranges::for_each(c, known);
    return {};
}

std::string readNpcs(Reader r, SaveData& d) {
    uint64_t count = r.take(4);
    if (count > uint64_t(kMaxSpawnedNpcs)) return std::format("more than {} NPCs", kMaxSpawnedNpcs);
    if (uint64_t(r.end - r.p) != count * kNpcBytes) return "NPC section size mismatch";
    std::vector<SavedNpc>& out = d.npcs.emplace();
    std::set<std::pair<uint32_t, int>> keys;
    for (uint64_t i = 0; i < count; ++i) {
        SavedNpc n;
        n.marker = uint32_t(r.take(4));
        n.slot = int(r.take(1));
        uint64_t kind = r.take(1);
        n.kind = EntityKind(kind);
        n.health = r.f32();
        n.deadTime = r.f32();
        uint64_t loot = r.take(1);
        n.loot = ItemId(loot);
        n.pos = {r.f32(), r.f32(), r.f32()};
        n.yaw = r.f32();
        if (n.marker == 0 || n.slot >= kMaxSpawnCount || !finite({n.health, n.deadTime, n.pos.x, n.pos.y, n.pos.z, n.yaw}) ||
            n.health < 0 || n.deadTime < 0 || !keys.insert({n.marker, n.slot}).second)
            return std::format("bad NPC (marker {} slot {})", n.marker, n.slot);
        if (kind >= defs().creatures.size()) {  // a kind this build's data doesn't know: no NPC to give it to
            d.notes.push_back(std::format("dropped NPC {} of marker {}: unknown kind {}", n.slot, n.marker, kind));
            continue;
        }
        if (!knownItem(loot)) {
            d.notes.push_back(std::format("unknown loot item {} on NPC {} of marker {}: lost", loot, n.slot, n.marker));
            n.loot = NoItem;
        }
        out.push_back(n);
    }
    return r.done("NPC");
}

std::string readQuests(Reader r, SaveData& d) {
    uint64_t count = r.take(4);
    if (count > 65535) return "more than 65535 quests";
    std::vector<SavedQuest>& out = d.quests.emplace();
    std::set<std::string> ids;
    for (uint64_t i = 0; i < count && !r.truncated; ++i) {
        SavedQuest q;
        q.id = r.str();
        uint64_t stage = r.take(4), bits = r.take(1), objectives = r.take(1);
        q.done = bits & 1, q.failed = bits & 2;
        q.stage = int(std::min<uint64_t>(stage, kMaxStageId + 1));
        std::set<std::string> objectiveIds;
        bool bad = q.id.empty() || stage > uint64_t(kMaxStageId) || bits > 2 || !ids.insert(q.id).second;  // done or failed, not both
        for (uint64_t k = 0; k < objectives && !r.truncated; ++k) {
            std::string id = r.str();
            uint64_t state = r.take(1);
            bad |= id.empty() || state >= uint64_t(ObjectiveState::Count) || !objectiveIds.insert(id).second;
            q.objectives.emplace_back(id, ObjectiveState(state));
        }
        if (bad && !r.truncated) return std::format("bad quest '{}'", q.id);
        out.push_back(std::move(q));
    }
    return r.done("quest");
}

std::string readFlags(Reader r, SaveData& d) {
    uint64_t count = r.take(4);
    if (count > kMaxFlags) return std::format("more than {} flags", kMaxFlags);
    auto& out = d.flags.emplace();
    for (uint64_t i = 0; i < count && !r.truncated; ++i) {
        std::string name = r.str();
        auto value = int32_t(uint32_t(r.take(4)));
        if (r.truncated) break;
        if (name.empty() || name.size() > 48 || !validName(name) || !out.emplace(name, value).second) return "bad flag";
    }
    return r.done("flag");
}

std::string readProgress(Reader r, SaveData& d) {
    Progress& p = d.progress.emplace();
    p.xp = int(std::min<uint64_t>(r.take(4), 1u << 30));
    std::set<std::string> ids;
    bool bad = false;
    for (uint64_t i = 0, n = r.take(1); i < n && !r.truncated; ++i) {
        std::string id = r.str();
        uint64_t level = r.take(4);
        float xp = r.f32();
        bad |= id.empty() || !ids.insert(id).second || level < kMinSkill || level > kMaxSkill || !finite({xp}) || xp < 0 || xp > 1e9f;
        auto s = std::ranges::find(kSkillIds, id);
        if (s == std::end(kSkillIds)) d.notes.push_back(std::format("dropped skill '{}': this build has no such skill", id));
        else p.skills[size_t(s - std::begin(kSkillIds))] = {int(level), xp};
    }
    ids.clear();
    for (uint64_t i = 0, n = r.take(2); i < n && !r.truncated; ++i) {
        std::string id = r.str();
        uint64_t rank = r.take(1);
        bad |= id.empty() || !ids.insert(id).second || rank == 0;
        auto it = defs().talentById.find(id);
        if (it == defs().talentById.end()) {
            d.notes.push_back(std::format("dropped talent '{}': the data has no such talent (its point is free again)", id));
            continue;
        }
        int most = defs().talents[it->second].maxRank;
        if (int(rank) > most) d.notes.push_back(std::format("talent '{}': rank {} is above its highest now, {}: lowered", id, rank, most));
        if (p.talents.size() <= it->second) p.talents.resize(it->second + 1u);
        p.talents[it->second] = uint8_t(std::min(int(rank), most));
    }
    if (bad && !r.truncated) return "bad progress";
    return r.done("progress");
}

std::string readWear(Reader r, SaveData& d) {
    uint64_t count = r.take(4);
    if (count > uint64_t(kMaxSpawnedNpcs)) return std::format("more than {} worn NPCs", kMaxSpawnedNpcs);
    if (uint64_t(r.end - r.p) != count * 13) return "wear section size mismatch";
    for (uint64_t i = 0; i < count; ++i) {
        uint32_t marker = uint32_t(r.take(4));
        int slot = int(r.take(1));
        float hits = r.f32(), dealt = r.f32();
        if (marker == 0 || slot >= kMaxSpawnCount || !finite({hits, dealt}) || hits < 0 || dealt < 0 || hits > 1e6f || dealt > 1e6f ||
            !d.wear.emplace(std::pair{marker, slot}, std::pair{hits, dealt}).second)
            return std::format("bad wear (marker {} slot {})", marker, slot);
    }
    return r.done("wear");
}

std::string readRest(Reader r, SaveData& d) {
    d.flasks = int(r.take(1));
    uint64_t rested = r.take(1);
    if (rested > 1) return "bad rest point";
    if (rested) {
        glm::vec3 at{r.f32(), r.f32(), r.f32()};
        if (!finite({at.x, at.y, at.z}) || std::abs(at.x) > 1e7f || std::abs(at.y) > 1e7f || std::abs(at.z) > 1e7f) return "bad rest point";
        d.restPoint = at;
    }
    return r.done("rest");
}

std::string readGrip(Reader r, SaveData& d) {
    uint64_t two = r.take(1);
    if (two > 1) return "bad grip";
    d.twoHanded = two;
    return r.done("grip");
}

std::string readArrows(Reader r, SaveData& d) {
    uint64_t n = r.take(1);
    if (n > uint64_t(kQuiver)) return "bad arrow count";
    d.arrows = int(n);
    return r.done("arrows");
}

std::string readPouch(Reader r, SaveData& d) {
    d.readied = int(r.take(1));
    uint64_t n = r.take(1);
    d.pouch.emplace();
    for (uint64_t i = 0; i < n; ++i) {
        int id = int(r.take(1)), count = int(r.take(1));
        d.pouch->emplace_back(id, count);
    }
    return r.done("pouch");
}

std::string parse(const char* path, SaveData& d) {
    std::error_code ec;
    uintmax_t fileBytes = std::filesystem::file_size(path, ec);
    std::ifstream f(path, std::ios::binary);
    if (ec || !f) return "cannot open the file";
    if (fileBytes > kMaxSaveBytes) return "too big for a save";
    std::vector<uint8_t> data(fileBytes);
    if (!f.read(reinterpret_cast<char*>(data.data()), std::streamsize(fileBytes))) return "cannot read the file";
    if (data.size() < 12 || std::memcmp(data.data(), "FLYS", 4) != 0) return "not a flycoos save";
    const uint8_t *p = data.data() + 8, *end = data.data() + data.size() - 4;
    if (fnv1a(data.data(), data.size() - 4) != get(end, 4)) return "corrupt or truncated (checksum mismatch)";
    if (uint32_t version = uint32_t(get(data.data() + 4, 4)); version != kSaveFileVersion)
        return std::format("unsupported version {} (this build reads {})", version, kSaveFileVersion);
    bool head = false;
    std::set<std::string> seen;
    while (p < end) {
        if (end - p < 12) return "truncated section header";
        std::string tag(p, p + 4);
        uint64_t bytes = get(p + 4, 8);
        p += 12;
        if (bytes > uint64_t(end - p)) return "truncated section";
        Reader r{p, p + bytes};
        p += bytes;
        std::string error;
        if (tag == "HEAD") error = readHead(r, d), head = true;
        else if (tag == "PLYR") error = readPlayer(r, d);
        else if (tag == "INVT") error = readInventory(r, d);
        else if (tag == "NPCS") error = readNpcs(r, d);
        else if (tag == "QUST") error = readQuests(r, d);
        else if (tag == "FLAG") error = readFlags(r, d);
        else if (tag == "PROG") error = readProgress(r, d);
        else if (tag == "WEAR") error = readWear(r, d);
        else if (tag == "REST") error = readRest(r, d);
        else if (tag == "GRIP") error = readGrip(r, d);
        else if (tag == "ARRW") error = readArrows(r, d);
        else if (tag == "THRW") error = readPouch(r, d);
        else continue;  // a later build's section
        if (!seen.insert(tag).second) return std::format("duplicate {} section", tag);
        if (!error.empty()) return error;
    }
    return head ? std::string() : "no header section";
}

// The saved inventory, fitted to the current data: an item no longer fit for its equip or bag slot,
// and one in a bag slot past the bag's (new) size, goes into the first free slot, else is lost.
Inventory fitInventory(const Inventory& saved, std::vector<std::string>& notes) {
    Inventory inv;
    std::vector<ItemId> loose;
    for (size_t i = 0; i < saved.equipped.size(); ++i)
        if (ItemId id = saved.equipped[i]; id && itemType(id).slot == EquipSlot(i)) inv.equipped[i] = id;
        else if (id) loose.push_back(id);
    for (size_t i = 0; i < saved.bags.size(); ++i)
        if (ItemId id = saved.bags[i]; id && itemType(id).slot == SlotBag) {
            inv.bags[i] = id;
            inv.containers[i + 1].assign(size_t(itemType(id).bagSlots), NoItem);
        } else if (id) loose.push_back(id);
    for (size_t c = 0; c < saved.containers.size(); ++c)
        for (size_t k = 0; k < saved.containers[c].size(); ++k)
            if (ItemId id = saved.containers[c][k]; id && k < inv.containers[c].size()) inv.containers[c][k] = id;
            else if (id) loose.push_back(id);
    for (ItemId id : loose)
        notes.push_back(giveItem(inv, id) ? std::format("moved {} into a free slot", itemType(id).name)
                                          : std::format("no room for {}: lost", itemType(id).name));
    return inv;
}

void apply(const SaveData& d, Session& s, uint32_t worldHash, SaveReport& r) {
    r.world = d.world;
    r.notes.insert(r.notes.end(), d.notes.begin(), d.notes.end());
    r.worldChanged = d.worldHash != worldHash || d.seed != s.seed;
    if (r.worldChanged)
        r.notes.push_back(std::format("the world changed since this save (checksum {:08x} seed {}, now {:08x} seed {}): restored what still matches",
                                      d.worldHash, d.seed, worldHash, s.seed));
    PlayerTuning tuning = s.player.tuning;  // the editor's flight stays on
    s.timeOfDay = d.timeOfDay;  // before spawning: markers may spawn by the time of day
    startSession(s);
    s.player.tuning = tuning;
    s.elapsed = s.regionSince = d.elapsed;
    Player& p = s.player;
    auto inWorld = [&](glm::vec3 pos) {  // clamped into the world and lifted out of the terrain (the endless world has no edge)
        if (s.world.streaming()) return pos;
        return clearOfTerrain(s.world, glm::clamp(pos, glm::vec3(s.world.lo) + glm::vec3(0.5f, 0, 0.5f),
                                                  glm::vec3(s.world.hi() + 1) - glm::vec3(0.5f, 0, 0.5f)));
    };
    if (d.inv) p.inv = fitInventory(*d.inv, r.notes);
    if (d.player) {
        p.stats = d.player->stats;
        p.level = d.player->level;
        p.yaw = d.player->yaw;
        p.teleport(inWorld(d.player->pos));
        p.deadTime = d.player->deadTime;
    }
    if (d.progress) {
        Progress& g = p.progress = *d.progress;
        // XP past the next level (the costs in the data may have dropped): just short of it.
        if (int next = levelXpToNext(p.level); p.level >= kMaxLevel ? g.xp > 0 : g.xp >= next) {
            r.notes.push_back(std::format("character XP {} is past level {}'s {}: lowered", g.xp, p.level, next));
            g.xp = p.level >= kMaxLevel ? 0 : next - 1;
        }
        for (size_t k = 0; k < g.skills.size(); ++k) {
            const SkillDef& sd = skillDef(Skill(k));
            SkillLevel& sk = g.skills[k];
            float next = sk.level >= kMaxSkill ? 0 : skillXpToNext(sd.improveMult, sd.improveOffset, sk.level);
            if (sk.level >= kMaxSkill ? sk.xp > 0 : sk.xp >= next) {
                r.notes.push_back(std::format("skill '{}': XP {:.0f} is past level {}'s {:.0f}: lowered", kSkillIds[k], sk.xp, sk.level, next));
                sk.xp = sk.level >= kMaxSkill ? 0 : std::nextafter(next, 0.0f);
            }
        }
        // The ranks must still follow the rules (points, tiers, prerequisites): else they're free again.
        if (int spent = pointsSpent(g); !replayTalents(g, p.level))
            r.notes.push_back(std::format("{} talent points no longer fit the talents' rules at level {}: talents reset", spent, p.level));
    }
    p.twoHanded = d.twoHanded;  // (without a weapon in hand updateMelee lets go)
    p.gripBlend = twoHanding(p) ? 1 : 0;  // already in both hands, not taking hold
    if (d.player) p.health = std::min(d.player->health, float(maxHealth(sheet(p))));  // after the gear and talents: they add health
    p.flasks = d.flasks ? std::min(*d.flasks, maxFlasks(sheet(p))) : maxFlasks(sheet(p));  // as health: what gear and talents allow
    p.arrows = d.arrows.value_or(kQuiver);
    p.pouch = {}, p.readied = NoItem;  // (ADR-053)
    if (d.pouch) {
        for (auto [id, count] : *d.pouch)
            if (knownItem(uint64_t(id)) && throwable(itemType(ItemId(id)))) p.pouch[size_t(id)] = uint8_t(std::min(count, itemType(ItemId(id)).thrown.pouch));
            else r.notes.push_back(std::format("dropped {} of item {} from the pouch: the data has no such throwable", count, id));
        if (knownItem(uint64_t(d.readied)) && throwable(itemType(ItemId(d.readied)))) p.readied = ItemId(d.readied);
        else cycleThrowable(p);  // the first kind there is
    } else {
        fillPouch(p);
    }
    if (d.restPoint) s.restPoint = inWorld(*d.restPoint);
    if (d.flags) s.flags = *d.flags;
    for (const SavedQuest& q : d.quests ? *d.quests : std::span<const SavedQuest>{}) {
        const Defs& defined = defs();
        auto it = defined.questById.find(q.id);
        if (it == defined.questById.end()) {  // a quest the data no longer has (or a later build's)
            r.notes.push_back(std::format("dropped quest '{}': the data has no such quest", q.id));
            continue;
        }
        const QuestDef& def = defined.quests[it->second];
        QuestState& st = s.quests[it->second];
        st.stage = q.stage, st.done = q.done, st.failed = q.failed;
        if (!findStage(def, q.stage))
            r.notes.push_back(std::format("quest '{}': stage {} is gone from the data: kept (the debug window's Quests tab can set another)", q.id, q.stage));
        for (const auto& [id, state] : q.objectives) {
            auto o = std::ranges::find(def.objectives, id, &Objective::id);
            if (o == def.objectives.end()) r.notes.push_back(std::format("quest '{}': dropped objective '{}': the data has no such objective", q.id, id));
            else st.objectives[size_t(o - def.objectives.begin())] = state;
        }
    }
    for (Entity& e : s.entities)  // by the same key as NPCS, but on its own
        if (auto w = d.wear.find({e.spawn.id, e.slot}); e.spawn.id && w != d.wear.end()) e.xpWear = w->second.first, e.armorWear = w->second.second;
    if (!d.npcs) return;
    for (const SavedNpc& n : *d.npcs) {
        auto e = std::ranges::find_if(s.entities, [&](const Entity& e) { return e.spawn.id == n.marker && e.slot == n.slot; });
        if (e == s.entities.end() || e->kind != n.kind) {
            ++r.droppedNpcs;
            auto m = std::ranges::find_if(s.markers, [&](const Marker& m) { return m.id == n.marker && m.kind == MarkerKind::Spawn; });
            const char* why = m == s.markers.end()                ? "the marker is gone"
                              : e != s.entities.end()              ? "it spawns another kind now"
                              : !activeAt(*m, s.timeOfDay)         ? "its marker is out of its active window at this time of day"
                                                                   : "it spawns fewer now";
            r.notes.push_back(std::format("dropped NPC {} of marker {} ({}): {}", n.slot, n.marker, defs().creatureIds[size_t(n.kind)], why));
            continue;
        }
        // Where it was, unless its home moved away since (the marker did): then it starts at home.
        e->body.yaw = n.yaw;
        float range = std::max({e->wanderRadius, e->type().leashRadius, 8.0f});
        e->body.teleport(glm::distance(glm::vec2(n.pos.x, n.pos.z), glm::vec2(e->home.x, e->home.z)) <= range
                             ? inWorld(n.pos) : e->body.pos);
        e->body.health = std::min(n.health, float(maxHealth(sheet(e->body))));
        e->body.deadTime = n.deadTime;
        e->loot = n.loot;
        if (!e->body.alive()) setState(*e, AiState::Dead, "loaded");  // alive ones start calm (the FSM isn't saved)
        ++r.npcs;
    }
}

}  // namespace

uint32_t worldFileChecksum(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    uint8_t b[4];
    if (!f || f.tellg() < 4 || !f.seekg(-4, std::ios::end) || !f.read(reinterpret_cast<char*>(b), 4)) return 0;
    return uint32_t(get(b, 4));
}

std::string savePlayer(const char* path, const Session& s, const std::string& worldName, uint32_t worldHash) {
    std::vector<uint8_t> out{'F', 'L', 'Y', 'S'};
    put(out, kSaveFileVersion, 4);
    auto section = [&](const char* tag, auto&& payload) {
        out.insert(out.end(), tag, tag + 4);
        size_t at = out.size();
        put(out, 0, 8);
        payload();
        uint64_t bytes = out.size() - at - 8;
        for (int i = 0; i < 8; ++i) out[at + size_t(i)] = uint8_t(bytes >> (8 * i));
    };
    section("HEAD", [&] {
        size_t n = std::min<size_t>(worldName.size(), 255);
        put(out, n, 1);
        out.insert(out.end(), worldName.begin(), worldName.begin() + long(n));
        put(out, worldHash, 4);
        put(out, s.seed, 4);
        put(out, std::bit_cast<uint64_t>(s.elapsed), 8);
        putF(out, s.timeOfDay);
    });
    const Player& p = s.player;
    section("PLYR", [&] {
        for (float f : {p.pos.x, p.pos.y, p.pos.z, p.yaw, p.health, p.deadTime}) putF(out, f);
        for (int v : {p.level, p.stats.stamina, p.stats.strength, p.stats.agility, p.stats.intellect, p.stats.spirit}) put(out, uint32_t(v), 4);
    });
    section("INVT", [&] {
        put(out, p.inv.equipped.size(), 1);
        out.insert(out.end(), p.inv.equipped.begin(), p.inv.equipped.end());
        put(out, p.inv.bags.size(), 1);
        out.insert(out.end(), p.inv.bags.begin(), p.inv.bags.end());
        put(out, p.inv.containers.size(), 1);
        for (const std::vector<ItemId>& c : p.inv.containers) {
            put(out, c.size(), 1);
            out.insert(out.end(), c.begin(), c.end());
        }
    });
    section("NPCS", [&] {
        size_t count = size_t(std::ranges::count_if(s.entities, [](const Entity& e) { return e.spawn.id != 0; }));
        put(out, count, 4);
        for (const Entity& e : s.entities) {
            if (!e.spawn.id) continue;  // not from a marker: nothing to key it by
            put(out, e.spawn.id, 4);
            put(out, uint32_t(e.slot), 1);
            put(out, uint8_t(e.kind), 1);
            putF(out, e.body.health);
            putF(out, e.body.deadTime);
            put(out, e.loot, 1);
            for (float f : {e.body.pos.x, e.body.pos.y, e.body.pos.z, e.body.yaw}) putF(out, f);
        }
    });
    section("QUST", [&] {
        const Defs& d = defs();
        size_t count = std::min(s.quests.size(), d.quests.size());  // equal, unless the defs were reloaded since the last tick
        put(out, count, 4);
        for (size_t q = 0; q < count; ++q) {
            const QuestState& st = s.quests[q];
            putStr(out, d.questIds[q]);
            put(out, uint32_t(st.stage), 4);
            put(out, (st.done ? 1 : 0) | (st.failed ? 2 : 0), 1);
            size_t n = std::min(st.objectives.size(), d.quests[q].objectives.size());
            put(out, n, 1);
            for (size_t o = 0; o < n; ++o) {
                putStr(out, d.quests[q].objectives[o].id);
                put(out, uint8_t(st.objectives[o]), 1);
            }
        }
    });
    section("FLAG", [&] {
        put(out, s.flags.size(), 4);
        for (const auto& [name, value] : s.flags) {
            putStr(out, name);
            put(out, uint32_t(value), 4);
        }
    });
    section("WEAR", [&] {
        auto worn = [](const Entity& e) { return e.spawn.id && (e.xpWear > 0 || e.armorWear > 0); };
        put(out, size_t(std::ranges::count_if(s.entities, worn)), 4);
        for (const Entity& e : s.entities)
            if (worn(e)) {
                put(out, e.spawn.id, 4);
                put(out, uint32_t(e.slot), 1);
                putF(out, e.xpWear);
                putF(out, e.armorWear);
            }
    });
    section("REST", [&] {
        put(out, uint32_t(std::clamp(p.flasks, 0, 255)), 1);
        put(out, s.restPoint ? 1 : 0, 1);
        if (s.restPoint)
            for (float f : {s.restPoint->x, s.restPoint->y, s.restPoint->z}) putF(out, f);
    });
    section("GRIP", [&] { put(out, p.twoHanded ? 1 : 0, 1); });
    section("ARRW", [&] { put(out, uint32_t(std::clamp(p.arrows, 0, kQuiver)), 1); });
    section("THRW", [&] {
        std::vector<size_t> carried;
        for (size_t i = 0; i < defs().items.size() && i < p.pouch.size(); ++i)
            if (throwable(defs().items[i])) carried.push_back(i);
        put(out, p.readied, 1);
        put(out, carried.size(), 1);
        for (size_t i : carried) put(out, i, 1), put(out, p.pouch[i], 1);
    });
    section("PROG", [&] {
        const Progress& g = p.progress;
        put(out, uint32_t(g.xp), 4);
        put(out, g.skills.size(), 1);
        for (size_t k = 0; k < g.skills.size(); ++k) {
            putStr(out, kSkillIds[k]);
            put(out, uint32_t(g.skills[k].level), 4);
            putF(out, g.skills[k].xp);
        }
        const std::vector<TalentDef>& ts = defs().talents;
        size_t n = std::min(g.talents.size(), ts.size());  // equal, unless the defs were reloaded with fewer
        put(out, size_t(std::ranges::count_if(g.talents.begin(), g.talents.begin() + long(n), [](uint8_t r) { return r > 0; })), 2);
        for (size_t t = 0; t < n; ++t)
            if (g.talents[t]) {
                putStr(out, ts[t].id);
                put(out, g.talents[t], 1);
            }
    });
    put(out, fnv1a(out.data(), out.size()), 4);
    return replaceFile(path, out, true);
}

std::string loadPlayer(const char* path, Session& s, uint32_t worldHash, SaveReport& report) {
    SaveData d;
    std::string error = parse(path, d);
    bool fromBackup = false;
    if (!error.empty()) {
        std::string bak = std::string(path) + ".bak";
        SaveData b;
        if (std::error_code ec; !std::filesystem::exists(bak, ec) || !parse(bak.c_str(), b).empty()) return error;
        d = std::move(b);
        fromBackup = true;
    }
    report = {};
    report.fromBackup = fromBackup;
    if (fromBackup) report.notes.push_back(std::format("the save didn't load ({}): loaded its backup", error));
    apply(d, s, worldHash, report);
    return {};
}
