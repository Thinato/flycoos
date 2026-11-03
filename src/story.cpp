#include "story.h"

#include <algorithm>
#include <bit>
#include <cstring>
#include <format>
#include <string_view>

#include "defs.h"
#include "text.h"

const char* checkMarker(const Grid& g, const Marker& m) {
    if (m.kind >= MarkerKind::Count) return "unknown marker kind";
    if (size_t(m.entity) >= defs().creatures.size()) return "unknown entity kind";
    if (m.count < 1 || m.count > kMaxSpawnCount) return "count out of range (1..8)";
    if (m.name.size() > kMaxMarkerName) return "name longer than 48 bytes";
    if (!validName(m.name)) return "name must be UTF-8 text without control, bidi or zero-width characters";
    if (m.name.empty() && (m.kind == MarkerKind::Poi || m.kind == MarkerKind::Region)) return "a poi or region needs a name";
    if (!(m.pos.x >= 0 && m.pos.x < float(g.size.x) && m.pos.z >= 0 && m.pos.z < float(g.size.z) && m.pos.y >= 0 &&
          m.pos.y <= float(g.size.y)))
        return "position outside the world";  // NaN fails every comparison
    if (!(m.radius >= 0 && m.radius <= kMaxMarkerRadius)) return "radius out of range (0..512)";
    if (!(m.activeFrom >= 0 && m.activeFrom < 1 && m.activeTo >= 0 && m.activeTo < 1))
        return "active times must be times of day in 0..1 (0 midnight, 0.5 noon)";
    if (m.activeFrom == m.activeTo && m.activeFrom != 0) return "an active window needs two different times (none: always)";
    if (m.activeFrom != m.activeTo && m.kind != MarkerKind::Spawn) return "an active window is for spawn markers";
    if (m.lines.empty()) return nullptr;
    if (!takesLines(m)) return "lines are for spawn markers, not the dummy: clear them first (lines: [])";
    if (m.lines.size() > kMaxMarkerLines) return "more than 8 lines";
    for (const std::string& line : m.lines) {
        if (line.empty() || line.size() > kMaxMarkerLine) return "each line needs 1..160 bytes";
        if (!validName(line)) return "lines must be UTF-8 text without control, bidi or zero-width characters";
    }
    return nullptr;
}

const Marker* regionAt(const std::vector<Marker>& markers, glm::vec3 p) {
    const Marker* best = nullptr;
    for (const Marker& m : markers)
        if (m.kind == MarkerKind::Region && glm::distance(glm::vec2(p.x, p.z), glm::vec2(m.pos.x, m.pos.z)) <= m.radius &&
            (!best || m.radius < best->radius))
            best = &m;
    return best;
}

// Little-endian numbers, as in world.cpp's container.
static void put(std::vector<uint8_t>& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(uint8_t(v >> (8 * i)));
}

static uint64_t get(const uint8_t* p, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}

// "MARK" section (always): u32 count, then per marker: u32 id | u8 kind | u8 name length | name |
//   f32 x, y, z, radius | u8 entity | u8 count.
// "LINE" section (only when a marker has lines; after MARK): u32 count, then per marker with lines,
//   by ascending id: u32 id | u8 line count | lines (u8 length | text). Older builds skip it.
// "OWNR" section (only when a prefab instance owns a marker; after MARK): u32 count, then per owned
//   marker, by ascending id: u32 marker id | u32 instance id (live instances only; never the player_spawn).
//   Older builds skip it (the markers stay, unowned).
// "MKID" section (only when a removed marker had a higher id than every live one): u32 the highest
//   marker id ever used, so new markers never reuse a removed one's id. Older builds skip it.
// "TIME" section (only when a spawn marker has an active window; after MARK): u32 count, then per such
//   marker, by ascending id: u32 id | f32 activeFrom, activeTo. Older builds skip it (always active).
std::string saveWorld(const char* path, const Grid& g, uint32_t seed, const std::vector<Instance>& instances,
                      const std::vector<Marker>& markers, uint32_t markerIds) {
    std::vector<Section> sections;
    // always: an empty one means "no markers", a missing one "older file"
    std::vector<uint8_t>& out = sections.emplace_back(Section{{'M', 'A', 'R', 'K'}, {}}).bytes;
    put(out, markers.size(), 4);
    for (const Marker& m : markers) {
        put(out, m.id, 4);
        put(out, uint8_t(m.kind), 1);
        put(out, m.name.size(), 1);
        out.insert(out.end(), m.name.begin(), m.name.end());
        for (float f : {m.pos.x, m.pos.y, m.pos.z, m.radius}) put(out, std::bit_cast<uint32_t>(f), 4);
        put(out, uint8_t(m.entity), 1);
        put(out, uint32_t(m.count), 1);
    }
    if (size_t talking = size_t(std::ranges::count_if(markers, [](const Marker& m) { return !m.lines.empty(); }))) {
        std::vector<uint8_t>& lines = sections.emplace_back(Section{{'L', 'I', 'N', 'E'}, {}}).bytes;
        put(lines, talking, 4);
        for (const Marker& m : markers) {
            if (m.lines.empty()) continue;
            put(lines, m.id, 4);
            put(lines, m.lines.size(), 1);
            for (const std::string& line : m.lines) {
                put(lines, line.size(), 1);
                lines.insert(lines.end(), line.begin(), line.end());
            }
        }
    }
    auto owned = [&](const Marker& m) {  // by a live instance (else the marker is saved as hand-placed)
        return m.owner && std::ranges::binary_search(instances, m.owner, {}, &Instance::id);
    };
    if (size_t count = size_t(std::ranges::count_if(markers, owned))) {
        std::vector<uint8_t>& owners = sections.emplace_back(Section{{'O', 'W', 'N', 'R'}, {}}).bytes;
        put(owners, count, 4);
        for (const Marker& m : markers)
            if (owned(m)) put(owners, m.id, 4), put(owners, m.owner, 4);
    }
    if (markerIds > (markers.empty() ? 0 : markers.back().id)) put(sections.emplace_back(Section{{'M', 'K', 'I', 'D'}, {}}).bytes, markerIds, 4);
    auto timed = [](const Marker& m) { return m.activeFrom != m.activeTo; };
    if (size_t count = size_t(std::ranges::count_if(markers, timed))) {
        std::vector<uint8_t>& times = sections.emplace_back(Section{{'T', 'I', 'M', 'E'}, {}}).bytes;
        put(times, count, 4);
        for (const Marker& m : markers)
            if (timed(m)) put(times, m.id, 4), put(times, std::bit_cast<uint32_t>(m.activeFrom), 4), put(times, std::bit_cast<uint32_t>(m.activeTo), 4);
    }
    return writeWorldFile(path, g, seed, instances, sections);
}

// Reads the MARK payload [p, end) into `out`; returns why it's malformed, or "".
static std::string readMarkers(const uint8_t* p, const uint8_t* end, const Grid& g, std::vector<Marker>& out) {
    bool truncated = false;
    auto take = [&](int64_t bytes) -> uint64_t {  // a little-endian number, or 0 past the end
        if (end - p < bytes) return truncated = true, p = end, 0;
        p += bytes;
        return get(p - bytes, int(bytes));
    };
    uint64_t count = take(4);
    int npcs = 0;
    if (count > kMaxMarkers) return std::format("more than {} markers", kMaxMarkers);
    for (uint64_t i = 0; i < count && !truncated; ++i) {
        Marker m;
        m.id = uint32_t(take(4));
        m.kind = MarkerKind(take(1));
        uint64_t nameBytes = take(1);
        if (end - p < int64_t(nameBytes)) return "truncated markers";
        m.name.assign(p, p + nameBytes);
        p += nameBytes;
        float f[4];
        for (float& v : f) v = std::bit_cast<float>(uint32_t(take(4)));
        m.pos = {f[0], f[1], f[2]};
        m.radius = f[3];
        m.entity = EntityKind(take(1));
        m.count = int(take(1));
        if (truncated) break;
        bool secondPlayerSpawn = m.kind == MarkerKind::PlayerSpawn && std::ranges::find(out, MarkerKind::PlayerSpawn, &Marker::kind) != out.end();
        if (const char* why = checkMarker(g, m)) return std::format("bad marker {}: {}", m.id, why);
        if (m.id == 0 || m.id == UINT32_MAX || (!out.empty() && m.id <= out.back().id) || secondPlayerSpawn)
            return std::format("bad marker {}", m.id);
        if (m.kind == MarkerKind::Spawn && (npcs += m.count) > kMaxSpawnedNpcs)
            return std::format("spawn markers make more than {} NPCs", kMaxSpawnedNpcs);
        out.push_back(std::move(m));
    }
    if (truncated) return "truncated markers";
    return p == end ? std::string() : "marker section size mismatch";
}

// Reads the LINE payload [p, end) onto the already loaded `markers`; returns why it's malformed, or "".
static std::string readLines(const uint8_t* p, const uint8_t* end, const Grid& g, std::vector<Marker>& markers) {
    bool truncated = false;
    auto take = [&](int64_t bytes) -> uint64_t {  // a little-endian number, or 0 past the end
        if (end - p < bytes) return truncated = true, p = end, 0;
        p += bytes;
        return get(p - bytes, int(bytes));
    };
    uint64_t count = take(4);
    if (count > markers.size()) return "more marker lines than markers";
    uint32_t last = 0;
    for (uint64_t i = 0; i < count && !truncated; ++i) {
        uint32_t id = uint32_t(take(4));
        auto m = std::ranges::lower_bound(markers, id, {}, &Marker::id);
        uint64_t n = take(1);
        if (m == markers.end() || m->id != id || id <= last || n == 0 || n > kMaxMarkerLines) return std::format("bad lines for marker {}", id);
        last = id;
        for (uint64_t k = 0; k < n; ++k) {
            uint64_t bytes = take(1);
            if (truncated || end - p < int64_t(bytes)) return "truncated marker lines";
            m->lines.emplace_back(p, p + bytes);
            p += bytes;
        }
        if (!truncated && checkMarker(g, *m)) return std::format("bad lines for marker {}", id);
    }
    if (truncated) return "truncated marker lines";
    return p == end ? std::string() : "marker lines section size mismatch";
}

// Reads the OWNR payload [p, end) onto the loaded `markers`; each owner must be a loaded instance.
static std::string readOwners(const uint8_t* p, const uint8_t* end, const std::vector<Instance>& instances, std::vector<Marker>& markers) {
    if (end - p < 4 || (end - p - 4) % 8) return "marker owners section size mismatch";
    uint64_t count = get(p, 4);
    if (count != uint64_t(end - p - 4) / 8) return "marker owners section size mismatch";
    uint32_t last = 0;
    for (p += 4; p < end; p += 8) {
        uint32_t id = uint32_t(get(p, 4)), owner = uint32_t(get(p + 4, 4));
        auto m = std::ranges::lower_bound(markers, id, {}, &Marker::id);
        auto in = std::ranges::lower_bound(instances, owner, {}, &Instance::id);
        if (m == markers.end() || m->id != id || id <= last || in == instances.end() || in->id != owner || !owner ||
            m->kind == MarkerKind::PlayerSpawn)  // no prefab places one
            return std::format("bad owner for marker {}", id);
        m->owner = owner;
        last = id;
    }
    return {};
}

// Reads the TIME payload [p, end) onto the loaded `markers`: each a real window on a spawn marker.
static std::string readTimes(const uint8_t* p, const uint8_t* end, const Grid& g, std::vector<Marker>& markers) {
    if (end - p < 4 || (end - p - 4) % 12 || get(p, 4) != uint64_t(end - p - 4) / 12) return "marker times section size mismatch";
    uint32_t last = 0;
    for (p += 4; p < end; p += 12) {
        uint32_t id = uint32_t(get(p, 4));
        auto m = std::ranges::lower_bound(markers, id, {}, &Marker::id);
        if (m == markers.end() || m->id != id || id <= last) return std::format("bad times for marker {}", id);
        m->activeFrom = std::bit_cast<float>(uint32_t(get(p + 4, 4)));
        m->activeTo = std::bit_cast<float>(uint32_t(get(p + 8, 4)));
        if (m->activeFrom == m->activeTo || checkMarker(g, *m)) return std::format("bad times for marker {}", id);
        last = id;
    }
    return {};
}

std::string loadWorld(const char* path, Grid& g, uint32_t& seed, std::vector<Instance>& instances,
                      std::optional<std::vector<Marker>>& markers, uint32_t* markerIds) {
    markers.reset();
    if (markerIds) *markerIds = 0;
    std::vector<Section> sections;
    if (std::string error = readWorldFile(path, g, seed, instances, sections); !error.empty()) return error;
    const Section *lines = nullptr, *owners = nullptr, *times = nullptr;  // read once the markers are in
    bool markerIdsSeen = false;
    for (const Section& sec : sections) {
        const uint8_t *p = sec.bytes.data(), *end = p + sec.bytes.size();
        if (std::memcmp(sec.tag, "MARK", 4) == 0) {
            if (std::string error = readMarkers(p, end, g, markers.emplace()); !error.empty()) return error;
        } else if (std::memcmp(sec.tag, "MKID", 4) == 0) {
            if (sec.bytes.size() != 4 || get(p, 4) == UINT32_MAX || markerIdsSeen) return "bad marker id mark";
            markerIdsSeen = true;
            if (markerIds) *markerIds = uint32_t(get(p, 4));
        } else if (std::memcmp(sec.tag, "LINE", 4) == 0 || std::memcmp(sec.tag, "OWNR", 4) == 0 || std::memcmp(sec.tag, "TIME", 4) == 0) {
            const Section*& seen = sec.tag[0] == 'L' ? lines : sec.tag[0] == 'O' ? owners : times;
            if (seen) return "duplicate marker lines, owners or times";
            seen = &sec;
        }
    }
    if ((lines || owners || times) && !markers) return "marker lines, owners or times without markers";
    if (lines)
        if (std::string error = readLines(lines->bytes.data(), lines->bytes.data() + lines->bytes.size(), g, *markers); !error.empty())
            return error;
    if (owners)
        if (std::string error = readOwners(owners->bytes.data(), owners->bytes.data() + owners->bytes.size(), instances, *markers); !error.empty())
            return error;
    if (times) return readTimes(times->bytes.data(), times->bytes.data() + times->bytes.size(), g, *markers);
    return {};
}
