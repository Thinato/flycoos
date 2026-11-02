#pragma once
#include <array>
#include <cstdint>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/glm.hpp>

#include "player.h"
#include "world.h"

struct FrameParams {
    glm::mat4 view{1};
    float fovY = 1.2f;       // radians; aspect comes from the swapchain
    glm::vec3 eye{};
    float timeOfDay = 0.3f;  // 0 midnight, 0.25 sunrise, 0.5 noon, 0.75 sunset
    std::array<glm::mat4, PartCount> pose{};
};

struct RenderStats {
    size_t opaqueQuads = 0, waterQuads = 0;
    int chunksTotal = 0, chunksDrawn = 0;
};

class Renderer {
public:
    Renderer() = default;
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;
    ~Renderer();

    bool init(SDL_Window* window, bool vsync);
    void uploadWorld(const Grid& world);  // meshes every chunk (in parallel) and uploads
    void uploadCharacter();
    // Draws one frame. With screenshotPath set, also writes the frame to a BMP.
    bool render(const FrameParams& frame, const char* screenshotPath = nullptr);

    RenderStats stats;

private:
    struct ChunkDraw { glm::ivec3 origin; uint32_t opaqueFirst, opaqueCount, waterFirst, waterCount; };

    SDL_GPUShader* shader(const std::vector<char>& source, const char* entry, SDL_GPUShaderStage stage,
                          uint32_t uniforms, uint32_t storage);
    SDL_GPUGraphicsPipeline* pipeline(SDL_GPUShader* vs, SDL_GPUShader* fs, bool depthTest, bool depthWrite, bool blend,
                                      SDL_GPUCullMode cull);
    SDL_GPUBuffer* upload(const void* data, uint32_t bytes);
    bool ensureTargets(uint32_t w, uint32_t h);
    bool saveScreenshot(SDL_GPUCommandBuffer* cmd, const char* path);

    SDL_Window* window_ = nullptr;
    SDL_GPUDevice* device_ = nullptr;
    SDL_GPUTextureFormat colorFormat_{};
    SDL_GPUSampleCount samples_ = SDL_GPU_SAMPLECOUNT_1;
    SDL_GPUGraphicsPipeline *sky_ = nullptr, *opaque_ = nullptr, *water_ = nullptr;
    SDL_GPUBuffer *worldOpaque_ = nullptr, *worldWater_ = nullptr, *character_ = nullptr;
    SDL_GPUTexture *msaaColor_ = nullptr, *depth_ = nullptr, *resolved_ = nullptr;
    uint32_t targetW_ = 0, targetH_ = 0;
    std::vector<ChunkDraw> chunks_;
    std::array<std::pair<uint32_t, uint32_t>, PartCount> parts_{};  // first quad, quad count
};
