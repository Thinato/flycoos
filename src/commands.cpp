#include "commands.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <climits>
#include <cmath>
#include <filesystem>
#include <format>
#include <numbers>
#include <stdexcept>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "defs.h"

using json = nlohmann::json;

namespace {

struct BadArgs : std::runtime_error {
    using std::runtime_error::runtime_error;
};

constexpr int kMaxOps = 256;           // per `apply`
constexpr int kMaxDepth = 64;          // JSON nesting; deeper requests are refused while parsing (copies recurse)
// Work budget of one edit command in estimated main-thread ms, so it can't stall the game for long.
// Measured worst cases (M1 Pro, release): brushes walk whole columns (~20 ns per voxel of their box,
// strided), fills walk rows (~1 ns per voxel), and the game remeshes every chunk column the
// command's bounding box spans (~0.8 ms each).
constexpr double kBudgetMs = 50, kBrushNs = 20, kFillNs = 1, kColumnMs = 0.8;
constexpr int kMaxPathSamples = 4096;  // brush positions per `path`
constexpr int kMaxSamples = 4096;      // `heights` columns
constexpr size_t kMaxListed = 500;     // `instances` and `markers` entries
constexpr int kMaxShot[2] = {1600, 900};
constexpr std::string_view kTools[] = {"raise", "lower", "smooth", "flatten", "paint", "fill", "plant"};
static_assert(std::size(kTools) == size_t(Tool::Count));

// Numbers are accepted as floats too (rounded where needed) and range-checked before converting.
double checked(const json& v, std::string_view name, double lo, double hi) {
    double d = v.is_number() ? v.get<double>() : NAN;
    if (!(d >= lo && d <= hi)) throw BadArgs(std::format("'{}' must be a number in {}..{}", name, lo, hi));
    return d;
}
double number(const json& a, const char* key, double lo, double hi, std::optional<double> fallback = {}) {
    if (a.contains(key)) return checked(a[key], key, lo, hi);
    if (!fallback) throw BadArgs(std::format("missing '{}'", key));
    return *fallback;
}
int integer(const json& a, const char* key, int lo, int hi, std::optional<int> fallback = {}) {
    return int(std::lround(number(a, key, lo, hi, fallback)));
}
glm::vec3 point(const json& a, const char* key, glm::vec3 lo, glm::vec3 hi) {
    if (!a.contains(key) || !a[key].is_array() || a[key].size() != 3) throw BadArgs(std::format("'{}' must be [x, y, z]", key));
    glm::vec3 p;
    for (int i = 0; i < 3; ++i) p[i] = float(checked(a[key][size_t(i)], std::format("{}.{}", key, "xyz"[i]), lo[i], hi[i]));
    return p;
}
// A voxel corner inside the world (outside coordinates are clamped to it).
glm::ivec3 voxel(const json& a, const char* key, const Grid& g) {
    glm::vec3 p = point(a, key, glm::vec3(-1e6f), glm::vec3(1e6f));
    return glm::clamp(glm::ivec3(glm::round(p)), glm::ivec3(0), g.size - 1);
}
// Any block, by name in any case (prefab files take the same): Air clears, Water makes still water.
// Paint and path check for a solid one (checkEdit).
Block material(const json& a, const char* key) {
    std::string name = a.contains(key) && a[key].is_string() ? a[key].get<std::string>() : "";
    for (int b = 0; b < BlockCount; ++b)
        if (std::ranges::equal(name, std::string_view(kBlockNames[b]), [](unsigned char x, unsigned char y) { return std::tolower(x) == std::tolower(y); }))
            return Block(b);
    throw BadArgs(std::format("'{}' must be a block name, e.g. Grass, Planks, Ember or Air (the catalog command lists them)", key));
}
std::string str(const json& a, const char* key, std::string fallback) {
    if (!a.contains(key)) return fallback;
    if (!a[key].is_string()) throw BadArgs(std::format("'{}' must be a string", key));
    return a[key].get<std::string>();
}
bool flag(const json& a, const char* key, bool fallback) {
    if (!a.contains(key)) return fallback;
    if (!a[key].is_boolean()) throw BadArgs(std::format("'{}' must be true or false", key));
    return a[key].get<bool>();
}

json boxJson(const Box& b) { return b.empty() ? json() : json{{"min", {b.lo.x, b.lo.y, b.lo.z}}, {"max", {b.hi.x, b.hi.y, b.hi.z}}}; }
Box merge(const Box& a, const Box& b) { return a.empty() ? b : b.empty() ? a : Box{glm::min(a.lo, b.lo), glm::max(a.hi, b.hi)}; }

EditOp parseOp(std::string_view tool, const json& a, const Grid& g) {
    auto it = std::ranges::find(kTools, tool);
    if (it == std::end(kTools)) throw BadArgs(std::format("unknown tool '{}' (raise, lower, smooth, flatten, paint, fill, plant)", tool));
    EditOp op;
    op.tool = Tool(it - std::begin(kTools));
    if (op.tool == Tool::Fill) {
        op.min = voxel(a, "min", g);
        op.max = voxel(a, "max", g);
        op.block = material(a, "material");
    } else {
        op.center = {integer(a, "x", 0, g.size.x - 1), integer(a, "z", 0, g.size.z - 1)};
        op.radius = integer(a, "radius", 0, kMaxBrushRadius, 6);
        bool full = op.tool == Tool::Flatten || op.tool == Tool::Smooth;  // one command should finish the job
        op.strength = integer(a, "strength", 1, kMaxStrength, full ? kMaxStrength : 4);
        if (op.tool == Tool::Flatten) op.height = integer(a, "height", 0, g.size.y - 1);
        if (op.tool == Tool::Paint) op.block = material(a, "material");
        if (op.tool == Tool::Plant) {
            op.block = a.contains("kind") ? material(a, "kind") : ShortGrass;
            op.strength = int(std::lround(number(a, "density", 0.1, 6.4, 1) * 10));
        }
    }
    if (const char* error = checkEdit(g, op)) throw BadArgs(error);
    return op;
}

double volume(const Box& b) { return b.empty() ? 0 : double(b.size().x) * b.size().y * b.size().z; }

// Refuses a command whose estimated main-thread time (`ms` of work plus remeshing every chunk
// column `all` spans) is over the budget.
void budget(double ms, const Box& all, std::string_view advice) {
    glm::ivec3 chunks = all.empty() ? glm::ivec3(0) : all.hi / kChunk - all.lo / kChunk + 1;
    ms += chunks.x * chunks.z * kColumnMs;
    if (ms > kBudgetMs)
        throw BadArgs(std::format("over the work budget of one command (~{:.0f} ms of game time, at most {:.0f}; {})", ms, kBudgetMs, advice));
}

// Applies the ops as one undo step. Every op was checked when parsed, so none is rejected halfway.
json edit(const std::vector<EditOp>& ops, Grid& g, EditHistory& h, Box& dirty) {
    double ms = 0;
    Box all;
    for (const EditOp& op : ops) {
        Box b = editBox(g, op);
        ms += volume(b) * (op.tool == Tool::Fill ? kFillNs : kBrushNs) * 1e-6;
        all = merge(all, b);
    }
    budget(ms, all, "big brushes and a wide spread cost most): split it into smaller or nearer groups");
    bool joined = false;
    for (const EditOp& op : ops) {
        Box d = applyEdit(g, op, h, joined).dirty;
        joined |= !d.empty();
        dirty = merge(dirty, d);
    }
    return {{"changed", !dirty.empty()}, {"box", boxJson(dirty)}, {"ops", ops.size()}, {"undo_steps", h.undo.size()}};
}

// Roads and river beds: flatten discs along the polyline to a height that follows the terrain
// (averaged along the path so it climbs gently) or is fixed, minus `depth`, then paint them.
std::vector<EditOp> pathOps(const json& a, const Grid& g) {
    if (!a.contains("points") || !a["points"].is_array() || a["points"].size() < 2)
        throw BadArgs("'points' must be a list of at least two [x, z] points");
    std::vector<glm::vec2> pts;
    for (const json& p : a["points"]) {
        if (!p.is_array() || p.size() != 2) throw BadArgs("each point must be [x, z]");
        pts.push_back({checked(p[0], "point x", 0, g.size.x - 1), checked(p[1], "point z", 0, g.size.z - 1)});
    }
    int width = integer(a, "width", 1, 2 * kMaxBrushRadius + 1, 5), radius = width / 2, depth = integer(a, "depth", 0, 64, 0);
    bool follow = flag(a, "follow_terrain", true);
    int fixed = follow ? 0 : integer(a, "height", 0, g.size.y - 1);
    std::optional<Block> paint;
    if (a.contains("material") && !isOpaque(*(paint = material(a, "material")))) throw BadArgs("path material must be solid");

    // Brush centers every half radius, so the discs overlap into a continuous band.
    float step = std::max(1.0f, float(radius) / 2);
    std::vector<glm::ivec2> centers;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        float len = glm::length(pts[i + 1] - pts[i]);
        for (float t = 0; t < len + step; t += step) {
            glm::ivec2 c(glm::round(glm::mix(pts[i], pts[i + 1], std::min(t / std::max(len, 1e-6f), 1.0f))));
            if (centers.empty() || centers.back() != c) centers.push_back(c);
            if (centers.size() > size_t(kMaxPathSamples)) throw BadArgs("path too long: split it into several commands");
        }
    }
    std::vector<int> ground, heights;  // ground under the trees, so a forest doesn't lift the road
    for (glm::ivec2 c : centers) {
        int y = groundTop(g, c.x, c.y);
        while (y > 0 && (isTreeBlock(g.get({c.x, y, c.y})) || isClear(g.get({c.x, y, c.y})))) --y;
        ground.push_back(y);
    }
    int window = int(std::ceil(float(16 + 2 * radius) / step));  // samples on each side to average over
    for (size_t i = 0; i < centers.size(); ++i) {
        float sum = 0;
        int n = 0;
        for (size_t j = i > size_t(window) ? i - size_t(window) : 0; j <= std::min(centers.size() - 1, i + size_t(window)); ++j)
            if (ground[j] >= 0) sum += float(ground[j]), ++n;
        heights.push_back(std::clamp((follow ? (n ? int(std::lround(sum / float(n))) : kSeaLevel) : fixed) - depth, 0, g.size.y - 1));
    }
    std::vector<EditOp> ops;
    for (size_t i = 0; i < centers.size(); ++i)
        ops.push_back({.tool = Tool::Flatten, .center = centers[i], .radius = radius, .strength = kMaxStrength, .height = heights[i]});
    if (paint)
        for (glm::ivec2 c : centers) ops.push_back({.tool = Tool::Paint, .center = c, .radius = radius, .block = *paint});
    return ops;
}

// The region [x0..x1] x [z0..z1] of the args, clamped to the world (corners in any order).
std::pair<glm::ivec2, glm::ivec2> region(const json& a, const Grid& g) {
    glm::ivec2 p(integer(a, "x0", -1000000, 1000000), integer(a, "z0", -1000000, 1000000));
    glm::ivec2 q(integer(a, "x1", -1000000, 1000000), integer(a, "z1", -1000000, 1000000));
    glm::ivec2 hi(g.size.x - 1, g.size.z - 1);
    return {glm::clamp(glm::min(p, q), glm::ivec2(0), hi), glm::clamp(glm::max(p, q), glm::ivec2(0), hi)};
}

// Highest voxel that isn't air or a plant (surface) and opaque (ground) voxel of every step-th column from lo, -1 if none.
// Scans layers top-down so reads run along x rows instead of striding down each column.
struct Columns {
    int nx, nz;
    std::vector<int> surface, ground;
};
Columns columns(const Grid& g, glm::ivec2 lo, glm::ivec2 hi, int step) {
    Columns c{(hi.x - lo.x) / step + 1, (hi.y - lo.y) / step + 1, {}, {}};
    size_t n = size_t(c.nx) * size_t(c.nz), left = n;
    c.surface.assign(n, -1);
    c.ground.assign(n, -1);
    for (int y = g.size.y - 1; y >= 0 && left; --y)
        for (int j = 0; j < c.nz; ++j) {
            const Block* row = &g.cells[g.index({lo.x, y, lo.y + j * step})];
            for (int i = 0; i < c.nx; ++i) {
                size_t k = size_t(j) * size_t(c.nx) + size_t(i);
                Block b = row[i * step];
                if (c.ground[k] >= 0 || isClear(b)) continue;
                if (c.surface[k] < 0) c.surface[k] = y;
                if (isOpaque(b)) c.ground[k] = y, --left;
            }
        }
    return c;
}

json heights(const json& a, const Grid& g) {
    auto [lo, hi] = region(a, g);
    glm::ivec2 size = hi - lo + 1;
    int step = 1;
    while (int64_t((size.x + step - 1) / step) * ((size.y + step - 1) / step) > kMaxSamples) ++step;
    Columns c = columns(g, lo, hi, step);
    json rows = json::array(), tops = json::object();
    int minH = INT_MAX, maxH = INT_MIN;
    for (int j = 0; j < c.nz; ++j) {
        json row = json::array();
        for (int i = 0; i < c.nx; ++i) {
            size_t k = size_t(j) * size_t(c.nx) + size_t(i);
            int h = c.ground[k];
            row.push_back(h);
            minH = std::min(minH, h), maxH = std::max(maxH, h);
            if (c.surface[k] >= 0) {
                json& n = tops[kBlockNames[g.get({lo.x + i * step, c.surface[k], lo.y + j * step})]];
                n = n.is_null() ? 1 : n.get<int>() + 1;
            }
        }
        rows.push_back(std::move(row));
    }
    return {{"x0", lo.x}, {"z0", lo.y}, {"step", step}, {"columns", c.nx}, {"rows", c.nz}, {"min", minH}, {"max", maxH},
            {"sea_level", kSeaLevel}, {"heights", std::move(rows)}, {"top_blocks", std::move(tops)}};
}

json inspect(const json& a, const Grid& g) {
    glm::ivec3 p = voxel(a, "min", g), q = voxel(a, "max", g), lo = glm::min(p, q), hi = glm::max(p, q);
    glm::ivec3 size = hi - lo + 1;
    if (int64_t(size.x) * size.y * size.z > kMaxEditVolume) throw BadArgs("box too large (at most 128^3 voxels)");
    int64_t counts[BlockCount] = {};
    for (int y = lo.y; y <= hi.y; ++y)
        for (int z = lo.z; z <= hi.z; ++z)
            for (int x = lo.x; x <= hi.x; ++x) ++counts[g.cells[g.index({x, y, z})]];
    json blocks = json::object();
    for (int b = 0; b < BlockCount; ++b)
        if (counts[b]) blocks[kBlockNames[b]] = counts[b];
    return {{"box", boxJson({lo, hi})}, {"volume", int64_t(size.x) * size.y * size.z}, {"blocks", std::move(blocks)}};
}

// 3x5 pixel digits for grid labels, row-major from the top-left bit.
constexpr uint16_t kDigits[10] = {0x7B6F, 0x2C97, 0x73E7, 0x73CF, 0x5BC9, 0x79CF, 0x79EF, 0x7249, 0x7BEF, 0x7BCF};

// Top-down map: surface colors from the palette, hillshaded (light from the north-west), water
// tinted by depth, grid lines every `grid` blocks labelled with their coordinate.
json map(const json& a, const Grid& g, const std::vector<Marker>& markers) {
    auto [lo, hi] = region(a, g);
    int side = std::max(hi.x - lo.x, hi.y - lo.y) + 1, maxSize = integer(a, "max_size", 64, 1024, 1024);
    int step = (side + maxSize - 1) / maxSize, scale = std::clamp(maxSize / side, 1, 16);  // blocks per sample, pixels per sample
    Columns c = columns(g, lo, hi, step);
    int w = c.nx * scale, h = c.nz * scale;
    int nice = 16;
    while (side / nice > 10) nice *= 2;
    int grid = integer(a, "grid", 0, 1024, nice);

    int minH = INT_MAX, maxH = INT_MIN;
    for (int v : c.ground) minH = std::min(minH, v), maxH = std::max(maxH, v);
    auto at = [&](int i, int j) {
        size_t k = size_t(std::clamp(j, 0, c.nz - 1)) * size_t(c.nx) + size_t(std::clamp(i, 0, c.nx - 1));
        return std::pair{c.surface[k], c.ground[k]};
    };
    std::vector<uint8_t> rgb(size_t(w) * size_t(h) * 3);
    for (int j = 0; j < c.nz; ++j)
        for (int i = 0; i < c.nx; ++i) {
            auto [top, ground] = at(i, j);
            glm::vec3 color(0);
            if (top >= 0) {
                Block b = g.get({lo.x + i * step, top, lo.y + j * step}), under = g.get({lo.x + i * step, std::max(ground, 0), lo.y + j * step});
                bool water = b == Water;
                auto land = [&](int di, int dj) { auto [t, gr] = at(i + di, j + dj); return float(water ? gr : t); };
                float slope = (land(1, 0) - land(-1, 0) + land(0, 1) - land(0, -1)) / float(step);
                float shade = std::clamp(1 + 0.25f * slope, 0.45f, 1.5f) *
                              (0.8f + 0.35f * float(ground - minH) / float(std::max(1, maxH - minH)));
                glm::vec3 base = glm::vec3(kPalette[water ? under : b][0], kPalette[water ? under : b][1], kPalette[water ? under : b][2]) * shade;
                color = water ? glm::mix(base, glm::vec3(kPalette[Water][0], kPalette[Water][1], kPalette[Water][2]),
                                         std::clamp(0.45f + 0.07f * float(top - ground), 0.0f, 0.92f))
                              : base;
            }
            for (int y = j * scale; y < (j + 1) * scale; ++y)
                for (int x = i * scale; x < (i + 1) * scale; ++x)
                    for (int k = 0; k < 3; ++k)
                        rgb[(size_t(y) * size_t(w) + size_t(x)) * 3 + size_t(k)] = uint8_t(std::clamp(color[k], 0.0f, 1.0f) * 255);
        }

    auto pixel = [&](int x, int y, glm::vec3 color, float alpha) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        uint8_t* p = &rgb[(size_t(y) * size_t(w) + size_t(x)) * 3];
        for (int k = 0; k < 3; ++k) p[k] = uint8_t(float(p[k]) * (1 - alpha) + color[k] * 255 * alpha);
    };
    auto label = [&](int x, int y, int value) {  // digits at 2x, white on a dark box
        std::string s = std::to_string(value);
        for (int py = -1; py < 11; ++py)
            for (int px = -1; px < int(s.size()) * 8; ++px) pixel(x + px, y + py, glm::vec3(0), 0.6f);
        for (size_t d = 0; d < s.size(); ++d)
            for (int bit = 0; bit < 15; ++bit)
                if (kDigits[s[d] - '0'] >> (14 - bit) & 1)
                    for (int k = 0; k < 4; ++k) pixel(x + int(d) * 8 + bit % 3 * 2 + k % 2, y + bit / 3 * 2 + k / 2, glm::vec3(1), 1);
    };
    auto toPixel = [&](int block, int origin) { return (block - origin) * scale / step; };
    if (grid > 0) {
        for (int x = (lo.x + grid - 1) / grid * grid; x <= hi.x; x += grid)
            for (int y = 0; y < h; ++y) pixel(toPixel(x, lo.x), y, glm::vec3(0), 0.45f);
        for (int z = (lo.y + grid - 1) / grid * grid; z <= hi.y; z += grid)
            for (int x = 0; x < w; ++x) pixel(x, toPixel(z, lo.y), glm::vec3(0), 0.45f);
        for (int x = (lo.x + grid - 1) / grid * grid; x <= hi.x; x += grid) label(toPixel(x, lo.x) + 3, 3, x);
        for (int z = (lo.y + grid - 1) / grid * grid; z <= hi.y; z += grid) label(3, toPixel(z, lo.y) + 3, z);
    }
    // Markers: a dot per marker (regions also their circle) labelled with its id; names go in the JSON.
    constexpr glm::vec3 kMarkerColors[] = {{1, 1, 1}, {0.95f, 0.2f, 0.15f}, {1, 0.85f, 0.1f}, {0.2f, 0.9f, 1}};
    json drawn = json::array();
    float perBlock = float(scale) / float(step);
    for (const Marker& m : markers) {
        glm::vec2 p((m.pos.x - float(lo.x)) * perBlock, (m.pos.z - float(lo.y)) * perBlock);
        float r = m.kind == MarkerKind::Region ? m.radius * perBlock : 0;
        glm::vec2 nearest = glm::clamp(p, glm::vec2(0), glm::vec2(float(w), float(h)));
        if (glm::distance(p, nearest) > r) continue;  // misses the image
        glm::vec3 color = kMarkerColors[int(m.kind)];
        glm::vec2 farthest(p.x < float(w) / 2 ? float(w) : 0, p.y < float(h) / 2 ? float(h) : 0);
        // About one point per pixel of the circle, at most what the image's outline needs; none when
        // the disc covers the whole image (the circle is outside it).
        int steps = glm::distance(p, farthest) <= r ? 0 : int(std::min(2 * std::numbers::pi_v<float> * r, 4.0f * float(w + h))) + 1;
        for (int i = 0; i < steps && r > 0; ++i) {
            float t = float(i) / float(steps) * 2 * std::numbers::pi_v<float>;
            for (int k = 0; k < 4; ++k) pixel(int(p.x + r * std::cos(t)) + k % 2, int(p.y + r * std::sin(t)) + k / 2, color, 0.9f);
        }
        for (int dy = -5; dy <= 5; ++dy)
            for (int dx = -5; dx <= 5; ++dx)
                if (int d = dx * dx + dy * dy; d <= 25) pixel(int(p.x) + dx, int(p.y) + dy, d <= 10 ? color : glm::vec3(0), 1);
        label(int(p.x) + 8, int(p.y) - 5, int(m.id));
        if (drawn.size() < kMaxListed) drawn.push_back({{"id", m.id}, {"kind", kMarkerKindNames[int(m.kind)]}, {"name", m.name}, {"pixel", {int(p.x), int(p.y)}}});
    }
    std::string path = writePng("map", w, h, rgb.data());
    if (path.empty()) throw std::runtime_error("cannot write the map PNG");
    return {{"path", path}, {"width", w}, {"height", h}, {"x0", lo.x}, {"z0", lo.y}, {"x1", hi.x}, {"z1", hi.y},
            {"blocks_per_pixel", float(step) / float(scale)}, {"grid", grid}, {"min_height", minH}, {"max_height", maxH},
            {"sea_level", kSeaLevel}, {"markers", std::move(drawn)}};
}

Shot shot(const json& a, const GameView& v) {
    Shot s;
    s.eye = point(a, "position", {-1e6f, -100, -1e6f}, {1e6f, 1000, 1e6f});  // anywhere (the endless world); far off sees the sky
    if (a.contains("look_at")) {
        s.target = point(a, "look_at", {-1e6f, -100, -1e6f}, {1e6f, 1000, 1e6f});
    } else {
        float yaw = glm::radians(float(number(a, "yaw", -360, 360))), pitch = glm::radians(float(number(a, "pitch", -89, 89, 20)));
        s.target = s.eye - glm::vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch));
    }
    glm::vec3 d = s.target - s.eye;
    if (glm::length(d) < 0.01f) throw BadArgs("look_at must differ from position");
    if (std::abs(d.x) + std::abs(d.z) < 1e-3f * std::abs(d.y)) s.target.z += 1e-3f * std::abs(d.y);  // lookAt needs a horizontal hint straight down
    s.fovDegrees = float(number(a, "fov", 20, 120, v.fovDegrees));
    s.width = integer(a, "width", 64, kMaxShot[0], 1280);
    s.height = integer(a, "height", 64, kMaxShot[1], 720);
    s.timeOfDay = float(number(a, "time", 0, 1, v.timeOfDay));
    return s;
}

// A copy may only be a .flyw file directly in worlds/ or next to the world file (after resolving
// symlinks and ..), so a request can't write anywhere else.
std::string savePath(const std::string& path, const GameView& v) {
    namespace fs = std::filesystem;
    if (path.empty()) return path;
    std::error_code ec;
    auto resolve = [&](const std::string& p) { return fs::weakly_canonical(fs::absolute(p, ec), ec); };
    fs::path p = resolve(path);
    if (path.find('\0') != std::string::npos || p.extension() != ".flyw" ||
        (p.parent_path() != resolve("worlds/x").parent_path() && (v.path.empty() || p.parent_path() != resolve(v.path).parent_path())))
        throw BadArgs("'path' must be a .flyw file in worlds/ or in the world file's folder");
    return p.string();
}

const Prefab& prefabNamed(const json& a, const PrefabLibrary& lib) {
    std::string name = str(a, "prefab", "");
    if (const Prefab* p = lib.find(name)) return *p;
    throw BadArgs(std::format("unknown prefab '{}' (the prefabs command lists them and rescans prefabs/)", name));
}

int rotation(const json& a, int fallback) {  // degrees in, quarter turns out
    int degrees = integer(a, "rotation", 0, 270, fallback * 90);
    if (degrees % 90) throw BadArgs("'rotation' must be 0, 90, 180 or 270");
    return degrees / 90;
}

std::vector<float> params(const json& a, const Prefab& p) {
    json given = a.contains("params") ? a["params"] : json::object();
    if (!given.is_object()) throw BadArgs("'params' must be an object");
    for (const auto& [key, value] : given.items())
        if (std::ranges::find(p.params, key, &PrefabParam::name) == p.params.end())
            throw BadArgs(p.generated() ? std::format("'{}' has no parameter '{}' (the prefabs command lists them)", p.name, key)
                                        : std::format("'{}' is a structure: it takes no params", p.name));
    std::vector<float> v;
    for (const PrefabParam& q : p.params) {
        double d = number(given, q.name.c_str(), q.lo, q.hi, q.fallback);
        v.push_back(float(q.whole ? std::round(d) : d));
    }
    return v;
}

// An id argument: a whole number (1.9 isn't 1).
uint32_t wholeId(const json& a) {
    double id = number(a, "id", 1, 4294967295.0);
    if (id != std::floor(id)) throw BadArgs("'id' must be a whole number");
    return uint32_t(id);
}

uint32_t instanceId(const json& a, const std::vector<Instance>& list) {
    uint32_t id = wholeId(a);
    if (std::ranges::find(list, id, &Instance::id) == list.end())
        throw BadArgs(std::format("no instance {} (the instances command lists them)", id));
    return id;
}

json instanceJson(const Instance& in, const PrefabLibrary& lib) {
    json params = json::object();
    const Prefab* p = lib.find(in.prefab);
    for (size_t i = 0; i < in.params.size(); ++i)
        params[p && i < p->params.size() ? p->params[i].name : std::to_string(i)] = in.params[i];
    return {{"id", in.id}, {"prefab", in.prefab}, {"position", {in.pos.x, in.pos.y, in.pos.z}}, {"rotation", in.rotation * 90},
            {"seed", in.seed}, {"params", std::move(params)}, {"box", boxJson(instanceBox(in))}, {"cells", in.cells.size()}};
}

json prefabsJson(PrefabLibrary& lib, const std::vector<Instance>& instances) {
    lib = loadPrefabs(lib.dir);
    json list = json::array();
    for (const Prefab& p : lib.prefabs) {
        json e = {{"name", p.name}, {"kind", p.generated() ? "generated" : "structure"}, {"description", p.description}};
        if (p.generated()) {
            e["params"] = json::array();
            for (const PrefabParam& q : p.params)
                e["params"].push_back({{"name", q.name}, {"min", q.lo}, {"max", q.hi}, {"default", q.fallback}, {"integer", q.whole}});
        } else {
            e["size"] = {p.size.x, p.size.y, p.size.z};
            if (p.foundation != kKeep) e["foundation"] = kBlockNames[p.foundation];
            if (p.y) e["y"] = p.y;
            if (!p.markers.empty()) e["markers"] = p.markers.size();
        }
        list.push_back(std::move(e));
    }
    std::error_code ec;
    return {{"dir", std::filesystem::absolute(lib.dir, ec).string()}, {"prefabs", std::move(list)}, {"errors", lib.errors},
            {"blocks", kBlockNames}, {"instances", instances.size()}};
}

// What the MCP bridge builds its tool schemas from: the NPC kinds (the loaded defs) and the material names.
json catalogJson() {
    const Defs& d = defs();
    json kinds = json::array(), materials = json::array(), solid = json::array();
    for (size_t k = 0; k < d.creatures.size(); ++k) {
        const EntityType& t = d.creatures[k];
        json e = {{"id", d.creatureIds[k]}, {"name", t.name ? json(t.name) : json()}, {"level", t.level}, {"health", maxHealth(compute(t.stats, itemType(t.weapon).mods))},
                  {"temper", kTemperNames[int(t.temper)]}, {"attackable", t.attackable}, {"immortal", t.immortal}, {"talks", t.talks},
                  {"wander_radius", t.wanderRadius}, {"chase_speed", t.chaseSpeed}, {"aggro_range", t.aggroRange}};
        if (t.weapon) e["weapon"] = itemType(t.weapon).name;
        if (t.loot) e["loot"] = itemType(t.loot).name, e["loot_chance"] = t.lootChance;
        if (t.rareLoot) e["rare_loot"] = itemType(t.rareLoot).name, e["rare_loot_chance"] = t.rareChance;
        kinds.push_back(std::move(e));
    }
    for (int b = 0; b < BlockCount; ++b) {
        materials.push_back(kBlockNames[b]);
        if (isOpaque(Block(b))) solid.push_back(kBlockNames[b]);
    }
    return {{"entities", std::move(kinds)}, {"materials", std::move(materials)}, {"solid_materials", std::move(solid)}, {"blocks", kBlockNames}};
}

// The index of a[key] in `names`.
template <class Names> int named(const json& a, const char* key, const Names& names) {
    std::string s = str(a, key, ""), all;
    for (size_t i = 0; i < std::size(names); ++i) {
        if (s == names[i]) return int(i);
        all += (i ? ", " : "") + std::string(names[i]);
    }
    throw BadArgs(std::format("'{}' must be one of {}", key, all));
}

json markerJson(const Marker& m) {
    json j = {{"id", m.id}, {"kind", kMarkerKindNames[int(m.kind)]}, {"name", m.name}, {"x", int(std::floor(m.pos.x))},
              {"z", int(std::floor(m.pos.z))}, {"y", m.pos.y - 1}, {"radius", m.radius}};
    if (m.kind == MarkerKind::Spawn) j["entity"] = defs().creatureIds[size_t(m.entity)], j["count"] = m.count;
    if (!m.lines.empty()) j["lines"] = m.lines;
    if (m.activeFrom != m.activeTo)  // rounded: 0.8, not the float's 0.800000011920929
        j["active"] = {std::round(double(m.activeFrom) * 1e4) / 1e4, std::round(double(m.activeTo) * 1e4) / 1e4};
    if (m.owner) j["instance"] = m.owner;  // a prefab instance's: it moves and goes with it
    return j;
}

// Sets the fields `a` gives on `m` (a new one needs x and z). Markers stand at the center of column
// x, z on ground voxel y (default: the column's ground, under any tree).
void markerArgs(const json& a, const Grid& g, Marker& m, bool create) {
    if (m.kind != MarkerKind::Spawn && (a.contains("entity") || a.contains("count")))
        throw BadArgs("'entity' and 'count' are for spawn markers");
    if (a.contains("name")) m.name = str(a, "name", "");
    if (a.contains("lines")) {  // checkMarker (in putMarker) checks the count, lengths and text
        const json& lines = a["lines"];
        if (!lines.is_array() || lines.size() > kMaxMarkerLines || !std::ranges::all_of(lines, &json::is_string))
            throw BadArgs("'lines' must be an array of at most 8 strings");
        m.lines.clear();
        for (const json& line : lines) m.lines.push_back(line.get<std::string>());
    }
    if (a.contains("active")) {  // checkMarker (in putMarker) checks the window
        const json& t = a["active"];
        if (!t.is_array() || (!t.empty() && t.size() != 2))
            throw BadArgs("'active' must be [from, to] (times of day 0..1, e.g. [0.8, 0.2]: at night) or [] (always)");
        auto time = [](const json& v) {
            double d = v.is_number() ? v.get<double>() : NAN;
            if (!(d >= 0 && d < 1)) throw BadArgs("'active' times must be times of day from 0 up to, not including, 1 (0 midnight, 0.5 noon)");
            return std::min(float(d), std::nextafter(1.0f, 0.0f));  // e.g. 0.99999999 would round up to 1
        };
        m.activeFrom = t.empty() ? 0 : time(t[0]);
        m.activeTo = t.empty() ? 0 : time(t[1]);
    }
    if (create || a.contains("x") || a.contains("z") || a.contains("y")) {
        int x = integer(a, "x", 0, g.size.x - 1, create ? std::nullopt : std::optional(int(std::floor(m.pos.x))));
        int z = integer(a, "z", 0, g.size.z - 1, create ? std::nullopt : std::optional(int(std::floor(m.pos.z))));
        int y = integer(a, "y", -1, g.size.y - 1, groundUnderTrees(g, x, z));
        m.pos = {float(x) + 0.5f, float(y + 1), float(z) + 0.5f};
    }
    if (a.contains("entity")) m.entity = EntityKind(named(a, "entity", defs().creatureIds));
    if (create && m.kind == MarkerKind::Spawn && !a.contains("entity")) throw BadArgs("a spawn marker needs 'entity'");
    m.count = integer(a, "count", 1, kMaxSpawnCount, m.count);
    float fallback = m.kind == MarkerKind::Spawn ? typeOf(m.entity).wanderRadius : m.kind == MarkerKind::Poi ? 16 : m.kind == MarkerKind::Region ? 32 : 0;
    m.radius = float(number(a, "radius", 0, kMaxMarkerRadius, create ? fallback : m.radius));
}

uint32_t markerId(const json& a, const std::vector<Marker>& list) {
    uint32_t id = wholeId(a);
    if (std::ranges::find(list, id, &Marker::id) == list.end())
        throw BadArgs(std::format("no marker {} (the markers command lists them)", id));
    return id;
}

json run(const std::string& cmd, const json& a, Grid& g, std::vector<Instance>& list, std::vector<Marker>& markers,
         EditHistory& h, PrefabLibrary& lib, const GameView& v, Command& out) {
    // ponytail: the commands' coordinates assume a fixed world; in the endless one, look and move only
    if (g.streaming() && cmd != "state" && cmd != "screenshot" && cmd != "goto")
        throw BadArgs(std::format("'{}' needs a world file: the endless world can't be edited yet (start the game with --world)", cmd));
    if (cmd == "state")
        return {{"world", v.path}, {"size", {g.size.x, g.size.y, g.size.z}}, {"seed", v.seed}, {"sea_level", kSeaLevel},
                {"unsaved", v.unsaved}, {"undo_steps", h.undo.size()}, {"redo_steps", h.redo.size()},
                {"player", {v.player.x, v.player.y, v.player.z}}, {"camera", {v.eye.x, v.eye.y, v.eye.z}},
                {"camera_yaw", v.yawDegrees}, {"camera_pitch", v.pitchDegrees}, {"editor", v.editing},
                {"follow_claude", v.follow}, {"time_of_day", v.timeOfDay}, {"instances", list.size()}, {"markers", markers.size()}};
    if (std::ranges::find(kTools, cmd) != std::end(kTools)) return edit({parseOp(cmd, a, g)}, g, h, out.dirty);
    if (cmd == "apply") {
        if (!a.contains("ops") || !a["ops"].is_array() || a["ops"].empty() || a["ops"].size() > size_t(kMaxOps))
            throw BadArgs(std::format("'ops' must be a list of 1..{} ops", kMaxOps));
        std::vector<EditOp> ops;
        for (size_t i = 0; i < a["ops"].size(); ++i) {
            const json& op = a["ops"][i];
            try {
                ops.push_back(parseOp(op.is_object() ? str(op, "tool", "") : "", op, g));
            } catch (const BadArgs& e) {
                throw BadArgs(std::format("op {}: {}", i, e.what()));
            }
        }
        return edit(ops, g, h, out.dirty);
    }
    if (cmd == "path") return edit(pathOps(a, g), g, h, out.dirty);
    if (cmd == "undo" || cmd == "redo") {
        out.dirty = cmd == "undo" ? undoEdit(g, h, list, markers) : redoEdit(g, h, list, markers);
        return {{"changed", !out.dirty.empty()}, {"box", boxJson(out.dirty)}, {"undo_steps", h.undo.size()}, {"redo_steps", h.redo.size()}};
    }
    if (cmd == "save") {
        out.save = savePath(str(a, "path", ""), v);
        return json::object();
    }
    if (cmd == "prefabs") return prefabsJson(lib, list);
    if (cmd == "catalog") return catalogJson();
    if (cmd == "place") {
        const Prefab& p = prefabNamed(a, lib);
        Instance in;
        in.prefab = p.name;
        in.pos = {integer(a, "x", 0, g.size.x - 1), 0, integer(a, "z", 0, g.size.z - 1)};
        std::optional<int> y;
        if (a.contains("y")) y = integer(a, "y", 0, g.size.y - 1);
        in.rotation = rotation(a, 0);
        in.seed = uint32_t(number(a, "seed", 0, 4294967295.0, double(uint32_t(in.pos.x) * 73856093u ^ uint32_t(in.pos.z) * 19349663u)));
        in.params = params(a, p);
        in.pos.y = y.value_or(groundTop(g, in.pos.x, in.pos.z));
        Box box = prefabBox(g, p, in);
        budget(volume(box) * kBrushNs * 1e-6, box, "the prefab is too big");
        EditResult r = placePrefab(g, p, std::move(in), y, list, h, false, &markers);
        if (r.error) throw BadArgs(r.error);
        out.dirty = r.dirty;
        json made = json::array();  // the markers the prefab brings
        for (const Marker& m : markers)
            if (m.owner == r.id) made.push_back(markerJson(m));
        return {{"instance", instanceJson(*std::ranges::find(list, r.id, &Instance::id), lib)}, {"markers", std::move(made)},
                {"undo_steps", h.undo.size()}};
    }
    if (cmd == "remove") {
        uint32_t id = instanceId(a, list);
        Box box = instanceBox(*std::ranges::find(list, id, &Instance::id));
        budget(volume(box) * kBrushNs * 1e-6, box, "the instance is too big");
        size_t had = markers.size();
        EditResult r = removePrefab(g, id, list, h, &markers);
        out.dirty = r.dirty;
        return {{"removed", r.id}, {"box", boxJson(r.dirty)}, {"markers_removed", had - markers.size()}, {"undo_steps", h.undo.size()}};
    }
    if (cmd == "move") {
        uint32_t id = instanceId(a, list);
        const Instance& in = *std::ranges::find(list, id, &Instance::id);
        const Prefab* p = lib.find(in.prefab);
        if (!p) throw BadArgs(std::format("prefab '{}' isn't loaded anymore: remove the instance instead", in.prefab));
        Instance to = in;
        to.pos = {integer(a, "x", 0, g.size.x - 1), 0, integer(a, "z", 0, g.size.z - 1)};
        std::optional<int> y;
        if (a.contains("y")) y = integer(a, "y", 0, g.size.y - 1);
        to.rotation = rotation(a, in.rotation);
        to.pos.y = y.value_or(groundTop(g, to.pos.x, to.pos.z));  // before the removal: close enough for the estimate
        if (const char* error = checkPlacement(g, *p, to)) throw BadArgs(error);  // e.g. params from a file
        Box from = instanceBox(in), box = prefabBox(g, *p, to);
        budget((volume(from) + volume(box)) * kBrushNs * 1e-6, merge(from, box), "a far move costs most: remove it and place it again");
        EditResult r = movePrefab(g, *p, id, {to.pos.x, to.pos.z}, y, to.rotation, list, h, &markers);
        if (r.error) throw BadArgs(r.error);
        out.dirty = r.dirty;
        return {{"instance", instanceJson(*std::ranges::find(list, id, &Instance::id), lib)}, {"undo_steps", h.undo.size()}};
    }
    if (cmd == "meadow") {  // like replant: one undo step for a region, no work budget (whole world: about a second)
        bool all = !a.contains("x0") && !a.contains("z0") && !a.contains("x1") && !a.contains("z1");
        auto [lo, hi] = all ? std::pair(glm::ivec2(0), glm::ivec2(g.size.x - 1, g.size.z - 1)) : region(a, g);
        Block kind = a.contains("kind") ? material(a, "kind") : ShortGrass;
        if (kind != Air && !isPlant(kind)) throw BadArgs("kind must be a plant (ShortGrass for the meadow mix) or Air to clear");
        float density = float(number(a, "density", 0.1, 6.4, 1));
        EditResult e = growMeadow(g, lo, hi, kind, density, h);
        out.dirty = e.dirty;
        int64_t plants = 0;
        if (!e.dirty.empty())
            for (int y = e.dirty.lo.y; y <= e.dirty.hi.y; ++y)
                for (int z = e.dirty.lo.z; z <= e.dirty.hi.z; ++z)
                    for (int x = e.dirty.lo.x; x <= e.dirty.hi.x; ++x) plants += isPlant(g.get({x, y, z}));
        return {{"changed", !e.dirty.empty()}, {"plants", plants}, {"box", boxJson(e.dirty)}, {"undo_steps", h.undo.size()}};
    }
    if (cmd == "replant") {  // a one-off for worlds made before ADR-029: no work budget, it may take a few seconds
        bool all = !a.contains("x0") && !a.contains("z0") && !a.contains("x1") && !a.contains("z1");
        auto [lo, hi] = all ? std::pair(glm::ivec2(0), glm::ivec2(g.size.x - 1, g.size.z - 1)) : region(a, g);
        bool dry = flag(a, "dry_run", false);
        ReplantReport r;
        EditResult e = replantTrees(g, lib, v.seed, lo, hi, dry, list, markers, h, r);
        out.dirty = e.dirty;
        return {{"dry_run", dry}, {"removed_trees", r.removedTrees}, {"removed_voxels", r.removedVoxels}, {"kept_clusters", r.keptClusters},
                {"regrown", r.regrown}, {"planted", r.planted}, {"skipped", r.skipped}, {"box", boxJson(e.dirty)}, {"undo_steps", h.undo.size()}};
    }
    if (cmd == "instances") {
        bool all = !a.contains("x0") && !a.contains("z0") && !a.contains("x1") && !a.contains("z1");
        auto [lo, hi] = all ? std::pair(glm::ivec2(0), glm::ivec2(g.size.x - 1, g.size.z - 1)) : region(a, g);
        json found = json::array();
        size_t count = 0;
        for (const Instance& in : list) {
            Box b = instanceBox(in);
            if (b.hi.x < lo.x || b.lo.x > hi.x || b.hi.z < lo.y || b.lo.z > hi.y) continue;
            if (++count <= kMaxListed) found.push_back(instanceJson(in, lib));
        }
        return {{"count", count}, {"listed", found.size()}, {"instances", std::move(found)}};
    }
    if (cmd == "markers") {
        bool all = !a.contains("x0") && !a.contains("z0") && !a.contains("x1") && !a.contains("z1");
        auto [lo, hi] = all ? std::pair(glm::ivec2(0), glm::ivec2(g.size.x - 1, g.size.z - 1)) : region(a, g);
        int kind = a.contains("kind") ? named(a, "kind", kMarkerKindNames) : -1;
        json found = json::array();
        size_t count = 0;
        for (const Marker& m : markers) {
            glm::vec2 p(m.pos.x, m.pos.z), nearest = glm::clamp(p, glm::vec2(lo), glm::vec2(hi + 1));  // a region's circle may reach in
            if ((kind >= 0 && int(m.kind) != kind) || glm::distance(p, nearest) > (m.kind == MarkerKind::Region ? m.radius : 0)) continue;
            if (++count <= kMaxListed) found.push_back(markerJson(m));
        }
        return {{"count", count}, {"listed", found.size()}, {"markers", std::move(found)}};
    }
    if (cmd == "mark" || cmd == "edit_marker") {
        Marker m;
        if (cmd == "mark") m.kind = MarkerKind(named(a, "kind", kMarkerKindNames));
        else m = *std::ranges::find(markers, markerId(a, markers), &Marker::id);
        markerArgs(a, g, m, cmd == "mark");
        EditResult r = putMarker(g, markers, h, m);
        if (r.error) throw BadArgs(r.error);
        out.dirty = r.dirty;
        return {{"marker", markerJson(*std::ranges::find(markers, r.id, &Marker::id))}, {"undo_steps", h.undo.size()}};
    }
    if (cmd == "unmark") {
        EditResult r = removeMarker(g, markers, h, markerId(a, markers));
        out.dirty = r.dirty;
        return {{"removed", r.id}, {"undo_steps", h.undo.size()}};
    }
    if (cmd == "heights") return heights(a, g);
    if (cmd == "inspect") return inspect(a, g);
    if (cmd == "map") return map(a, g, markers);
    if (cmd == "screenshot") {
        out.shot = shot(a, v);
        return json::object();
    }
    if (cmd == "goto") {
        glm::vec3 lo = g.streaming() ? glm::vec3(-1e6f, 0, -1e6f) : glm::vec3(g.lo) - glm::vec3(512, 0, 512);  // the endless world: anywhere
        glm::vec3 hi = g.streaming() ? glm::vec3(1e6f, 1000, 1e6f) : glm::vec3(float(g.hi().x) + 513, 1000, float(g.hi().z) + 513);
        Goto go{point(a, "position", lo, hi), {}};
        if (a.contains("look_at")) go.lookAt = point(a, "look_at", glm::vec3(-1e6f), glm::vec3(1e6f));
        out.go = go;
        return {{"moved", v.follow}, {"note", v.follow ? "the user's camera flies there" : "Follow Claude is off in the Editor panel"}};
    }
    throw BadArgs(std::format("unknown command '{}'", cmd));
}

}  // namespace

Command runCommand(std::string_view request, Grid& world, std::vector<Instance>& instances, std::vector<Marker>& markers,
                   EditHistory& history, PrefabLibrary& prefabs, const GameView& view) {
    Command out;
    json id;
    try {
        json req = json::parse(request, [](int depth, json::parse_event_t, json&) {
            if (depth > kMaxDepth) throw BadArgs(std::format("request nested deeper than {} levels", kMaxDepth));
            return true;
        });
        if (!req.is_object() || !req.contains("cmd") || !req["cmd"].is_string()) throw BadArgs("request must be {\"id\", \"cmd\", \"args\"}");
        id = req.value("id", json());
        json args = req.value("args", json::object());
        if (!args.is_object()) throw BadArgs("'args' must be an object");
        std::string cmd = req["cmd"].get<std::string>();
        std::string brief = args.dump();
        std::erase(brief, '"');
        out.summary = cmd + " " + (brief.size() > 56 ? brief.substr(0, 53) + "..." : brief);
        out.response = {{"id", id}, {"ok", true}, {"result", run(cmd, args, world, instances, markers, history, prefabs, view, out)}};
    } catch (const std::exception& e) {  // bad JSON or args: thrown before anything changed
        std::string error = e.what();
        std::ranges::replace_if(error, [](unsigned char c) { return c >= 0x80; }, '?');  // parse errors quote raw bytes: keep it valid UTF-8
        out.response = {{"id", id}, {"ok", false}, {"error", error}};
        out.summary = (out.summary + ": " + error).substr(0, 120);
    }
    return out;
}

std::string writePng(const char* name, int w, int h, const uint8_t* rgb) {
    static std::atomic<int> counter;
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::temp_directory_path(ec) / "flycoos";
    std::filesystem::create_directories(dir, ec);
    auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))  // earlier ones, read by now
        if (it->path().extension() == ".png" && it->last_write_time(ec) < old) std::filesystem::remove(it->path(), ec);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    std::string path = std::filesystem::absolute(dir / std::format("{}-{}-{}.png", name, ms, counter++), ec).string();
    return !ec && writePngFile(path.c_str(), w, h, rgb) ? path : "";
}

bool writePngFile(const char* path, int w, int h, const uint8_t* rgb) { return stbi_write_png(path, w, h, 3, rgb, w * 3); }
