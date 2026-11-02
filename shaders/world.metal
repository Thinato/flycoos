// All world shaders. Compiled at runtime by Metal through SDL_GPU (SDL_GPU_SHADERFORMAT_MSL).
// Binding order follows SDL_gpu.h (MSL): [[buffer]] = uniform buffers, then storage buffers.
//   voxel_vs: buffer(0) Frame uniforms, buffer(1) Draw uniforms, buffer(2) packed quads (storage)
//   *_fs:     buffer(0) Frame uniforms
#include <metal_stdlib>
using namespace metal;

constant uint kWater = 8;  // Block::Water in src/world.h

// Mirrors FrameUniforms / DrawUniforms in src/renderer.cpp (all float4 to avoid float3 padding).
struct Frame {
    float4x4 viewProj;
    float4x4 invViewProj;
    float4 eye;           // xyz camera
    float4 sunDir;        // xyz towards the sun (moon is -sunDir)
    float4 lightDir;      // xyz towards the active light (sun by day, moon by night)
    float4 lightColor;
    float4 skyZenith;
    float4 skyHorizon;
    float4 ambientSky;
    float4 ambientGround;
    float4 fog;           // x density, y night factor (0..1), z sun disk visibility, w sea surface y
    float4 palette[32];
};
struct Draw {
    float4x4 model;
    float4 hashOffset;    // shifts the per-voxel color jitter so chunks/parts don't repeat
    uint4 firstQuad;      // x: index of this draw's first quad in the storage buffer
};

struct VOut {
    float4 position [[position]];
    float3 worldPos;
    float3 voxelPos;
    float3 normal;
    float ao;
    uint block [[flat]];
};

constant float3 kNormals[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
// Corner index per vertex for the two triangles of a quad. Second set uses the other diagonal
// (picked when AO is anisotropic). Negative faces swap two vertices per triangle to stay CCW.
constant uint kTri[2][6] = {{0, 1, 2, 0, 2, 3}, {1, 2, 3, 1, 3, 0}};
constant uint kReverse[6] = {0, 2, 1, 3, 5, 4};

vertex VOut voxel_vs(uint vid [[vertex_id]],
                     constant Frame& f [[buffer(0)]],
                     constant Draw& d [[buffer(1)]],
                     const device uint2* quads [[buffer(2)]]) {
    // Draws always start at vertex 0 (SDL_gpu.h: first_vertex + vertex_id isn't portable),
    // so the quad offset comes from the Draw uniforms.
    uint2 q = quads[d.firstQuad.x + vid / 6];
    float3 p = float3(q.x & 63, (q.x >> 6) & 63, (q.x >> 12) & 63);
    float w = float(((q.x >> 18) & 63) + 1), h = float(((q.x >> 24) & 63) + 1);
    uint face = q.y & 7, block = (q.y >> 11) & 255;
    float ao[4] = {float((q.y >> 3) & 3), float((q.y >> 5) & 3), float((q.y >> 7) & 3), float((q.y >> 9) & 3)};

    uint axis = face >> 1, ua = (axis + 1) % 3, va = (axis + 2) % 3;
    bool positive = (face & 1) == 0;
    uint k = positive ? vid % 6 : kReverse[vid % 6];
    uint c = kTri[ao[0] + ao[2] < ao[1] + ao[3] ? 1 : 0][k];

    float3 local = p;
    if (positive) local[axis] += 1.0;
    local[ua] += (c == 1 || c == 2) ? w : 0.0;
    local[va] += (c >= 2) ? h : 0.0;
    float4 world = d.model * float4(local, 1.0);
    // Water surface sits a bit below the block top. Lower only vertices at sea level, so side faces
    // split at chunk borders don't open gaps inside a water wall.
    if (block == kWater && world.y > f.fog.w - 0.01) world.y -= 0.12;
    VOut out;
    out.position = f.viewProj * world;
    out.worldPos = world.xyz;
    out.voxelPos = local - kNormals[face] * 0.5 + d.hashOffset.xyz;
    out.normal = normalize((d.model * float4(kNormals[face], 0.0)).xyz);
    out.ao = ao[c] / 3.0;
    out.block = block;
    return out;
}

static float hash3(float3 p) {
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

// ACES filmic curve (Narkowicz fit): keeps bright sunlit surfaces from clipping to white.
static float3 tonemap(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

static float3 skyColor(constant Frame& f, float3 dir) {
    float up = saturate(dir.y);
    float3 c = mix(f.skyHorizon.rgb, f.skyZenith.rgb, sqrt(up));
    // Warm glow around the sun, strongest near the horizon.
    float glow = pow(saturate(dot(dir, f.sunDir.xyz)), 8.0) * (1.0 - up);
    return c + f.lightColor.rgb * glow * 0.25 * f.fog.z;
}

static float3 applyFog(constant Frame& f, float3 color, float3 worldPos) {
    float3 toPoint = worldPos - f.eye.xyz;
    float dist = length(toPoint);
    float t = 1.0 - exp(-pow(dist * f.fog.x, 2.0));
    return mix(color, skyColor(f, toPoint / dist), t);
}

static float3 light(constant Frame& f, VOut in, float3 albedo) {
    float3 n = normalize(in.normal);
    float ao = mix(0.35, 1.0, in.ao);
    float diffuse = saturate(dot(n, f.lightDir.xyz));
    float3 hemi = mix(f.ambientGround.rgb, f.ambientSky.rgb, n.y * 0.5 + 0.5);
    return albedo * (hemi * ao + f.lightColor.rgb * diffuse * mix(0.5, 1.0, in.ao));
}

fragment float4 voxel_fs(VOut in [[stage_in]], constant Frame& f [[buffer(0)]]) {
    float3 albedo = f.palette[in.block].rgb * (0.9 + 0.2 * hash3(floor(in.voxelPos)));
    return float4(tonemap(applyFog(f, light(f, in, albedo), in.worldPos)), 1.0);
}

fragment float4 water_fs(VOut in [[stage_in]], constant Frame& f [[buffer(0)]]) {
    float3 n = normalize(in.normal);
    float3 v = normalize(in.worldPos - f.eye.xyz);
    float fresnel = pow(1.0 - saturate(dot(-v, n)), 4.0);
    float3 color = mix(light(f, in, f.palette[in.block].rgb), skyColor(f, reflect(v, n)), fresnel * 0.7);
    float spec = pow(saturate(dot(reflect(v, n), f.lightDir.xyz)), 120.0);
    color += f.lightColor.rgb * spec * 0.6;
    return float4(tonemap(applyFog(f, color, in.worldPos)), mix(0.78, 0.95, fresnel));
}

// ---- Sky: fullscreen triangle, drawn first without depth ----

struct SkyOut {
    float4 position [[position]];
    float2 ndc;
};

vertex SkyOut sky_vs(uint vid [[vertex_id]]) {
    float2 p = float2((vid << 1) & 2, vid & 2) * 2.0 - 1.0;
    SkyOut out;
    out.position = float4(p, 0.0, 1.0);
    out.ndc = p;
    return out;
}

fragment float4 sky_fs(SkyOut in [[stage_in]], constant Frame& f [[buffer(0)]]) {
    float4 a = f.invViewProj * float4(in.ndc, 0.0, 1.0);
    float4 b = f.invViewProj * float4(in.ndc, 1.0, 1.0);
    float3 dir = normalize(b.xyz / b.w - a.xyz / a.w);
    float3 c = skyColor(f, dir);

    float sunDot = dot(dir, f.sunDir.xyz);
    c += float3(1.0, 0.95, 0.8) * (smoothstep(0.9994, 0.9997, sunDot) * 6.0 + pow(saturate(sunDot), 300.0)) * f.fog.z;

    float night = f.fog.y;
    float moonDot = dot(dir, -f.sunDir.xyz);
    c += float3(0.75, 0.8, 0.95) * smoothstep(0.9990, 0.9993, moonDot) * night;
    float star = step(0.9985, hash3(floor(dir * 400.0)));
    c += star * night * saturate(dir.y * 4.0) * 0.7;
    return float4(tonemap(c), 1.0);
}
