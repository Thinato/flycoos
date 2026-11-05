#include "edit.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <tuple>
#include <utility>

#include "plants.h"

int groundTop(const Grid& g, int x, int z) {
    for (int y = g.size.y - 1; y >= 0; --y)
        if (isOpaque(g.get({x, y, z}))) return y;
    return -1;
}

int groundUnderTrees(const Grid& g, int x, int z) {
    int y = groundTop(g, x, z);
    for (Block b; y > 0 && (isTreeBlock(b = g.get({x, y, z})) || isClear(b));) --y;
    return y;
}

namespace {

// Calls fn(index in g of the row's first voxel, row number) for each x-row of b (inside g), z then y.
template <class Fn> void rows(const Grid& g, Box b, Fn fn) {
    size_t r = 0;
    for (int y = b.lo.y; y <= b.hi.y; ++y)
        for (int z = b.lo.z; z <= b.hi.z; ++z) fn(g.index({b.lo.x, y, z}), r++);
}

// What a column's surface turns into when it's buried, as generation layers it.
Block subsoil(Block surface) { return surface == Grass ? Dirt : surface == Snow ? Stone : surface; }

// Ground as generation lays it (rocks included). Trees and structures stand on it: brushes keep its
// material on top instead of spreading theirs (leaves, planks, thatch...).
bool natural(Block b) { return b == Grass || b == Dirt || b == Stone || b == Sand || b == Snow || b == Rock; }

// Moves a column's ground top to `to`, keeping its surface material on top: its top voxel, except
// that lowering a tree or structure takes that of the natural ground under it (a painted top lying
// right on natural ground, such as a Cobble road, stays). Lowering recolors the new top only where it
// is natural ground, so cutting into a canopy leaves leaves.
void setColumnTop(Grid& g, int x, int z, int to) {
    int top = groundTop(g, x, z);
    if (top < 0) return;
    to = std::clamp(to, 0, g.size.y - 1);
    Block surface = g.get({x, top, z}), plant = g.get({x, top + 1, z});
    if (isPlant(plant)) g.set({x, top + 1, z}, Air);  // moves with the surface (applyEdit clears it if that's no longer grass)
    bool painted = top > 0 && natural(g.get({x, top - 1, z}));
    for (int y = top - 1; to < top && !natural(surface) && !painted && y >= 0; --y)
        if (Block b = g.get({x, y, z}); natural(b)) {
            surface = b;
            break;
        }
    if (to > top) {
        for (int y = top; y < to; ++y) g.set({x, y, z}, subsoil(surface));
        g.set({x, to, z}, surface);
    } else if (to < top) {
        Block fill = g.get({x, top + 1 + int(isPlant(plant)), z}) == Water ? Water : Air;  // a lakebed stays under water
        for (int y = to + 1; y <= top; ++y) g.set({x, y, z}, fill);
        if (natural(g.get({x, to, z}))) g.set({x, to, z}, surface);
    }
    if (isPlant(plant) && g.get({x, to + 1, z}) == Air) g.set({x, to + 1, z}, plant);
}

// Brush weight: 1 at the center, easing to 0 just past `radius` columns away.
float falloff(int dx, int dz, int radius) {
    float r = float(radius) + 0.5f, t = float(dx * dx + dz * dz) / (r * r);
    return t >= 1 ? 0 : (1 - t) * (1 - t);
}

// Stable per column and height, in [0, 1): turns fractional raise/lower amounts into smooth slopes
// over repeated strokes instead of terraces.
float dither(int x, int z, int y) {
    uint32_t h = uint32_t(x) * 0x27d4eb2du ^ uint32_t(z) * 0x165667b1u ^ uint32_t(y) * 0x9e3779b9u;
    h ^= h >> 15;
    h *= 0x85ebca6bu;
    h ^= h >> 13;
    return float(h >> 8) / 16777216.0f;
}

void brush(Grid& g, const EditOp& op, Box box) {
    const int m = op.radius + 2, side = 2 * m + 1;  // Smooth averages over 5x5 columns
    std::vector<int> heights;  // Smooth: ground tops before the edit, so the order of columns doesn't matter
    if (op.tool == Tool::Smooth)
        for (int dz = -m; dz <= m; ++dz)
            for (int dx = -m; dx <= m; ++dx) heights.push_back(groundTop(g, op.center.x + dx, op.center.y + dz));

    for (int z = box.lo.z; z <= box.hi.z; ++z)
        for (int x = box.lo.x; x <= box.hi.x; ++x) {
            int dx = x - op.center.x, dz = z - op.center.y, top = groundTop(g, x, z);
            float w = falloff(dx, dz, op.radius);
            if (w <= 0 || top < 0) continue;
            int to = top;
            switch (op.tool) {
            case Tool::Raise: to = top + int(float(op.strength) * w + dither(x, z, top)); break;
            case Tool::Lower: to = top - int(float(op.strength) * w + dither(x, z, top)); break;
            case Tool::Flatten: to = top + std::clamp(op.height - top, -op.strength, op.strength); break;
            case Tool::Smooth: {
                float sum = 0;
                int n = 0;
                for (int j = -2; j <= 2; ++j)
                    for (int i = -2; i <= 2; ++i)
                        if (int h = heights[size_t(dz + j + m) * size_t(side) + size_t(dx + i + m)]; h >= 0) sum += float(h), ++n;
                to = top + std::clamp(int(std::lround((sum / float(n) - float(top)) * w)), -op.strength, op.strength);
                break;
            }
            case Tool::Paint: g.set({x, top, z}, op.block); continue;
            case Tool::Plant: plantColumn(g, x, z, op.block, float(op.strength) / 10); continue;
            default: break;
            }
            setColumnTop(g, x, z, to);
        }
}

Box markerBox(const Grid& g, const Marker& m) {
    glm::ivec3 p = glm::clamp(glm::ivec3(glm::floor(m.pos)), glm::ivec3(0), g.size - 1);
    return {p, p};
}

// Swaps a record's (id, item on the other side) entries with the live list sorted by id (the last
// entry first when undoing). `touched(live, other)` sees both sides first (null: none).
template <class T, class Fn>
void swapEntries(std::vector<T>& list, std::vector<std::pair<uint32_t, std::optional<T>>>& entries, bool undo, Fn touched) {
    for (size_t k = 0; k < entries.size(); ++k) {
        auto& [id, other] = entries[undo ? entries.size() - 1 - k : k];
        auto it = std::ranges::lower_bound(list, id, {}, &T::id);
        bool live = it != list.end() && it->id == id;
        touched(live ? &*it : nullptr, other ? &*other : nullptr);
        std::optional<T> was = live ? std::optional(std::move(*it)) : std::nullopt;
        if (other && live) *it = std::move(*other);
        else if (other) list.insert(it, std::move(*other));
        else if (live) list.erase(it);
        other = std::move(was);
    }
}

// Swaps the record's cells, instances and markers with the live ones (the last change first when
// undoing) and moves it to the other stack.
Box swapRecord(Grid& g, std::vector<Instance>& list, std::vector<Marker>& markers, std::vector<EditRecord>& from,
               std::vector<EditRecord>& to, bool undo) {
    if (from.empty()) return {};
    EditRecord rec = std::move(from.back());
    from.pop_back();
    for (const EditRecord::Part& p : rec.parts)
        if (!g.inside(p.box.lo) || !g.inside(p.box.hi)) return {};  // stale (the world was replaced)
    Box all{glm::ivec3(INT_MAX), glm::ivec3(INT_MIN)};
    auto grow = [&](Box b) { all = {glm::min(all.lo, b.lo), glm::max(all.hi, b.hi)}; };
    for (size_t k = 0; k < rec.parts.size(); ++k) {
        EditRecord::Part& p = rec.parts[undo ? rec.parts.size() - 1 - k : k];
        size_t w = size_t(p.box.size().x);
        rows(g, p.box, [&](size_t i, size_t r) { std::swap_ranges(&p.cells[r * w], &p.cells[r * w] + w, &g.cells[i]); });
        grow(p.box);
    }
    swapEntries(list, rec.instances, undo, [&](const Instance* live, const Instance* other) {
        if (live) grow(instanceBox(*live));  // both sides are boxes the step touched
        if (other) grow(instanceBox(*other));
    });
    // Only the side that ends up live (or the removed one): a far move mustn't remesh all in between.
    swapEntries(markers, rec.markers, undo, [&](const Marker* live, const Marker* other) {
        if (other || live) grow(markerBox(g, other ? *other : *live));
    });
    to.push_back(std::move(rec));
    return all;
}

}  // namespace

// (A fill of water over plants drowns them.)
void clearUnsupported(Grid& g, Box b) {
    for (int y = b.lo.y; y <= b.hi.y; ++y)
        for (int z = b.lo.z; z <= b.hi.z; ++z)
            for (int x = b.lo.x; x <= b.hi.x; ++x)
                if (isPlant(g.get({x, y, z})) && (g.get({x, y - 1, z}) != Grass || g.get({x, y + 1, z}) == Water)) g.set({x, y, z}, Air);
}

uint32_t nextMarkerId(const std::vector<Marker>& list, const EditHistory& h) {
    uint32_t id = std::max(list.empty() ? 0 : list.back().id, h.markerIds);
    for (const std::vector<EditRecord>* stack : {&h.undo, &h.redo})  // ids an undo or redo may bring back
        for (const EditRecord& r : *stack)
            for (const auto& entry : r.markers) id = std::max(id, entry.first);
    return id >= UINT32_MAX - 1 ? 0 : id + 1;  // 0 when the ids ran out (2^32-1 is never an id)
}

const char* checkEdit(const Grid& g, const EditOp& op) {
    if (op.tool >= Tool::Count) return "unknown tool";
    if (op.block >= BlockCount) return "unknown block";
    if (op.tool == Tool::Fill) {
        int64_t volume = 1;
        for (int a = 0; a < 3; ++a)  // checked per axis so huge corners can't overflow
            if ((volume *= std::abs(int64_t(op.max[a]) - op.min[a]) + 1) > kMaxEditVolume) return "fill box too large";
        return nullptr;
    }
    if (op.radius < 0 || op.radius > kMaxBrushRadius) return "radius out of range";
    if (op.strength < 1 || op.strength > kMaxStrength) return "strength out of range";
    if (glm::any(glm::greaterThan(op.center, glm::ivec2(1 << 24))) || glm::any(glm::lessThan(op.center, glm::ivec2(-(1 << 24)))))
        return "center out of range";  // keeps center +- radius from overflowing
    if (op.tool == Tool::Flatten && (op.height < 0 || op.height >= g.size.y)) return "height out of range";
    if (op.tool == Tool::Paint && !isOpaque(op.block)) return "paint needs a solid block";
    if (op.tool == Tool::Plant && op.block != Air && !isPlant(op.block)) return "plant needs a plant (or Air to clear)";
    return nullptr;
}

Box editBox(const Grid& g, const EditOp& op) {
    Box box;
    if (op.tool == Tool::Fill) box = {glm::min(op.min, op.max), glm::max(op.min, op.max)};
    else box = {glm::ivec3(op.center.x - op.radius, 0, op.center.y - op.radius),
                glm::ivec3(op.center.x + op.radius, g.size.y - 1, op.center.y + op.radius)};
    return {glm::max(box.lo, glm::ivec3(0)), glm::min(box.hi, g.size - 1)};
}

std::vector<Block> readBox(const Grid& g, Box b) {
    size_t w = size_t(b.size().x);
    std::vector<Block> cells(w * size_t(b.size().y) * size_t(b.size().z));
    rows(g, b, [&](size_t i, size_t r) { std::copy_n(&g.cells[i], w, &cells[r * w]); });
    return cells;
}

EditRecord::Part changedPart(const Grid& g, Box box, const std::vector<Block>& before) {
    // The bounding box of what changed, so remeshing and undo stay small.
    Box dirty{glm::ivec3(INT_MAX), glm::ivec3(INT_MIN)};
    size_t w = size_t(box.size().x);
    rows(g, box, [&](size_t i, size_t r) {
        const Block *old = &before[r * w], *now = &g.cells[i];
        auto first = std::mismatch(old, old + w, now).first;
        if (first == old + w) return;
        int last = int(w) - 1;
        while (old[last] == now[last]) --last;
        int y = box.lo.y + int(r / size_t(box.size().z)), z = box.lo.z + int(r % size_t(box.size().z));
        dirty.lo = glm::min(dirty.lo, glm::ivec3(box.lo.x + int(first - old), y, z));
        dirty.hi = glm::max(dirty.hi, glm::ivec3(box.lo.x + last, y, z));
    });
    if (dirty.empty()) return {};

    EditRecord::Part part{dirty, {}};
    part.cells.reserve(size_t(dirty.size().x) * size_t(dirty.size().y) * size_t(dirty.size().z));
    for (int y = dirty.lo.y; y <= dirty.hi.y; ++y)
        for (int z = dirty.lo.z; z <= dirty.hi.z; ++z) {
            const Block* row = &before[(size_t(y - box.lo.y) * size_t(box.size().z) + size_t(z - box.lo.z)) * w +
                                       size_t(dirty.lo.x - box.lo.x)];
            part.cells.insert(part.cells.end(), row, row + dirty.size().x);
        }
    return part;
}

void pushRecord(EditHistory& history, EditRecord rec, bool join) {
    for (const auto& entry : rec.markers) history.markerIds = std::max(history.markerIds, entry.first);
    history.redo.clear();  // their marker ids stay used: see markerIds
    if (!join || history.undo.empty()) history.undo.emplace_back();
    EditRecord& last = history.undo.back();
    for (EditRecord::Part& p : rec.parts) {
        last.bytes += p.cells.size();
        last.parts.push_back(std::move(p));
    }
    for (auto& entry : rec.instances) {
        last.bytes += entry.second ? entry.second->cells.size() * sizeof(Instance::Cell) : 0;
        last.instances.push_back(std::move(entry));
    }
    for (auto& entry : rec.markers) last.markers.push_back(std::move(entry));
    size_t total = 0;
    for (const EditRecord& r : history.undo) total += r.bytes;
    while (history.undo.size() > kMaxUndo || (total > kMaxUndoBytes && history.undo.size() > 1)) {
        total -= history.undo.front().bytes;
        history.undo.erase(history.undo.begin());
    }
}

Box instanceBox(const Instance& in) {
    Box b{glm::ivec3(INT_MAX), glm::ivec3(INT_MIN)};
    for (const Instance::Cell& c : in.cells) {
        b.lo = glm::min(b.lo, glm::ivec3(c.x, c.y, c.z));
        b.hi = glm::max(b.hi, glm::ivec3(c.x, c.y, c.z));
    }
    return b;
}

EditResult applyEdit(Grid& g, const EditOp& op, EditHistory& history, bool join) {
    if (const char* error = checkEdit(g, op)) return {{}, error};
    Box box = editBox(g, op);
    if (box.empty()) return {};

    Box touched = box;  // and the plants just above a fill, which may lose their ground, or just under it (drowned)
    touched.lo.y = std::max(touched.lo.y - 1, 0), touched.hi.y = std::min(touched.hi.y + 1, g.size.y - 1);
    std::vector<Block> before = readBox(g, touched);
    if (op.tool == Tool::Fill) rows(g, box, [&](size_t i, size_t) { std::fill_n(&g.cells[i], box.size().x, op.block); });
    else brush(g, op, box);
    clearUnsupported(g, touched);

    EditRecord rec;
    rec.parts.push_back(changedPart(g, touched, before));
    Box dirty = rec.parts[0].box;
    if (!dirty.empty()) pushRecord(history, std::move(rec), join);
    return {dirty};
}

EditResult growMeadow(Grid& g, glm::ivec2 lo, glm::ivec2 hi, Block kind, float density, EditHistory& history) {
    constexpr int kTile = 64;
    std::tie(lo, hi) = std::pair(glm::max(glm::min(lo, hi), glm::ivec2(0)), glm::min(glm::max(lo, hi), glm::ivec2(g.size.x, g.size.z) - 1));
    EditRecord rec;
    Box dirty;
    for (int tz = lo.y; tz <= hi.y; tz += kTile)
        for (int tx = lo.x; tx <= hi.x; tx += kTile) {
            glm::ivec2 a(tx, tz), b = glm::min(a + kTile - 1, hi);
            Box tile{glm::ivec3(a.x, g.size.y - 1, a.y), glm::ivec3(b.x, 0, b.y)};  // y: the plant cells' range
            for (int z = a.y; z <= b.y; ++z)
                for (int x = a.x; x <= b.x; ++x)
                    if (int top = groundTop(g, x, z); top >= 0 && top + 1 < g.size.y)
                        tile.lo.y = std::min(tile.lo.y, groundUnderTrees(g, x, z) + 1), tile.hi.y = std::max(tile.hi.y, top + 1);
            if (tile.empty()) continue;
            std::vector<Block> before = readBox(g, tile);
            for (int z = a.y; z <= b.y; ++z)
                for (int x = a.x; x <= b.x; ++x) plantColumn(g, x, z, kind, density);
            EditRecord::Part part = changedPart(g, tile, before);
            if (part.box.empty()) continue;
            dirty = dirty.empty() ? part.box : Box{glm::min(dirty.lo, part.box.lo), glm::max(dirty.hi, part.box.hi)};
            rec.parts.push_back(std::move(part));
        }
    if (!rec.parts.empty()) pushRecord(history, std::move(rec), false);
    return {dirty};
}

Box undoEdit(Grid& g, EditHistory& history, std::vector<Instance>& instances, std::vector<Marker>& markers) {
    return swapRecord(g, instances, markers, history.undo, history.redo, true);
}
Box redoEdit(Grid& g, EditHistory& history, std::vector<Instance>& instances, std::vector<Marker>& markers) {
    return swapRecord(g, instances, markers, history.redo, history.undo, false);
}

EditResult putMarker(const Grid& g, std::vector<Marker>& list, EditHistory& h, Marker m) {
    if (const char* error = checkMarker(g, m)) return {{}, error};
    auto spawn = std::ranges::find(list, MarkerKind::PlayerSpawn, &Marker::kind);
    if (m.kind == MarkerKind::PlayerSpawn && spawn != list.end() && spawn->id != m.id)
        return {{}, "there is a player_spawn already: move that one instead"};
    int npcs = m.kind == MarkerKind::Spawn ? m.count : 0;
    for (const Marker& o : list) npcs += o.kind == MarkerKind::Spawn && o.id != m.id ? o.count : 0;
    if (npcs > kMaxSpawnedNpcs) return {{}, "spawn markers would make more than 256 NPCs: lower some counts or remove some"};
    std::optional<Marker> old;
    if (m.id) {
        auto it = std::ranges::lower_bound(list, m.id, {}, &Marker::id);
        if (it == list.end() || it->id != m.id) return {{}, "no marker with that id"};
        m.owner = it->owner;  // only placing and removing its instance change who owns it
        if (*it == m) return {{}, nullptr, m.id};  // nothing changes: no undo step
        old = std::exchange(*it, m);
    } else {
        m.owner = 0;  // placed by hand
        if (list.size() >= kMaxMarkers) return {{}, "too many markers: remove some first"};
        if (!(m.id = nextMarkerId(list, h))) return {{}, "out of marker ids"};
        list.insert(std::ranges::lower_bound(list, m.id, {}, &Marker::id), m);
    }
    EditRecord rec;
    rec.markers.push_back({m.id, std::move(old)});
    pushRecord(h, std::move(rec), false);
    return {markerBox(g, m), nullptr, m.id};
}

EditResult removeMarker(const Grid& g, std::vector<Marker>& list, EditHistory& h, uint32_t id) {
    auto it = std::ranges::lower_bound(list, id, {}, &Marker::id);
    if (it == list.end() || it->id != id) return {{}, "no marker with that id"};
    Box box = markerBox(g, *it);
    EditRecord rec;
    rec.markers.push_back({id, std::move(*it)});
    list.erase(it);
    pushRecord(h, std::move(rec), false);
    return {box, nullptr, id};
}

std::optional<RayHit> raycast(const Grid& g, glm::vec3 from, glm::vec3 dir, float maxDistance) {
    glm::ivec3 cell(glm::floor(from)), step(0), normal(0);
    glm::vec3 next(INFINITY), delta(INFINITY);  // ray distance to the next cell boundary, and between boundaries
    for (int a = 0; a < 3; ++a) {
        step[a] = dir[a] > 0 ? 1 : -1;
        if (dir[a] == 0) continue;
        delta[a] = std::abs(1 / dir[a]);
        next[a] = (dir[a] > 0 ? float(cell[a] + 1) - from[a] : from[a] - float(cell[a])) * delta[a];
    }
    for (float t = 0; t <= maxDistance;) {
        if (isOpaque(g.get(cell))) return RayHit{cell, normal, t};
        int a = next.x < next.y ? (next.x < next.z ? 0 : 2) : (next.y < next.z ? 1 : 2);
        t = next[a];
        next[a] += delta[a];
        cell[a] += step[a];
        normal = glm::ivec3(0);
        normal[a] = -step[a];
    }
    return std::nullopt;
}
