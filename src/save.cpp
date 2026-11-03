#include "save.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <fstream>
#include <optional>
#include <set>

#include "defs.h"

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
constexpr uint32_t kSaveFileVersion = 1;
constexpr size_t kMaxSaveBytes = size_t(1) << 20;  // the biggest valid one is a few KB
constexpr size_t kNpcBytes = 31;

namespace {

// Little-endian numbers, as in world.cpp's container.
void put(std::vector<uint8_t>& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(uint8_t(v >> (8 * i)));
}
void putF(std::vector<uint8_t>& out, float f) { put(out, std::bit_cast<uint32_t>(f), 4); }

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

// A parsed save, checked against the bounds (unknown ids already dropped), not yet applied.
struct SaveData {
    std::string world;
    uint32_t worldHash = 0, seed = 0;
    double elapsed = 0;
    float timeOfDay = 0;
    std::optional<Player> player;  // pos, yaw, health, deadTime, level, stats
    std::optional<Inventory> inv;  // as saved: containers may not match the bags (applyInventory fixes that)
    std::optional<std::vector<SavedNpc>> npcs;
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
    auto inWorld = [&](glm::vec3 pos) {  // clamped into the world and lifted out of the terrain
        return clearOfTerrain(s.world, glm::clamp(pos, glm::vec3(0.5f, 0, 0.5f), glm::vec3(s.world.size) - glm::vec3(0.5f, 0, 0.5f)));
    };
    if (d.inv) p.inv = fitInventory(*d.inv, r.notes);
    if (d.player) {
        p.stats = d.player->stats;
        p.level = d.player->level;
        p.yaw = d.player->yaw;
        p.teleport(inWorld(d.player->pos));
        p.deadTime = d.player->deadTime;
        p.health = std::min(d.player->health, float(maxHealth(sheet(p))));  // after the gear: it adds health
    }
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
