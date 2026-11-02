#include "renderer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <print>
#include <string>

#include <glm/gtc/matrix_transform.hpp>

#include "mesher.h"

namespace {

// Block colors in sRGB, indexed by Block. Converted to linear when uploaded.
constexpr float kPalette[BlockCount][3] = {
    {0, 0, 0},              // Air
    {0.36f, 0.62f, 0.24f},  // Grass
    {0.47f, 0.33f, 0.20f},  // Dirt
    {0.50f, 0.50f, 0.52f},  // Stone
    {0.80f, 0.72f, 0.48f},  // Sand
    {0.95f, 0.96f, 1.00f},  // Snow
    {0.42f, 0.28f, 0.15f},  // Wood
    {0.20f, 0.48f, 0.18f},  // Leaves
    {0.08f, 0.30f, 0.55f},  // Water
    {0.93f, 0.74f, 0.58f},  // Skin
    {0.25f, 0.45f, 0.72f},  // Tunic
    {0.35f, 0.22f, 0.12f},  // Belt
    {0.28f, 0.18f, 0.12f},  // Boots
    {0.80f, 0.45f, 0.20f},  // Hair
    {0.08f, 0.08f, 0.10f},  // Eye
};

// Must match struct Frame / Draw in shaders/world.metal.
struct FrameUniforms {
    glm::mat4 viewProj, invViewProj;
    glm::vec4 eye, sunDir, lightDir, lightColor, skyZenith, skyHorizon, ambientSky, ambientGround, fog;
    glm::vec4 palette[32];
};
struct DrawUniforms {
    glm::mat4 model;
    glm::vec4 hashOffset;
    glm::uvec4 firstQuad;  // x used
};
static_assert(sizeof(FrameUniforms) == 2 * 64 + 9 * 16 + 32 * 16);
static_assert(sizeof(DrawUniforms) == 64 + 16 + 16);
static_assert(BlockCount <= 32);

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
    u.fog = {0.0055f, night, glm::smoothstep(-0.05f, 0.05f, h), float(kSeaLevel + 1)};
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
    for (auto* p : {sky_, opaque_, water_}) if (p) SDL_ReleaseGPUGraphicsPipeline(device_, p);
    for (auto* b : {worldOpaque_, worldWater_, character_}) if (b) SDL_ReleaseGPUBuffer(device_, b);
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
    if (voxelVs && voxelFs && waterFs && skyVs && skyFs) {
        sky_ = pipeline(skyVs, skyFs, false, false, false, SDL_GPU_CULLMODE_NONE);
        opaque_ = pipeline(voxelVs, voxelFs, true, true, false, SDL_GPU_CULLMODE_BACK);
        water_ = pipeline(voxelVs, waterFs, true, false, true, SDL_GPU_CULLMODE_NONE);
    }
    for (auto* s : {voxelVs, voxelFs, waterFs, skyVs, skyFs}) if (s) SDL_ReleaseGPUShader(device_, s);
    if (!sky_ || !opaque_ || !water_) {
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
                                            SDL_GPUCullMode cull) {
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
    info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
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

SDL_GPUBuffer* Renderer::upload(const void* data, uint32_t dataBytes) {
    uint32_t bytes = std::max<uint32_t>(dataBytes, sizeof(Quad));  // zero-sized buffers are invalid
    SDL_GPUBufferCreateInfo bi{};
    bi.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    bi.size = bytes;
    SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(device_, &bi);
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    ti.size = bytes;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!buffer || !transfer) {
        std::println(stderr, "buffer allocation of {} bytes failed: {}", bytes, SDL_GetError());
        std::exit(1);
    }
    auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(device_, transfer, false));
    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device_);
    if (!mapped || !cmd) {
        std::println(stderr, "upload failed: {}", SDL_GetError());
        std::exit(1);
    }
    if (dataBytes) std::memcpy(mapped, data, dataBytes);
    std::memset(mapped + dataBytes, 0, bytes - dataBytes);
    SDL_UnmapGPUTransferBuffer(device_, transfer);

    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTransferBufferLocation src{};
    src.transfer_buffer = transfer;
    SDL_GPUBufferRegion dst{};
    dst.buffer = buffer;
    dst.size = bytes;
    SDL_UploadToGPUBuffer(copy, &src, &dst, false);
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return buffer;
}

void Renderer::uploadWorld(const Grid& world) {
    glm::ivec3 counts = world.size / kChunk;
    int n = counts.x * counts.y * counts.z;
    std::vector<Mesh> meshes(static_cast<size_t>(n));
    auto chunkOrigin = [&](int i) {
        return glm::ivec3{i % counts.x, i / counts.x % counts.y, i / (counts.x * counts.y)} * kChunk;
    };
    parallelFor(n, [&](int i) { meshes[size_t(i)] = meshRegion(world, chunkOrigin(i), glm::ivec3(kChunk)); });

    std::vector<Quad> opaque, water;
    chunks_.clear();
    for (int i = 0; i < n; ++i) {
        const Mesh& m = meshes[size_t(i)];
        if (m.opaque.empty() && m.water.empty()) continue;
        chunks_.push_back({chunkOrigin(i), uint32_t(opaque.size()), uint32_t(m.opaque.size()),
                           uint32_t(water.size()), uint32_t(m.water.size())});
        opaque.insert(opaque.end(), m.opaque.begin(), m.opaque.end());
        water.insert(water.end(), m.water.begin(), m.water.end());
    }
    for (auto* b : {worldOpaque_, worldWater_}) if (b) SDL_ReleaseGPUBuffer(device_, b);
    worldOpaque_ = upload(opaque.data(), uint32_t(opaque.size() * sizeof(Quad)));
    worldWater_ = upload(water.data(), uint32_t(water.size() * sizeof(Quad)));
    stats.opaqueQuads = opaque.size();
    stats.waterQuads = water.size();
    stats.chunksTotal = int(chunks_.size());
}

void Renderer::uploadCharacter() {
    std::vector<Quad> quads;
    for (int p = 0; p < PartCount; ++p) {
        Grid part = buildPart(Part(p));
        Mesh m = meshRegion(part, {0, 0, 0}, part.size);
        parts_[size_t(p)] = {uint32_t(quads.size()), uint32_t(m.opaque.size())};
        quads.insert(quads.end(), m.opaque.begin(), m.opaque.end());
    }
    if (character_) SDL_ReleaseGPUBuffer(device_, character_);
    character_ = upload(quads.data(), uint32_t(quads.size() * sizeof(Quad)));
}

bool Renderer::ensureTargets(uint32_t w, uint32_t h) {
    if (w == targetW_ && h == targetH_) return true;
    for (auto* t : {msaaColor_, depth_, resolved_}) if (t) SDL_ReleaseGPUTexture(device_, t);
    auto make = [&](SDL_GPUTextureFormat format, SDL_GPUTextureUsageFlags usage, SDL_GPUSampleCount samples) {
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
    };
    // Scene renders into our own (MSAA) targets and resolves into `resolved_`, which is
    // blitted to the swapchain. Owning the final image lets us read it back for screenshots.
    resolved_ = make(colorFormat_, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER, SDL_GPU_SAMPLECOUNT_1);
    msaaColor_ = samples_ == SDL_GPU_SAMPLECOUNT_1 ? nullptr
                                                   : make(colorFormat_, SDL_GPU_TEXTUREUSAGE_COLOR_TARGET, samples_);
    depth_ = make(SDL_GPU_TEXTUREFORMAT_D32_FLOAT, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET, samples_);
    if (!resolved_ || !depth_ || (samples_ != SDL_GPU_SAMPLECOUNT_1 && !msaaColor_)) {
        std::println(stderr, "render target creation ({}x{}) failed: {}", w, h, SDL_GetError());
        targetW_ = targetH_ = 0;
        return false;  // fatal: render() reports failure and the app exits
    }
    targetW_ = w;
    targetH_ = h;
    return true;
}

bool Renderer::render(const FrameParams& frame, const char* screenshotPath) {
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

    FrameUniforms u{};
    glm::mat4 proj = glm::perspectiveRH_ZO(frame.fovY, float(w) / float(h), 0.1f, 1000.0f);
    u.viewProj = proj * frame.view;
    u.invViewProj = glm::inverse(u.viewProj);
    u.eye = glm::vec4(frame.eye, 0);
    ambience(frame.timeOfDay, u);
    for (int b = 0; b < BlockCount; ++b)
        u.palette[b] = glm::vec4(lin({kPalette[b][0], kPalette[b][1], kPalette[b][2]}), 1);
    SDL_PushGPUVertexUniformData(cmd, 0, &u, sizeof u);
    SDL_PushGPUFragmentUniformData(cmd, 0, &u, sizeof u);

    SDL_GPUColorTargetInfo color{};
    color.clear_color = {0, 0, 0, 1};
    color.load_op = SDL_GPU_LOADOP_CLEAR;
    if (msaaColor_) {
        color.texture = msaaColor_;
        color.store_op = SDL_GPU_STOREOP_RESOLVE;
        color.resolve_texture = resolved_;
    } else {
        color.texture = resolved_;
        color.store_op = SDL_GPU_STOREOP_STORE;
    }
    SDL_GPUDepthStencilTargetInfo depth{};
    depth.texture = depth_;
    depth.clear_depth = 1;
    depth.load_op = SDL_GPU_LOADOP_CLEAR;
    depth.store_op = SDL_GPU_STOREOP_DONT_CARE;
    depth.stencil_load_op = SDL_GPU_LOADOP_DONT_CARE;
    depth.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;
    SDL_GPURenderPass* pass = SDL_BeginGPURenderPass(cmd, &color, 1, &depth);

    SDL_BindGPUGraphicsPipeline(pass, sky_);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);

    // Cull chunks once; the same visible list serves the opaque and water passes.
    std::vector<const ChunkDraw*> visible;
    for (const ChunkDraw& c : chunks_)
        if (inFrustum(u.viewProj, glm::vec3(c.origin), glm::vec3(c.origin + kChunk))) visible.push_back(&c);
    stats.chunksDrawn = int(visible.size());

    auto drawChunks = [&](SDL_GPUBuffer* buffer, bool waterPass) {
        SDL_BindGPUVertexStorageBuffers(pass, 0, &buffer, 1);
        for (const ChunkDraw* c : visible) {
            uint32_t first = waterPass ? c->waterFirst : c->opaqueFirst;
            uint32_t count = waterPass ? c->waterCount : c->opaqueCount;
            if (!count) continue;
            DrawUniforms d{glm::translate(glm::mat4(1), glm::vec3(c->origin)), glm::vec4(glm::vec3(c->origin), 0),
                           glm::uvec4(first, 0, 0, 0)};
            SDL_PushGPUVertexUniformData(cmd, 1, &d, sizeof d);
            SDL_DrawGPUPrimitives(pass, count * 6, 1, 0, 0);
        }
    };

    SDL_BindGPUGraphicsPipeline(pass, opaque_);
    drawChunks(worldOpaque_, false);
    SDL_BindGPUVertexStorageBuffers(pass, 0, &character_, 1);
    for (int p = 0; p < PartCount; ++p) {
        DrawUniforms d{frame.pose[size_t(p)], glm::vec4(float(p) * 37), glm::uvec4(parts_[size_t(p)].first, 0, 0, 0)};
        SDL_PushGPUVertexUniformData(cmd, 1, &d, sizeof d);
        SDL_DrawGPUPrimitives(pass, parts_[size_t(p)].second * 6, 1, 0, 0);
    }
    SDL_BindGPUGraphicsPipeline(pass, water_);
    drawChunks(worldWater_, true);
    SDL_EndGPURenderPass(pass);

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

bool Renderer::saveScreenshot(SDL_GPUCommandBuffer* cmd, const char* path) {
    SDL_GPUTransferBufferCreateInfo ti{};
    ti.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    ti.size = targetW_ * targetH_ * 4;
    SDL_GPUTransferBuffer* transfer = SDL_CreateGPUTransferBuffer(device_, &ti);
    if (!transfer) {
        std::println(stderr, "screenshot failed: {}", SDL_GetError());
        SDL_SubmitGPUCommandBuffer(cmd);
        return false;
    }
    SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureRegion src{};
    src.texture = resolved_;
    src.w = targetW_;
    src.h = targetH_;
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

    bool bgra = colorFormat_ == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM ||
                colorFormat_ == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB;
    void* pixels = SDL_MapGPUTransferBuffer(device_, transfer, false);
    SDL_Surface* surface = !pixels ? nullptr : SDL_CreateSurfaceFrom(int(targetW_), int(targetH_),
                                                 bgra ? SDL_PIXELFORMAT_ARGB8888 : SDL_PIXELFORMAT_ABGR8888,
                                                 pixels, int(targetW_ * 4));
    bool ok = surface && SDL_SaveBMP(surface, path);
    if (!ok) std::println(stderr, "screenshot failed: {}", SDL_GetError());
    else std::println("screenshot saved to {}", path);
    SDL_DestroySurface(surface);
    if (pixels) SDL_UnmapGPUTransferBuffer(device_, transfer);
    SDL_ReleaseGPUTransferBuffer(device_, transfer);
    return ok;
}
