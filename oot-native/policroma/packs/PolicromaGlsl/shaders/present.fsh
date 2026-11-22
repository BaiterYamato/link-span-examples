#version 450 core

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
layout(binding = 0) uniform texture2D sceneColor;
layout(binding = 1) uniform texture2D gradedScene;
layout(binding = 4) uniform sampler linearClamp;

void main() {
    vec4 scene = texture(sampler2D(sceneColor, linearClamp), uv);
#if POLICROMA_GRADE
    vec3 graded = texture(sampler2D(gradedScene, linearClamp), uv).rgb;
    outColor = vec4(mix(scene.rgb, graded, 0.65), scene.a);
#else
    outColor = scene;
#endif
}
