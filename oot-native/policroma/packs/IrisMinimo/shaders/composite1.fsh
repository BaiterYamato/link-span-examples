#version 120

// World-space ground mist. It only stays level with the terrain when the Iris matrices, the depth and the
// OpenGL orientation of texcoord all agree, so it doubles as a visual check of the reduced Iris pipeline.
uniform sampler2D colortex1;
uniform sampler2D depthtex0;
uniform mat4 gbufferProjectionInverse, gbufferModelViewInverse;
uniform vec3 cameraPosition;
uniform float far;

varying vec2 texcoord;

// Mist below this world height (OoT units; the Kokiri valley floor sits near -80).
const float MistTop = -20.0;
const float MistBottom = -80.0;

vec3 worldPosition(sampler2D depthTexture, vec2 coord) {
    float depth = texture2D(depthTexture, coord).r;
    vec4 view = gbufferProjectionInverse * vec4(coord * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    view /= view.w;
    return (gbufferModelViewInverse * vec4(view.xyz, 1.0)).xyz + cameraPosition;
}

void main() {
    vec3 color = texture2D(colortex1, texcoord).rgb;
    float depth = texture2D(depthtex0, texcoord).r;
    vec3 world = worldPosition(depthtex0, texcoord);
    float range = length(world - cameraPosition);

    float mist = 1.0 - smoothstep(MistBottom, MistTop, world.y);
    mist *= smoothstep(30.0, 250.0, range) * step(depth, 0.9999) * step(range, far);
    color = mix(color, vec3(0.74, 0.84, 0.52), mist * 0.5);

/* DRAWBUFFERS:1 */
    gl_FragData[0] = vec4(color, 1.0);
}
