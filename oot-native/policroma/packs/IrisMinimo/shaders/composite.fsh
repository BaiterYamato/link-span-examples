#version 120

// Iris style of the 1.12 era on purpose: varying, texture2D, gl_FragData and DRAWBUFFERS.
uniform sampler2D colortex0;
uniform sampler2D depthtex0;
uniform float near;
uniform float far;

varying vec2 texcoord;

#include "/lib/common.glsl"

void main() {
    vec3 color = texture2D(colortex0, texcoord).rgb;
    float depth = linearDepth(texture2D(depthtex0, texcoord).r);
    // Aerial perspective: distant geometry drifts toward a cool haze; the sky (far plane) keeps its color.
    float haze = smoothstep(far * 0.04, far * 0.55, depth) * step(depth, far * 0.99);
    vec3 hazed = mix(color, vec3(0.62, 0.70, 0.78) * (0.55 + 0.45 * luminance(color)), haze * 0.45);

/* DRAWBUFFERS:1 */
    gl_FragData[0] = vec4(hazed, 1.0);
}
