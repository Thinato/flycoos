#include "prefab.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <format>
#include <fstream>
#include <utility>

#include "defs.h"

namespace {

std::vector<Prefab> generatedPrefabs() {
    auto make = [](const char* name, const char* description, std::vector<PrefabParam> params) {
        Prefab p;
        p.name = name;
        p.description = description;
        p.params = std::move(params);
        return p;
    };
    return {
        make("tree", "Broadleaf tree like the forests': trunk (2x2 from 12 up) and a flattened ragged canopy",
             {{"trunk", 4, 20, 10, true}, {"radius", 2, 7, 4.5f}}),
        make("pine", "Conifer: thin trunk and stacked tiers of needles", {{"height", 6, 28, 14, true}}),
        make("bush", "Low round clump of leaves", {{"radius", 1, 4, 2}}),
        make("boulder", "Half-buried rock like the plains' boulders", {{"radius", 1.5f, 6, 3}}),
        make("spire", "Tapering rock spire like the rocky highlands'", {{"height", 6, 32, 16, true}, {"radius", 1.5f, 5, 3}}),
    };
}

std::string_view trim(std::string_view s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
    return a == std::string_view::npos ? std::string_view() : s.substr(a, b - a + 1);
}

std::optional<Block> blockNamed(std::string_view name) {
    for (int b = 0; b < BlockCount; ++b)
        if (std::ranges::equal(name, std::string_view(kBlockNames[b]), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); }))
            return Block(b);
    return std::nullopt;
}

// A `marker:` value: "<spawn|poi|region> x=N z=N [entity=id] [count=N] [radius=N] [active=FROM-TO]
// [name=rest of the line]", x, z a cell of the layers, FROM and TO times of day (Marker::activeFrom).
// Returns why it's wrong, or "". The parser checks the rest once the size is known.
std::string parseMarker(std::string_view v, Marker& m) {
    std::string_view kind = v.substr(0, v.find(' '));
    auto k = std::ranges::find(kMarkerKindNames, kind);
    if (k == std::end(kMarkerKindNames) || MarkerKind(k - std::begin(kMarkerKindNames)) == MarkerKind::PlayerSpawn)
        return std::format("marker kind must be spawn, poi or region, not '{}'", kind);
    m.kind = MarkerKind(k - std::begin(kMarkerKindNames));
    bool x = false, z = false, entity = false, radius = false;
    for (v = trim(v.substr(kind.size())); !v.empty();) {
        size_t eq = v.find('='), end = std::min(v.find(' '), v.size());
        std::string_view key = v.substr(0, std::min(eq, end));
        if (eq > end) return std::format("expected key=value, not '{}'", key);
        if (key == "name") {
            m.name = trim(v.substr(eq + 1));
            break;
        }
        std::string_view value = v.substr(eq + 1, end - eq - 1);
        v = trim(v.substr(end));
        if (key == "entity") {
            auto it = defs().creatureById.find(value);
            if (it == defs().creatureById.end()) return std::format("unknown entity '{}'", value);
            m.entity = it->second;
            entity = true;
            continue;
        }
        if (key == "active") {  // e.g. 0.8-0.2: at night
            size_t dash = value.find('-');
            std::string from(value.substr(0, dash)), to(dash == value.npos ? "" : value.substr(dash + 1));
            char *fromEnd, *toEnd;
            m.activeFrom = std::strtof(from.c_str(), &fromEnd);
            m.activeTo = std::strtof(to.c_str(), &toEnd);
            if (from.empty() || to.empty() || *fromEnd || *toEnd) return "marker active must be FROM-TO, times of day 0..1 (e.g. active=0.8-0.2)";
            continue;
        }
        int n = 0;
        auto [last, ec] = std::from_chars(value.data(), value.data() + value.size(), n);
        if (key != "x" && key != "z" && key != "count" && key != "radius")
            return std::format("unknown marker field '{}' (x, z, entity, count, radius, active, name)", key);
        if (ec != std::errc() || last != value.data() + value.size() || n < 0 || n > 1 << 16)
            return std::format("marker {} must be a whole number", key);
        if (key == "x") m.pos.x = float(n) + 0.5f, x = true;
        if (key == "z") m.pos.z = float(n) + 0.5f, z = true;
        if (key == "count") m.count = n;
        if (key == "radius") m.radius = float(n), radius = true;
    }
    if (!x || !z) return "a marker needs x= and z= (a cell of the layers)";
    if (m.kind == MarkerKind::Spawn && !entity) return "a spawn marker needs entity=";
    if (m.kind != MarkerKind::Spawn && (entity || m.count != 1)) return "entity and count are for spawn markers";
    if (!radius) m.radius = m.kind == MarkerKind::Spawn ? typeOf(m.entity).wanderRadius : m.kind == MarkerKind::Poi ? 16 : 32;
    return {};
}

// Footprint size after `rotation` quarter turns, and where structure cell (x, z) lands in it.
glm::ivec3 rotatedSize(glm::ivec3 s, int rotation) { return rotation % 2 ? glm::ivec3(s.z, s.y, s.x) : s; }
glm::ivec2 rotate(int x, int z, glm::ivec3 s, int rotation) {
    switch (rotation) {
    case 1: return {z, s.x - 1 - x};  // counterclockwise seen from above: +x turns toward -z
    case 2: return {s.x - 1 - x, s.z - 1 - z};
    case 3: return {s.z - 1 - z, x};
    default: return {x, z};
    }
}

// rotate() for points: where (x, z) of a footprint of size `s` (cells 1 wide) lands after `rotation`
// quarter turns. Turning back is turn(q, rotatedSize(s, rotation), (4 - rotation) % 4).
glm::vec2 turn(glm::vec2 c, glm::ivec3 s, int rotation) {
    switch (rotation) {
    case 1: return {c.y, float(s.x) - c.x};
    case 2: return {float(s.x) - c.x, float(s.z) - c.y};
    case 3: return {float(s.z) - c.y, c.x};
    default: return c;
    }
}

// Where structure cell (0, 0, 0) of the rotated footprint goes: centered on the column, the bottom layer above the ground.
glm::ivec3 structureOrigin(const Prefab& p, const Instance& in) {
    glm::ivec3 s = rotatedSize(p.size, in.rotation);
    return in.pos + glm::ivec3(-(s.x / 2), 1 + p.y, -(s.z / 2));
}

// A marker position kept inside the world (checkMarker's rule), for instances at its edge.
glm::vec3 inWorld(const Grid& g, glm::vec3 p) {
    return glm::clamp(p, glm::vec3(0.5f, 0, 0.5f), glm::vec3(float(g.size.x) - 0.5f, float(g.size.y), float(g.size.z) - 0.5f));
}

void stamp(Grid& g, const Prefab& p, const Instance& in) {
    const float* v = in.params.data();
    if (p.name == "tree") return growTree(g, in.pos, int(v[0]), v[1], in.seed, in.pos);
    if (p.name == "pine") return growPine(g, in.pos, int(v[0]), in.seed, in.pos);
    if (p.name == "bush") return growBush(g, in.pos, v[0], in.seed, in.pos);
    if (p.name == "boulder") return growRock(g, in.pos, false, v[0], 0, in.seed, in.pos);
    if (p.name == "spire") return growRock(g, in.pos, true, v[1], int(v[0]), in.seed, in.pos);

    glm::ivec3 origin = structureOrigin(p, in);
    auto at = [&](int x, int y, int z) {
        glm::ivec2 q = rotate(x, z, p.size, in.rotation);
        return origin + glm::ivec3(q.x, y, q.y);
    };
    for (int y = 0, i = 0; y < p.size.y; ++y)
        for (int z = 0; z < p.size.z; ++z)
            for (int x = 0; x < p.size.x; ++x, ++i)
                if (p.cells[size_t(i)] != kKeep) g.set(at(x, y, z), p.cells[size_t(i)]);
    if (p.foundation == kKeep) return;
    for (int z = 0; z < p.size.z; ++z)
        for (int x = 0; x < p.size.x; ++x) {
            if (Block b = p.cells[size_t(z * p.size.x + x)]; b == kKeep || !isOpaque(b)) continue;  // kKeep counts as opaque
            glm::ivec3 q = at(x, 0, z);
            for (int n = 1; n <= kMaxFoundation && q.y - n >= 0 && !isOpaque(g.get(q - glm::ivec3(0, n, 0))); ++n)
                g.set(q - glm::ivec3(0, n, 0), p.foundation);
        }
}

uint32_t nextId(const std::vector<Instance>& list, const EditHistory& h) {
    uint32_t id = list.empty() ? 0 : list.back().id;
    for (const std::vector<EditRecord>* stack : {&h.undo, &h.redo})  // ids an undo or redo may bring back
        for (const EditRecord& r : *stack)
            for (const auto& entry : r.instances) id = std::max(id, entry.first);
    return id >= UINT32_MAX - 1 ? 0 : id + 1;  // 0 when the ids ran out (2^32-1 is never an id)
}

}  // namespace

const Prefab* PrefabLibrary::find(std::string_view name) const {
    auto it = std::ranges::find(prefabs, name, &Prefab::name);
    return it == prefabs.end() ? nullptr : &*it;
}

std::string parsePrefab(std::string_view text, Prefab& out) {
    std::array<int, 128> legend;  // block (or kKeep) per ASCII char, -1 = not in the legend
    legend.fill(-1);
    legend[' '] = kKeep;
    legend['.'] = Air;
    Prefab p;
    p.name = out.name;
    std::vector<std::vector<std::string_view>> layers;
    std::vector<int> layerLines, markerLines;
    std::vector<std::string_view> given;  // header keys seen
    const Grid any({1, 1, 1});
    auto unplaced = [&](Marker m) {  // checkMarker but the position: placing sets that
        m.pos = {};
        return checkMarker(any, m);
    };
    int n = 0, npcs = 0;
    auto fail = [&](const std::string& why) { return std::format("line {}: {}", n, why); };
    for (size_t at = 0; at < text.size();) {
        size_t end = std::min(text.find('\n', at), text.size());
        std::string_view line = text.substr(at, end - at);
        if (line.ends_with('\r')) line.remove_suffix(1);
        std::string_view t = trim(line);
        at = end + 1;
        ++n;
        if (t.starts_with('#')) continue;
        if (t == "layer") {
            if (layers.size() == kMaxPrefabSize) return fail(std::format("more than {} layers", kMaxPrefabSize));
            layers.emplace_back();
            layerLines.push_back(n);
            continue;
        }
        if (!layers.empty()) {  // a row of the current layer
            for (char c : line)
                if (static_cast<unsigned char>(c) >= 128 || legend[size_t(c)] < 0)
                    return fail(c == '\t' ? std::string("tab in a layer (use spaces)") : std::format("'{}' is not in the legend", c));
            if (line.size() > kMaxPrefabSize) return fail(std::format("row longer than {}", kMaxPrefabSize));
            if (layers.back().size() == kMaxPrefabSize) return fail(std::format("layer has more than {} rows", kMaxPrefabSize));
            layers.back().push_back(line);
            continue;
        }
        if (t.empty()) continue;
        size_t colon = t.find(':'), eq = t.find('=');
        std::string_view key = colon == std::string_view::npos ? "" : t.substr(0, colon), value = trim(t.substr(colon + 1));
        if (!key.empty() && std::ranges::all_of(key, [](unsigned char c) { return std::isalpha(c); })) {
            if (key != "marker" && key != "line") {  // those repeat; the rest would silently override
                if (std::ranges::find(given, key) != given.end()) return fail(std::format("'{}' is given twice", key));
                given.push_back(key);
            }
            if (key == "description") {
                if (value.size() > 200) return fail("description longer than 200 characters");
                p.description = value;
            } else if (key == "foundation") {
                std::optional<Block> b = blockNamed(value);
                if (!b || !isOpaque(*b)) return fail(std::format("foundation must be a solid block, not '{}'", value));
                p.foundation = *b;
            } else if (key == "y") {
                auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), p.y);
                if (ec != std::errc() || end != value.data() + value.size() || p.y < -kMaxPrefabSize || p.y > kMaxPrefabSize)
                    return fail(std::format("y must be an integer in -{0}..{0}", kMaxPrefabSize));
            } else if (key == "marker") {
                if (std::string why = parseMarker(value, p.markers.emplace_back()); !why.empty()) return fail(why);
                if (const char* why = unplaced(p.markers.back())) return fail(std::format("marker: {}", why));
                if (p.markers.size() > kMaxMarkers) return fail(std::format("more than {} markers", kMaxMarkers));
                if ((npcs += p.markers.back().kind == MarkerKind::Spawn ? p.markers.back().count : 0) > kMaxSpawnedNpcs)
                    return fail(std::format("the spawn markers make more than {} NPCs", kMaxSpawnedNpcs));
                markerLines.push_back(n);
            } else if (key == "line") {
                if (p.markers.empty()) return fail("a line belongs to the marker above it: put 'marker:' first");
                p.markers.back().lines.emplace_back(value);
                if (const char* why = unplaced(p.markers.back())) return fail(std::format("line: {}", why));
            } else {
                return fail(std::format("unknown key '{}' (description, foundation, y, marker, line)", key));
            }
        } else if (eq != std::string_view::npos && trim(t.substr(0, eq)).size() == 1) {
            unsigned char c = static_cast<unsigned char>(t[0]);
            std::string_view name = trim(t.substr(eq + 1));
            if (c < 0x21 || c > 0x7e || c == '.' || c == '#') return fail(std::format("'{}' can't be a legend character", char(c)));
            if (legend[c] >= 0) return fail(std::format("'{}' is already in the legend", char(c)));
            std::optional<Block> b = blockNamed(name);
            if (!b) {
                std::string names;
                for (int i = 0; i < BlockCount; ++i) names += std::format("{}{}", i ? ", " : "", kBlockNames[i]);
                return fail(std::format("unknown block '{}' (blocks: {})", name, names));
            }
            legend[c] = *b;
        } else {
            return fail("expected 'key: value', 'X = Block' or 'layer'");
        }
    }
    if (layers.empty()) return fail("no layers: each layer starts with a line 'layer', bottom first");
    for (size_t i = 0; i < layers.size(); ++i) {
        while (!layers[i].empty() && trim(layers[i].back()).empty()) layers[i].pop_back();  // trailing blank lines
        if (layers[i].empty()) return n = layerLines[i], fail("empty layer");
        p.size.z = std::max(p.size.z, int(layers[i].size()));
        for (std::string_view row : layers[i]) p.size.x = std::max(p.size.x, int(row.size()));
    }
    p.size.y = int(layers.size());
    p.cells.assign(size_t(p.size.x) * size_t(p.size.y) * size_t(p.size.z), kKeep);
    for (size_t y = 0; y < layers.size(); ++y)
        for (size_t z = 0; z < layers[y].size(); ++z)
            for (size_t x = 0; x < layers[y][z].size(); ++x)
                p.cells[(y * size_t(p.size.z) + z) * size_t(p.size.x) + x] = Block(legend[size_t(layers[y][z][x])]);
    if (std::ranges::all_of(p.cells, [](Block b) { return b == kKeep; })) return fail("no blocks: every cell is a space");
    for (size_t i = 0; i < p.markers.size(); ++i) {
        const Marker& m = p.markers[i];
        n = markerLines[i];
        if (m.pos.x > float(p.size.x) || m.pos.z > float(p.size.z))
            return fail(std::format("marker cell {} {} is outside the layers ({} x {})", int(m.pos.x), int(m.pos.z), p.size.x, p.size.z));
    }
    out = std::move(p);
    return {};
}

PrefabLibrary loadPrefabs(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    PrefabLibrary lib{dir, generatedPrefabs(), {}};
    std::error_code ec;
    std::vector<fs::path> files;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))  // not symlinks or subfolders
        if (it->path().extension() == ".txt" && it->symlink_status(ec).type() == fs::file_type::regular) files.push_back(it->path());
    if (ec) lib.errors.push_back(std::format("{}: {}", dir.string(), ec.message()));
    std::ranges::sort(files);
    uintmax_t total = 0;
    if (files.size() > kMaxPrefabFiles) {
        lib.errors.push_back(std::format("more than {} prefab files: the rest are skipped", kMaxPrefabFiles));
        files.resize(kMaxPrefabFiles);
    }
    for (const fs::path& path : files) {
        Prefab p;
        p.name = path.stem().string();
        std::string file = path.filename().string(), error;
        uintmax_t bytes = fs::file_size(path, ec);
        if (p.name.empty() || p.name.size() > 32 ||
            !std::ranges::all_of(p.name, [](unsigned char c) { return std::isalnum(c) || c == '_' || c == '-'; }))
            error = "the name must be 1-32 letters, digits, - or _";
        else if (lib.find(p.name)) error = std::format("'{}' is a generated prefab", p.name);
        else if (ec || bytes > kMaxPrefabFileBytes) error = ec ? ec.message() : std::format("larger than {} KB", kMaxPrefabFileBytes >> 10);
        else if ((total += bytes) > kMaxPrefabBytes) error = std::format("skipped: the prefab files are over {} MB in total", kMaxPrefabBytes >> 20);
        else {
            std::string text(bytes, '\0');
            std::ifstream f(path, std::ios::binary);
            if (!f.read(text.data(), std::streamsize(bytes))) error = "cannot read it";
            else error = parsePrefab(text, p);
        }
        if (error.empty()) lib.prefabs.push_back(std::move(p));
        else lib.errors.push_back(file + ": " + error);
    }
    return lib;
}

const char* checkPlacement(const Grid& g, const Prefab& p, const Instance& in) {
    if (in.pos.x < 0 || in.pos.z < 0 || in.pos.x >= g.size.x || in.pos.z >= g.size.z || in.pos.y < -1 || in.pos.y >= g.size.y)
        return "position outside the world";
    if (in.rotation < 0 || in.rotation > 3) return "rotation must be 0, 90, 180 or 270";
    if (in.params.size() != p.params.size()) return "wrong number of parameters";
    for (size_t i = 0; i < in.params.size(); ++i)
        if (!(in.params[i] >= p.params[i].lo && in.params[i] <= p.params[i].hi)) return "parameter out of range";
    return nullptr;
}

Box prefabBox(const Grid& g, const Prefab& p, const Instance& in) {
    if (checkPlacement(g, p, in)) return {};  // the params below are trusted
    int x = in.pos.x, y = in.pos.y, z = in.pos.z;
    const float* v = in.params.data();
    auto around = [&](int r, int below, int above) { return Box{{x - r, y - below, z - r}, {x + r, y + above, z + r}}; };
    Box b;
    if (p.name == "tree") {  // matches growTree: a 2x2 trunk's canopy is centered one further along +x and +z
        int trunk = int(v[0]), r = int(std::ceil(v[1]));
        b = around(r + 1, std::max(2, r - trunk), trunk + r);
        b.hi += glm::ivec3(1, 0, 1);
    } else if (p.name == "pine") {
        b = around(int(std::ceil(std::clamp(v[0] * 0.3f, 2.0f, 6.0f) + 0.5f)) + 1, 2, int(v[0]) + 1);
    } else if (p.name == "bush") {
        int r = int(std::ceil(v[0])) + 1;
        b = around(r, 2, 1 + r);
    } else if (p.name == "boulder") {
        b = around(int(std::ceil(v[0])) + 1, 3, int(v[0] * 1.5f));
    } else if (p.name == "spire") {
        b = around(int(std::ceil(v[1])) + 1, 3, int(v[0]));
    } else {
        glm::ivec3 origin = structureOrigin(p, in);
        b = {origin, origin + rotatedSize(p.size, in.rotation) - 1};
        if (p.foundation != kKeep) b.lo.y -= kMaxFoundation;
    }
    return {glm::max(b.lo, glm::ivec3(0)), glm::min(b.hi, g.size - 1)};
}

EditResult placePrefab(Grid& g, const Prefab& p, Instance inst, std::optional<int> y, std::vector<Instance>& list,
                       EditHistory& h, bool join, std::vector<Marker>* markers) {
    inst.pos.y = y.value_or(groundTop(g, inst.pos.x, inst.pos.z));
    if (const char* error = checkPlacement(g, p, inst)) return {{}, error};
    if (list.size() >= kMaxInstances) return {{}, "too many instances: remove some first"};
    if (!inst.id) inst.id = nextId(list, h);
    if (!inst.id) return {{}, "out of instance ids"};
    if (!markers || p.markers.empty()) markers = nullptr;  // none to make
    uint32_t firstMarker = 0;
    if (markers) {
        int npcs = 0;
        for (const std::vector<Marker>* l : {&std::as_const(*markers), &p.markers})
            for (const Marker& m : *l) npcs += m.kind == MarkerKind::Spawn ? m.count : 0;
        if (markers->size() + p.markers.size() > kMaxMarkers) return {{}, "too many markers for the prefab's: remove some first"};
        if (npcs > kMaxSpawnedNpcs) return {{}, "the prefab's spawn markers would make more than 256 NPCs: remove some spawns first"};
        firstMarker = nextMarkerId(*markers, h);
        if (!firstMarker || UINT32_MAX - 1 - firstMarker < p.markers.size()) return {{}, "out of marker ids"};
    }
    Box box = prefabBox(g, p, inst);
    if (box.empty()) return {{}, "the prefab is outside the world"};
    std::vector<Block> before = readBox(g, box);
    stamp(g, p, inst);

    inst.cells.clear();
    size_t i = 0;
    for (int cy = box.lo.y; cy <= box.hi.y; ++cy)
        for (int cz = box.lo.z; cz <= box.hi.z; ++cz)
            for (int cx = box.lo.x; cx <= box.hi.x; ++cx, ++i)
                if (Block now = g.cells[g.index({cx, cy, cz})]; now != before[i])
                    inst.cells.push_back({uint16_t(cx), uint16_t(cy), uint16_t(cz), before[i], now});
    size_t total = inst.cells.size();
    for (const Instance& in : list) total += in.cells.size();
    if (inst.cells.empty() || total > kMaxInstanceCells) {
        for (const Instance::Cell& c : inst.cells) g.set({c.x, c.y, c.z}, c.before);
        return {{}, inst.cells.empty() ? "the prefab changes nothing there" : "the instances hold too many voxels: remove some first"};
    }
    EditRecord rec;
    rec.parts.push_back(changedPart(g, box, before));
    rec.instances.push_back({inst.id, std::nullopt});
    if (markers) {  // standing on the bottom layer, turned with the footprint; the ids are above every other
        glm::ivec3 origin = structureOrigin(p, inst);
        for (const Marker& d : p.markers) {
            Marker& m = markers->emplace_back(d);
            glm::vec2 xz = glm::vec2(origin.x, origin.z) + turn({d.pos.x, d.pos.z}, p.size, inst.rotation);
            m.id = firstMarker++;
            m.owner = inst.id;
            m.pos = inWorld(g, {xz.x, float(origin.y + 1), xz.y});
            rec.markers.push_back({m.id, std::nullopt});
        }
    }
    EditResult result{rec.parts[0].box, nullptr, inst.id};
    list.insert(std::ranges::lower_bound(list, inst.id, {}, &Instance::id), std::move(inst));
    pushRecord(h, std::move(rec), join);
    return result;
}

EditResult removePrefab(Grid& g, uint32_t id, std::vector<Instance>& list, EditHistory& h, std::vector<Marker>* markers) {
    auto it = std::ranges::lower_bound(list, id, {}, &Instance::id);
    if (it == list.end() || it->id != id) return {{}, "no instance with that id"};
    Box box = instanceBox(*it);
    std::vector<Block> before = readBox(g, box);
    for (const Instance::Cell& c : it->cells)
        if (glm::ivec3 q(c.x, c.y, c.z); g.get(q) == c.after) g.set(q, c.before);
    EditRecord rec;
    if (EditRecord::Part part = changedPart(g, box, before); !part.box.empty()) rec.parts.push_back(std::move(part));
    rec.instances.push_back({id, std::move(*it)});
    list.erase(it);
    if (markers) {
        for (auto m = markers->begin(); m != markers->end();) {
            if (m->owner == id) rec.markers.push_back({m->id, std::move(*m)}), m = markers->erase(m);
            else ++m;
        }
    }
    pushRecord(h, std::move(rec), false);
    return {box, nullptr, id};  // the whole box, even if later edits covered all of it: the list changed
}

EditResult movePrefab(Grid& g, const Prefab& p, uint32_t id, glm::ivec2 column, std::optional<int> y, int rotation,
                      std::vector<Instance>& list, EditHistory& h, std::vector<Marker>* markers) {
    auto it = std::ranges::lower_bound(list, id, {}, &Instance::id);
    if (it == list.end() || it->id != id) return {{}, "no instance with that id"};
    const Instance was{.pos = it->pos, .rotation = it->rotation};  // where its markers stand relative to
    Instance moved{id, it->prefab, {column.x, y.value_or(0), column.y}, rotation, it->seed, it->params, {}};
    if (const char* error = checkPlacement(g, p, moved)) return {{}, error};
    EditHistory step;  // the move's own record, so a failure leaves the real history untouched
    Box from = removePrefab(g, id, list, step).dirty;
    EditResult placed = placePrefab(g, p, std::move(moved), y, list, step, true);
    if (placed.error) {
        std::vector<Marker> none;  // the step changed no markers
        undoEdit(g, step, list, none);
        return placed;
    }
    if (markers) {  // its markers keep their place on the footprint (even moved by hand), turned and moved with it
        const Instance& now = *std::ranges::find(list, id, &Instance::id);
        glm::ivec3 from = structureOrigin(p, was), to = structureOrigin(p, now);
        for (Marker& m : *markers) {
            if (m.owner != id) continue;
            glm::vec2 local = turn(glm::vec2(m.pos.x - float(from.x), m.pos.z - float(from.z)), rotatedSize(p.size, was.rotation),
                                   (4 - was.rotation) % 4);
            glm::vec2 xz = glm::vec2(to.x, to.z) + turn(local, p.size, now.rotation);
            Marker old = m;
            m.pos = inWorld(g, {xz.x, m.pos.y + float(to.y - from.y), xz.y});
            if (m != old) step.undo.back().markers.push_back({m.id, std::move(old)});
        }
    }
    pushRecord(h, std::move(step.undo.back()), false);
    placed.dirty = {glm::min(from.lo, placed.dirty.lo), glm::max(from.hi, placed.dirty.hi)};
    return placed;
}

const Instance* instanceAt(const std::vector<Instance>& list, const Grid& g, glm::ivec3 cell) {
    for (const Instance& in : list)
        for (const Instance::Cell& c : in.cells)
            if (glm::ivec3(c.x, c.y, c.z) == cell && g.get(cell) == c.after) return &in;
    return nullptr;
}
