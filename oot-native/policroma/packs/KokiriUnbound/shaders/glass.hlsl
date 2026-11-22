#include "/lib/world.hlsli"
#include "/lib/transmission.hlsli"

float4 main(float4 position : SV_Position) : SV_Target {
    int3 pixel = int3(int2(position.xy), 0);
    float4 scene = SceneColor.Load(pixel);
    if (TransmissionData[0].y < 1.0) return scene;
    float depth = SceneDepth.Load(pixel);
    if (depth >= 0.999999) return scene;

    uint2 size;
    SceneColor.GetDimensions(size.x, size.y);
    float distance = ViewDepth(depth);
    float3x3 inverseView = ViewToWorld();
    float3 eye = mul(-View[3].xyz, inverseView);
    float3 surface = eye + mul(ViewPosition(position.xy / size, distance), inverseView);
    float3 normal = SurfaceNormal(surface, eye, distance);
#if POLICROMA_GLASS_LIGHTING
    float3 throughGlass = SceneTransmittedLight(surface, normal, size, POLICROMA_GLASS_LIGHTING == 1);
#else
    float3 throughGlass = TransmittedLight(surface, normal);
#endif
    if (!any(throughGlass > 0.0)) return scene;
    float3 color = ToLightSpace(scene.rgb);
    return float4(saturate(ToDisplay(color + throughGlass * (0.32 + 0.55 * color))), scene.a);
}
