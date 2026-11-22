#ifndef POLICROMA_OCCLUSION
#define POLICROMA_OCCLUSION

static const float AmbientRadius = 72.0;
static const float2 AmbientDirections[8] = {
    float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
    float2(0.7071, 0.7071), float2(-0.7071, 0.7071),
    float2(0.7071, -0.7071), float2(-0.7071, -0.7071),
};

bool AmbientArea(uint area) {
    return UnboundArea(area) || area == 0x49u;
}

float3 ReadAmbientPosition(int2 pixel, uint2 size) {
    pixel = clamp(pixel, 0, int2(size) - 1);
    float distance = ViewDepth(SceneDepth.Load(int3(pixel, 0)));
    return ViewPosition((float2(pixel) + 0.5) / size, distance);
}

float3 AmbientTangent(int2 pixel, uint2 size, int2 axis, float3 center) {
    float3 before = center - ReadAmbientPosition(pixel - axis, size);
    float3 after = ReadAmbientPosition(pixel + axis, size) - center;
    // A one-sided derivative avoids using a background sample at a silhouette.
    float beforeLength = dot(before, before);
    float afterLength = dot(after, after);
    if (beforeLength < 0.00001) return after;
    if (afterLength < 0.00001) return before;
    return beforeLength < afterLength ? before : after;
}

float AmbientTap(int2 pixel, uint2 size, float3 center, float3 normal, float bias) {
    if (any(pixel < 0) || any(pixel >= int2(size))) return 0.0;
    float3 delta = ReadAmbientPosition(pixel, size) - center;
    float distanceSquared = dot(delta, delta);
    if (distanceSquared < 0.01 || distanceSquared >= AmbientRadius * AmbientRadius) return 0.0;
    float elevation = saturate(2.0 * (dot(delta, normal) - bias) * rsqrt(distanceSquared));
    float range = 1.0 - distanceSquared / (AmbientRadius * AmbientRadius);
    return elevation * range * range;
}

float AmbientObscurance(float2 uv, uint2 size) {
    int2 pixel = min(int2(uv * size), int2(size) - 1);
    float3 center = ReadAmbientPosition(pixel, size);
    float distance = -center.z;
    if (distance < 4.0 || distance >= min(1800.0, CameraInfo.y - 1.0)) return 0.0;

    float3 dx = AmbientTangent(pixel, size, int2(1, 0), center);
    float3 dy = AmbientTangent(pixel, size, int2(0, 1), center);
    float maxStep = max(16.0, distance * 0.035);
    float3 normal = cross(dx, dy);
    float lengthSquared = dot(normal, normal);
    if (lengthSquared < 0.00001 || max(dot(dx, dx), dot(dy, dy)) > maxStep * maxStep) return 0.0;
    normal *= (dot(normal, -center) < 0.0 ? -1.0 : 1.0) * rsqrt(lengthSquared);

    float2 focalPixels = 0.5 * abs(float2(Projection[0][0], Projection[1][1])) * size;
    float2 radiusPixels = clamp(focalPixels * AmbientRadius / distance, 3.0, 68.0);
    float bias = max(1.25, distance * 0.003);
    float obscurance = 0.0;
    [unroll] for (int i = 0; i < 8; ++i) {
        float2 offset = AmbientDirections[i] * radiusPixels;
        // Accumulate along each ray so a single thin surface contributes only its sampled extent.
        [unroll] for (int step = 0; step < 4; ++step) {
            float radiusFraction = 0.16 + 0.21 * step;
            obscurance += AmbientTap(pixel + int2(round(offset * radiusFraction)), size, center, normal, bias);
        }
    }
    return (1.0 - exp(-obscurance * 0.22)) * (1.0 - smoothstep(1100.0, 1800.0, distance));
}

#endif
