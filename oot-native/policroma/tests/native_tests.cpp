#include "Catalog.h"

#include <windows.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <zip.h>

#include <array>
#include <cstdio>

namespace {

using Microsoft::WRL::ComPtr;
using namespace ShaderPacks;

constexpr std::string_view Shader = R"(
Texture2D<float4> MaterialNormal : register(t0);
Texture2D<float4> MaterialSpecular : register(t1);
Texture2D<float> SceneDepth : register(t2);
float4 main(float4 position : SV_Position) : SV_Target {
    int3 coordinate = int3(int2(position.xy), 0);
    float4 normal = MaterialNormal.Load(coordinate);
    return abs(normal.a - (SceneDepth.Load(coordinate) + 1.0)) < 0.00001
        ? MaterialSpecular.Load(coordinate) : float4(0, 0, 0, 0);
}
)";

bool WritePack(const char* path, std::string_view manifest) {
    std::unique_ptr<zip_t, decltype(&zip_discard)> zip(zip_open(path, ZIP_CREATE | ZIP_TRUNCATE, nullptr),
                                                      zip_discard);
    if (!zip) return false;
    const auto add = [&](const char* name, std::string_view contents) {
        std::unique_ptr<zip_source_t, decltype(&zip_source_free)> file(
            zip_source_buffer(zip.get(), contents.data(), contents.size(), 0), zip_source_free);
        if (!file || zip_file_add(zip.get(), name, file.get(), ZIP_FL_ENC_UTF_8) < 0) return false;
        file.release();
        return true;
    };
    if (!add("shaders/linkspan-v2.json", manifest) || !add("shaders/material.hlsl", Shader)) return false;
    if (zip_close(zip.get()) != 0) return false;
    zip.release();
    return true;
}

bool CompileMaterialShader(const PassSource& pass) {
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(pass.source.data(), pass.source.size(), pass.file.c_str(), nullptr, nullptr, "main",
                          "ps_5_0", D3DCOMPILE_WARNINGS_ARE_ERRORS, 0,
                          code.GetAddressOf(), errors.GetAddressOf()))) {
        if (errors) std::printf("%s\n", static_cast<const char*>(errors->GetBufferPointer()));
        return false;
    }
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(reflection.GetAddressOf()))))
        return false;
    constexpr std::array names{"MaterialNormal", "MaterialSpecular", "SceneDepth"};
    for (UINT slot = 0; slot < names.size(); ++slot) {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        if (FAILED(reflection->GetResourceBindingDescByName(names[slot], &binding)) ||
            binding.BindPoint != slot || binding.Type != D3D_SIT_TEXTURE ||
            binding.Dimension != D3D_SRV_DIMENSION_TEXTURE2D || binding.ReturnType != D3D_RETURN_TYPE_FLOAT)
            return false;
    }
    return true;
}

} // namespace

int main() {
    constexpr auto V3 = R"({"version":3,"shaderModel":5,"passes":[{"name":"final","file":"shaders/material.hlsl",
        "inputs":["scene.material.normal","scene.material.specular","scene.depth"]}]})";
    constexpr auto V2 = R"({"version":2,"passes":[{"name":"final","file":"shaders/material.hlsl",
        "inputs":["scene.material.specular"]}]})";
    if (!WritePack("native-material-v3.zip", V3) || !WritePack("native-material-v2.zip", V2)) {
        std::puts("FAIL criação das fixtures ZIP");
        return 1;
    }

    std::vector<PassSource> passes;
    NativePackMetadata metadata;
    std::string error;
    if (!ReadNativePack("native-material-v3.zip", passes, error, {}, nullptr, &metadata) ||
        passes.size() != 1 || !metadata.requiresV3 || passes[0].shaderModel != 5 ||
        passes[0].inputs[0] != SceneMaterialNormalInput || passes[0].inputs[1] != SceneMaterialSpecularInput ||
        passes[0].inputs[2] != SceneDepthInput || !CompileMaterialShader(passes[0])) {
        std::printf("FAIL ZIP v3, slots ou HLSL do material: %s\n", error.c_str());
        return 1;
    }
    std::puts("PASS ZIP v3 carrega normal/specular/depth e compila HLSL nos slots t0/t1/t2");

    if (ReadNativePack("native-material-v2.zip", passes, error, {}, nullptr, &metadata) || error.empty() ||
        !metadata.requiresV3 || passes.size() != 1 || passes[0].inputs[1] != SceneMaterialSpecularInput) {
        std::puts("FAIL entrada specular deveria ser recusada no v2 preservando o grafo");
        return 1;
    }
    std::puts("PASS v2 recusa a entrada specular e preserva o grafo v3 anterior");
}
