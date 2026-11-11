#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <print>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include <imgui_impl_sdlgpu3.h>

#include "mesher.h"

namespace {

// Must match struct Frame / Draw in shaders/world.metal.
struct FrameUniforms {
    glm::mat4 viewProj, invViewProj;
    glm::vec4 eye, sunDir, lightDir, lightColor, skyZenith, skyHorizon, ambientSky, ambientGround, fog;
    glm::vec4 debug;  // x: ViewMode, y: 1 under water
    glm::vec4 aoCurve, shadowTint;
    glm::vec4 lightParams;  // x sun gate, y light falloff, z dark ambient, w voxel light on
    glm::vec4 emberColor;   // rgb linear, times strength
    glm::vec4 palette[128];
    glm::vec4 toneA[128], toneB[128];  // rgb linear patch variants; toneA.w size (voxels), toneB.w strength
};
struct DrawUniforms {
    glm::mat4 model;
    glm::vec4 modelLight;  // x models: light + 1 (0: a chunk); y > 0: it glows (ADR-053)
    glm::uvec4 firstQuad;  // x first quad; plants: y first instance, zw the world window's low corner x, z (int)
};
static_assert(sizeof(FrameUniforms) == 2 * 64 + 14 * 16 + 3 * 128 * 16);
static_assert(sizeof(DrawUniforms) == 64 + 16 + 16);
static_assert(BlockCount <= 128);  // also: shaders/world.metal packs block ids into 7 bits

glm::vec3 lin(glm::vec3 srgb) { return glm::pow(srgb, glm::vec3(2.2f)); }

// Day/night cycle: sun path, sky gradient, light and ambient colors for a time of day.
void ambience(float t, FrameUniforms& u) {
    float angle = (t - 0.25f) * 2 * std::numbers::pi_v<float>;  // sunrise in the east (+X) at t=0.25
    // The 0.8 tilt keeps the sun's peak at ~51° so it stays inside the orbit camera's view.
    glm::vec3 sun = glm::normalize(glm::vec3(std::cos(angle), std::sin(angle), 0.8f));
    float h = sun.y;
    float day = glm::smoothstep(-0.15f, 0.25f, h);
    float dusk = glm::clamp(1 - std::abs(h) / 0.3f, 0.0f, 1.0f);
    float night = 1 - glm::smoothstep(-0.25f, 0.0f, h);

    glm::vec3 zenith = glm::mix(lin({0.01f, 0.015f, 0.05f}), lin({0.22f, 0.45f, 0.90f}), day);
    glm::vec3 horizon = glm::mix(lin({0.04f, 0.05f, 0.10f}), lin({0.68f, 0.82f, 0.97f}), day);
    zenith = glm::mix(zenith, lin({0.32f, 0.32f, 0.55f}), dusk * 0.4f);
    horizon = glm::mix(horizon, lin({1.0f, 0.55f, 0.32f}), dusk * 0.7f);

    float sunUp = glm::smoothstep(0.0f, 0.15f, h), moonUp = glm::smoothstep(0.0f, 0.15f, -h);
    glm::vec3 sunColor = glm::mix(glm::vec3(1.0f, 0.45f, 0.2f), glm::vec3(1.0f, 0.95f, 0.85f), glm::smoothstep(0.0f, 0.4f, h));
    bool sunActive = h >= 0;
    glm::vec3 lightColor = sunActive ? sunColor * 1.7f * sunUp : glm::vec3(0.35f, 0.45f, 0.75f) * 0.5f * moonUp;

    glm::vec3 ambient = glm::mix(lin({0.16f, 0.20f, 0.34f}), lin({0.45f, 0.55f, 0.70f}), day);
    u.sunDir = glm::vec4(sun, 0);
    u.lightDir = glm::vec4(sunActive ? sun : -sun, 0);
    u.lightColor = glm::vec4(lightColor, 0);
    u.skyZenith = glm::vec4(zenith, 0);
    u.skyHorizon = glm::vec4(horizon, 0);
    u.ambientSky = glm::vec4(ambient, 0);
    u.ambientGround = glm::vec4(ambient * glm::vec3(0.45f, 0.40f, 0.32f), 0);
    u.fog = {0.0035f, night, glm::smoothstep(-0.05f, 0.05f, h), float(kSeaLevel + 1)};
}

// True if the AABB is at least partly inside the frustum of viewProj (depth 0..1).
bool inFrustum(const glm::mat4& m, glm::vec3 lo, glm::vec3 hi) {
    auto row = [&](int i) { return glm::vec4(m[0][i], m[1][i], m[2][i], m[3][i]); };
    const glm::vec4 planes[6] = {row(3) + row(0), row(3) - row(0), row(3) + row(1),
                                 row(3) - row(1), row(2), row(3) - row(2)};
    for (const glm::vec4& p : planes) {
        glm::vec3 v{p.x > 0 ? hi.x : lo.x, p.y > 0 ? hi.y : lo.y, p.z > 0 ? hi.z : lo.z};
        if (glm::dot(glm::vec3(p), v) + p.w < 0) return false;
    }
    return true;
}

}  // namespace

Renderer::~Renderer() {
    if (!device_) return;
    SDL_WaitForGPUIdle(device_);
    for (auto* p : {sky_, opaque_, water_, wire_, plants_}) if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    for (auto* b : {world_[0], world_[1], models_, plantInstances_}) if (b) SDL_ReleaseGPUBuffer(device_, b);
    if (plantTransfer_) SDL_ReleaseGPUTransferBuffer(device_, plantTransfer_);
    for (auto* t : {msaaColor_, depth_, resolved_}) if (t) SDL_ReleaseGPUTexture(device_, t);
    SDL_ReleaseWindowFromGPUDevice(device_, window_);
    SDL_DestroyGPUDevice(device_);
}

bool Renderer::init(SDL_Window* window, bool vsync) {
    window_ = window;
#ifdef NDEBUG
    constexpr bool debug = false;
#else
    constexpr bool debug = true;
#endif
    device_ = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, debug, nullptr);
    if (!device_ || !SDL_ClaimWindowForGPUDevice(device_, window)) {
        std::println(stderr, "GPU init failed: {}", SDL_GetError());
        return false;
    }
    std::println("GPU driver: {}", SDL_GetGPUDeviceDriver(device_));

    // Linear swapchain (sRGB format) so lighting math happens in linear space.
    // ponytail: the SDR fallback would show linear output un-encoded (too dark); every Metal device
    // supports SDR_LINEAR, revisit with the Vulkan/D3D12 port.
    auto composition = SDL_WindowSupportsGPUSwapchainComposition(device_, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR)
                           ? SDL_GPU_SWAPCHAINCOMPOSITION_SDR_LINEAR : SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    auto present = SDL_GPU_PRESENTMODE_VSYNC;
    if (!vsync && SDL_WindowSupportsGPUPresentMode(device_, window, SDL_GPU_PRESENTMODE_IMMEDIATE))
        present = SDL_GPU_PRESENTMODE_IMMEDIATE;
    SDL_SetGPUSwapchainParameters(device_, window, composition, present);
    colorFormat_ = SDL_GetGPUSwapchainTextureFormat(device_, window);

    if (SDL_GPUTextureSupportsSampleCount(device_, colorFormat_, SDL_GPU_SAMPLECOUNT_4) &&
        SDL_GPUTextureSupportsSampleCount(device_, SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_SAMPLECOUNT_4))
        samples_ = SDL_GPU_SAMPLECOUNT_4;

    const char* base = SDL_GetBasePath();
    std::string path = std::string(base ? base : "") + "shaders/world.metal";
    size_t size = 0;
    void* text = SDL_LoadFile(path.c_str(), &size);
    if (!text) {
        std::println(stderr, "cannot read {}: {}", path, SDL_GetError());
        return false;
    }
    std::vector<char> src(static_cast<char*>(text), static_cast<char*>(text) + size);
    src.push_back('\0');
    SDL_free(text);

    SDL_GPUShader* voxelVs = shader(src, "voxel_vs", SDL_GPU_SHADERSTAGE_VERTEX, 2, 1);
    SDL_GPUShader* voxelFs = shader(src, "voxel_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    SDL_GPUShader* waterFs = shader(src, "water_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    SDL_GPUShader* skyVs = shader(src, "sky_vs", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0);
    SDL_GPUShader* skyFs = shader(src, "sky_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    SDL_GPUShader* wireFs = shader(src, "wire_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 0, 0);
    SDL_GPUShader* plantVs = shader(src, "plant_vs", SDL_GPU_SHADERSTAGE_VERTEX, 2, 2);
    SDL_GPUShader* plantFs = shader(src, "plant_fs", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0);
    if (voxelVs && voxelFs && waterFs && skyVs && skyFs && wireFs && plantVs && plantFs) {
        sky_ = pipeline(skyVs, skyFs, false, false, false, SDL_GPU_CULLMODE_NONE);
        opaque_ = pipeline(voxelVs, voxelFs, true, true, false, SDL_GPU_CULLMODE_BACK);
        water_ = pipeline(voxelVs, waterFs, true, false, true, SDL_GPU_CULLMODE_NONE);
        wire_ = pipeline(voxelVs, wireFs, true, false, false, SDL_GPU_CULLMODE_BACK, SDL_GPU_FILLMODE_LINE);
        plants_ = pipeline(plantVs, plantFs, true, true, false, SDL_GPU_CULLMODE_BACK);
    }
    for (auto* s : {voxelVs, voxelFs, waterFs, skyVs, skyFs, wireFs, plantVs, plantFs}) if (s) SDL_ReleaseGPUShader(device_, s);
    if (!sky_ || !opaque_ || !water_ || !wire_ || !plants_) {
        std::println(stderr, "pipeline creation failed: {}", SDL_GetError());
        return false;
    }
    return true;
}

SDL_GPUShader* Renderer::shader(const std::vector<char>& source, const char* entry, SDL_GPUShaderStage stage,
                                uint32_t uniforms, uint32_t storage) {
    SDL_GPUShaderCreateInfo info{};
    info.code = reinterpret_cast<const Uint8*>(source.data());
    info.code_size = source.size() - 1;
    info.entrypoint = entry;
    info.format = SDL_GPU_SHADERFORMAT_MSL;
    info.stage = stage;
    info.num_uniform_buffers = uniforms;
    info.num_storage_buffers = storage;
    SDL_GPUShader* s = SDL_CreateGPUShader(device_, &info);
    if (!s) std::println(stderr, "shader {} failed: {} (Metal compiler output is in the SDL log above)", entry, SDL_GetError());
    return s;
}

SDL_GPUGraphicsPipeline* Renderer::pipeline(SDL_GPUShader* vs, SDL_GPUShader* fs, bool depthTest, bool depthWrite, bool blend,
                                            SDL_GPUCullMode cull, SDL_GPUFillMode fill) {
    SDL_GPUColorTargetDescription target{};
    target.format = colorFormat_;
    if (blend) {
        auto& b = target.blend_state;
        b.enable_blend = true;
        b.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
        b.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        b.color_blend_op = SDL_GPU_BLENDOP_ADD;
        b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
        b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
        b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
    }
    SDL_GPUGraphicsPipelineCreateInfo info{};
    info.vertex_shader = vs;
    info.fragment_shader = fs;
    info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
    info.rasterizer_state.fill_mode = fill;
    info.rasterizer_state.cull_mode = cull;
    info.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
    info.rasterizer_state.enable_depth_clip = true;
    info.multisample_state.sample_count = samples_;
    info.depth_stencil_state.enable_depth_test = depthTest;
    info.depth_stencil_state.enable_depth_write = depthWrite;
    info.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    info.target_info.color_target_descriptions = &target;
    info.target_info.num_color_targets = 1;
    info.target_info.depth_stencil_format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    info.target_info.has_depth_stencil_target = true;
    return SDL_CreateGPUGraphicsPipeline(device_, &info);
}

SDL_GPUBuffer* Renderer::upload(const void* data, uint32_t dataBytes, uint32_t capacity) {
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    bi.size = std::max({dataBytes, capacity, uint32_t(sizeof(Quad))});  // zero-sized buffers are invalid
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device_, &bi);
    if (!buffer) {
        std::println(stderr, "buffer allocation of {} bytes failed: {}", bi.size, SDL_GetError());
        std::exit(1);
    }
    if (dataBytes) write(data, dataBytes, {{buffer, 0, 0, dataBytes}});
    return buffer;
}

// Stages `data` in one transfer buffer and copies pieces of it into GPU buffers. Command buffers
// submitted later see the new contents; frames already submitted keep drawing the old ones.
void Renderer::write(const void* data, uint32_t bytes, const std::vector<Upload>& uploads) {
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &ti);
    void* mapped = transfer ? SDL_MapGPUTransferBuffer(device_, transfer, false) : nullptr;
    SDL_GPUCommandBuffer* cmd = mapped ? SDL_AcquireGPUCommandBuffer(device_) : nullptr;
    if (!cmd) {
        std::println(stderr, "upload of {} bytes failed: {}", bytes, SDL_GetError());
        std::exit(1);
    }
    std::memcpy(mapped, data, bytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    for (const Upload& u : uploads) {
        SDL_GPUTransferBufferLocation src{};
        src.transfer_buffer = transfer;
        src.offset = u.from;
        SDL_GPUBufferRegion dst{};
        dst.buffer = u.buffer;
        dst.offset = u.offset;
        dst.size = u.bytes;
        SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
}

void Renderer::uploadWorld(const Grid& world) {
    uint64_t start = SDL_GetTicksNS();
    fillLight(world, light_);
    stats.lightSeconds = double(SDL_GetTicksNS() - start) * 1e-9;
    stats.lightBytes = light_.bytes();
    stats.lightChunks = light_.allocated();
    start = SDL_GetTicksNS();
    meshes_ = {};  // free the old world's meshes before building the new ones
    meshes_ = meshChunks(world, &light_);
    worldLo_ = world.lo;
    headroom_ = 65536;
    chunks_.assign(meshes_.size(), {});
    for (size_t i = 0; i < chunks_.size(); ++i) chunks_[i].origin = world.chunkOrigin(i);
    packWorld();
    stats.meshSeconds = double(SDL_GetTicksNS() - start) * 1e-9;
}

void Renderer::streamWorld(const Grid& world) {
    light_ = Light(world);
    meshes_ = {};
    meshes_.resize(size_t((world.size.x / kChunk) * (world.size.y / kChunk) * (world.size.z / kChunk)));
    chunks_.assign(meshes_.size(), {});
    for (size_t i = 0; i < chunks_.size(); ++i) chunks_[i].origin = world.chunkOrigin(i);
    worldLo_ = world.lo;
    headroom_ = uint32_t(1) << 21;  // the window's quads come in and go without repacking often
    packWorld();
}

void Renderer::updateChunks(const Grid& world, const std::vector<size_t>& mesh, const std::vector<size_t>& empty) {
    worldLo_ = world.lo;
    if (mesh.empty() && empty.empty()) return;
    uint64_t start = SDL_GetTicksNS();
    for (size_t i : empty) meshes_[i] = {};
    parallelFor(int(mesh.size()), [&](int j) {
        size_t i = mesh[size_t(j)];
        meshes_[i] = meshRegion(world, world.chunkOrigin(i), glm::ivec3(kChunk), &light_);
    });
    std::vector<size_t> ids = mesh;
    ids.insert(ids.end(), empty.begin(), empty.end());
    uploadChunks(world, ids);
    stats.chunksRemeshed = int(mesh.size());
    stats.remeshMs = double(SDL_GetTicksNS() - start) * 1e-6;
}

// Lays every chunk's quads out back to back in fresh buffers, with headroom at the end for chunks
// that outgrow their slot after edits.
void Renderer::packWorld() {
    const uint32_t headroom = headroom_;
    stats.chunksTotal = 0;
    for (size_t k = 0; k < 2; ++k) {
        std::vector<Quad> all;
        for (size_t i = 0; i < chunks_.size(); ++i) {
            const std::vector<Quad>& q = k ? meshes_[i].water : meshes_[i].opaque;
            // A quarter more room, so a chunk that grows (an edit; streamed, the next column in its slot) stays put.
            uint32_t cap = q.empty() ? 0 : uint32_t(q.size() + q.size() / 4);
            chunks_[i].slot[k] = {uint32_t(all.size()), uint32_t(q.size()), cap};
            all.insert(all.end(), q.begin(), q.end());
            all.resize(all.size() + (cap - q.size()));
            stats.chunksTotal += k == 0 && !(meshes_[i].opaque.empty() && meshes_[i].water.empty());
        }
        worldUsed_[k] = uint32_t(all.size());
        worldCap_[k] = worldUsed_[k] + worldUsed_[k] / 8 + (k ? std::max(headroom / 8, 65536u) : headroom);  // water needs less
        if (world_[k]) SDL_ReleaseGPUBuffer(device_, world_[k]);  // SDL frees it once in-flight frames are done
        world_[k] = upload(all.data(), uint32_t(all.size() * sizeof(Quad)), worldCap_[k] * uint32_t(sizeof(Quad)));
        size_t quads = 0;
        for (const ChunkDraw& c : chunks_) quads += c.slot[k].count;
        (k ? stats.waterQuads : stats.opaqueQuads) = quads;
    }
}

void Renderer::updateWorld(const Grid& world, const Box& dirty) {
    if (dirty.empty()) return;
    uint64_t start = SDL_GetTicksNS();
    Box lit = relight(world, light_, dirty);
    stats.relightMs = double(SDL_GetTicksNS() - start) * 1e-6;
    stats.lightBytes = light_.bytes();
    stats.lightChunks = light_.allocated();
    glm::ivec3 lo = lit.empty() ? dirty.lo : glm::min(dirty.lo, lit.lo), hi = lit.empty() ? dirty.hi : glm::max(dirty.hi, lit.hi);
    std::vector<size_t> ids = remeshChunks(world, lo, hi, meshes_, &light_);
    uploadChunks(world, ids);
    stats.chunksRemeshed = int(ids.size());
    stats.remeshMs = double(SDL_GetTicksNS() - start) * 1e-6;
}

void Renderer::uploadChunks(const Grid& world, const std::vector<size_t>& ids) {
    for (size_t i : ids) chunks_[i].origin = world.chunkOrigin(i);  // a streamed slot may hold another column now
    // A chunk's new quads overwrite its slot when they fit. Otherwise they move to a new slot with
    // spare room at the end of the buffer; once that headroom is used up, everything is repacked.
    std::vector<Quad> staged;
    std::vector<Upload> uploads;
    bool repack = false;
    for (size_t i : ids)
        for (size_t k = 0; k < 2; ++k) {
            const std::vector<Quad>& q = k ? meshes_[i].water : meshes_[i].opaque;
            Slot& slot = chunks_[i].slot[k];
            (k ? stats.waterQuads : stats.opaqueQuads) += q.size() - slot.count;  // unsigned wrap-around nets out
            if (q.size() > slot.cap) {
                uint32_t cap = uint32_t(q.size() + q.size() / 4 + 64);
                repack |= worldUsed_[k] + cap > worldCap_[k];
                slot = {worldUsed_[k], 0, cap};
                worldUsed_[k] += cap;
            }
            slot.count = uint32_t(q.size());
            if (q.empty()) continue;
            uploads.push_back({world_[k], uint32_t(slot.first * sizeof(Quad)), uint32_t(staged.size() * sizeof(Quad)),
                               uint32_t(q.size() * sizeof(Quad))});
            staged.insert(staged.end(), q.begin(), q.end());
        }
    if (repack) {  // rebuilds every slot from meshes_, which already holds the new meshes
        packWorld();
        ++stats.repacks;
    } else {
        if (!staged.empty()) write(staged.data(), uint32_t(staged.size() * sizeof(Quad)), uploads);
        stats.chunksTotal = 0;
        for (const ChunkDraw& c : chunks_) stats.chunksTotal += c.slot[0].count || c.slot[1].count;
    }
}

void Renderer::uploadModels() {
    std::vector<Quad> quads;
    for (int model = 0; model < ModelCount; ++model)
        for (int p = 0; p < PartCount; ++p) {
            Grid part = buildPart(Model(model), Part(p));
            Mesh m = meshRegion(part, {0, 0, 0}, part.size);
            parts_[size_t(model)][size_t(p)] = {uint32_t(quads.size()), uint32_t(m.opaque.size())};
            quads.insert(quads.end(), m.opaque.begin(), m.opaque.end());
        }
    for (size_t k = 0; k < plantParts_.size(); ++k) {
        std::vector<Quad> q = plantQuads(PlantModel(k / 2), k % 2);
        plantParts_[k] = {uint32_t(quads.size()), uint32_t(q.size())};
        quads.insert(quads.end(), q.begin(), q.end());
    }
    if (models_) SDL_ReleaseGPUBuffer(device_, models_);
    models_ = upload(quads.data(), uint32_t(quads.size() * sizeof(Quad)));
}

SDL_GPUTexture* Renderer::texture(SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, SDL_GPUSampleCount samples,
                                  uint32_t w, uint32_t h) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = usage;
    info.width = w;
    info.height = h;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = samples;
    return SDL_CreateGPUTexture(device_, &info);
}

bool Renderer::ensureTargets(uint32_t w, uint32_t h) {
    if (w == targetW_ && h == targetH_) return true;
    for (auto* t : {msaaColor_, depth_, resolved_}) if (t) SDL_ReleaseGPUTexture(device_, t);
    // Scene renders into our own (MSAA) targets and resolves into `resolved_`, which is
    // blitted to the swapchain. Owning the final image lets us read it back for screenshots.
    resolved_ = texture(colorFormat_, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, SDL_GPU_SAMPLECOUNT_1, w, h);
    msaaColor_ = samples_ == SDL_GPU_SAMPLECOUNT_1 ? nullptr
                                                   : texture(colorFormat_, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, samples_, w, h);
    depth_ = texture(SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, samples_, w, h);
    if (!resolved_ || !depth_ || (samples_ != SDL_GPU_SAMPLECOUNT_1 && !msaaColor_)) {
        std::println(stderr, "render target creation ({}x{}) failed: {}", w, h, SDL_GetError());
        targetW_ = targetH_ = 0;
        return false;  // fatal: render() reports failure and the app exits
    }
    targetW_ = w;
    targetH_ = h;
    return true;
}

// Sky, world, models and water from `frame` into the given targets (no MSAA target: draws straight
// into `resolved`). `onScreen`: the window's frame, which may use the frozen cull view.
void Renderer::drawScene(SDL_GPUCommandBuffer* cmd, const FrameParams& frame, const RenderSettings& settings,
                         SDL_GPUTexture* msaa, SDL_GPUTexture* resolved, SDL_GPUTexture* depthTexture, uint32_t w,
                         uint32_t h, bool onScreen) {
    FrameUniforms u{};
    glm::mat4 proj = glm::perspectiveRH_ZO(frame.fovY, float(w) / float(h), 0.1f, 1000.0f);
    u.viewProj = proj * frame.view;
    u.invViewProj = glm::inverse(u.viewProj);
    u.eye = glm::vec4(frame.eye, 0);
    ambience(frame.timeOfDay, u);
    if (!settings.fog) u.fog.x = 0;
    u.debug = {float(settings.viewMode), frame.underwater && settings.fog ? 1 : 0,
               float(windPhase_), settings.plantRadius};
    if (settings.occlusionRamp) {
        u.aoCurve = settings.aoCurve;
        // Moonlight is already blue, so the cool tint fades to 30% at night.
        u.shadowTint = glm::vec4(lin(settings.tintColor), settings.tintStrength * (1 - 0.7f * u.fog.y));
    } else {
        u.aoCurve = {0.35f, 0.35f + 0.65f / 3, 0.35f + 1.3f / 3, 1};  // the old linear AO, no tint (A/B)
    }
    u.lightParams = {settings.sunGate, settings.lightFalloff, settings.darkAmbient, settings.voxelLight};
    u.emberColor = glm::vec4(lin(settings.emberColor) * settings.emberStrength, 0);
    for (int b = 0; b < BlockCount; ++b) {
        int mid = isLeaf(Block(b)) ? leafBlock(kLeafOf[size_t(b)].kind, 2) : b;  // a leaf shade blends from its kind's colour
        auto shaded = [&](const float* own, const float* base) {
            return lin(glm::mix(glm::vec3(base[0], base[1], base[2]), glm::vec3(own[0], own[1], own[2]), settings.leafShade));
        };
        const BlockTone &t = settings.tones[size_t(b)], &m = settings.tones[size_t(mid)];
        u.palette[b] = glm::vec4(shaded(kPalette[size_t(b)].data(), kPalette[size_t(mid)].data()), 1);
        u.toneA[b] = glm::vec4(shaded(t.a, m.a), std::max(t.size * settings.patchSize, 2.0f));  // smaller reads as per-voxel noise
        u.toneB[b] = glm::vec4(shaded(t.b, m.b), t.strength * settings.patchStrength);
    }
    SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof u);
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);

    // Cull chunks once; the same visible list serves every pass. A frozen cull keeps using the
    // view from when it was frozen (on screen only).
    if (onScreen && !settings.freezeCulling) cullViewProj_ = u.viewProj;
    const glm::mat4& cull = onScreen ? cullViewProj_ : u.viewProj;
    std::vector<const ChunkDraw*> visible;
    for (const ChunkDraw& c : chunks_)
        if ((c.slot[0].count || c.slot[1].count || !meshes_[size_t(&c - chunks_.data())].plants.empty()) &&
            (!settings.frustumCulling || inFrustum(cull, glm::vec3(c.origin) - 1.0f, glm::vec3(c.origin + kChunk) + 1.0f)))  // plants reach out a little
            visible.push_back(&c);
    stats.chunksDrawn = int(visible.size());
    stats.quadsDrawn = 0;
    stats.drawCalls = 0;

    // Plants of the visible chunks within the radius, grouped by model and detail (a counting sort
    // on the plantParts_ index), staged for this frame's instanced draws.
    constexpr size_t kPlantDraws = 2 * PlantModelCount;
    std::array<uint32_t, kPlantDraws + 1> plantFirst{};
    plantStage_.clear();
    if (settings.plants) {
        glm::vec2 eye(frame.eye.x, frame.eye.z);
        float r2 = settings.plantRadius * settings.plantRadius, near2 = kPlantNear * kPlantNear;
        auto inRadius = [&](const ChunkDraw* c) {  // the chunk's box comes within the radius (horizontally)
            glm::vec2 lo(c->origin.x, c->origin.z), d = glm::max(lo - eye, glm::vec2(0)) + glm::max(eye - lo - float(kChunk), glm::vec2(0));
            return glm::dot(d, d) < r2;
        };
        auto draw = [&](PlantInstance p) {
            glm::ivec3 cell = plantCell(p.cell, worldLo_);
            glm::vec2 d = glm::vec2(float(cell.x) + 0.5f, float(cell.z) + 0.5f) - eye;
            return size_t(p.info >> 8) * 2 + (glm::dot(d, d) > near2);
        };
        std::vector<const std::vector<PlantInstance>*> lists;
        for (const ChunkDraw* c : visible)
            if (const std::vector<PlantInstance>& l = meshes_[size_t(c - chunks_.data())].plants; !l.empty() && inRadius(c))
                lists.push_back(&l);
        for (const auto* l : lists)
            for (PlantInstance p : *l) ++plantFirst[draw(p) + 1];
        for (size_t k = 1; k <= kPlantDraws; ++k) plantFirst[k] += plantFirst[k - 1];
        plantStage_.resize(plantFirst[kPlantDraws]);
        std::array<uint32_t, kPlantDraws + 1> next = plantFirst;
        for (const auto* l : lists)
            for (PlantInstance p : *l) plantStage_[next[draw(p)]++] = p;
    }
    stats.plantsDrawn = plantStage_.size();
    stats.plantQuadsDrawn = 0;
    if (!plantStage_.empty()) {
        uint32_t n = uint32_t(plantStage_.size()), bytes = n * uint32_t(sizeof(PlantInstance));
        if (n > plantCap_) {
            if (plantInstances_) SDL_ReleaseGPUBuffer(device_, plantInstances_);
            if (plantTransfer_) SDL_ReleaseGPUTransferBuffer(device_, plantTransfer_);
            uint32_t cap = n + n / 2 + 1024;
            plantInstances_ = upload(nullptr, 0, cap * uint32_t(sizeof(PlantInstance)));
            SDL_GPUTransferBufferCreateInfo ti{};
            ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
            ti.size = cap * uint32_t(sizeof(PlantInstance));
            plantTransfer_ = SDL_CreateGPUTransferBuffer(device_, &ti);
            plantCap_ = plantTransfer_ ? cap : 0;  // failed: try again next frame
        }
        // Cycling: frames still in flight keep the buffers they were given.
        if (void* mapped = plantTransfer_ ? SDL_MapGPUTransferBuffer(device_, plantTransfer_, true) : nullptr) {
            std::memcpy(mapped, plantStage_.data(), bytes);
            SDL_UnmapGPUTransferBuffer(device_, plantTransfer_);
            SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTransferBufferLocation src{plantTransfer_, 0};
            SDL_GPUBufferRegion dst{plantInstances_, 0, bytes};
            SDL_UploadToGPUBuffer(copy, &src, &dst, true);
            SDL_EndGPUCopyPass(copy);
        } else {
            plantStage_.clear();
        }
    }

    SDL_GPUColorTargetInfo color{};
    color.clear_color = {0.03f, 0.03f, 0.04f, 1};  // visible only with the sky off
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    if (msaa) {
        color.texture = msaa;
        color.store_op = SDL_GPU_STOREOP_RESOLVE;
        color.resolve_texture = resolved;
    } else {
        color.texture = resolved;
        color.store_op = SDL_GPU_STOREOP_STORE;
    }
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depthTexture;
    depth.clear_depth = 1;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);

    if (settings.sky) {
        SDL_BindGPUGraphicsPipeline(pass, sky_);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    }


    auto draw = [&](const DrawUniforms& d, uint32_t count) {
        SDL_PushGPUVertexUniformData(cmd, 1, &d, sizeof d);
        SDL_DrawGPUPrimitives(pass, count * 6, 1, 0, 0);
        stats.quadsDrawn += count;
        ++stats.drawCalls;
    };
    auto drawChunks = [&](size_t k) {  // 0 opaque, 1 water
        SDL_BindGPUVertexStorageBuffers(pass, 0, &world_[k], 1);
        for (const ChunkDraw* c : visible)
            if (const Slot& s = c->slot[k]; s.count)
                draw({glm::translate(glm::mat4(1), glm::vec3(c->origin)), glm::vec4(0), glm::uvec4(s.first, 0, 0, 0)},
                     s.count);
    };
    auto drawModels = [&] {
        SDL_BindGPUVertexStorageBuffers(pass, 0, &models_, 1);
        for (const ModelInstance& inst : frame.models) {
            glm::vec3 center(0);  // of the parts' origins: inside the body, so in the air it stands in
            for (const glm::mat4& m : inst.pose) center += glm::vec3(m[3]) / float(PartCount);
            glm::ivec3 at(glm::floor(center));  // and the voxel above, in case the center is in a step
            uint8_t a = light_.get(at), b = light_.get(at + glm::ivec3(0, 1, 0));
            float lit = float(std::max(skyLight(a), skyLight(b)) << 4 | std::max(blockLight(a), blockLight(b))) + 1;
            for (size_t p = 0; p < PartCount; ++p) {
                auto [first, count] = parts_[size_t(inst.parts[p])][p];
                if (count) draw({inst.pose[p], glm::vec4(lit, inst.glow ? 1 : 0, 0, 0), glm::uvec4(first, 0, 0, 0)}, count);
            }
        }
    };

    auto drawPlants = [&] {  // one instanced draw per plant model
        SDL_GPUBuffer* buffers[2] = {models_, plantInstances_};
        SDL_BindGPUVertexStorageBuffers(pass, 0, buffers, 2);
        for (size_t k = 0; k < kPlantDraws; ++k) {
            uint32_t count = plantFirst[k + 1] - plantFirst[k];
            auto [first, quads] = plantParts_[k];
            if (!count || !quads) continue;
            DrawUniforms d{glm::mat4(1), glm::vec4(0), glm::uvec4(first, plantFirst[k], uint32_t(worldLo_.x), uint32_t(worldLo_.z))};
            SDL_PushGPUVertexUniformData(cmd, 1, &d, sizeof d);
            SDL_DrawGPUPrimitives(pass, quads * 6, count, 0, 0);
            stats.quadsDrawn += size_t(quads) * count;
            stats.plantQuadsDrawn += size_t(quads) * count;
            ++stats.drawCalls;
        }
    };

    SDL_BindGPUGraphicsPipeline(pass, opaque_);
    drawChunks(0);
    if (settings.character) drawModels();
    if (!plantStage_.empty()) {
        SDL_BindGPUGraphicsPipeline(pass, plants_);
        drawPlants();
    }
    if (settings.wireframe) {  // quad edges on top: shows how the greedy mesher merged faces
        SDL_BindGPUGraphicsPipeline(pass, wire_);
        drawChunks(0);
        if (settings.character) drawModels();
    }
    if (settings.water) {
        SDL_BindGPUGraphicsPipeline(pass, water_);
        drawChunks(1);
    }
    SDL_EndGPURenderPass(pass);
}

bool Renderer::render(const FrameParams& frame, const RenderSettings& settings, ImDrawData* ui,
                      const char* screenshotPath) {
    windPhase_ = std::fmod(windPhase_ + std::clamp(frame.seconds - windSeconds_, 0.0, 0.25) * 1.4 * settings.sway, 2 * std::numbers::pi);
    windSeconds_ = frame.seconds;
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!cmd) {
        std::println(stderr, "command buffer acquire failed: {}", SDL_GetError());
        return false;
    }
    SDL_GPUTexture* swapchain = nullptr;
    uint32_t w = 0, h = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, window_, &swapchain, &w, &h)) {
        std::println(stderr, "swapchain acquire failed: {}", SDL_GetError());
        SDL_CancelGPUCommandBuffer(cmd);
        return false;
    }
    if (!swapchain) {  // minimized/occluded: nothing to draw into
        if (screenshotPath) std::println(stderr, "screenshot skipped: window not visible");
        return SDL_SubmitGPUCommandBuffer(cmd) && !screenshotPath;
    }
    if (!ensureTargets(w, h)) {
        SDL_SubmitGPUCommandBuffer(cmd);  // cancelling after a swapchain acquire is an SDL error
        return false;
    }

    drawScene(cmd, frame, settings, msaaColor_, resolved_, depth_, w, h, true);

    if (ui && ui->TotalVtxCount > 0) {  // HUD and debug UI go into `resolved_` too, so screenshots include them
        ImGui_ImplSDLGPU3_PrepareDrawData(ui, cmd);  // uploads; must happen outside a render pass
        SDL_GPUColorTargetInfo uiTarget{};
        uiTarget.texture = resolved_;
        uiTarget.load_op = SDL_GPU_LOADOP_LOAD;
        uiTarget.store_op = SDL_GPU_STOREOP_STORE;
        SDL_GPURenderPass* uiPass = SDL_BeginGPURenderPass(cmd, &uiTarget, 1, nullptr);
        ImGui_ImplSDLGPU3_RenderDrawData(ui, cmd, uiPass);
        SDL_EndGPURenderPass(uiPass);
    }

    SDL_GPUBlitInfo blit{};
    blit.source.texture = resolved_;
    blit.source.w = w;
    blit.source.h = h;
    blit.destination.texture = swapchain;
    blit.destination.w = w;
    blit.destination.h = h;
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(cmd, &blit);

    if (screenshotPath) return saveScreenshot(cmd, screenshotPath);
    return SDL_SubmitGPUCommandBuffer(cmd);
}

// Copies the texture into `rgba` (bytes R, G, B, A), submitting `cmd` and waiting for the GPU.
bool Renderer::readPixels(SDL_GPUCommandBuffer* cmd, SDL_GPUTexture* texture, uint32_t w, uint32_t h,
                          std::vector<uint8_t>& rgba) {
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = w * h * 4;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!transfer) {
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = texture;
    src.w = w;
    src.h = h;
    src.d = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = transfer;
    SDL_DownloadFromGPUTexture(copy, &src, &dst);
    SDL_EndGPUCopyPass(copy);
    SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
    if (fence) {
        SDL_WaitForGPUFences(device_, true, &fence, 1);
        SDL_ReleaseGPUFence(device_, fence);
    } else {
        SDL_WaitForGPUIdle(device_);
    }
    const uint8_t* pixels = static_cast<const uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
    if (pixels) {
        rgba.assign(pixels, pixels + ti.size);
        SDL_UnmapGPUTransferBuffer(device_, transfer);
        if (colorFormat_ == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || colorFormat_ == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB)
            for (size_t i = 0; i < rgba.size(); i += 4) std::swap(rgba[i], rgba[i + 2]);
    }
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return pixels;
}

bool Renderer::snapshot(const FrameParams& frame, const RenderSettings& settings, uint32_t w, uint32_t h,
                        std::vector<uint8_t>& rgb) {
    SDL_GPUTexture* resolved = texture(colorFormat_, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, SDL_GPU_SAMPLECOUNT_1, w, h);
    SDL_GPUTexture* msaa = samples_ == SDL_GPU_SAMPLECOUNT_1 ? nullptr : texture(colorFormat_, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, samples_, w, h);
    SDL_GPUTexture* depth = texture(SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, samples_, w, h);
    SDL_GPUCommandBuffer* cmd = resolved && depth && (msaa || samples_ == SDL_GPU_SAMPLECOUNT_1) ? SDL_AcquireGPUCommandBuffer(device_) : nullptr;
    std::vector<uint8_t> rgba;
    bool ok = false;
    if (cmd) {
        RenderStats onScreen = stats;  // the stats describe the window's frame
        drawScene(cmd, frame, settings, msaa, resolved, depth, w, h, false);
        stats = onScreen;
        ok = readPixels(cmd, resolved, w, h, rgba);
    }
    for (auto* t : {resolved, msaa, depth}) if (t) SDL_ReleaseGPUTexture(device_, t);
    if (!ok) {
        std::println(stderr, "snapshot ({}x{}) failed: {}", w, h, SDL_GetError());
        return false;
    }
    rgb.resize(size_t(w) * h * 3);
    for (size_t i = 0; i < size_t(w) * h; ++i) std::copy_n(&rgba[i * 4], 3, &rgb[i * 3]);
    return true;
}

bool Renderer::saveScreenshot(SDL_GPUCommandBuffer* cmd, const char* path) {
    std::vector<uint8_t> rgba;
    SDL_Surface* surface = !readPixels(cmd, resolved_, targetW_, targetH_, rgba) ? nullptr
        : SDL_CreateSurfaceFrom(int(targetW_), int(targetH_), SDL_PIXELFORMAT_ABGR8888, rgba.data(), int(targetW_ * 4));
    bool ok = surface && SDL_SaveBMP(surface, path);
    if (!ok) std::println(stderr, "screenshot failed: {}", SDL_GetError());
    else std::println("screenshot saved to {}", path);
    SDL_DestroySurface(surface);
    return ok;
}
