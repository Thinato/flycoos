#include "stream.h"

#include <algorithm>
#include <chrono>
#include <cstring>

static_assert(kMaxLight < kChunk, "lighting a column changes only its 8 neighbours (the stage rules)");

Stream::Stream(Grid& world, Light& light, uint32_t seed, int threads)
    : world_(world), light_(light), seed_(seed), count_(world.size.x / kChunk, world.size.z / kChunk),
      center_((glm::ivec2(world.lo.x, world.lo.z) >> 5) + count_ / 2), columns_(size_t(count_.x * count_.y)) {
    glm::ivec2 lo = center_ - count_ / 2;
    for (int z = 0; z < count_.y; ++z)
        for (int x = 0; x < count_.x; ++x) columns_[slot(lo + glm::ivec2(x, z))].at = lo + glm::ivec2(x, z);
    requeue();
    for (int i = 0; i < std::max(1, threads); ++i) workers_.emplace_back([this] { work(); });
}

Stream::~Stream() {
    {
        std::lock_guard lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    for (std::thread& t : workers_) t.join();
}

size_t Stream::slot(glm::ivec2 c) const { return size_t((c.y & (count_.y - 1)) * count_.x + (c.x & (count_.x - 1))); }

Stream::Column* Stream::column(glm::ivec2 c) {
    Column& col = columns_[slot(c)];
    return col.at == c ? &col : nullptr;
}

Stream::Stage Stream::stage(glm::ivec3 p) const { return columns_[slot(glm::ivec2(p.x, p.z) >> 5)].stage; }

bool Stream::meshedAround(glm::vec3 p, int radius) const {
    glm::ivec2 c = glm::ivec2(glm::floor(glm::vec2(p.x, p.z))) >> 5;
    for (int dz = -radius; dz <= radius; ++dz)
        for (int dx = -radius; dx <= radius; ++dx) {
            const Column& col = columns_[slot(c + glm::ivec2(dx, dz))];
            if (col.at != c + glm::ivec2(dx, dz) || col.stage != Stage::Meshed) return false;
        }
    return true;
}

void Stream::requeue() {
    std::vector<glm::ivec2> queued;
    for (const Column& c : columns_)
        if (c.stage == Stage::Queued) queued.push_back(c.at);
    std::ranges::sort(queued, {}, [&](glm::ivec2 c) { glm::ivec2 d = c - center_; return d.x * d.x + d.y * d.y; });
    {
        std::lock_guard lock(mutex_);
        jobs_.clear();
        for (glm::ivec2 c : queued)
            if (std::ranges::find(working_, c) == working_.end() && std::ranges::find(done_, c, &Job::at) == done_.end())
                jobs_.push_back(c);
    }
    wake_.notify_all();
}

void Stream::work() {
    for (;;) {
        glm::ivec2 at;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            at = jobs_.front();
            jobs_.pop_front();
            working_.push_back(at);
        }
        auto start = std::chrono::steady_clock::now();
        Grid g({kChunk, world_.size.y, kChunk}, glm::ivec3(at.x, 0, at.y) * kChunk);
        generateChunkColumn(g, at, seed_);
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::lock_guard lock(mutex_);
        done_.push_back({at, std::move(g.cells)});
        std::erase(working_, at);
        generateSum_ += ms;
        ++generateCount_;
    }
}

void Stream::follow(glm::vec3 center) {
    glm::ivec2 c = glm::ivec2(glm::floor(glm::clamp(glm::vec2(center.x, center.z), glm::vec2(-1e6f), glm::vec2(1e6f)))) >> 5;
    if (c == center_) return;
    center_ = c;
    glm::ivec2 lo = c - count_ / 2;
    world_.lo = glm::ivec3(lo.x, 0, lo.y) * kChunk;
    light_.lo = world_.lo;
    stats_.dropped = 0;
    for (int z = 0; z < count_.y; ++z)
        for (int x = 0; x < count_.x; ++x) {
            glm::ivec2 at = lo + glm::ivec2(x, z);
            Column& col = columns_[slot(at)];
            if (col.at == at) continue;
            glm::ivec3 o(at.x * kChunk, 0, at.y * kChunk);  // drop what the slot held: air, dark, nothing drawn
            for (int y = 0; y < world_.size.y; ++y)
                for (int zz = 0; zz < kChunk; ++zz) std::fill_n(&world_.cells[world_.index(o + glm::ivec3(0, y, zz))], kChunk, Air);
            clearColumn(light_, o);
            for (int y = 0; y < world_.size.y; y += kChunk) empty_.push_back(world_.chunkSlot(o + glm::ivec3(0, y, 0)));
            col = {at, Stage::Queued};
            ++stats_.dropped;
        }
    requeue();
}

Stream::Update Stream::update(double lightMs) {
    Update out;
    out.empty.swap(empty_);
    auto start = std::chrono::steady_clock::now();
    std::vector<Job> done;
    {
        std::lock_guard lock(mutex_);
        done.swap(done_);
        if (generateCount_) stats_.generateMs = generateSum_ / generateCount_;
    }
    stats_.installed = 0;
    for (const Job& job : done) {
        Column* col = column(job.at);
        if (!col || col->stage != Stage::Queued) continue;  // left the window while it was generated
        glm::ivec3 o(job.at.x * kChunk, 0, job.at.y * kChunk);
        for (int y = 0; y < world_.size.y; ++y)
            for (int z = 0; z < kChunk; ++z)  // chunk-aligned rows never cross the wrap seam
                std::memcpy(&world_.cells[world_.index(o + glm::ivec3(0, y, z))], &job.cells[size_t(y * kChunk + z) * kChunk], kChunk);
        clearColumn(light_, o);  // dark until it's lit (nothing may have lit the air it was)
        col->stage = Stage::Generated;
        ++stats_.installed;
    }
    auto mid = std::chrono::steady_clock::now();
    stats_.installMs = std::chrono::duration<double, std::milli>(mid - start).count();

    // Nearest first: what the player sees comes in before the far edge.
    std::vector<Column*> order;
    for (Column& c : columns_) order.push_back(&c);
    std::ranges::sort(order, {}, [&](const Column* c) { glm::ivec2 d = c->at - center_; return d.x * d.x + d.y * d.y; });
    auto around = [&](glm::ivec2 at, Stage least) {  // all 8 neighbours in the window and at `least`
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                const Column* n = column(at + glm::ivec2(dx, dz));
                if ((dx || dz) && (!n || n->stage < least)) return false;
            }
        return true;
    };
    stats_.litNow = 0;
    for (Column* c : order) {
        if (lightMs > 0 && stats_.litNow &&
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mid).count() >= lightMs)
            break;
        if (c->stage != Stage::Generated || !around(c->at, Stage::Generated)) continue;
        glm::ivec2 lo = c->at * kChunk;
        lightColumns(world_, light_, lo, lo + kChunk - 1);
        c->stage = Stage::Lit;
        ++stats_.litNow;
    }
    stats_.lightMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mid).count();
    for (Column* c : order)
        if (c->stage == Stage::Lit && around(c->at, Stage::Lit)) {
            c->stage = Stage::Meshed;
            for (int y = 0; y < world_.size.y; y += kChunk) out.mesh.push_back(world_.chunkSlot(glm::ivec3(c->at.x * kChunk, y, c->at.y * kChunk)));
        }
    return out;
}

Stream::Stats Stream::stats() const {
    Stats s = stats_;
    s.queued = s.generated = s.lit = s.meshed = 0;
    for (const Column& c : columns_) {
        int* n[] = {&s.queued, &s.generated, &s.lit, &s.meshed};
        ++*n[int(c.stage)];
    }
    return s;
}
