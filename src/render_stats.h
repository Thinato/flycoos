#pragma once
#include <cstddef>

// What the renderer last did, for the debug UI and the editor panel (no GPU types).
struct RenderStats {
    size_t opaqueQuads = 0, waterQuads = 0, quadsDrawn = 0;
    int chunksTotal = 0, chunksDrawn = 0, drawCalls = 0;
    double genSeconds = 0, meshSeconds = 0;  // last world generation / meshing + upload
    double lightSeconds = 0;  // last world light fill (fillLight)
    size_t lightBytes = 0;    // light arrays allocated (light.h)
    int lightChunks = 0;      // chunks with their own light array
    double relightMs = 0;     // last updateWorld: relight (part of remeshMs)
    double remeshMs = 0;      // last updateWorld: relight + remesh + upload
    int chunksRemeshed = 0;   // by the last updateWorld
    int repacks = 0;          // updateWorld calls that ran out of buffer headroom and repacked everything
};
