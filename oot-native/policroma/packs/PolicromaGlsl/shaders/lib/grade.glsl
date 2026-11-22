vec3 gradeColor(vec3 color, float depth) {
    float distanceTint = smoothstep(0.93, 0.997, depth);
    vec3 warm = color * vec3(1.10, 0.94, 0.75) + vec3(0.08, 0.035, 0.02);
    vec3 cool = color * vec3(0.77, 0.95, 1.18) + vec3(0.02, 0.045, 0.08);
    return mix(warm, cool, distanceTint);
}
