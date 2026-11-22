#version 450 core
#include "/lib/grade.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform texture2D sceneColor;
layout(binding = 1) uniform texture2D sceneDepth;
layout(binding = 4) uniform sampler linearClamp;

void main() {
    vec4 scene = texture(sampler2D(sceneColor, linearClamp), uv);
    float depth = texture(sampler2D(sceneDepth, linearClamp), uv).r;
    outColor = vec4(gradeColor(scene.rgb, depth), scene.a);
}
