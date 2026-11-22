#include "lib/lighting.hlsli"
#if KOKIRI_STATIC_SUN_SHADOW
#include "/lib/sunshadow.hlsli"
#endif
#if POLICROMA_AO
#include "/lib/occlusion.hlsli"
#endif

float4 main(float4 position : SV_Position) : SV_Target {
    uint area = uint(SceneData[0].x);
    bool unbound = UnboundArea(area);
#if POLICROMA_AO
    if (!AmbientArea(area)) return 0.0;
#else
    if (!unbound) return 0.0;
#endif
    uint2 size;
    SceneDepth.GetDimensions(size.x, size.y);
    float2 uv = position.xy / ((size + 1u) / 2u);
    float distance = DepthAt(uv, size);
    float3 viewPoint = ViewPosition(uv, distance);
    float3x3 inverseView = ViewToWorld();
    float3 eye = mul(-View[3].xyz, inverseView);
    float3 surface = eye + mul(viewPoint, inverseView);
    float3 normal = SurfaceNormal(surface, eye, distance);
    float shadow = unbound ? ContactShadow(viewPoint, normal, SceneSun(), distance, size) : 0.0;
    float worldShadow = 0.0;
#if KOKIRI_STATIC_SUN_SHADOW
    if (unbound && distance < CameraInfo.y - 1.0) {
        worldShadow = SunOcclusion(surface, normal);
        shadow = max(shadow, worldShadow * SunDaylight() * 0.88);
    }
#endif
    float ambient = 0.0;
#if POLICROMA_AO
    ambient = AmbientObscurance(uv, size);
#endif
    return float4(shadow, distance, worldShadow, ambient);
}
