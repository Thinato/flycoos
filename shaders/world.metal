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
    float4 debug;         // x view mode: 0 shaded, 1 greedy quads, 2 chunks, 3 AO, 4 normals, 5 light
    float4 aoCurve;       // ambient factor for AO levels 0..3 (applied per vertex)
    float4 shadowTint;    // rgb multiplier occluded albedo shades toward, a strength (weaker at night)
    float4 lightParams;   // voxel light: x sun gate, y falloff, z dark ambient, w on (else all open sky)
    float4 emberColor;    // rgb block light color (linear, times strength)
    float4 palette[128];
    float4 toneA[128];     // patch variants (ADR-028), rgb linear; w patch size in voxels
    float4 toneB[128];     // rgb linear; w strength (0: plain)
};
struct Draw {
    float4x4 model;
    float4 modelLight;    // x models: light + 1 (0: a chunk)
    uint4 firstQuad;      // x: index of this draw's first quad in the storage buffer
};

struct VOut {
    float4 position [[position]];
    float3 worldPos;
    float3 normal;
    float ao;  // aoCurve value: 1 open, lower in corners
    // block id in bits 0-6, the quad's light (sky << 4 | block) in bits 7-14, quad index in bits
    // 15-30 (for the greedy-quads debug view), bit 31 set for models (flat colour). Packed into one
    // output because every extra vertex output costs bandwidth on tile-based GPUs (~15% here).
    uint blockAndQuad [[flat]];
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
    bool model = d.modelLight.x > 0.0;
    uint lit = model ? uint(d.modelLight.x) - 1 : (q.y >> 19) & 255;  // models: the light where they stand
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
    out.normal = normalize((d.model * float4(kNormals[face], 0.0)).xyz);
    out.ao = f.aoCurve[uint(ao[c])];
    out.blockAndQuad = block | lit << 7 | ((d.firstQuad.x + vid / 6) & 0xFFFFu) << 15 | uint(model) << 31;  // ponytail: quad index wraps past 64K quads (debug color only)
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

// Undoes the AO curve: 0 at the fully occluded end, 1 open (the old linear AO for a linear curve).
static float rawAO(constant Frame& f, float o) {
    return saturate((o - f.aoCurve.x) / max(1.0 - f.aoCurve.x, 1e-4));
}

// Brightness of a light level 0..15: x / (f - (f-1) x), x = level / 15 (f = 1 is linear).
static float lightCurve(constant Frame& f, uint level) {
    float x = float(level) / 15.0, k = f.lightParams.y;
    return x / (k - (k - 1.0) * x);
}

static uint voxelLight(constant Frame& f, VOut in) {
    return f.lightParams.w > 0.0 ? (in.blockAndQuad >> 7) & 255u : 0xF0u;
}

static float3 light(constant Frame& f, VOut in, float3 albedo) {
    float3 n = normalize(in.normal);
    uint lit = voxelLight(f, in);
    float sky = lightCurve(f, lit >> 4), glow = lightCurve(f, lit & 15u);
    float o = in.ao * sky;  // occlusion: AO times how much sky the voxel in front sees, 1 = open
    // Occluded albedo shifts toward a cool tint under sky light, so light still sets the value and
    // corners and interiors read as a deeper hue instead of grey. Ember light sees the plain albedo.
    float3 tinted = mix(albedo, albedo * f.shadowTint.rgb, f.shadowTint.a * (1.0 - o));
    // No shadow map: direct light needs a view of the sky (fades in over sky light sunGate..1).
    float diffuse = saturate(dot(n, f.lightDir.xyz)) * smoothstep(f.lightParams.x, 1.0, float(lit >> 4) / 15.0);
    float3 hemi = mix(f.ambientGround.rgb, f.ambientSky.rgb, n.y * 0.5 + 0.5);
    // Direct light is occluded half as much as ambient (0.5 at the fully occluded curve end).
    float sunAO = mix(0.5, 1.0, rawAO(f, in.ao));
    float3 skyLit = tinted * (hemi * o + f.lightColor.rgb * diffuse * sunAO);
    // Ember light ignores the time of day, so embers glow the same at night. It fades out where the
    // sky's light is brighter (gone at twice as bright; a soft form of Minecraft's max of sky and
    // block light), so it doesn't tint sunlit ground; a per-channel max would turn blue sky light
    // plus orange firelight purple. The dark floor keeps unlit voxels from going black.
    float3 emberLit = albedo * f.emberColor.rgb * glow * in.ao;
    constexpr float3 kLuma = float3(0.2126, 0.7152, 0.0722);
    float emberShare = saturate(1.0 - 0.5 * dot(skyLit, kLuma) / max(dot(emberLit, kLuma), 1e-4));
    return skyLit + emberLit * emberShare + albedo * f.lightParams.z * (1.0 - sky) * in.ao;
}

static uint pcg(uint n) {
    n = n * 747796405u + 2891336453u;
    n = ((n >> ((n >> 28u) + 4u)) ^ n) * 277803737u;
    return (n >> 22u) ^ n;
}

// Distinct, fairly bright color per id.
static float3 idColor(uint n) {
    n = pcg(n);
    return float3(n & 255u, (n >> 8) & 255u, (n >> 16) & 255u) / 255.0 * 0.75 + 0.25;
}

// Gradient directions for noise (Perlin's 12 cube edges, 4 repeated to make 16).
constant float3 kGrad[16] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1}, {1, 0, -1},
                             {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}, {1, 1, 0}, {-1, 1, 0},
                             {0, -1, 1}, {0, -1, -1}};

// Two independent gradient-noise values (about -1..1) from lattice corner i + o, offset r from
// it: one PCG hash per corner picks a gradient for each field. `seed`: the block.
static float2 corner(int3 i, int3 o, float3 r, uint seed) {
    int3 c = i + o;
    uint n = pcg(uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ seed * 2654435761u);
    float3 d = r - float3(o);
    return float2(dot(kGrad[n & 15u], d), dot(kGrad[(n >> 16) & 15u], d));
}

// Two smooth gradient-noise fields (Perlin, quintic fade). Value noise was tried first: its
// trilinear blend makes axis-aligned bands that read as stripes once quantised per voxel.
static float2 noise2(float3 p, uint seed) {
    int3 i = int3(floor(p));
    float3 r = fract(p);
    float3 u = r * r * r * (r * (r * 6.0 - 15.0) + 10.0);
    float2 x00 = mix(corner(i, int3(0, 0, 0), r, seed), corner(i, int3(1, 0, 0), r, seed), u.x);
    float2 x10 = mix(corner(i, int3(0, 1, 0), r, seed), corner(i, int3(1, 1, 0), r, seed), u.x);
    float2 x01 = mix(corner(i, int3(0, 0, 1), r, seed), corner(i, int3(1, 0, 1), r, seed), u.x);
    float2 x11 = mix(corner(i, int3(0, 1, 1), r, seed), corner(i, int3(1, 1, 1), r, seed), u.x);
    return mix(mix(x00, x10, u.y), mix(x01, x11, u.y), u.z);
}

// One colour per voxel (docs/ART.md): the block's palette colour, faded toward its two variants by
// two smooth noise fields sampled at the voxel cell, so neighbours step gently and a face is uniform.
// Colour is per block, so material edges stay sharp. Models and strength-0 blocks are flat.
static float3 albedoOf(constant Frame& f, VOut in) {
    uint block = in.blockAndQuad & 127u;
    float3 c = f.palette[block].rgb;
    float strength = f.toneB[block].w;
    if (strength <= 0.0 || (in.blockAndQuad >> 31) != 0u) return c;
    float3 cell = floor(in.worldPos - in.normal * 0.5) + 0.5;  // chunks draw in world voxel coords
    float2 t = saturate(smoothstep(0.0, 0.4, noise2(cell / f.toneA[block].w, block)) * strength);
    return mix(mix(c, f.toneA[block].rgb, t.x), f.toneB[block].rgb, t.y);
}

// Light levels debug view (display values): sky blue-white, block orange, shaded a little by facing.
static float3 lightLevels(VOut in) {
    uint lit = (in.blockAndQuad >> 7) & 255u;
    float3 c = float(lit >> 4) / 15.0 * float3(0.55, 0.7, 1.0) + float(lit & 15u) / 15.0 * float3(1.0, 0.5, 0.1);
    return pow(saturate(c * (0.8 + 0.2 * normalize(in.normal).y)), 2.2);
}

fragment float4 voxel_fs(VOut in [[stage_in]], constant Frame& f [[buffer(0)]]) {
    uint mode = uint(f.debug.x);
    // Unlit debug views are written as display values: the sRGB target would otherwise brighten them.
    if (mode == 3) return float4(float3(pow(mix(0.05, 1.0, rawAO(f, in.ao)), 2.2)), 1.0);  // raw AO, independent of the curve
    if (mode == 4) return float4(pow(normalize(in.normal) * 0.5 + 0.5, 2.2), 1.0);  // normals as color
    if (mode == 5) return float4(lightLevels(in), 1.0);
    float3 albedo;
    if (mode == 0) {  // normal path; `mode` is uniform, so this branch is coherent and cheap
        albedo = albedoOf(f, in);
    } else if (mode == 1) {  // greedy quads
        albedo = idColor((in.blockAndQuad >> 15) & 0xFFFFu);
    } else {  // chunks: 32^3 cell containing the surface (nudged inside along the normal)
        uint3 cell = uint3(int3(floor((in.worldPos - in.normal * 0.01) / 32.0)));
        albedo = idColor(cell.x * 73856093u ^ cell.y * 19349663u ^ cell.z * 83492791u);
    }
    return float4(tonemap(applyFog(f, light(f, in, albedo), in.worldPos)), 1.0);
}

fragment float4 wire_fs(VOut in [[stage_in]]) {
    return float4(0.01, 0.01, 0.015, 1.0);
}

fragment float4 water_fs(VOut in [[stage_in]], constant Frame& f [[buffer(0)]]) {
    if (uint(f.debug.x) == 5) return float4(lightLevels(in), 0.85);
    float3 n = normalize(in.normal);
    float3 v = normalize(in.worldPos - f.eye.xyz);
    float fresnel = pow(1.0 - saturate(dot(-v, n)), 4.0);
    float3 color = mix(light(f, in, f.palette[in.blockAndQuad & 127u].rgb), skyColor(f, reflect(v, n)), fresnel * 0.7);
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
