#ifndef KOKIRI_TRANSMISSION
#define KOKIRI_TRANSMISSION

cbuffer LinkSpanTransmission : register(b3) {
    float4 TransmissionData[153];
};

bool CrossesGlass(float3 receiver, float3 ray, uint material) {
    uint count = uint(TransmissionData[0].y);
    [loop] for (uint i = 0; i < count; ++i) {
        uint index = 9 + 3 * i;
        float4 a = TransmissionData[index];
        if (uint(a.w) != material) continue;
        float3 e1 = TransmissionData[index + 1].xyz - a.xyz;
        float3 e2 = TransmissionData[index + 2].xyz - a.xyz;
        float3 p = cross(ray, e2);
        float det = dot(e1, p);
        if (abs(det) < 0.0001) continue;
        float3 offset = receiver - a.xyz;
        float u = dot(offset, p) / det;
        if (u < -0.0001 || u > 1.0001) continue;
        float3 q = cross(offset, e1);
        float v = dot(ray, q) / det;
        if (v < -0.0001 || u + v > 1.0001) continue;
        float t = dot(e2, q) / det;
        if (t > 0.01 && t < 0.99) return true;
    }
    return false;
}

float GlassCoverage(float3 receiver, float3 ray, uint material) {
    // Four points on the emitter soften the aperture in world space.
    float coverage = 0.0;
    [unroll] for (int j = 0; j < 4; ++j) {
        float3 offset = float3((j & 1) ? 4.0 : -4.0, 0.0, (j & 2) ? 4.0 : -4.0);
        if (CrossesGlass(receiver, ray + offset, material)) coverage += 0.25;
    }
    return coverage;
}

float3 TransmittedLight(float3 receiver, float3 normal) {
    if (TransmissionData[0].y < 1.0) return 0.0;
    float3 color = 0.0;
    [loop] for (uint i = 0; i < uint(TransmissionData[0].x); ++i) {
        float4 tint = TransmissionData[1 + 2 * i];
        float4 light = TransmissionData[2 + 2 * i];
        float3 ray = light.xyz - receiver;
        float distance = length(ray);
        if (distance >= light.w || distance < 1.0) continue;
        float3 toLight = ray / distance;
        float incidence = saturate(dot(normal, toLight));
#ifdef KOKIRI_TRANSMISSION_CONE
        incidence *= smoothstep(0.76, 0.97, dot(-toLight, KOKIRI_TRANSMISSION_CONE));
#endif
        if (incidence <= 0.0 || tint.w <= 0.0) continue;

        float coverage = GlassCoverage(receiver, ray, i);
        float attenuation = 1.0 - smoothstep(0.1, 1.0, distance / light.w);
        color += tint.rgb * tint.w * coverage * incidence * attenuation;
    }
    return color;
}

#if POLICROMA_GLASS_LIGHTING
#ifdef KOKIRI_POINT_SHADOW
#ifndef KOKIRI_POINT_SHADOW_LIGHTS
#define KOKIRI_POINT_SHADOW_LIGHTS 1
#endif
Texture2D<float> PointShadowDepth : register(t2);
cbuffer LinkSpanPointShadow : register(b4) {
    // Each pair contains position and range, then light index + 1, near, face size and overscan.
    float4 PointShadowData[2 * KOKIRI_POINT_SHADOW_LIGHTS];
};

// A negative result requests the screen-space fallback when this emitter has no usable map.
float PointTransmissionVisibility(float3 receiver, float3 normal, uint lightIndex) {
    if (lightIndex >= KOKIRI_POINT_SHADOW_LIGHTS) return -1.0;
    float4 source = PointShadowData[2u * lightIndex];
    float4 parameters = PointShadowData[2u * lightIndex + 1u];
    if (uint(parameters.x) != lightIndex + 1u) return -1.0;
    float3 relative = receiver + normal * 1.2 - source.xyz;
    float3 axis = abs(relative);
    float depth = max(axis.x, max(axis.y, axis.z));
    float nearPlane = parameters.y, farPlane = source.w;
    if (depth <= nearPlane || depth >= farPlane) return -1.0;

    // Match the +X, -X, +Y, -Y, +Z, -Z raster views, in that order.
    uint face;
    float2 xy;
    if (axis.x >= axis.y && axis.x >= axis.z) {
        face = relative.x >= 0.0 ? 0u : 1u;
        xy = float2(face == 0u ? -relative.z : relative.z, relative.y);
    } else if (axis.y >= axis.z) {
        face = relative.y >= 0.0 ? 2u : 3u;
        xy = float2(face == 2u ? -relative.x : relative.x, relative.z);
    } else {
        face = relative.z >= 0.0 ? 4u : 5u;
        xy = float2(face == 4u ? relative.x : -relative.x, relative.y);
    }
    int size = int(parameters.z);
    int2 tile = int2(face % 3u, face / 3u + 2u * lightIndex) * size;
    float2 uv = xy / depth * parameters.w * float2(0.5, -0.5) + 0.5;
    float2 pixel = uv * size - 0.5;
    int2 base = int2(floor(pixel));
    float2 fraction = frac(pixel);
    float reverseDepth = nearPlane * (farPlane - depth) / ((farPlane - nearPlane) * depth);
    float visibility = 0.0;
    [unroll] for (int y = 0; y < 2; ++y) {
        [unroll] for (int x = 0; x < 2; ++x) {
            int2 coord = tile + clamp(base + int2(x, y), 0, size - 1);
            float nearest = PointShadowDepth.Load(int3(coord, 0));
            float2 weight = 1.0 - abs(float2(x, y) - fraction);
            visibility += weight.x * weight.y * step(nearest, reverseDepth + 0.00006);
        }
    }
    return visibility;
}
#endif

float TransmissionVisibility(float3 receiver, float3 normal, float3 source, uint2 size) {
    float3 start = mul(float4(receiver + normal * 2.0, 1.0), View).xyz;
    float3 end = mul(float4(source, 1.0), View).xyz;
    float shadow = 0.0;
    [loop] for (int step = 1; step <= 16; ++step) {
        float t = step / 17.0;
        float3 samplePoint = lerp(start, end, t);
        float2 uv;
        if (!ProjectView(samplePoint, uv)) continue;
        float depth = -samplePoint.z;
        float gap = depth - DepthAt(uv, size);
        float thickness = max(6.0, depth * 0.025);
        float hit = smoothstep(0.8, 3.5, gap) * (1.0 - smoothstep(thickness, 2.6 * thickness, gap));
        float edge = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
        shadow = max(shadow, hit * smoothstep(0.0, 0.035, edge));
    }
    return 1.0 - shadow;
}

float3 SceneTransmittedLight(float3 receiver, float3 normal, uint2 size, bool useVisibility) {
    float3 result = 0.0;
    [loop] for (uint material = 0; material < uint(TransmissionData[0].x); ++material) {
        float4 tint = TransmissionData[1 + 2 * material];
        // The material anchor limits evaluation to local scene lights and receivers.
        float4 anchor = TransmissionData[2 + 2 * material];
        if (tint.w <= 0.0 || length(receiver - anchor.xyz) >= anchor.w * 1.4) continue;

        [loop] for (uint i = 0; i < min(8u, uint(SceneData[2].x)); ++i) {
            float4 light = SceneData[4 + i];
            float sourceWeight = 1.0 - smoothstep(0.32, 0.8, length(light.xyz - anchor.xyz) / anchor.w);
            float3 ray = light.xyz - receiver;
            float distance = length(ray);
            float range = min(light.w, anchor.w * 1.4);
            if (sourceWeight <= 0.0 || distance < 1.0 || distance >= range) continue;
            float incidence = saturate(dot(normal, ray / distance));
            if (incidence <= 0.0) continue;
            float coverage = GlassCoverage(receiver, ray, material);
            if (coverage <= 0.0) continue;

            float visibility = 1.0;
            if (useVisibility) {
#ifdef KOKIRI_POINT_SHADOW
                visibility = PointTransmissionVisibility(receiver, normal, i);
                if (visibility < 0.0)
#endif
                    visibility = TransmissionVisibility(receiver, normal, light.xyz, size);
            }
            float attenuation = 1.0 - smoothstep(0.1, 1.0, distance / range);
            float3 sourceColor = ToLightSpace(SceneData[12 + i].rgb);
            result += sourceColor * tint.rgb * (tint.w * sourceWeight * incidence * attenuation * coverage * visibility);
        }
    }
    return min(result, 1.0);
}
#endif

#endif
