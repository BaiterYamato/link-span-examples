#version 150

// Modern Iris style: in/out, gl_FragCoord and the frame uniforms.
uniform sampler2D colortex1;
uniform float viewWidth, viewHeight;
uniform float frameTimeCounter;
uniform int worldTime;

in vec2 texcoord;
out vec4 fragColor;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

void main() {
    vec3 color = texture(colortex1, texcoord).rgb;

    // Night (worldTime 13000..23000) leans blue; the day keeps the game's own colors.
    float night = smoothstep(12500.0, 13500.0, float(worldTime)) * (1.0 - smoothstep(22500.0, 23500.0, float(worldTime)));
    color = mix(color, color * vec3(0.82, 0.90, 1.12), night * 0.6);

    // Soft vignette from the pixel position, so it does not depend on texcoord orientation.
    vec2 centered = gl_FragCoord.xy / vec2(viewWidth, viewHeight) - 0.5;
    centered.x *= viewWidth / viewHeight;
    color *= 1.0 - 0.38 * smoothstep(0.35, 0.95, length(centered));

    // Fine grain that moves with game time (20 Hz frames).
    float grain = hash(floor(gl_FragCoord.xy) + floor(frameTimeCounter * 20.0)) - 0.5;
    color += grain * 0.018;

/* DRAWBUFFERS:0 */
    fragColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
