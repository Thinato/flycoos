#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include "edit.h"
#include "fx.h"
#include "light.h"
#include "mesher.h"
#include "model.h"
#include "plants.h"
#include "render_stats.h"

struct ImDrawData;

struct ModelInstance {
    std::array<Model, PartCount> parts;  // the model each part is drawn from (worn armor swaps some)
    std::array<glm::mat4, PartCount> pose;
    bool glow = false;                   // drawn in its own colours, unshaded (a burst's fire, ADR-053)
    glm::vec4 tint{0};                   // rgb (sRGB; above 1 brighter) it turns toward, unshaded, by a (0..1): a charge's glow, a flash (ADR-060)
};

struct FrameParams {
    glm::mat4 view{1};
    float fovY = 1.2f;       // radians; aspect comes from the swapchain
    glm::vec3 eye{};
    float timeOfDay = 0.3f;  // 0 midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset
    bool underwater = false; // the eye is under water (underWater): blue fog and sky
    double seconds = 0;      // real time (benchmarks: ticks), for the grass swaying in the wind
    std::vector<ModelInstance> models;  // player and NPCs
    const Fx* fx = nullptr;  // hit sparks, blood, chips and blade smears (ADR-058); null: none
};

// Debug-view switches (debug UI "Render" tab).
enum ViewMode { ViewShaded, ViewQuads, ViewChunks, ViewAO, ViewNormals, ViewLight, ViewModeCount };  // = Frame.debug.x
struct RenderSettings {
    int viewMode = ViewShaded;
    bool wireframe = false;  // quad edges drawn over the scene
    bool sky = true, fog = true, water = true, character = true;
    bool frustumCulling = true;
    bool freezeCulling = false;  // keep the current visible set, then fly away to inspect it
    // Lighting: AO levels 0..3 map through aoCurve; occluded albedo shades toward tintColor (sRGB).
    bool occlusionRamp = true;  // off: linear AO, no tint
    glm::vec4 aoCurve{0.35f, 0.62f, 0.82f, 1.0f};
    glm::vec3 tintColor{0.55f, 0.70f, 1.0f};
    float tintStrength = 0.8f;
    // Voxel light (light.h): ambient and occlusion scale by curve(sky), sun diffuse fades in over
    // sky visibility sunGate..1, embers add emberColor (sRGB) * emberStrength * curve(block).
    bool voxelLight = true;  // off: everything under open sky, no ember light (A/B)
    float sunGate = 0.6f;
    float lightFalloff = 2;  // curve(l) = x / (f - (f-1) x), x = l/15: 1 linear, higher darker
    glm::vec3 emberColor{1.0f, 0.8f, 0.6f};
    float emberStrength = 1.5f;
    float darkAmbient = 0.03f;  // ambient left where no light reaches
    // Colour patches (ADR-028): every tone's strength and size times these; tones edited live.
    float patchStrength = 1, patchSize = 1;  // strength 0: the flat palette
    std::array<BlockTone, BlockCount> tones = kTones;
    float leafShade = 1;  // tree crowns: 0 every shade its kind's colour, 1 the kLeafRamp steps (docs/ART.md)
    // Plants (plants.h): drawn within plantRadius blocks of the eye, growing in over its last 12.
    bool plants = true;
    float plantRadius = 64;
    float sway = 1;  // wind: 0 still
};

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    bool init(SDL_Window* window, bool vsync);
    void uploadWorld(const Grid& world);  // lights (fillLight), meshes every chunk (meshChunks) and uploads
    // After an edit: relights around `dirty`, then remeshes (remeshChunks) and re-uploads only the
    // chunks overlapping `dirty` and the voxels whose light changed, grown by one voxel (faces and AO
    // look at neighbours).
    void updateWorld(const Grid& world, const Box& dirty);
    // A streamed world (stream.h): starts with no chunks drawn and all dark; the Stream lights
    // light() and says which chunk slots to mesh or empty (updateChunks), as they come and go.
    void streamWorld(const Grid& world);
    Light& light() { return light_; }
    void updateChunks(const Grid& world, const std::vector<size_t>& mesh, const std::vector<size_t>& empty);
    void uploadModels();  // character and NPC models
    // Draws one frame, then the debug UI (if any) on top. With screenshotPath set, also writes
    // the frame to a BMP.
    bool render(const FrameParams& frame, const RenderSettings& settings, ImDrawData* ui = nullptr,
                const char* screenshotPath = nullptr);

    // Renders the scene (no UI) from `frame` into offscreen w x h targets and reads it back as RGB8
    // (sRGB), leaving the window's targets and stats alone. Blocks until the GPU is done.
    bool snapshot(const FrameParams& frame, const RenderSettings& settings, uint32_t w, uint32_t h,
                  std::vector<uint8_t>& rgb);

    SDL_GPUDevice* device() const { return device_; }
    SDL_GPUTextureFormat colorFormat() const { return colorFormat_; }

    RenderStats stats;

private:
    struct Slot { uint32_t first = 0, count = 0, cap = 0; };  // a chunk's quads in a world buffer
    struct ChunkDraw { glm::ivec3 origin; std::array<Slot, 2> slot; };  // [0] opaque, [1] water
    struct Upload { SDL_GPUBuffer* buffer; uint32_t offset, from, bytes; };  // to buffer offset, from data offset

    SDL_GPUShader* shader(const std::vector<char>& source, const char* entry, SDL_GPUShaderStage stage,
                          uint32_t uniforms, uint32_t storage);
    SDL_GPUGraphicsPipeline* pipeline(SDL_GPUShader* vs, SDL_GPUShader* fs, bool depthTest, bool depthWrite, bool blend,
                                      SDL_GPUCullMode cull, SDL_GPUFillMode fill = SDL_GPU_FILLMODE_FILL);
    SDL_GPUBuffer* upload(const void* data, uint32_t bytes, uint32_t capacity = 0);  // capacity: bytes, >= data
    void write(const void* data, uint32_t bytes, const std::vector<Upload>& uploads);
    void packWorld();  // with headroom_ quads to spare beyond an eighth more
    void uploadChunks(const Grid& world, const std::vector<size_t>& ids);
    // A storage buffer rewritten every frame through a cycled transfer buffer (frames in flight keep theirs).
    struct Streamed { SDL_GPUBuffer* buffer = nullptr; SDL_GPUTransferBuffer* transfer = nullptr; uint32_t cap = 0; };  // cap: bytes
    // Copies `bytes` of `data` into it (growing it first if needed); false (nothing to draw) if it failed.
    bool stream(SDL_GPUCommandBuffer* cmd, Streamed& s, const void* data, uint32_t bytes);  // meshes_[ids] into their slots (repacking if they don't fit)
    SDL_GPUTexture* texture(SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, SDL_GPUSampleCount samples,
                            uint32_t w, uint32_t h);
    bool ensureTargets(uint32_t w, uint32_t h);
    void drawScene(SDL_GPUCommandBuffer* cmd, const FrameParams& frame, const RenderSettings& settings,
                   SDL_GPUTexture* msaa, SDL_GPUTexture* resolved, SDL_GPUTexture* depthTexture, uint32_t w, uint32_t h,
                   bool onScreen);
    bool readPixels(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* texture, uint32_t w, uint32_t h, std::vector<uint8_t>& rgba);
    bool saveScreenshot(SDL_GPUCommandBuffer* cmd, const char* path);

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTextureFormat colorFormat_{};
    SDL_GPUSampleCount samples_ = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUGraphicsPipeline *sky_ = nullptr, *opaque_ = nullptr, *water_ = nullptr, *wire_ = nullptr, *plants_ = nullptr;
    SDL_GPUGraphicsPipeline *particles_ = nullptr, *ribbon_ = nullptr;  // hit effects (ADR-058): voxel bits, the blade's smear
    glm::mat4 cullViewProj_{1};
    SDL_GPUBuffer* models_ = nullptr;
    std::array<SDL_GPUBuffer*, 2> world_{};  // opaque, water quads of every chunk
    std::array<uint32_t, 2> worldUsed_{}, worldCap_{};  // quads allocated to slots, buffer size in quads
    SDL_GPUTexture *msaaColor_ = nullptr, *depth_ = nullptr, *resolved_ = nullptr;
    uint32_t targetW_ = 0, targetH_ = 0;
    std::vector<ChunkDraw> chunks_;  // every chunk, empty ones too, in chunkOrigin order
    std::vector<Mesh> meshes_;       // CPU copy of every chunk's mesh, for repacking
    Light light_;                    // the world's light, kept in step with it by updateWorld
    glm::ivec3 worldLo_{};           // the world's window's low corner (plant cells are modulo 1024)
    uint32_t headroom_ = 65536;      // opaque quads packWorld leaves spare: ~0.5 MB, or ~16 MB for a streamed world
    std::array<std::array<std::pair<uint32_t, uint32_t>, PartCount>, ModelCount> parts_{};  // first quad, quad count
    std::array<std::pair<uint32_t, uint32_t>, 2 * PlantModelCount> plantParts_{};  // [model * 2 + far], in models_ too
    // This frame's plants near the eye, grouped by plantParts_ index, uploaded to plantInstances_ every frame.
    std::vector<PlantInstance> plantStage_;
    struct FxInstance { glm::vec4 posSize; glm::uvec4 info; };  // particle_vs's: x colour rgb8, glow bit 24, flat bit 25; y light
    std::vector<FxInstance> fxStage_;
    Streamed plantInstances_;
    Streamed fxParticles_, fxRibbon_;  // this frame's hit effects (ADR-058)
    double windPhase_ = 0, windSeconds_ = 0;  // advanced by render() at the Wind speed, kept in 0..2 pi
};
