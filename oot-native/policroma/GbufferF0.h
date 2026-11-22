#pragma once

#include <string_view>

namespace ShaderPacks {

// F0 gbuffer pass (logs/policroma-001/gbuffers-minimos): a synthetic Iris program that runs before the pack's first
// program and fills colortex4 / colortex6 / colortex3 from the scene depth alone. The IrisPack builder prepends
// "#define LINKSPAN_F0_NORMAL / _MATERIAL / _TRANSLUCENT 0|1" after the #version line, one per buffer some pack
// program reads, and writes only those. This text is the single source of the pass.
//
//   colortex4.rgb = world normal (player space: rotation only, Y up), raw xyz in [-1, 1], a = 0. The pack reads it as
//                   normalM = mat3(gbufferModelView) * colortex4.rgb and rewrites .a with its own fresnel.
//   colortex6     = (smoothnessD, materialMask / 255, skyLightFactor)
//   colortex3     = 0 (translucentMult 1.0, no tint)
//
// The normal comes from the depth: per axis, the side whose linear extrapolation of the hardware depth (affine in 1/z,
// so linear on screen for any plane) best predicts the center texel, which keeps the normal off the far side of an edge.
// Edge texels fade to the eye direction (fresnel 0: no reflection, no outline on the silhouette).
inline constexpr std::string_view GbufferF0Glsl = R"GLSL(#version 130

// ---- Options (precedence: a -D from the builder, then these defaults) ----------------------------------------------

#ifndef LINKSPAN_F0_NORMAL
    #define LINKSPAN_F0_NORMAL 1
#endif
#ifndef LINKSPAN_F0_MATERIAL
    #define LINKSPAN_F0_MATERIAL 1
#endif
#ifndef LINKSPAN_F0_TRANSLUCENT
    #define LINKSPAN_F0_TRANSLUCENT 0
#endif

// Taps per side on each axis: 2 = linear extrapolation (robust on slopes and edges); 1 = plain difference.
#ifndef LINKSPAN_F0_NORMAL_TAPS
    #define LINKSPAN_F0_NORMAL_TAPS 2
#endif

// colortex6.r of the nearly horizontal surfaces; walls and slopes stay at 0 (no reflection). A constant turns the
// reflection on for every flat floor, water included, not only for water: it is the F0 trade-off.
#ifndef LINKSPAN_F0_FLAT_SMOOTHNESS
    #define LINKSPAN_F0_FLAT_SMOOTHNESS 0.75
#endif
// World normal.y from which a surface counts as horizontal: 0.92 = ~23 degrees of tilt.
#ifndef LINKSPAN_F0_FLAT_NY_MIN
    #define LINKSPAN_F0_FLAT_NY_MIN 0.92
#endif

// materialMask (0..255). With RP_MODE 1 (iPBR) the pack only acts on 1, 2, 3, 5 and 100..199; 0 is "no special material".
#ifndef LINKSPAN_F0_MATERIAL_DEFAULT
    #define LINKSPAN_F0_MATERIAL_DEFAULT 0
#endif

// Relative extrapolation error (in linear distance) between these two limits blends the normal toward the eye.
#ifndef LINKSPAN_F0_EDGE_LO
    #define LINKSPAN_F0_EDGE_LO 0.02
#endif
#ifndef LINKSPAN_F0_EDGE_HI
    #define LINKSPAN_F0_EDGE_HI 0.08
#endif

// The diagonal residual is ~0 on any plane (unlike the per-axis ones, which a shallow crease also keeps small), so it
// counts this many times toward the edge.
#ifndef LINKSPAN_F0_DIAGONAL_GAIN
    #define LINKSPAN_F0_DIAGONAL_GAIN 4.0
#endif

// gl_FragData slot of each buffer: they are written in the order colortex4, 6, 3, only the requested ones.
#define LINKSPAN_F0_SLOT_NORMAL 0
#define LINKSPAN_F0_SLOT_MATERIAL (LINKSPAN_F0_NORMAL)
#define LINKSPAN_F0_SLOT_TRANSLUCENT (LINKSPAN_F0_NORMAL + LINKSPAN_F0_MATERIAL)

// ---- Uniforms (Iris names; each has an alias in IrisPack.cpp) -----------------------------------------------------

uniform sampler2D depthtex0;
uniform float viewWidth;
uniform float viewHeight;
uniform mat4 gbufferProjectionInverse;
uniform mat4 gbufferModelViewInverse;
// 0..1 sky light: the provider's eyeBrightness sky / 240 (LinkSpanIris vector 11 .w), 1 without provider parameters.
uniform float linkspanSkyLight;

const float DEPTH_SKY = 1.0;
const float NO_CANDIDATE = 1.0e9;

ivec2 ViewSize() {
    return ivec2(viewWidth, viewHeight);
}

bool InBounds(ivec2 texel) {
    ivec2 size = ViewSize();
    return texel.x >= 0 && texel.y >= 0 && texel.x < size.x && texel.y < size.y;
}

float SceneDepth(ivec2 texel) {
    return texelFetch(depthtex0, clamp(texel, ivec2(0), ViewSize() - 1), 0).r;
}

// Distance along the view axis (positive) from the hardware depth [0, 1].
float LinearViewZ(float depth) {
    vec2 zw = (depth * 2.0 - 1.0) * gbufferProjectionInverse[2].zw + gbufferProjectionInverse[3].zw;
    return -zw.x / zw.y;
}

// View-space position (looking down -Z) of the texel center; the texel origin is OpenGL's (bottom), as in the pack.
vec3 ViewPosition(ivec2 texel, float depth) {
    vec2 uv = (vec2(texel) + 0.5) / vec2(viewWidth, viewHeight);
    vec4 viewPos = gbufferProjectionInverse * vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    return viewPos.xyz / viewPos.w;
}

// P(chosen side) - P0 oriented toward +axis; `edge` receives the relative prediction error (linear distance), `side`
// the chosen direction along the axis (+1/-1, 0 without a neighbour) and `sideDepth` the depth of that neighbour.
vec3 AxisDelta(ivec2 texel, ivec2 axis, float d0, float z0, vec3 p0, out float edge, out int side, out float sideDepth) {
    float dPos1 = SceneDepth(texel + axis);
    float dNeg1 = SceneDepth(texel - axis);
    float errPos = NO_CANDIDATE;
    float errNeg = NO_CANDIDATE;
    float predPos = d0;
    float predNeg = d0;

    #if LINKSPAN_F0_NORMAL_TAPS >= 2
        if (InBounds(texel + 2 * axis)) {
            predPos = 2.0 * dPos1 - SceneDepth(texel + 2 * axis);
            errPos = abs(predPos - d0);
        }
        if (InBounds(texel - 2 * axis)) {
            predNeg = 2.0 * dNeg1 - SceneDepth(texel - 2 * axis);
            errNeg = abs(predNeg - d0);
        }
    #else
        if (InBounds(texel + axis)) {
            predPos = dPos1;
            errPos = abs(dPos1 - d0);
        }
        if (InBounds(texel - axis)) {
            predNeg = dNeg1;
            errNeg = abs(dNeg1 - d0);
        }
    #endif

    bool usePos = errPos < errNeg;
    float pred = usePos ? predPos : predNeg;
    float best = usePos ? errPos : errNeg;
    edge = best >= NO_CANDIDATE
        ? 1.0
        : abs(LinearViewZ(clamp(pred, 0.0, 0.9999999)) - z0) / z0;

    side = usePos ? 1 : -1;
    sideDepth = usePos ? dPos1 : dNeg1;
    if (best >= NO_CANDIDATE) {
        side = 0;
        return vec3(0.0); // no neighbour: the caller falls back to the eye vector
    }
    if (usePos) return ViewPosition(texel + axis, dPos1) - p0;
    return p0 - ViewPosition(texel - axis, dNeg1);
}

// View normal facing the eye. `edgeFade` 0 = inside the surface, 1 = edge / no neighbour.
vec3 ReconstructViewNormal(ivec2 texel, float d0, out float edgeFade) {
    vec3 p0 = ViewPosition(texel, d0);
    float z0 = LinearViewZ(d0);
    vec3 towardEye = -normalize(p0);

    float edgeX, edgeY, depthX, depthY;
    int sideX, sideY;
    vec3 dx = AxisDelta(texel, ivec2(1, 0), d0, z0, p0, edgeX, sideX, depthX); // +X of the screen
    vec3 dy = AxisDelta(texel, ivec2(0, 1), d0, z0, p0, edgeY, sideY, depthY); // +Y of the screen (up, GL origin)

    // Both axes may each fit a different surface (a step meeting a crease): on one plane the diagonal texel between
    // the two chosen sides has the depth sum of the two steps, so a mismatch there is an edge as well.
    float edgeDiagonal = 0.0;
    if (sideX != 0 && sideY != 0) {
        float predicted = depthX + depthY - d0;
        float actual = SceneDepth(texel + ivec2(sideX, sideY));
        edgeDiagonal = LINKSPAN_F0_DIAGONAL_GAIN * abs(LinearViewZ(clamp(predicted, 0.0, 0.9999999)) - LinearViewZ(actual)) / z0;
    }

    edgeFade = smoothstep(LINKSPAN_F0_EDGE_LO, LINKSPAN_F0_EDGE_HI, max(max(edgeX, edgeY), edgeDiagonal));

    vec3 n = cross(dx, dy);
    float len = length(n);
    if (len < 1.0e-12) {
        edgeFade = 1.0;
        return towardEye;
    }
    n /= len;
    if (dot(n, towardEye) < 0.0) n = -n; // guards against a sign flipped by quantization
    return n;
}

void main() {
    ivec2 texel = ivec2(gl_FragCoord.xy);
    float d0 = SceneDepth(texel);

    vec3 normalWorld = vec3(0.0);
    float smoothness = 0.0;
    float materialMask = 0.0;
    float skyLight = 0.0;

    if (d0 < DEPTH_SKY) {
        float edgeFade;
        vec3 viewNormal = ReconstructViewNormal(texel, d0, edgeFade);
        // On an edge the normal becomes the eye direction: fresnel 0, no reflection or outline on the silhouette.
        vec3 towardEye = -normalize(ViewPosition(texel, d0));
        viewNormal = normalize(mix(viewNormal, towardEye, edgeFade));

        normalWorld = normalize(mat3(gbufferModelViewInverse) * viewNormal);

        float flatness = smoothstep(LINKSPAN_F0_FLAT_NY_MIN, 1.0, normalWorld.y);
        smoothness = LINKSPAN_F0_FLAT_SMOOTHNESS * flatness * (1.0 - edgeFade);
        materialMask = float(LINKSPAN_F0_MATERIAL_DEFAULT) / 255.0;
        skyLight = clamp(linkspanSkyLight, 0.0, 1.0);
    }

    #if LINKSPAN_F0_NORMAL && LINKSPAN_F0_MATERIAL && LINKSPAN_F0_TRANSLUCENT
        /* DRAWBUFFERS:463 */
    #elif LINKSPAN_F0_NORMAL && LINKSPAN_F0_MATERIAL
        /* DRAWBUFFERS:46 */
    #elif LINKSPAN_F0_NORMAL && LINKSPAN_F0_TRANSLUCENT
        /* DRAWBUFFERS:43 */
    #elif LINKSPAN_F0_MATERIAL && LINKSPAN_F0_TRANSLUCENT
        /* DRAWBUFFERS:63 */
    #elif LINKSPAN_F0_NORMAL
        /* DRAWBUFFERS:4 */
    #elif LINKSPAN_F0_MATERIAL
        /* DRAWBUFFERS:6 */
    #else
        /* DRAWBUFFERS:3 */
    #endif
    #if LINKSPAN_F0_NORMAL
        gl_FragData[LINKSPAN_F0_SLOT_NORMAL] = vec4(normalWorld, 0.0);
    #endif
    #if LINKSPAN_F0_MATERIAL
        gl_FragData[LINKSPAN_F0_SLOT_MATERIAL] = vec4(smoothness, materialMask, skyLight, 1.0);
    #endif
    #if LINKSPAN_F0_TRANSLUCENT
        gl_FragData[LINKSPAN_F0_SLOT_TRANSLUCENT] = vec4(0.0);
    #endif
}
)GLSL";

} // namespace ShaderPacks
