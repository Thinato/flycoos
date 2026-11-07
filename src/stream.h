#pragma once
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "light.h"
#include "world.h"

// Keeps a streamed grid (Grid::streamed) centred on a point, the player, as it moves through the
// endless world (docs/DECISIONS.md ADR-031). Its chunk columns (kChunk x height x kChunk) go:
//   Queued    worker threads generate it (generateChunkColumn), nearest to the centre first;
//   Generated update() copied it into the grid (its light dark);
//   Lit       once its 8 neighbours were generated: lightColumns;
//   Meshed    once its 8 neighbours were lit: update() hands out its chunk slots to mesh.
// So a chunk is meshed only when every voxel and light value its faces and AO read is final, and
// light, raised in any order, ends up exact. A column that leaves the window is dropped: its
// voxels turn to air, its light dark, and its chunk slots go out to be emptied.
class Stream {
public:
    enum class Stage : uint8_t { Queued, Generated, Lit, Meshed };

    Stream(Grid& world, Light& light, uint32_t seed, int threads);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // Moves the window, whole chunk columns at a time, so `center` is in its middle column. The
    // columns that left are dropped and the ones that came in queued. Does nothing while it's there.
    void follow(glm::vec3 center);

    struct Update {
        std::vector<size_t> mesh;   // chunk slots to (re)mesh: their columns just became Meshed
        std::vector<size_t> empty;  // chunk slots whose column was dropped: draw nothing there
    };
    // Main thread, once a frame: installs the columns the workers finished, lights columns (nearest
    // first) until `lightMs` is spent (at least one; 0: all that can be), and returns what to mesh and empty.
    Update update(double lightMs);

    Stage stage(glm::ivec3 p) const;  // of p's column (inside the window)
    bool generated(glm::ivec3 p) const { return world_.inside(p) && stage(p) != Stage::Queued; }
    bool meshedAround(glm::vec3 p, int radius) const;  // every column within `radius` columns of p's is Meshed

    struct Stats {
        int queued = 0, generated = 0, lit = 0, meshed = 0;  // columns at each stage
        int installed = 0, litNow = 0, dropped = 0;          // by the last update() / follow()
        double installMs = 0, lightMs = 0;                   // the last update()'s
        double generateMs = 0;                               // workers' average per column
    };
    Stats stats() const;

private:
    struct Column {
        glm::ivec2 at;  // chunk coordinates of the column this slot holds
        Stage stage = Stage::Queued;
    };
    struct Job {
        glm::ivec2 at;
        std::vector<Block> cells;  // kChunk x height x kChunk, Grid order
    };
    size_t slot(glm::ivec2 c) const;  // of chunk column c
    Column* column(glm::ivec2 c);     // the slot holding c, if it does
    void requeue();                   // rebuilds the job queue from the Queued columns, nearest first
    void work();

    Grid& world_;
    Light& light_;
    const uint32_t seed_;
    const glm::ivec2 count_;  // columns in the window
    glm::ivec2 center_;       // the window's middle column
    std::vector<Column> columns_;
    Stats stats_;

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<glm::ivec2> jobs_;  // chunk columns to generate, nearest first
    std::vector<Job> done_;        // generated, not yet installed
    double generateSum_ = 0;
    int generateCount_ = 0;
    std::vector<glm::ivec2> working_;  // being generated
    std::vector<size_t> empty_;        // chunk slots dropped since the last update()
    bool quit_ = false;
    std::vector<std::thread> workers_;
};
