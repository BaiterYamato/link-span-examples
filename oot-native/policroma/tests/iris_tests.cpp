// Iris reduzido: tradução sem GPU. Compila cada passe com o FXC (ps_4_0) e confere a interface que o host
// (WorldPass.cpp::CheckPassInterface) exige. Uso: policroma_iris_tests <pasta packs/>
#include "Catalog.h"
#include "GlslFragment.h"
#include "IrisPack.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;
using namespace ShaderPacks;
namespace fs = std::filesystem;

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    std::printf("%s %s\n", condition ? "PASS" : "FAIL", message.c_str());
    if (!condition) ++failures;
}

std::string ReadText(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

struct IrisFixture {
    std::vector<IrisProgram> programs;
    IncludeFiles includes;
    std::vector<IrisTexture> textures;
    std::vector<IrisCustomUniform> uniforms;
    std::string properties;
};

std::string ReadBytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

IrisFixture LoadPack(const fs::path& root) {
    IrisFixture pack;
    for (const auto& name : IrisProgramOrder()) {
        for (const auto& file : IrisProgramFiles(name)) {
            if (!fs::exists(root / file)) continue;
            pack.programs.push_back({ name, file, ReadText(root / file) });
            if (const auto vertex = IrisVertexFile(file); fs::exists(root / vertex)) {
                pack.programs.back().vertexFile = vertex;
                pack.programs.back().vertexSource = ReadText(root / vertex);
            }
            break;
        }
    }
    for (const auto& entry : fs::recursive_directory_iterator(root / "shaders")) {
        if (entry.path().extension() == ".glsl")
            pack.includes.emplace(fs::relative(entry.path(), root).generic_string(), ReadText(entry.path()));
    }
    // Custom textures, as the catalog reads them.
    if (fs::exists(root / "shaders/shaders.properties")) {
        pack.properties = ReadText(root / "shaders/shaders.properties");
        pack.uniforms = ParseIrisUniforms(pack.properties);
        for (auto& texture : ParseIrisTextures(pack.properties)) {
            if (!fs::exists(root / texture.file)) continue;
            texture.bytes = ReadBytes(root / texture.file);
            if (fs::exists(root / (texture.file + ".mcmeta")))
                texture.clamp = IrisTextureClamp(ReadText(root / (texture.file + ".mcmeta")));
            pack.textures.push_back(std::move(texture));
        }
    }
    return pack;
}

// --pack with a folder: the rewritten GLSL of each pass also goes there, beside the HLSL.
fs::path DumpFolder;

bool Translate(const IrisFixture& pack, std::vector<PassSource>& passes, std::string& error, IrisHost host = {},
               std::string* manifestText = nullptr) {
    std::vector<std::string> glsl, vertexGlsl;
    std::string manifest;
    NativePackMetadata metadata;
    if (!BuildIrisGraph(pack.programs, pack.includes, glsl, manifest, error, host, &vertexGlsl,
                        pack.textures, pack.uniforms, pack.properties))
        return false;
    if (manifestText) *manifestText = manifest;
    auto files = std::make_shared<IncludeFiles>();
    for (const auto& texture : pack.textures) files->emplace(texture.file, texture.bytes);
    if (!ParseCatalogManifest(manifest, passes, metadata, error,
                              files->empty() ? nullptr : std::shared_ptr<const IncludeFiles>(files)))
        return false;
    const IncludeFiles none;
    for (size_t i = 0; i < passes.size(); ++i) {
        if (!DumpFolder.empty()) {
            const auto base = DumpFolder / fs::path(passes[i].file).filename();
            std::ofstream(base.string() + ".glsl", std::ios::binary) << glsl[i];
            if (!vertexGlsl[i].empty()) std::ofstream(base.string() + ".vs.glsl", std::ios::binary) << vertexGlsl[i];
        }
        passes[i].source = glsl[i];
        if (passes[i].file.ends_with(".hlsl")) continue;
        passes[i].vertexSource = vertexGlsl[i];
        if (!(vertexGlsl[i].empty() ? TranslateGlslFragment(passes[i], none, {}, error)
                                    : TranslateGlslProgram(passes[i], error))) {
            error = passes[i].file + ": " + error + "\n--- GLSL ---\n" + glsl[i] +
                    (vertexGlsl[i].empty() ? std::string() : "\n--- GLSL de vértice ---\n" + vertexGlsl[i]);
            return false;
        }
    }
    return true;
}

struct Interface {
    bool compiled = false;
    bool signature = false;
    bool camera = false;
    bool scene = false;
    bool iris = false;
    bool resources = true;
    std::string log;
};

// Same rules as the host: SV_Position in register 0, TEXCOORD (xy) in register 1, sampler s0, textures within the
// pass inputs, LinkSpanCamera at b0 (two row-major float4x4 and a float4) and LinkSpanScene at b1 (float4[36]).
Interface Inspect(const PassSource& pass) {
    Interface result;
    ComPtr<ID3DBlob> code, errors;
    if (FAILED(D3DCompile(pass.source.data(), pass.source.size(), pass.file.c_str(), nullptr, nullptr, "main",
                          pass.shaderModel == 5 ? "ps_5_0" : "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
                          code.GetAddressOf(), errors.GetAddressOf()))) {
        result.log = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "sem diagnóstico";
        return result;
    }
    result.compiled = true;
    ComPtr<ID3D11ShaderReflection> reflection;
    if (FAILED(D3DReflect(code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(reflection.GetAddressOf()))))
        return result;
    D3D11_SHADER_DESC desc{};
    reflection->GetDesc(&desc);
    result.signature = true;
    if (!pass.vertexSource.empty()) {
        // The pack's vertex shader: same rule as WorldPass::CheckVertexInterface (only SV_VertexID in, SV_Position
        // out, every pixel input matched by register, semantic, type and mask; resources within the contract).
        ComPtr<ID3DBlob> vertexCode;
        if (FAILED(D3DCompile(pass.vertexSource.data(), pass.vertexSource.size(), pass.vertexFile.c_str(), nullptr,
                              nullptr, "main", pass.shaderModel == 5 ? "vs_5_0" : "vs_4_0",
                              D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vertexCode.GetAddressOf(), errors.ReleaseAndGetAddressOf()))) {
            result.compiled = false;
            result.log = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "vs sem diagnóstico";
            return result;
        }
        ComPtr<ID3D11ShaderReflection> vertex;
        D3D11_SHADER_DESC vertexDesc{};
        if (FAILED(D3DReflect(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(),
                              IID_PPV_ARGS(vertex.GetAddressOf()))) ||
            FAILED(vertex->GetDesc(&vertexDesc))) {
            result.signature = false;
            return result;
        }
        for (UINT i = 0; i < vertexDesc.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC input{};
            vertex->GetInputParameterDesc(i, &input);
            result.signature &= input.SystemValueType == D3D_NAME_VERTEX_ID;
        }
        std::vector<D3D11_SIGNATURE_PARAMETER_DESC> outputs(vertexDesc.OutputParameters);
        bool position = false;
        for (UINT i = 0; i < vertexDesc.OutputParameters; ++i) {
            vertex->GetOutputParameterDesc(i, &outputs[i]);
            position |= outputs[i].SystemValueType == D3D_NAME_POSITION;
        }
        result.signature &= position;
        for (UINT i = 0; i < desc.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC input{};
            reflection->GetInputParameterDesc(i, &input);
            result.signature &= std::any_of(outputs.begin(), outputs.end(), [&](const auto& output) {
                return output.Register == input.Register && output.SemanticIndex == input.SemanticIndex &&
                       _stricmp(output.SemanticName, input.SemanticName) == 0 &&
                       output.ComponentType == input.ComponentType && (input.Mask & ~output.Mask) == 0;
            });
        }
        for (UINT i = 0; i < vertexDesc.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC binding{};
            vertex->GetResourceBindingDesc(i, &binding);
            if (binding.Type == D3D_SIT_SAMPLER) result.resources &= binding.BindPoint <= 1;
            else if (binding.Type == D3D_SIT_TEXTURE)
                result.resources &= binding.BindPoint < MaxPassInputs && pass.inputs[binding.BindPoint] != UnboundInput;
            else if (binding.Type == D3D_SIT_CBUFFER)
                result.resources &= (binding.BindPoint == 0 && std::string_view(binding.Name) == "LinkSpanCamera") ||
                                    (binding.BindPoint == 1 && std::string_view(binding.Name) == "LinkSpanScene") ||
                                    (binding.BindPoint == 10 && std::string_view(binding.Name) == "LinkSpanIris");
            else result.resources = false;
        }
    }
    for (UINT i = 0; pass.vertexSource.empty() && i < desc.InputParameters; ++i) {
        D3D11_SIGNATURE_PARAMETER_DESC input{};
        reflection->GetInputParameterDesc(i, &input);
        const bool position = input.Register == 0 && input.SystemValueType == D3D_NAME_POSITION;
        const bool uv = input.Register == 1 && !(input.Mask & ~3u) && _stricmp(input.SemanticName, "TEXCOORD") == 0;
        result.signature &= position || uv;
    }
    for (UINT i = 0; i < desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        reflection->GetResourceBindingDesc(i, &binding);
        if (binding.Type == D3D_SIT_SAMPLER) {
            result.resources &= binding.BindPoint <= 1; // s0 clamps, s1 repeats
        } else if (binding.Type == D3D_SIT_TEXTURE) {
            result.resources &= binding.BindPoint < MaxPassInputs && pass.inputs[binding.BindPoint] != UnboundInput;
        } else if (binding.Type == D3D_SIT_CBUFFER) {
            auto* buffer = reflection->GetConstantBufferByName(binding.Name);
            D3D11_SHADER_BUFFER_DESC info{};
            buffer->GetDesc(&info);
            if (std::string_view(binding.Name) == "LinkSpanCamera" && binding.BindPoint == 0 && info.Size == 144 &&
                info.Variables == 3) {
                bool rows = true;
                for (UINT v = 0; v < 2; ++v) {
                    D3D11_SHADER_TYPE_DESC type{};
                    buffer->GetVariableByIndex(v)->GetType()->GetDesc(&type);
                    rows &= type.Class == D3D_SVC_MATRIX_ROWS && type.Rows == 4 && type.Columns == 4;
                }
                result.camera = rows;
                result.resources &= rows;
            } else if (std::string_view(binding.Name) == "LinkSpanScene" && binding.BindPoint == 1 &&
                       info.Size == 576 && info.Variables == 1) {
                result.scene = true;
            } else if (std::string_view(binding.Name) == "LinkSpanIris" && binding.BindPoint == 10 &&
                       (info.Size == 176 || info.Size == 192)) {
                result.iris = true;
            } else {
                result.resources = false;
            }
        } else {
            result.resources = false;
        }
    }
    return result;
}

// Runs a translated pass on the WARP rasterizer with the host's fullscreen vertex shader and bindings: inputs at
// t0..t3, linear clamp sampler at s0, LinkSpanCamera at b0, LinkSpanScene at b1.
// The unit tests use 8x4; --pack enlarges it so screen-space filters (TAA, blur) have room.
UINT RunWidth = 8, RunHeight = 4;
using Image = std::vector<std::array<float, 4>>; // row 0 = top of the screen, as the host stores it

struct Camera {
    float projection[4][4];
    float view[4][4];
    float info[4];
};

constexpr const char* HostFullscreenVertex = R"(
void main(uint vertex : SV_VertexID, out float4 position : SV_Position, out float2 uv : TEXCOORD0) {
    uv = float2(vertex == 1 ? 2.0 : 0.0, vertex == 2 ? 2.0 : 0.0);
    position = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
}
)";

// PNG to RGBA8 (straight alpha, like stb_image with STBI_rgb_alpha).
bool DecodeImage(const std::string& bytes, UINT& width, UINT& height, std::vector<uint8_t>& rgba) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    if (bytes.empty() ||
        FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateStream(&stream)) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(bytes.data())),
                                            static_cast<DWORD>(bytes.size()))) ||
        FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeCustom)) ||
        FAILED(converter->GetSize(&width, &height)) || !width || !height)
        return false;
    rgba.resize(static_cast<size_t>(width) * height * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(rgba.size()), rgba.data()));
}

// 4x4 RGBA: texel (x, y), y counted from the top row of the file, is (40 + 50x, 40 + 50y, 200, 255) / 255.
const std::string GridPng = [] {
    const unsigned char bytes[] = {
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,0x00,0x00,0x00,0x04,0x00,0x00,
        0x00,0x04,0x08,0x06,0x00,0x00,0x00,0xa9,0xf1,0x9e,0x7e,0x00,0x00,0x00,0x33,0x49,0x44,0x41,0x54,0x78,0x9c,0x15,
        0xc8,0x31,0x11,0x00,0x41,0x10,0xc3,0xb0,0x87,0xb3,0x98,0x82,0x29,0x98,0x52,0x9b,0x95,0x7f,0x4e,0xa5,0xbe,0x3b,
        0xcc,0x61,0x0f,0x77,0xf8,0x5d,0x30,0xc1,0x06,0x97,0x17,0xc5,0x14,0x5b,0x5c,0x5f,0x0c,0x33,0xec,0x70,0xc3,0x1f,
        0x4e,0x5f,0x2a,0xd1,0x22,0xdd,0x4a,0xa9,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82 };
    return std::string(reinterpret_cast<const char*>(bytes), sizeof(bytes));
}();

bool RunPass(const PassSource& pass, const Image& color, const Image& depth, const Camera& camera, Image& output,
             std::string& error) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 device.GetAddressOf(), nullptr, context.GetAddressOf()))) {
        error = "WARP indisponível";
        return false;
    }
    ComPtr<ID3DBlob> vsCode, psCode, errors;
    if (FAILED(D3DCompile(HostFullscreenVertex, std::strlen(HostFullscreenVertex), "vs", nullptr, nullptr, "main",
                          "vs_4_0", 0, 0, vsCode.GetAddressOf(), errors.GetAddressOf())) ||
        FAILED(D3DCompile(pass.source.data(), pass.source.size(), pass.file.c_str(), nullptr, nullptr, "main", "ps_4_0",
                          0, 0, psCode.GetAddressOf(), errors.ReleaseAndGetAddressOf()))) {
        error = errors ? static_cast<const char*>(errors->GetBufferPointer()) : "falha no FXC";
        return false;
    }
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, vs.GetAddressOf());
    device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, ps.GetAddressOf());

    const auto texture = [&](const Image& image, ComPtr<ID3D11ShaderResourceView>& view) {
        D3D11_TEXTURE2D_DESC desc{ RunWidth, RunHeight, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, { 1, 0 },
                                   D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        const D3D11_SUBRESOURCE_DATA data{ image.data(), RunWidth * 16, 0 };
        ComPtr<ID3D11Texture2D> resource;
        device->CreateTexture2D(&desc, &data, resource.GetAddressOf());
        device->CreateShaderResourceView(resource.Get(), nullptr, view.GetAddressOf());
    };
    ComPtr<ID3D11ShaderResourceView> colorView, depthView;
    texture(color, colorView);
    texture(depth, depthView);
    ID3D11ShaderResourceView* views[MaxPassInputs]{};
    for (size_t i = 0; i < MaxPassInputs; ++i) {
        if (pass.inputs[i] == SceneColorInput) views[i] = colorView.Get();
        if (pass.inputs[i] == SceneDepthInput) views[i] = depthView.Get();
    }

    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    device->CreateSamplerState(&samplerDesc, sampler.GetAddressOf());

    const auto buffer = [&](const void* data, UINT size, ComPtr<ID3D11Buffer>& result) {
        D3D11_BUFFER_DESC desc{ size, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        const D3D11_SUBRESOURCE_DATA initial{ data, 0, 0 };
        device->CreateBuffer(&desc, &initial, result.GetAddressOf());
    };
    std::array<float, 144> scene{};
    scene[2] = 200.0f;  // gameplay frames
    scene[3] = 0.75f;   // day fraction 18:00
    ComPtr<ID3D11Buffer> cameraBuffer, sceneBuffer;
    buffer(&camera, sizeof(camera), cameraBuffer);
    buffer(scene.data(), sizeof(float) * 144, sceneBuffer);
    ID3D11Buffer* constants[] = { cameraBuffer.Get(), sceneBuffer.Get() };

    D3D11_TEXTURE2D_DESC targetDesc{ RunWidth, RunHeight, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, { 1, 0 },
                                     D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> targetView;
    device->CreateTexture2D(&targetDesc, nullptr, target.GetAddressOf());
    device->CreateRenderTargetView(target.Get(), nullptr, targetView.GetAddressOf());
    targetDesc.Usage = D3D11_USAGE_STAGING;
    targetDesc.BindFlags = 0;
    targetDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    device->CreateTexture2D(&targetDesc, nullptr, staging.GetAddressOf());

    const D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(RunWidth), static_cast<float>(RunHeight), 0, 1 };
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, targetView.GetAddressOf(), nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    context->PSSetShaderResources(0, MaxPassInputs, views);
    context->PSSetSamplers(0, 1, sampler.GetAddressOf());
    context->PSSetConstantBuffers(0, 2, constants);
    context->Draw(3, 0);
    context->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        error = "readback";
        return false;
    }
    output.assign(RunWidth * RunHeight, {});
    for (UINT y = 0; y < RunHeight; ++y)
        std::memcpy(&output[y * RunWidth], static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, RunWidth * 16);
    context->Unmap(staging.Get(), 0);
    return true;
}

struct IrisData {
    float previousProjection[4][4];
    float previousView[4][4];
    float fog[4];
    float info[4];
    float clock[4]; // render frames, seconds
    float parameters[4]; // provider parameters (setParameters): x = cloudHeight
    float sky[4];        // vector 12 (bridge feature PolicromaFeatureSkyExposure): x = sky openness 0..1
};
using SceneData = std::array<float, 144>;

SceneData DefaultScene() {
    SceneData scene{};
    scene[2] = 200.0f; // gameplay frames
    scene[3] = 0.75f;  // day fraction 18:00
    return scene;
}

// Runs a whole graph like WorldPass::Draw: every non-final pass gets one RGBA32F target per output, inputs resolve
// scene, depth, scene.noise (random RGBA8 256x256), zero (null SRV) and pass outputs, and the constant buffers sit
// at b0, b1 and b10. The final pass is read back.
bool RunGraph(const std::vector<PassSource>& passes, const Image& color, const Image& depth, const Camera& camera,
              const IrisData& iris, const SceneData& scene, Image& output, std::string& error, int frames = 1,
              const Image* materialNormal = nullptr, const Image* materialSpecular = nullptr) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION,
                                 device.GetAddressOf(), nullptr, context.GetAddressOf()))) {
        error = "WARP 11_0 indisponível";
        return false;
    }
    ComPtr<ID3DBlob> vsCode, errors;
    if (FAILED(D3DCompile(HostFullscreenVertex, std::strlen(HostFullscreenVertex), "vs", nullptr, nullptr, "main",
                          "vs_4_0", 0, 0, vsCode.GetAddressOf(), errors.GetAddressOf()))) {
        error = "vs";
        return false;
    }
    ComPtr<ID3D11VertexShader> vs;
    device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, vs.GetAddressOf());
    const auto texture = [&](const Image& image, ComPtr<ID3D11ShaderResourceView>& view) {
        D3D11_TEXTURE2D_DESC desc{ RunWidth, RunHeight, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, { 1, 0 },
                                   D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        const D3D11_SUBRESOURCE_DATA data{ image.data(), RunWidth * 16, 0 };
        ComPtr<ID3D11Texture2D> resource;
        device->CreateTexture2D(&desc, &data, resource.GetAddressOf());
        device->CreateShaderResourceView(resource.Get(), nullptr, view.GetAddressOf());
    };
    ComPtr<ID3D11ShaderResourceView> colorView, depthView, noiseView, normalView, specularView;
    texture(color, colorView);
    texture(depth, depthView);
    if (materialNormal) texture(*materialNormal, normalView);
    if (materialSpecular) texture(*materialSpecular, specularView);
    {
        std::vector<uint32_t> texels(256 * 256);
        uint32_t state = 0x9E3779B9u;
        for (auto& texel : texels) {
            state ^= state << 13;
            state ^= state >> 17;
            state ^= state << 5;
            texel = state;
        }
        D3D11_TEXTURE2D_DESC desc{ 256, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 },
                                   D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
        const D3D11_SUBRESOURCE_DATA data{ texels.data(), 256 * 4, 0 };
        ComPtr<ID3D11Texture2D> resource;
        device->CreateTexture2D(&desc, &data, resource.GetAddressOf());
        device->CreateShaderResourceView(resource.Get(), nullptr, noiseView.GetAddressOf());
    }
    D3D11_SAMPLER_DESC samplerDesc{};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;
    device->CreateSamplerState(&samplerDesc, sampler.GetAddressOf());
    // s1 repeats (PolicromaFeaturePackTextures), like the host.
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    ComPtr<ID3D11SamplerState> wrapSampler;
    device->CreateSamplerState(&samplerDesc, wrapSampler.GetAddressOf());
    ID3D11SamplerState* const samplers[2]{ sampler.Get(), wrapSampler.Get() };
    // The manifest textures, decoded to RGBA8 (WIC here, stb_image in the host) and stored bottom-up with "flip".
    std::array<ComPtr<ID3D11ShaderResourceView>, MaxPackTextures> packViews;
    if (!passes.empty() && passes.front().textures) {
        for (size_t k = 0; k < passes.front().textures->size(); ++k) {
            const auto& entry = (*passes.front().textures)[k];
            UINT width = 0, height = 0;
            std::vector<uint8_t> rgba;
            if (!DecodeImage(entry.bytes, width, height, rgba)) {
                error = "PNG ilegível no teste: " + entry.file;
                return false;
            }
            if (entry.flip)
                for (UINT top = 0, bottom = height - 1; top < bottom; ++top, --bottom)
                    std::swap_ranges(rgba.begin() + top * width * 4, rgba.begin() + (top + 1) * width * 4,
                                     rgba.begin() + bottom * width * 4);
            D3D11_TEXTURE2D_DESC desc{ width, height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 },
                                       D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
            const D3D11_SUBRESOURCE_DATA data{ rgba.data(), width * 4, 0 };
            ComPtr<ID3D11Texture2D> resource;
            device->CreateTexture2D(&desc, &data, resource.GetAddressOf());
            device->CreateShaderResourceView(resource.Get(), nullptr, packViews[k].GetAddressOf());
        }
    }
    const auto buffer = [&](const void* data, UINT size, ComPtr<ID3D11Buffer>& result) {
        D3D11_BUFFER_DESC desc{ size, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0 };
        const D3D11_SUBRESOURCE_DATA initial{ data, 0, 0 };
        device->CreateBuffer(&desc, &initial, result.GetAddressOf());
    };
    ComPtr<ID3D11Buffer> cameraBuffer, sceneBuffer, irisBuffer;
    buffer(&camera, sizeof(camera), cameraBuffer);
    buffer(scene.data(), sizeof(float) * 144, sceneBuffer);
    buffer(&iris, sizeof(iris), irisBuffer);
    ID3D11Buffer* constants[] = { cameraBuffer.Get(), sceneBuffer.Get() };

    D3D11_TEXTURE2D_DESC targetDesc{ RunWidth, RunHeight, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, { 1, 0 },
                                     D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    struct Target {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11RenderTargetView> rtv;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    // Two sets of targets: a persistent pass writes set (frame % 2) and its history reads the other, like the host.
    std::vector<std::vector<Target>> targets[2];
    for (auto& set : targets) {
        set.resize(passes.size());
        for (size_t i = 0; i < passes.size(); ++i) {
            set[i].resize(passes[i].outputs);
            for (auto& target : set[i]) {
                device->CreateTexture2D(&targetDesc, nullptr, target.texture.GetAddressOf());
                device->CreateRenderTargetView(target.texture.Get(), nullptr, target.rtv.GetAddressOf());
                device->CreateShaderResourceView(target.texture.Get(), nullptr, target.srv.GetAddressOf());
                const float zero[4]{};
                context->ClearRenderTargetView(target.rtv.Get(), zero);
            }
        }
    }
    std::vector<ComPtr<ID3D11PixelShader>> pixelShaders(passes.size());
    std::vector<ComPtr<ID3D11VertexShader>> vertexShaders(passes.size());
    for (size_t i = 0; i < passes.size(); ++i) {
        const auto& pass = passes[i];
        ComPtr<ID3DBlob> psCode;
        if (FAILED(D3DCompile(pass.source.data(), pass.source.size(), pass.file.c_str(), nullptr, nullptr, "main",
                              pass.shaderModel == 5 ? "ps_5_0" : "ps_4_0", 0, 0, psCode.GetAddressOf(),
                              errors.ReleaseAndGetAddressOf()))) {
            error = pass.file + ": " + (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "falha no FXC");
            return false;
        }
        device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr,
                                  pixelShaders[i].GetAddressOf());
        // A pass with its own vertex shader binds it with the same resources as the pixel stage, like the host.
        if (!pass.vertexSource.empty()) {
            ComPtr<ID3DBlob> packVsCode;
            if (FAILED(D3DCompile(pass.vertexSource.data(), pass.vertexSource.size(), pass.vertexFile.c_str(), nullptr,
                                  nullptr, "main", pass.shaderModel == 5 ? "vs_5_0" : "vs_4_0", 0, 0,
                                  packVsCode.GetAddressOf(), errors.ReleaseAndGetAddressOf()))) {
                error = pass.vertexFile + ": " +
                        (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "falha no FXC");
                return false;
            }
            device->CreateVertexShader(packVsCode->GetBufferPointer(), packVsCode->GetBufferSize(), nullptr,
                                       vertexShaders[i].GetAddressOf());
        }
    }
    const auto write = [&](size_t frame, size_t i) -> std::vector<Target>& {
        return targets[passes[i].persistent ? frame % 2 : 0][i];
    };
    const D3D11_VIEWPORT viewport{ 0, 0, static_cast<float>(RunWidth), static_cast<float>(RunHeight), 0, 1 };
    context->RSSetViewports(1, &viewport);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->PSSetSamplers(0, 2, samplers);
    context->PSSetConstantBuffers(0, 2, constants);
    context->PSSetConstantBuffers(10, 1, irisBuffer.GetAddressOf());
    context->VSSetSamplers(0, 2, samplers);
    context->VSSetConstantBuffers(0, 2, constants);
    context->VSSetConstantBuffers(10, 1, irisBuffer.GetAddressOf());
    size_t last = 0;
    for (int frame = 0; frame < frames; ++frame) {
        // frameCounter (gameplay frames) advances one per frame, as the packs' temporal dither expects.
        SceneData current = scene;
        current[2] += static_cast<float>(frame);
        context->UpdateSubresource(sceneBuffer.Get(), 0, nullptr, current.data(), 0, 0);
        IrisData clock = iris;
        clock.clock[0] += static_cast<float>(frame);
        clock.clock[1] += static_cast<float>(frame) * iris.info[1];
        context->UpdateSubresource(irisBuffer.Get(), 0, nullptr, &clock, 0, 0);
        for (size_t i = 0; i < passes.size(); ++i) {
            const auto& pass = passes[i];
            const auto& packVs = vertexShaders[i];
            context->VSSetShader(packVs ? packVs.Get() : vs.Get(), nullptr, 0);
            ID3D11ShaderResourceView* views[MaxPassInputs]{};
            for (size_t slot = 0; slot < MaxPassInputs; ++slot) {
                const int input = pass.inputs[slot];
                if (input == SceneColorInput) views[slot] = colorView.Get();
                else if (input == SceneDepthInput) views[slot] = depthView.Get();
                else if (input == SceneNoiseInput) views[slot] = noiseView.Get();
                else if (input == SceneMaterialNormalInput) views[slot] = normalView.Get();
                else if (input == SceneMaterialSpecularInput) views[slot] = specularView.Get();
                else if (IsPackTextureInput(input)) views[slot] = packViews[PackTextureIndex(input)].Get();
                else if (IsHistoryInput(input) && HistoryPass(input) < passes.size())
                    views[slot] = targets[(frame + 1) % 2][HistoryPass(input)][HistoryOutput(input)].srv.Get();
                else if (input > 0 && !IsHistoryInput(input))
                    views[slot] = write(frame, InputPass(input))[InputOutput(input)].srv.Get();
            }
            std::array<ID3D11RenderTargetView*, MaxPassOutputs> rtvs{};
            for (size_t k = 0; k < pass.outputs; ++k) rtvs[k] = write(frame, i)[k].rtv.Get();
            context->OMSetRenderTargets(pass.outputs, rtvs.data(), nullptr);
            context->PSSetShader(pixelShaders[i].Get(), nullptr, 0);
            context->PSSetShaderResources(0, MaxPassInputs, views);
            if (packVs) context->VSSetShaderResources(0, MaxPassInputs, views);
            context->Draw(3, 0);
            ID3D11ShaderResourceView* none[MaxPassInputs]{};
            context->PSSetShaderResources(0, MaxPassInputs, none);
            context->VSSetShaderResources(0, MaxPassInputs, none);
            context->OMSetRenderTargets(0, nullptr, nullptr);
        }
        last = static_cast<size_t>(frame);
    }
    targetDesc.Usage = D3D11_USAGE_STAGING;
    targetDesc.BindFlags = 0;
    targetDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    device->CreateTexture2D(&targetDesc, nullptr, staging.GetAddressOf());
    context->CopyResource(staging.Get(), write(last, passes.size() - 1)[0].texture.Get());
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        error = "readback";
        return false;
    }
    output.assign(RunWidth * RunHeight, {});
    for (UINT y = 0; y < RunHeight; ++y)
        std::memcpy(&output[y * RunWidth], static_cast<const char*>(mapped.pData) + y * mapped.RowPitch, RunWidth * 16);
    context->Unmap(staging.Get(), 0);
    return true;
}

// Host-convention math (row vectors, as the native HLSL packs use them).
using Matrix = std::array<std::array<double, 4>, 4>;

Matrix Multiply(const Matrix& a, const Matrix& b) {
    Matrix r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r[i][j] += a[i][k] * b[k][j];
    return r;
}

Matrix Inverse(Matrix m) {
    Matrix r{};
    for (int i = 0; i < 4; ++i) r[i][i] = 1.0;
    for (int c = 0; c < 4; ++c) {
        int pivot = c;
        for (int i = c + 1; i < 4; ++i)
            if (std::abs(m[i][c]) > std::abs(m[pivot][c])) pivot = i;
        std::swap(m[c], m[pivot]);
        std::swap(r[c], r[pivot]);
        const double p = m[c][c];
        for (int j = 0; j < 4; ++j) {
            m[c][j] /= p;
            r[c][j] /= p;
        }
        for (int i = 0; i < 4; ++i) {
            if (i == c) continue;
            const double f = m[i][c];
            for (int j = 0; j < 4; ++j) {
                m[i][j] -= f * m[c][j];
                r[i][j] -= f * r[c][j];
            }
        }
    }
    return r;
}

std::array<double, 4> Transform(const std::array<double, 4>& v, const Matrix& m) {
    std::array<double, 4> r{};
    for (int j = 0; j < 4; ++j)
        for (int k = 0; k < 4; ++k) r[j] += v[k] * m[k][j];
    return r;
}

IrisFixture Inline(std::vector<std::pair<std::string, std::string>> programs) {
    IrisFixture pack;
    for (auto& [name, source] : programs) pack.programs.push_back({ name, "shaders/" + name + ".fsh", source });
    return pack;
}

// The last program of the pack gets a .vsh.
IrisFixture WithVertex(IrisFixture pack, std::string source) {
    auto& program = pack.programs.back();
    program.vertexFile = IrisVertexFile(program.file);
    program.vertexSource = std::move(source);
    return pack;
}

std::string Inputs(const PassSource& pass) {
    std::string text;
    for (int input : pass.inputs) text += std::to_string(input) + " ";
    return text;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("uso: policroma_iris_tests <pasta packs> | --pack <pasta de um pack Iris extraído> [pasta do HLSL]\n");
        return 2;
    }
    // --pack options, taken out of argv: "--fog-divisor N" serves the provider parameters live with k = 1/N (0 = automatic k
    // from the scene's fog; without the option the pack runs as a host without setParameters), "--ground-y Y" is the floor height of the fog calibration.
    double fogDivisor = -1.0, groundY = 0.0; // fogDivisor < 0: no --fog-divisor (host without setParameters)
    bool gbufferF0 = false; // "--gbuffer-f0": the opt-in F0 program (reconstructed normals) before the pack
    bool skyExposure = false; // "--sky-exposure": the 0.7.9 bridge bit, with sky_light = -1 (automatic) and openness 0.5
    if (std::string_view(argv[1]) == "--pack") {
        int kept = 2;
        for (int i = 2; i < argc; ++i) {
            const std::string_view option = argv[i];
            if (option == "--gbuffer-f0")
                gbufferF0 = true;
            else if (option == "--sky-exposure")
                skyExposure = true;
            else if ((option == "--fog-divisor" || option == "--ground-y") && i + 1 < argc)
                (option == "--ground-y" ? groundY : fogDivisor) = std::atof(argv[++i]);
            else
                argv[kept++] = argv[i];
        }
        argc = kept;
    }
    if ((argc == 3 || argc == 4) && std::string_view(argv[1]) == "--pack") {
        // Diagnostic for real packs: translate and compile like the provider and the host would.
        std::vector<PassSource> passes;
        std::string error;
        std::string manifest;
        if (argc == 4) DumpFolder = argv[3];
        const IrisHost host{ true, true, fogDivisor >= 0.0, DefaultIrisCloudHeight, gbufferF0, false, skyExposure };
        if (!Translate(LoadPack(argv[2]), passes, error, host, &manifest)) {
            std::printf("RECUSADO: %s\n", error.c_str());
            return 1;
        }
        for (const auto& pass : passes) {
            if (argc == 4) {
                // Optional third argument: a folder for the translated HLSL of each pass.
                const auto base = fs::path(argv[3]) / fs::path(pass.file).filename();
                std::ofstream(base.string() + ".hlsl", std::ios::binary) << pass.source;
                if (!pass.vertexSource.empty())
                    std::ofstream(base.string() + ".vs.hlsl", std::ios::binary) << pass.vertexSource;
            }
            const auto shape = Inspect(pass);
            std::printf("%s: inputs %s saídas=%u sm=%u compilado=%d assinatura=%d recursos=%d %s\n", pass.file.c_str(),
                        Inputs(pass).c_str(), pass.outputs, pass.shaderModel, shape.compiled, shape.signature,
                        shape.resources, shape.log.c_str());
        }
        std::printf("manifesto: %s\n", manifest.c_str());
        RunWidth = 64;
        RunHeight = 32;
        // Run the graph on WARP like OoT in Kokiri (fovy 60, near 10, far 12800, eye at (-97, 170, 906), terrain
        // 50..2000 units away, the top row is sky at depth 1), pass by pass: catches NaN and black output.
        Image color(RunWidth * RunHeight), depth(RunWidth * RunHeight), out;
        const double n = 10.0, f = 12800.0, fy = 1.0 / std::tan(0.5236);
        Matrix projection{};
        projection[0][0] = fy / (4.0 / 3.0); // the game's 4:3 matrix; info.z (4:3 over the window) fixes x
        projection[1][1] = fy;
        projection[2][2] = (n + f) / (n - f);
        projection[2][3] = -1.0;
        projection[3][2] = 2.0 * n * f / (n - f);
        const Matrix view{ { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { 97, -170, -906, 1 } } };
        for (UINT y = 0; y < RunHeight; ++y)
            for (UINT x = 0; x < RunWidth; ++x) {
                const auto i = y * RunWidth + x;
                color[i] = { 0.15f + 0.5f * x / RunWidth, 0.35f, 0.12f, 1.0f };
                const double distance = 50.0 * std::pow(40.0, double(RunHeight - 1 - y) / (RunHeight - 1));
                const double clipZ = -distance * projection[2][2] + projection[3][2], clipW = distance;
                depth[i] = { y == 0 ? 1.0f : static_cast<float>(clipZ / clipW * 0.5 + 0.5), 0, 0, 1 };
            }
        Camera camera{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                camera.projection[i][j] = static_cast<float>(projection[i][j]);
                camera.view[i][j] = static_cast<float>(view[i][j]);
            }
        camera.info[0] = static_cast<float>(n);
        camera.info[1] = static_cast<float>(f);
        IrisData iris{};
        std::memcpy(iris.previousProjection, camera.projection, sizeof(iris.previousProjection));
        // The host hands the previous projection with the aspect fix already applied to the x column.
        for (int i = 0; i < 4; ++i) iris.previousProjection[i][0] *= static_cast<float>(1009.0 / 1920.0 * 4.0 / 3.0);
        std::memcpy(iris.previousView, camera.view, sizeof(iris.previousView));
        iris.fog[0] = 0.3f; iris.fog[1] = 0.4f; iris.fog[2] = 0.3f; iris.fog[3] = 900.0f;
        iris.info[0] = 1.0f;
        iris.info[1] = 1.0f / 20.0f;
        iris.info[2] = static_cast<float>(1009.0 / 1920.0 * 4.0 / 3.0);
        iris.info[3] = 3200.0f;
        if (fogDivisor >= 0.0) {
            // What the provider sends: cloudHeight, k, groundY and the sky light of open air.
            iris.parameters[0] = 3000.0f;
            iris.parameters[1] = fogDivisor > 0.0 ? static_cast<float>(1.0 / fogDivisor) : AutoIrisFogScale;
            iris.parameters[2] = static_cast<float>(groundY);
            iris.parameters[3] = skyExposure ? AutoIrisSkyLight : 240.0f;
            iris.sky[0] = 0.5f;
        }
        int invalidTotal = 0;
        for (size_t count = 1; count <= passes.size(); ++count) {
            std::vector<PassSource> prefix(passes.begin(), passes.begin() + count);
            if (!RunGraph(prefix, color, depth, camera, iris, DefaultScene(), out, error, 4)) {
                std::printf("execução: FALHOU %s\n", error.c_str());
                return 1;
            }
            int invalid = 0;
            float low = 1e30f, high = -1e30f;
            for (const auto& p : out)
                for (int c = 0; c < 3; ++c) {
                    if (!std::isfinite(p[c])) ++invalid;
                    else { low = std::min(low, p[c]); high = std::max(high, p[c]); }
                }
            invalidTotal += invalid;
            const auto& sky = out[2];
            const auto& ground = out[(RunHeight - 1) * RunWidth + 2];
            std::printf("execução até %s: %d inválidos, rgb em [%g, %g], céu %g %g %g, chão %g %g %g\n",
                        prefix.back().file.c_str(), invalid, low, high, sky[0], sky[1], sky[2], ground[0], ground[1],
                        ground[2]);
        }
        // Temporal stability: the last two frames of a long run, and the spread inside the sky row (cloud dither).
        for (size_t count = 1; count <= passes.size(); ++count) {
            std::vector<PassSource> prefix(passes.begin(), passes.begin() + count);
            Image previous;
            if (!RunGraph(prefix, color, depth, camera, iris, DefaultScene(), previous, error, 15) ||
                !RunGraph(prefix, color, depth, camera, iris, DefaultScene(), out, error, 16)) {
                std::printf("estabilidade: FALHOU %s\n", error.c_str());
                return 1;
            }
            double change = 0.0, sky = 0.0, ground = 0.0;
            for (size_t i = 0; i < out.size(); ++i)
                for (int c = 0; c < 3; ++c) {
                    const double delta = std::abs(out[i][c] - previous[i][c]);
                    change = std::max(change, delta);
                    (i < RunWidth ? sky : ground) = std::max(i < RunWidth ? sky : ground, delta);
                }
            std::printf("estabilidade até %s (saída 0): mudança 15→16 máx %g (céu %g, resto %g)\n",
                        prefix.back().file.c_str(), change, sky, ground);
        }
        return invalidTotal ? 1 : 0;
    }
    const fs::path packs = argv[1];

    {
        std::vector<PassSource> passes;
        NativePackMetadata metadata;
        std::string error;
        const auto folder = packs / "IrisMaterial";
        std::error_code scanError;
        const auto catalog = Scan(packs, scanError);
        Check(!scanError && std::any_of(catalog.begin(), catalog.end(), [&](const auto& pack) {
            return pack.path == folder && pack.format == Format::Iris;
        }), "IrisMaterial aparece no catálogo do menu");
        const bool loaded = ReadIrisPack(folder, passes, metadata, error, { true });
        Check(loaded && metadata.requiresV3 && passes.size() == 2 && passes[0].outputs == 2 &&
                  passes[0].inputs[0] == SceneMaterialNormalInput && passes[0].inputs[1] == SceneMaterialSpecularInput &&
                  passes[0].inputs[2] == SceneDepthInput,
              "catálogo Iris liga os dois MRT do material antes de final: " + error);
        // The zip is the CTest argument (argv[2], built next to the binary): without it there is nothing to read, and a
        // missing file must not look like a corrupt one.
        if (argc != 3) {
            std::printf("SKIP: IrisMaterial.zip (passe o caminho do zip como segundo argumento)\n");
        } else if (const fs::path archive = argv[2]; !fs::exists(archive)) {
            Check(false, "IrisMaterial.zip existe: " + archive.string());
        } else {
            const auto zipCatalog = Scan(archive.parent_path().empty() ? fs::current_path() : archive.parent_path(),
                                         scanError);
            Check(!scanError && std::any_of(zipCatalog.begin(), zipCatalog.end(), [&](const auto& pack) {
                return pack.path.filename() == archive.filename() && pack.format == Format::Iris;
            }), "IrisMaterial.zip aparece no catálogo do menu");
            std::vector<PassSource> zippedPasses;
            NativePackMetadata zippedMetadata;
            const bool loadedZip = ReadIrisPack(archive, zippedPasses, zippedMetadata, error, { true });
            Check(loadedZip && loaded && zippedMetadata.manifest == metadata.manifest && zippedPasses.size() == 2 &&
                      zippedPasses.front().inputs[1] == SceneMaterialSpecularInput &&
                      zippedPasses.front().source == passes.front().source &&
                      zippedPasses.back().source == passes.back().source &&
                      zippedPasses.back().vertexSource == passes.back().vertexSource,
                  "catálogo de IrisMaterial.zip entrega os passes e shaders da pasta: " + error);
            if (loadedZip) {
                const auto seed = Inspect(zippedPasses.front());
                Check(seed.compiled && seed.signature && seed.resources, "FXC e assinatura HLSL do seed de material " + seed.log);
                const Image color(RunWidth * RunHeight, { 0.25f, 0.25f, 0.5f, 1.0f });
                const Image depth(RunWidth * RunHeight, { 0.5f, 0.0f, 0.0f, 1.0f });
                const Image specular(RunWidth * RunHeight, { 0.125f, 0.25f, 0.5f, 0.875f });
                Image normal(RunWidth * RunHeight, { 0.25f, 0.5f, 0.75f, 1.5f });
                normal[1][3] = 1.75f; // superfície mais distante, encoberta pelo depth da cena
                normal[2] = {}; // lacuna abaixo do material ou pixel inválido na resolução MSAA
                Image rendered;
                const bool drawn = RunGraph(zippedPasses, color, depth, {}, {}, DefaultScene(), rendered, error, 1,
                                            &normal, &specular);
                constexpr std::array<float, 4> valid{ 0.125f, 0.5f, 0.75f, 0.875f };
                Check(drawn && rendered[0] == valid && rendered[1] == color[1] && rendered[2] == color[2] &&
                          rendered[RunWidth] == valid,
                      "WARP: Iris recebe F0, roughness e AO sem vazar para oclusão/lacuna: " + error);
            }
        }
        Check(!ReadIrisPack(folder, passes, metadata, error) && error.find("V3") != error.npos,
              "opt-in de material exige V3: " + error);

        auto fixture = LoadPack(folder);
        for (const char* invalid : {
                 "linkspan.materialGbuffer=colortex13 colortex13",
                 "linkspan.materialGbuffer=colortex0 colortex14",
                 "linkspan.materialGbuffer=colortex16 colortex14",
                 "linkspan.materialGbuffer=colortex13 colortex14 extra",
                 "linkspan.materialGbuffer=colortex13 colortex14\nlinkspan.materialGbuffer=colortex13 colortex14" }) {
            fixture.properties = invalid;
            const bool rejected = !Translate(fixture, passes, error, { true });
            Check(rejected && error.find("distintos") != error.npos, "mapeamento Iris inválido recusado");
        }
        fixture = LoadPack(folder);
        fixture.programs.insert(fixture.programs.begin(), { "composite", "shaders/composite.fsh",
            "#version 120\n/* RENDERTARGETS: 13 */\nvoid main(){gl_FragData[0]=vec4(1.0);}\n" });
        const bool collides = !Translate(fixture, passes, error, { true });
        Check(collides && error.find("colide com colortex13") != error.npos,
              "Iris recusa escrita em colortex reservado para material: " + error);
        fixture = LoadPack(folder);
        fixture.programs.back().source.insert(fixture.programs.back().source.find('\n') + 1,
                                              "const bool colortex14Clear = false;\n");
        const bool usesHistory = !Translate(fixture, passes, error, { true });
        Check(usesHistory && error.find("colide com colortex14") != error.npos,
              "Iris recusa histórico em colortex reservado para material: " + error);
        fixture = LoadPack(folder);
        fixture.textures.push_back({ "composite", "colortex14", "shaders/material.png", "PNG" });
        const bool usesCustomTexture = !Translate(fixture, passes, error, { true, true });
        Check(usesCustomTexture && error.find("colide com colortex14") != error.npos,
              "Iris recusa textura própria em colortex reservado para material: " + error);
    }

    {
        std::vector<PassSource> passes;
        std::string error;
        const bool ok = Translate(LoadPack(packs / "IrisMinimo"), passes, error);
        Check(ok, "IrisMinimo traduz: " + error);
        if (ok) {
            Check(passes.size() == 3 && passes[0].file == "shaders/composite.fsh" &&
                      passes[1].file == "shaders/composite1.fsh" && passes[2].file == "shaders/final.fsh",
                  "IrisMinimo: composite, composite1 e final, nessa ordem");
            Check(passes[0].inputs[0] == SceneColorInput && passes[0].inputs[1] == SceneDepthInput,
                  "composite lê colortex0 = cena e depthtex0 = profundidade (" + Inputs(passes[0]) + ")");
            Check(passes[1].inputs[0] == 1 && passes[1].inputs[1] == SceneDepthInput,
                  "composite1 lê o colortex1 do composite e reescreve o mesmo alvo (" + Inputs(passes[1]) + ")");
            Check(passes[2].inputs[0] == 2, "final lê o colortex1 do composite1 (" + Inputs(passes[2]) + ")");
            const auto composite = Inspect(passes[0]);
            const auto mist = Inspect(passes[1]);
            const auto final = Inspect(passes[2]);
            Check(composite.compiled && mist.compiled && final.compiled,
                  "os três programas compilam no FXC ps_4_0 " + composite.log + mist.log + final.log);
            Check(composite.signature && mist.signature && final.signature, "assinatura SV_Position/TEXCOORD do host");
            Check(composite.resources && mist.resources && final.resources, "recursos dentro do contrato do host");
            Check(composite.camera && mist.camera, "near/far e matrizes gbuffer* chegam por LinkSpanCamera em b0");
            Check(final.scene, "frameTimeCounter/worldTime chegam por LinkSpanScene em b1");
        }
    }
    {
        std::vector<PassSource> passes;
        std::string error;
        const bool ok = Translate(Inline({ { "composite", "#version 120\nuniform sampler2D colortex0;\nvarying vec2 texcoord;\n"
                                                         "void main(){\n/* DRAWBUFFERS:01 */\ngl_FragData[0]=texture2D(colortex0,texcoord);\n"
                                                         "gl_FragData[1]=vec4(1.0);\n}\n" } }),
                                  passes, error);
        Check(!ok && error.find("um alvo") != error.npos, "dois alvos num programa são recusados: " + error);
    }
    {
        std::vector<PassSource> passes;
        std::string error;
        const bool ok = Translate(Inline({ { "final", "#version 120\nuniform sampler2D colortex2;\nvarying vec2 texcoord;\n"
                                                     "void main(){ gl_FragColor=texture2D(colortex2,texcoord); }\n" } }),
                                  passes, error);
        Check(!ok && error.find("colortex2") != error.npos, "ler colortex nunca escrito é recusado: " + error);
    }
    {
        std::vector<PassSource> passes;
        std::string error;
        const bool ok = Translate(Inline({ { "composite", "#version 120\nuniform sampler2D colortex0;\nvarying vec2 texcoord;\n"
                                                         "void main(){\n/* DRAWBUFFERS:1 */\ngl_FragData[0]=texture2D(colortex0,texcoord);\n}\n" } }),
                                  passes, error);
        // Iris without final.fsh shows colortex0; the composite here wrote colortex1, so the scene is presented.
        Check(ok && passes.size() == 2 && passes[1].file == "shaders/final.fsh" && passes[1].inputs[0] == SceneColorInput,
              "sem final, um passe final apresenta o colortex0 (aqui a cena) " + error);
    }
    {
        std::vector<PassSource> passes;
        std::string error;
        const bool ok = Translate(Inline({ { "final", "#version 120\nuniform sampler2D colortex0;\nuniform float centerDepthSmooth;\n"
                                                     "varying vec2 texcoord;\nvoid main(){ gl_FragColor=texture2D(colortex0,texcoord)*centerDepthSmooth; }\n" } }),
                                  passes, error);
        Check(ok && Inspect(passes[0]).compiled, "uniform fora do subconjunto vira variável com zero (Vulkan GLSL recusa uniform solto) " + error);
        const bool previous = Translate(Inline({ { "final", "#version 120\nuniform sampler2D colortex0;\nuniform mat4 gbufferPreviousProjection;\n"
                                                           "varying vec2 texcoord;\nvoid main(){ gl_FragColor=texture2D(colortex0,texcoord)*gbufferPreviousProjection[0][0]; }\n" } }),
                                        passes, error);
        Check(previous && !Inspect(passes[0]).iris, "gbufferPreviousProjection na ponte V2 lê zero, sem LinkSpanIris " + error);
    }
    {
        std::vector<PassSource> passes;
        std::string error;
        const bool ok = Translate(Inline({ { "final", "#version 120\nuniform sampler2D colortex0;\n#ifdef SHADOWS\nuniform vec3 sunPosition;\n"
                                                     "uniform sampler2D shadowtex0;\n#endif\nvarying vec2 texcoord;\n"
                                                     "void main(){ gl_FragColor=texture2D(colortex0,texcoord); }\n" } }),
                                  passes, error);
        Check(ok, "uniform desconhecido dentro de #ifdef desligado não atrapalha: " + error);
        if (ok) Check(Inspect(passes[0]).compiled, "final simples compila no FXC");
    }
    {
        std::vector<PassSource> passes;
        std::string error;
        const bool ok = Translate(Inline({ { "final", "#version 120\nuniform float viewWidth;\nuniform float viewHeight;\n"
                                                     "void main(){ gl_FragColor=vec4(gl_FragCoord.xy/vec2(viewWidth,viewHeight),0.0,1.0); }\n" } }),
                                  passes, error);
        Check(ok && passes[0].inputs[0] == SceneColorInput, "viewWidth sem textura declarada usa a cena para o tamanho: " + error);
        if (ok) {
            const auto shape = Inspect(passes[0]);
            Check(shape.compiled && shape.signature, "gl_FragCoord sem UV compila com a assinatura do host " + shape.log);
        }
    }
    {
        // Execution on WARP: OpenGL orientation for texcoord, gl_FragCoord, texelFetch and offsets, and the Iris
        // matrices reconstructing world positions the same way the host convention does.
        Image color(RunWidth * RunHeight), depth(RunWidth * RunHeight);
        for (UINT y = 0; y < RunHeight; ++y)
            for (UINT x = 0; x < RunWidth; ++x)
                color[y * RunWidth + x] = { (x + 0.5f) / RunWidth, (y + 0.5f) / RunHeight, 0.25f, 1.0f };
        const double n = 10.0, f = 1000.0, fy = 1.0 / std::tan(0.5), aspect = double(RunWidth) / RunHeight;
        Matrix projection{};
        projection[0][0] = fy / aspect;
        projection[1][1] = fy;
        projection[2][2] = (n + f) / (n - f);
        projection[2][3] = -1.0;
        projection[3][2] = 2.0 * n * f / (n - f);
        // View: yaw 0.7 rad around Y and pitch -0.3 around X, eye at (120, 45, -300); row vectors.
        const double yaw = 0.7, pitch = -0.3;
        const Matrix rotationY{ { { std::cos(yaw), 0, std::sin(yaw), 0 }, { 0, 1, 0, 0 },
                                  { -std::sin(yaw), 0, std::cos(yaw), 0 }, { 0, 0, 0, 1 } } };
        const Matrix rotationX{ { { 1, 0, 0, 0 }, { 0, std::cos(pitch), -std::sin(pitch), 0 },
                                  { 0, std::sin(pitch), std::cos(pitch), 0 }, { 0, 0, 0, 1 } } };
        const Matrix translation{ { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 }, { -120, -45, 300, 1 } } };
        const Matrix view = Multiply(translation, Multiply(rotationY, rotationX));
        Camera camera{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) {
                camera.projection[i][j] = static_cast<float>(projection[i][j]);
                camera.view[i][j] = static_cast<float>(view[i][j]);
            }
        camera.info[0] = static_cast<float>(n);
        camera.info[1] = static_cast<float>(f);
        const double clipZ = -100.0 * projection[2][2] + projection[3][2], clipW = -100.0 * projection[2][3];
        const float sceneDepth = static_cast<float>(clipZ / clipW * 0.5 + 0.5);
        for (auto& texel : depth) texel = { sceneDepth, 0, 0, 1 };

        const auto run = [&](const std::string& source, Image& output, std::string& error) {
            std::vector<PassSource> passes;
            return Translate(Inline({ { "final", source } }), passes, error) &&
                   RunPass(passes[0], color, depth, camera, output, error);
        };
        const auto equal = [](float a, double b, double tolerance = 0.002) { return std::abs(a - b) <= tolerance; };
        Image out;
        std::string error;

        bool ok = run("#version 120\nuniform sampler2D colortex0;\nvarying vec2 texcoord;\n"
                      "void main(){ gl_FragColor = vec4(texcoord, texture2D(colortex0, texcoord).rg); }\n",
                      out, error);
        bool oriented = ok;
        for (UINT y = 0; ok && y < RunHeight; ++y)
            for (UINT x = 0; x < RunWidth; ++x) {
                const auto& p = out[y * RunWidth + x];
                const double u = (x + 0.5) / RunWidth, v = (y + 0.5) / RunHeight;
                oriented &= equal(p[0], u) && equal(p[1], 1.0 - v) && equal(p[2], u) && equal(p[3], v);
            }
        Check(oriented, "texcoord na orientação OpenGL e texture2D devolve o próprio pixel " + error);

        ok = run("#version 120\nuniform sampler2D colortex0;\n"
                 "void main(){ gl_FragColor = vec4(gl_FragCoord.xy, texelFetch(colortex0, ivec2(gl_FragCoord.xy), 0).rg); }\n",
                 out, error);
        bool fragCoord = ok;
        for (UINT y = 0; ok && y < RunHeight; ++y)
            for (UINT x = 0; x < RunWidth; ++x) {
                const auto& p = out[y * RunWidth + x];
                fragCoord &= equal(p[0], x + 0.5) && equal(p[1], RunHeight - (y + 0.5)) &&
                             equal(p[2], (x + 0.5) / RunWidth) && equal(p[3], (y + 0.5) / RunHeight);
            }
        Check(fragCoord, "gl_FragCoord com origem embaixo e texelFetch no próprio pixel " + error);

        ok = run("#version 120\nuniform sampler2D colortex0;\nvarying vec2 texcoord;\n"
                 "vec4 tap(sampler2D s, vec2 c) { return textureOffset(s, c, ivec2(1, 1)); }\n"
                 "void main(){ gl_FragColor = tap(colortex0, texcoord); }\n",
                 out, error);
        // Offset (1, 1) in OpenGL is one texel right and one up: row y - 1 in the D3D image.
        Check(ok && equal(out[2 * RunWidth + 2][0], 3.5 / RunWidth) && equal(out[2 * RunWidth + 2][1], 1.5 / RunHeight),
              "textureOffset por função do pack sobe na tela como no OpenGL " + error);

        ok = run("#version 120\nuniform sampler2D depthtex0;\n"
                 "uniform mat4 gbufferProjectionInverse, gbufferModelViewInverse;\nuniform vec3 cameraPosition;\n"
                 "varying vec2 texcoord;\nvoid main(){\n"
                 "  float d = texture2D(depthtex0, texcoord).r;\n"
                 "  vec4 v = gbufferProjectionInverse * vec4(texcoord * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
                 "  v /= v.w;\n"
                 "  gl_FragColor = vec4((gbufferModelViewInverse * vec4(v.xyz, 1.0)).xyz + cameraPosition, 1.0);\n"
                 "}\n",
                 out, error);
        const auto inverse = Inverse(Multiply(view, projection));
        bool world = ok;
        double worst = 0.0;
        for (UINT y = 0; ok && y < RunHeight; ++y)
            for (UINT x = 0; x < RunWidth; ++x) {
                // Host path: D3D UV -> NDC with y up, back through (View * Projection) with row vectors.
                const double u = (x + 0.5) / RunWidth, v = (y + 0.5) / RunHeight;
                auto p = Transform({ u * 2.0 - 1.0, 1.0 - v * 2.0, sceneDepth * 2.0 - 1.0, 1.0 }, inverse);
                for (int i = 0; i < 3; ++i) {
                    const double expected = p[i] / p[3];
                    worst = std::max(worst, std::abs(out[y * RunWidth + x][i] - expected));
                    world &= equal(out[y * RunWidth + x][i], expected, 0.05);
                }
            }
        Check(world, "gbufferProjectionInverse + gbufferModelViewInverse + cameraPosition reconstroem o mundo do host "
                     "(erro máximo " + std::to_string(worst) + ") " + error);
    }
    {
        // Samplers only declared (common in packs with one big uniforms include) take no texture slot.
        std::vector<PassSource> passes;
        std::string error;
        const bool ok = Translate(Inline({ { "final", "#version 120\nuniform sampler2D colortex0, colortex1;\n"
                                                     "uniform sampler2D colortex2;\nuniform sampler2D colortex3;\n"
                                                     "uniform sampler2D colortex4;\nuniform sampler2D depthtex1;\n"
                                                     "varying vec2 texcoord;\n"
                                                     "void main(){ gl_FragColor=texture2D(colortex0,texcoord)"
                                                     "*texture2D(depthtex1,texcoord).r; }\n" } }),
                                  passes, error);
        Check(ok && passes[0].inputs[0] == SceneColorInput && passes[0].inputs[1] == SceneDepthInput &&
                  passes[0].inputs[2] == UnboundInput,
              "samplers só declarados não ocupam entrada: " + error);
        if (ok) Check(Inspect(passes[0]).compiled, "final com samplers sem uso compila");
        const bool shared = Translate(Inline({ { "final", "#version 120\nuniform sampler2D colortex0, gcolor;\n"
                                                         "uniform sampler2D depthtex0, depthtex1;\nvarying vec2 texcoord;\n"
                                                         "void main(){ gl_FragColor=texture2D(colortex0,texcoord)+texture2D(gcolor,texcoord)"
                                                         "*(texture2D(depthtex0,texcoord).r-texture2D(depthtex1,texcoord).r); }\n" } }),
                                      passes, error);
        Check(shared && passes[0].inputs[2] == UnboundInput && Inspect(passes[0]).compiled,
              "apelidos da mesma fonte dividem a entrada: " + error);
    }
    {
        // An unused varying declared before texcoord must not push the UV off location 0.
        std::vector<PassSource> passes;
        std::string error;
        const bool ok = Translate(Inline({ { "final", "#version 120\nuniform sampler2D colortex0;\nvarying vec4 glcolor;\n"
                                                     "flat varying vec3 sunVec, upVec;\nvarying vec2 texcoord;\n"
                                                     "void main(){ gl_FragColor=texture2D(colortex0,texcoord); }\n" } }),
                                  passes, error);
        Check(ok, "varyings não usados antes do texcoord não deslocam a UV: " + error);
        if (ok) {
            const auto shape = Inspect(passes[0]);
            Check(shape.compiled && shape.signature, "UV continua em TEXCOORD0 " + shape.log);
        }
        const bool used = Translate(Inline({ { "final", "#version 120\nuniform sampler2D colortex0;\nvarying vec4 glcolor;\n"
                                                       "varying vec2 texcoord;\n"
                                                       "void main(){ gl_FragColor=texture2D(colortex0,texcoord)*glcolor; }\n" } }),
                                    passes, error);
        Check(!used, "varying extra usado é recusado (o host só fornece a UV)");
    }

    {
        const auto textures = ParseIrisTextures(
            "# Custom Textures\n"
            "    texture.noise=lib/textures/noise.png\n"
            "    texture.deferred.colortex3=lib/textures/cloud-water.png\n"
            "    texture.gbuffers.gaux4=lib/textures/cloud-water.png\n"
            "    #if WORLD_SPACE_REFLECTIONS > 0\n"
            "        texture.composite.colortex5=minecraft:textures/atlas/blocks.png\n"
            "    #endif\n"
            "texture.composite.gaux1 = /img/a.png\n"
            "texture.composite.colortex6=raw.dat TEXTURE_2D RGBA8 4 4\n"
            "texture.deferred.colortex3=./lib/textures/other.png\n");
        const auto has = [&](std::string_view stage, std::string_view buffer, std::string_view file) {
            return std::any_of(textures.begin(), textures.end(), [&](const IrisTexture& texture) {
                return texture.stage == stage && texture.buffer == buffer && texture.file == file;
            });
        };
        Check(textures.size() == 3 && has("noise", "", "shaders/lib/textures/noise.png") &&
                  has("deferred", "colortex3", "shaders/lib/textures/other.png") &&
                  has("composite", "gaux1", "shaders/img/a.png"),
              "shaders.properties: texture.noise e texture.<deferred|composite>.<buffer>, PNG do pack, a última vence");
        Check(IrisTextureClamp(R"({"texture":{"blur":true,"clamp":true}})") &&
                  !IrisTextureClamp(R"({"texture":{"blur":true,"clamp":false}})") && !IrisTextureClamp("{") &&
                  !IrisTextureClamp(R"({"animation":{}})"),
              ".mcmeta: clamp true prende a borda; ausente ou ilegível repete");

        // Host contract: textures only in v3, files among the includes, flip boolean, inputs by name.
        const auto parse = [](std::string_view manifest, bool withFile) {
            auto files = std::make_shared<IncludeFiles>();
            if (withFile) files->emplace("shaders/tex/grid.png", GridPng);
            std::vector<PassSource> passes;
            NativePackMetadata metadata;
            std::string error;
            const bool ok = ParseCatalogManifest(manifest, passes, metadata, error, files);
            return std::pair{ ok, ok ? passes : std::vector<PassSource>{} };
        };
        const std::string pass = R"(,"passes":[{"name":"final","file":"shaders/final.fsh","inputs":["texture:grid"]}]})";
        const std::string list = R"("textures":[{"name":"grid","file":"shaders/tex/grid.png"}])";
        const auto accepted = parse(R"({"version":3,)" + list + pass, true);
        Check(accepted.first && accepted.second[0].inputs[0] == PackTextureInputBase && accepted.second[0].textures &&
                  accepted.second[0].textures->front().bytes == GridPng && !accepted.second[0].textures->front().flip,
              "manifesto v3 com textures e entrada texture:nome");
        Check(!parse(R"({"version":2,)" + list + pass, true).first, "textures exige manifesto v3");
        Check(!parse(R"({"version":3,)" + list + pass, false).first, "textura fora dos includes é recusada");
        Check(!parse(R"({"version":3,"textures":[{"name":"grid","file":"shaders/tex/grid.png","flip":1}])" + pass, true).first,
              "flip não booleano é recusado");
        Check(!parse(R"({"version":3,"textures":[{"name":"Grid","file":"shaders/tex/grid.png"}])" + pass, true).first,
              "nome de textura fora de [a-z0-9_.-] é recusado");
        Check(!parse(R"({"version":3,)" + list +
                         R"(,"passes":[{"name":"final","file":"shaders/final.fsh","inputs":["texture:other"]}]})", true).first,
              "texture:nome sem declaração é recusada");
    }

    {
        // Bridge V3 (RFC 0035): several targets, 8 textures, zero, noisetex, textureGather and LinkSpanIris, run as a
        // whole graph on WARP the way WorldPass::Draw binds it.
        const IrisHost v3{ true };
        Image color(RunWidth * RunHeight), depth(RunWidth * RunHeight);
        for (UINT y = 0; y < RunHeight; ++y)
            for (UINT x = 0; x < RunWidth; ++x) {
                // r = column + 10 * OpenGL row (0 at the bottom), for the gather order check.
                const float glRow = static_cast<float>(RunHeight - 1 - y);
                color[y * RunWidth + x] = { x + 10.0f * glRow, 0.5f, 0.25f, 1.0f };
                depth[y * RunWidth + x] = { 0.5f, 0, 0, 1 };
            }
        Camera camera{};
        for (int i = 0; i < 4; ++i) camera.projection[i][i] = camera.view[i][i] = 1.0f;
        camera.projection[0][0] = 2.0f;
        camera.info[0] = 10.0f;
        camera.info[1] = 1000.0f;
        IrisData iris{};
        for (int i = 0; i < 4; ++i) iris.previousProjection[i][i] = iris.previousView[i][i] = 1.0f;
        // Previous eye at (5, -6, 7): row-vector view translation is minus the eye.
        iris.previousView[3][0] = -5.0f;
        iris.previousView[3][1] = 6.0f;
        iris.previousView[3][2] = -7.0f;
        iris.fog[0] = 0.2f;
        iris.fog[1] = 0.4f;
        iris.fog[2] = 0.6f;
        iris.fog[3] = 150.0f;
        iris.info[0] = 1.0f;
        iris.info[1] = 0.05f;
        iris.info[2] = 0.75f;
        iris.info[3] = 900.0f;
        const auto equal = [](float a, double b, double tolerance = 0.002) { return std::abs(a - b) <= tolerance; };
        const auto graph = [&](const IrisFixture& pack, Image& out, std::string& error, std::vector<PassSource>* used = nullptr,
                               const SceneData& scene = DefaultScene(), std::string* manifest = nullptr) {
            std::vector<PassSource> passes;
            const bool ok = Translate(pack, passes, error, v3, manifest) &&
                            RunGraph(passes, color, depth, camera, iris, scene, out, error);
            if (used) *used = passes;
            return ok;
        };
        Image out;
        std::string error, manifest;
        std::vector<PassSource> passes;

        bool ok = graph(Inline({ { "composite", "#version 120\nvarying vec2 texcoord;\nvoid main(){\n"
                                                "/* DRAWBUFFERS:13 */\ngl_FragData[0]=vec4(0.25);\ngl_FragData[1]=vec4(0.5);\n}\n" },
                                 { "final", "#version 120\nuniform sampler2D colortex1, colortex3;\nvarying vec2 texcoord;\n"
                                            "void main(){ gl_FragColor=texture2D(colortex1,texcoord)+texture2D(colortex3,texcoord); }\n" } }),
                        out, error, &passes, DefaultScene(), &manifest);
        Check(ok && passes[0].outputs == 2 && passes[1].inputs[0] == PassOutputInput(0, 0) &&
                  passes[1].inputs[1] == PassOutputInput(0, 1) && equal(out[0][0], 0.75) && equal(out[13][2], 0.75),
              "DRAWBUFFERS:13 escreve dois alvos de uma vez e o final lê os dois (" + manifest + ") " + error);

        ok = graph(Inline({ { "composite", "#version 120\nvoid main(){\n/* RENDERTARGETS: 1,2,3,4 */\n"
                                           "gl_FragData[0]=vec4(0.01);gl_FragData[1]=vec4(0.02);gl_FragData[2]=vec4(0.03);"
                                           "gl_FragData[3]=vec4(0.04);\n}\n" },
                            { "composite1", "#version 120\nvoid main(){\n/* DRAWBUFFERS:567 */\n"
                                            "gl_FragData[0]=vec4(0.05);gl_FragData[1]=vec4(0.06);gl_FragData[2]=vec4(0.07);\n}\n" },
                            { "final", "#version 120\nuniform sampler2D colortex0, colortex1, colortex2, colortex3, colortex4,"
                                       " colortex5, colortex6, colortex7;\nvarying vec2 texcoord;\nvoid main(){\n"
                                       "gl_FragColor=texture2D(colortex1,texcoord)+texture2D(colortex2,texcoord)+texture2D(colortex3,texcoord)"
                                       "+texture2D(colortex4,texcoord)+texture2D(colortex5,texcoord)+texture2D(colortex6,texcoord)"
                                       "+texture2D(colortex7,texcoord)+vec4(texture2D(colortex0,texcoord).g);\n}\n" } }),
                   out, error, &passes);
        Check(ok && passes[2].inputs[7] != UnboundInput && equal(out[5][0], 0.78) && equal(out[5][3], 0.78),
              "um programa lê 8 texturas (colortex0..7, de dois passes com várias saídas) " + Inputs(passes.empty() ? PassSource{} : passes.back()) + error);

        {
            // Texture slots 8..10 share GLSL bindings with LinkSpanCamera, LinkSpanScene and LinkSpanIris.
            std::string source = "#version 120\nuniform float near, sunAngle, frameTime;\nvarying vec2 texcoord;\n";
            std::string sum = "vec4(near + sunAngle + frameTime)";
            for (int i = 0; i <= 10; ++i) {
                source += "uniform sampler2D colortex" + std::to_string(i) + ";\n";
                sum += "+texture2D(colortex" + std::to_string(i) + ",texcoord)";
            }
            ok = graph(Inline({ { "final", source + "void main(){ gl_FragColor=" + sum + "; }\n" } }), out, error, &passes);
            const auto shape = ok ? Inspect(passes[0]) : Interface{};
            Check(ok && shape.camera && shape.scene && shape.iris && shape.resources && equal(out[0][1], 10 + 0.5 + 0.05 + 0.5, 0.01),
                  "11 texturas e os três blocos do host não disputam registradores " + shape.log + error);
        }

        ok = graph(Inline({ { "final", "#version 120\nuniform sampler2D colortex0, colortex9;\nvarying vec2 texcoord;\n"
                                       "void main(){ gl_FragColor=vec4(texture2D(colortex0,texcoord).g)+texture2D(colortex9,texcoord); }\n" } }),
                   out, error, &passes);
        Check(ok && passes[0].inputs[1] == ZeroInput && equal(out[3][0], 0.5) && equal(out[3][3], 0.5),
              "colortex nunca escrito lê zero na V3 " + error);

        ok = graph(Inline({ { "final", "#version 120\nuniform sampler2D noisetex;\nvarying vec2 texcoord;\n"
                                       "void main(){ vec2 c = texcoord * 0.03;\n"
                                       "gl_FragColor=abs(texture2D(noisetex, c + vec2(1.0, 2.0)) - texture2D(noisetex, c))"
                                       " + vec4(0.0, 0.0, 0.0, texture2D(noisetex, c).r); }\n" } }),
                   out, error, &passes);
        bool wrapped = ok;
        float spread = 0.0f;
        for (const auto& p : out) {
            wrapped &= equal(p[0], 0.0, 0.0001) && equal(p[1], 0.0, 0.0001) && equal(p[2], 0.0, 0.0001);
            spread = std::max(spread, std::abs(p[3] - out[0][3]));
        }
        Check(wrapped && spread > 0.001f && passes[0].inputs[0] == SceneNoiseInput,
              "noisetex vem de scene.noise e repete fora de 0..1 " + error);

        {
            // Custom uniforms of shaders.properties: OptiFine expressions become GLSL over the served uniforms.
            IrisFixture pack = Inline({ { "final", "#version 120\nuniform float fm, eyeBrightnessM, inNether, mixed;\n"
                                            "uniform vec3 parts;\nvoid main(){\n"
                                            "gl_FragColor = vec4(fm / 4.0, eyeBrightnessM, inNether + parts.x + parts.y * 0.5 + parts.z * 0.125,"
                                            " mixed); }\n" } });
            pack.uniforms = ParseIrisUniforms(
                "    uniform.float.fm = 7 % 4\n"
                "uniform.float.eyeBrightnessM = smooth(4, eyeBrightness.y / 240.0, 5, 5)\n"
                "uniform.float.inNether=smooth(50, if(in(biome, BIOME_NETHER_WASTES), 1, 0), 15, 15)\n"
                "variable.float.a = 2 * 3 + 1\n"
                "uniform.float.mixed = if(a > 6 && !(a == 8), clamp(a / 14.0, 0.0, 1.0), 0.25)\n"
                "uniform.vec3.parts = vec3(frac(1.25), if(between(5, 1, 9), 1, 0), if(equals(1.0, 1.05, 0.1), 1, 0))\n"
                "uniform.float.broken = nosuchfunction(1)\n");
            Image out;
            std::string error;
            ok = graph(pack, out, error);
            Check(pack.uniforms.size() == 7 && ok && equal(out[0][0], 0.75) && equal(out[0][1], 1.0) &&
                      equal(out[0][2], 0.875) && equal(out[0][3], 0.5),
                  "uniforms próprios do shaders.properties (%, smooth, if, in, between, equals, variable) " +
                      (out.empty() ? std::string() : std::to_string(out[0][0]) + " " + std::to_string(out[0][1]) + " " +
                                                         std::to_string(out[0][2]) + " " + std::to_string(out[0][3])) + " " + error);
        }
        {
            // cloudHeight: live from the provider parameters (vector 11), else the constant of the activation.
            const IrisFixture pack = Inline({ { "final", "#version 120\nuniform float cloudHeight;\n"
                                                  "void main(){ gl_FragColor = vec4(cloudHeight / 1000.0); }\n" } });
            const auto run = [&](IrisHost host, Image& image, std::string& failure) {
                std::vector<PassSource> passes;
                return Translate(pack, passes, failure, host) &&
                       RunGraph(passes, color, depth, camera, iris, DefaultScene(), image, failure);
            };
            Image live, constant;
            std::string failure;
            // The provider always sends the four values: k = 1, no ground offset and open sky are its defaults.
            iris.parameters[0] = 2500.0f;
            iris.parameters[1] = 1.0f;
            iris.parameters[3] = 240.0f;
            IrisHost liveHost{ true, true, true, 900.0f };
            IrisHost constantHost{ true, true, false, 900.0f };
            const bool liveOk = run(liveHost, live, failure);
            const bool constantOk = run(constantHost, constant, failure);
            iris.parameters[0] = iris.parameters[1] = iris.parameters[3] = 0.0f;
            Check(liveOk && constantOk && equal(live[0][0], 2.5) && equal(constant[0][0], 0.9),
                  "cloudHeight ao vivo pelo vetor 11 e constante sem setParameters " + failure);
        }
        {
            // Fog calibration (vector 11: y = k, z = groundY, w = eyeBrightness). With k = 1, groundY = 0 and
            // 240 (not the 0.7.6 provider defaults, k = 1/32) every uniform must equal the expression the table had before the scale existed; compared on WARP
            // against that old expression written out in a second pack.
            Camera moved = camera; // eye at (320, 640, -160); row-vector view translation is minus the eye
            moved.view[3][0] = -320.0f;
            moved.view[3][1] = -640.0f;
            moved.view[3][2] = 160.0f;
            const IrisHost liveHost{ true, true, true, 900.0f };
            const IrisHost constantHost{ true, true, false, 900.0f };
            const auto setParameters = [&](float cloud, float scale, float ground, float sky) {
                IrisData data = iris;
                data.parameters[0] = cloud;
                data.parameters[1] = scale;
                data.parameters[2] = ground;
                data.parameters[3] = sky;
                return data;
            };
            const IrisData defaults = setParameters(2500.0f, 1.0f, 0.0f, 240.0f);
            const auto run = [&](const std::string& declarations, const std::string& body, const IrisHost& host,
                                 const IrisData& data, Image& image, std::string& failure) {
                std::vector<PassSource> passes;
                const IrisFixture pack = Inline({ { "final", "#version 120\n" + declarations + "varying vec2 texcoord;\n"
                                                              "void main(){ " + body + " }\n" } });
                return Translate(pack, passes, failure, host) &&
                       RunGraph(passes, color, depth, moved, data, DefaultScene(), image, failure);
            };
            const auto same = [&](const Image& a, const Image& b) {
                bool identical = !a.empty() && a.size() == b.size();
                for (size_t i = 0; identical && i < 4; ++i)
                    identical = std::abs(a[0][i] - b[0][i]) <= 1e-5f * (1.0f + std::abs(b[0][i]));
                return identical;
            };
            struct Probe {
                std::string name, declarations, now, before;
                bool needsParameters = false; // the old expression reads vector 11, so only the live host has it
            };
            const std::string probeVector = "vec4 p = vec4(0.3, -0.2, 0.5, 1.0);\n";
            const std::vector<Probe> probes = {
                { "near/far/fogStart/fogEnd", "uniform float near, far, fogStart, fogEnd;\n",
                  "gl_FragColor = vec4(near, far, fogStart, fogEnd);",
                  "gl_FragColor = vec4(PolicromaCameraInfo.xy, PolicromaFog.w, PolicromaIrisInfo.w) + vec4(near * 0.0 + "
                  "far * 0.0 + fogStart * 0.0 + fogEnd * 0.0);" },
                { "cameraPosition/eyeAltitude", "uniform vec3 cameraPosition;\nuniform float eyeAltitude;\n",
                  "gl_FragColor = vec4(cameraPosition, eyeAltitude);",
                  "gl_FragColor = vec4(policroma_camera_position, policroma_camera_position.y) + "
                  "vec4(cameraPosition * 0.0, eyeAltitude * 0.0);" },
                { "eyePosition e Fract/Int", "uniform vec3 eyePosition, cameraPositionFract;\nuniform ivec3 cameraPositionInt;\n",
                  "gl_FragColor = vec4(eyePosition + cameraPositionFract + vec3(cameraPositionInt), 1.0);",
                  "gl_FragColor = vec4(policroma_camera_position + fract(policroma_camera_position) + "
                  "vec3(ivec3(floor(policroma_camera_position))), 1.0) + vec4(eyePosition * 0.0 + "
                  "cameraPositionFract * 0.0 + vec3(cameraPositionInt) * 0.0, 0.0);" },
                { "previousCameraPosition", "uniform vec3 previousCameraPosition;\n",
                  "gl_FragColor = vec4(previousCameraPosition, 1.0);",
                  "gl_FragColor = vec4((inverse(PolicromaPreviousView) * vec4(0.0, 0.0, 0.0, 1.0)).xyz, 1.0) + "
                  "vec4(previousCameraPosition * 0.0, 0.0);" },
                { "gbufferProjection", "uniform mat4 gbufferProjection;\n",
                  probeVector + "gl_FragColor = gbufferProjection * p;",
                  probeVector + "gl_FragColor = policroma_projection * p + gbufferProjection * vec4(0.0);" },
                { "gbufferProjectionInverse", "uniform mat4 gbufferProjectionInverse;\n",
                  probeVector + "gl_FragColor = gbufferProjectionInverse * p;",
                  probeVector + "gl_FragColor = inverse(policroma_projection) * p + gbufferProjectionInverse * vec4(0.0);" },
                { "gbufferPreviousProjection", "uniform mat4 gbufferPreviousProjection;\n",
                  probeVector + "gl_FragColor = gbufferPreviousProjection * p;",
                  probeVector + "gl_FragColor = PolicromaPreviousProjection * p + gbufferPreviousProjection * vec4(0.0);" },
                { "cloudHeight", "uniform float cloudHeight;\n", "gl_FragColor = vec4(cloudHeight);",
                  "gl_FragColor = vec4(PolicromaIrisParameters.x) + vec4(cloudHeight * 0.0);", true },
                { "eyeBrightness", "uniform ivec2 eyeBrightness;\n", "gl_FragColor = vec4(eyeBrightness, 0.0, 1.0);",
                  "gl_FragColor = vec4(0.0, 240.0, 0.0, 1.0) + vec4(eyeBrightness, 0.0, 0.0) * 0.0;" },
            };
            for (const auto& probe : probes) {
                Image now, before;
                std::string failure;
                const bool liveOk = run(probe.declarations, probe.now, liveHost, defaults, now, failure) &&
                                    run(probe.declarations, probe.before, liveHost, defaults, before, failure);
                Check(liveOk && same(now, before),
                      "k = 1, chão 0 e céu 240 reproduzem a expressão anterior: " + probe.name + " " + failure);
                if (probe.needsParameters) continue;
                Image fallbackNow, fallbackBefore;
                const bool fallbackOk = run(probe.declarations, probe.now, constantHost, defaults, fallbackNow, failure) &&
                                        run(probe.declarations, probe.before, constantHost, defaults, fallbackBefore, failure);
                Check(fallbackOk && same(fallbackNow, fallbackBefore),
                      "sem setParameters (k = 1 fixo) reproduz a expressão anterior: " + probe.name + " " + failure);
            }

            // k = 1/32, groundY 0: positions and distances scale by k; heights keep no offset.
            const IrisData scaled = setParameters(3000.0f, 1.0f / 32.0f, 0.0f, 60.0f);
            Image out;
            std::string failure;
            bool ok = run("uniform float near, far, fogStart, fogEnd;\n", "gl_FragColor = vec4(near, far, fogStart, fogEnd);",
                          liveHost, scaled, out, failure);
            Check(ok && equal(out[0][0], 10.0 / 32.0, 1e-5) && equal(out[0][1], 1000.0 / 32.0, 1e-4) &&
                      equal(out[0][2], 150.0 / 32.0, 1e-4) && equal(out[0][3], 900.0 / 32.0, 1e-4),
                  "k = 1/32 escala near, far, fogStart e fogEnd " + failure);
            ok = run("uniform vec3 cameraPosition, previousCameraPosition;\nuniform float eyeAltitude;\n",
                     "gl_FragColor = vec4(cameraPosition, eyeAltitude) + vec4(previousCameraPosition * 0.0, 0.0);",
                     liveHost, scaled, out, failure);
            Check(ok && equal(out[0][0], 320.0 / 32.0, 1e-4) && equal(out[0][1], 640.0 / 32.0, 1e-4) &&
                      equal(out[0][2], -160.0 / 32.0, 1e-4) && equal(out[0][3], 640.0 / 32.0, 1e-4),
                  "k = 1/32 escala cameraPosition e eyeAltitude " + failure);
            ok = run("uniform vec3 previousCameraPosition;\nuniform float cloudHeight;\nuniform ivec2 eyeBrightness;\n",
                     "gl_FragColor = vec4(previousCameraPosition.y, cloudHeight, eyeBrightness.y, 1.0);",
                     liveHost, scaled, out, failure);
            Check(ok && equal(out[0][0], -6.0 / 32.0, 1e-4) && equal(out[0][1], 3000.0 / 32.0, 1e-3) &&
                      equal(out[0][2], 60.0, 1e-4),
                  "k = 1/32 escala previousCameraPosition e cloudHeight; eyeBrightness vem do vetor 11 " + failure);

            // groundY: the OoT floor lands on block 64 (camera y 640 over a floor at 600 -> 40 / 32 + 64).
            const IrisData lifted = setParameters(3000.0f, 1.0f / 32.0f, 600.0f, 240.0f);
            ok = run("uniform vec3 cameraPosition;\nuniform float eyeAltitude, cloudHeight;\n",
                     "gl_FragColor = vec4(cameraPosition, eyeAltitude) + vec4(cloudHeight * 0.0);", liveHost, lifted, out,
                     failure);
            Check(ok && equal(out[0][0], 10.0, 1e-4) && equal(out[0][1], 40.0 / 32.0 + 64.0, 1e-4) &&
                      equal(out[0][2], -5.0, 1e-4) && equal(out[0][3], 40.0 / 32.0 + 64.0, 1e-4),
                  "groundY leva o chão do jogo para o bloco 64 e só desloca a altura " + failure);
            ok = run("uniform float cloudHeight;\n", "gl_FragColor = vec4(cloudHeight);", liveHost, lifted, out, failure);
            Check(ok && equal(out[0][0], (3000.0 - 600.0) / 32.0 + 64.0, 1e-3),
                  "groundY desloca também a altura das nuvens " + failure);

            // Automatic k (0.7.7): parameters.y = 0 makes the shader derive k from the scene's native fog. `near` is
            // zNear * k, so k = near / zNear. Reference values: k_auto.py (nevoa-complementary/k-auto), same clamp.
            struct FogScene {
                std::string name;
                float zNear, zFar, start, end;
                double expected; // k of k_auto.py
            };
            const auto legacyStart = [](double nativeNear) { return static_cast<float>(10000.0 / (1000.0 - nativeNear)); };
            const std::vector<FogScene> scenes = {
                { "Kokiri (994 / 5800)", 10.0f, 5800.0f, legacyStart(994), 5800.0f, 0.04691 },
                { "Hyrule Field (996 / 12800)", 10.0f, 12800.0f, legacyStart(996), 12800.0f, 0.03150 },
                { "Kakariko (996 / 12800)", 10.0f, 12800.0f, legacyStart(996), 12800.0f, 0.03150 },
                { "campo aberto (990 / 12800)", 10.0f, 12800.0f, legacyStart(990), 12800.0f, 0.09440 },
                { "neblina curta: limite 1/4", 10.0f, 300.0f, 50.0f, 300.0f, MaxAutoIrisFogScale },
                { "sem rampa (fim = início): d = far, limite 1/64", 10.0f, 12800.0f, 2500.0f, 2500.0f, MinAutoIrisFogScale },
                { "início NaN: d = far, sem divisão por zero", 10.0f, 12800.0f, std::nanf(""), 12800.0f, MinAutoIrisFogScale },
                { "far 0 e início 0: finito, no limite", 10.0f, 0.0f, 0.0f, 0.0f, -1.0 },
            };
            for (const auto& scene : scenes) {
                Camera sceneCamera = moved;
                sceneCamera.info[0] = scene.zNear;
                sceneCamera.info[1] = scene.zFar;
                IrisData data = setParameters(3000.0f, AutoIrisFogScale, 0.0f, 240.0f);
                data.fog[3] = scene.start;
                data.info[3] = scene.end;
                std::vector<PassSource> passes;
                Image image;
                std::string failure;
                const IrisFixture pack = Inline({ { "final", "#version 120\nuniform float near;\nvarying vec2 texcoord;\n"
                                                              "void main(){ gl_FragColor = vec4(near); }\n" } });
                const bool ran = Translate(pack, passes, failure, liveHost) &&
                                 RunGraph(passes, color, depth, sceneCamera, data, DefaultScene(), image, failure);
                const double k = ran ? image[0][0] / scene.zNear : -1.0;
                const bool finite = ran && std::isfinite(k) && k >= MinAutoIrisFogScale * 0.999 && k <= MaxAutoIrisFogScale * 1.001;
                const bool matches = scene.expected < 0.0 || std::abs(k - scene.expected) <= 0.002 * scene.expected;
                Check(finite && matches, "k automático da névoa nativa: " + scene.name + " (k = " + std::to_string(k) + ") " + failure);
            }
            {
                // Any k above 0 is used as given, whatever the scene's fog says (the 0.7.6 behavior).
                Camera kokiri = moved;
                kokiri.info[0] = 10.0f;
                kokiri.info[1] = 5800.0f;
                IrisData data = setParameters(3000.0f, 1.0f / 32.0f, 0.0f, 240.0f);
                data.fog[3] = legacyStart(994);
                data.info[3] = 5800.0f;
                std::vector<PassSource> passes;
                Image image;
                std::string failure;
                const IrisFixture pack = Inline({ { "final", "#version 120\nuniform float near;\nvarying vec2 texcoord;\n"
                                                              "void main(){ gl_FragColor = vec4(near); }\n" } });
                const bool ran = Translate(pack, passes, failure, liveHost) &&
                                 RunGraph(passes, color, depth, kokiri, data, DefaultScene(), image, failure);
                Check(ran && image[0][0] == 10.0f * (1.0f / 32.0f), "k > 0 ignora a névoa da cena (Kokiri com 1/32) " + failure);
            }

            // Automatic k from the host's fog ramp (0.7.8, PolicromaFeatureFogRamp): clock.zw = {start, end} in u, with
            // 50% at u = (start + end) / 2. The phase 1 values in fog/info stay Kokiri's short fog to show the ramp wins.
            {
                const IrisHost rampHost{ true, true, true, 900.0f, false, true };
                struct RampScene {
                    std::string name;
                    float zNear, zFar, start, end;
                    double expected; // k; < 0: phase 1 (Kokiri 994 / 5800 from fog/info)
                    bool announced = true; // the host sets PolicromaFeatureFogRamp
                };
                const double kokiriK = 1.0 / 21.3, hyruleK = 1.0 / 31.7;
                const std::vector<RampScene> ramps = {
                    { "Kokiri 994 / 5800, u 994..1000", 10.0f, 5800.0f, 994.0f, 1000.0f, kokiriK },
                    { "Hyrule 996 / 12800, u 996..1000", 10.0f, 12800.0f, 996.0f, 1000.0f, hyruleK },
                    { "campo 997 / 12800 (span 3: ainda rampa)", 10.0f, 12800.0f, 997.0f, 1000.0f, 1.0 / 42.7 },
                    { "{0, 0} cai na fase 1 (Kokiri)", 10.0f, 5800.0f, 0.0f, 0.0f, 0.04691 },
                    { "{0, 0,0001}: névoa total, limite 1/4", 10.0f, 12800.0f, 0.0f, 0.0001f, MaxAutoIrisFogScale },
                    { "{1000, 1000}: sem névoa, d = far, limite 1/64", 10.0f, 12800.0f, 1000.0f, 1000.0f, MinAutoIrisFogScale },
                    { "fogNear 998 (w - z = 2): só borda, limite 1/64", 10.0f, 12800.0f, 998.0f, 1000.0f, MinAutoIrisFogScale },
                    { "fogNear 999 (w - z = 1): só borda, limite 1/64", 10.0f, 5800.0f, 999.0f, 1000.0f, MinAutoIrisFogScale },
                    { "sem o bit 16, a rampa é ignorada (fase 1)", 10.0f, 5800.0f, 994.0f, 1000.0f, 0.04691, false },
                };
                for (const auto& ramp : ramps) {
                    Camera rampCamera = moved;
                    rampCamera.info[0] = ramp.zNear;
                    rampCamera.info[1] = ramp.zFar;
                    IrisData data = setParameters(3000.0f, AutoIrisFogScale, 0.0f, 240.0f);
                    data.fog[3] = legacyStart(994);
                    data.info[3] = 5800.0f;
                    data.clock[2] = ramp.start;
                    data.clock[3] = ramp.end;
                    std::vector<PassSource> passes;
                    Image image;
                    std::string failure;
                    const IrisFixture pack = Inline({ { "final", "#version 120\nuniform float near;\nvarying vec2 texcoord;\n"
                                                                  "void main(){ gl_FragColor = vec4(near); }\n" } });
                    IrisHost host = rampHost;
                    host.fogRamp = ramp.announced;
                    const bool ran = Translate(pack, passes, failure, host) &&
                                     RunGraph(passes, color, depth, rampCamera, data, DefaultScene(), image, failure);
                    const double k = ran ? image[0][0] / ramp.zNear : -1.0;
                    const double tolerance = ramp.expected >= MinAutoIrisFogScale && ramp.expected <= MaxAutoIrisFogScale &&
                                                     ramp.expected != MinAutoIrisFogScale && ramp.expected != MaxAutoIrisFogScale
                                                 ? 0.01 : 0.001;
                    Check(ran && std::abs(k - ramp.expected) <= tolerance * ramp.expected,
                          "k automático pela rampa do host: " + ramp.name + " (k = " + std::to_string(k) + ") " + failure);
                }
                // The generated GLSL reads clock.zw only with the announced feature.
                std::vector<PassSource> passes;
                std::string failure;
                const IrisFixture pack = Inline({ { "final", "#version 120\nuniform float near;\nvarying vec2 texcoord;\n"
                                                              "void main(){ gl_FragColor = vec4(near); }\n" } });
                IrisHost off = rampHost;
                off.fogRamp = false;
                std::vector<PassSource> passesOff;
                const bool translated = Translate(pack, passes, failure, rampHost) && Translate(pack, passesOff, failure, off);
                const auto readsRamp = [](const std::vector<PassSource>& list) {
                    return !list.empty() && list.back().source.find("PolicromaIrisClock.z") != std::string::npos;
                };
                Check(translated && readsRamp(passes) && !readsRamp(passesOff),
                      "o GLSL lê clock.zw só com o bit de rampa: " + failure);
            }

            // Automatic sky light (0.7.9, PolicromaFeatureSkyExposure): parameters.w < 0 reads sky.x, the host's openness,
            // as mix(30, 240, x); w >= 0 is manual and wins. Without the bit the cbuffer has no 13th vector.
            {
                const IrisHost skyHost{ true, true, true, 900.0f, false, false, true };
                IrisHost noBit = skyHost;
                noBit.skyExposure = false;
                struct SkyCase {
                    std::string name;
                    float manual, openness;
                    bool announced; // the host sets the bit
                    double expected; // eyeBrightness.y
                };
                const std::vector<SkyCase> cases = {
                    { "automático, céu aberto (1) = 240", -1.0f, 1.0f, true, 240.0 },
                    { "automático, interior (0) = 30", -1.0f, 0.0f, true, 30.0 },
                    { "automático, meio (0,5) = 135", -1.0f, 0.5f, true, 135.0 },
                    { "automático, abertura acima de 1 limita em 240", -1.0f, 2.0f, true, 240.0 },
                    { "automático, abertura negativa limita em 30", -1.0f, -3.0f, true, 30.0 },
                    { "manual 100 vence o interior", 100.0f, 0.0f, true, 100.0 },
                    { "manual 0 (escuro) vence o céu aberto", 0.0f, 1.0f, true, 0.0 },
                    { "manual 240 vence o interior", 240.0f, 0.0f, true, 240.0 },
                    { "sem o bit o vetor sky é ignorado (manual 240)", 240.0f, 0.0f, false, 240.0 },
                    { "sem o bit o manual 60 segue valendo", 60.0f, 1.0f, false, 60.0 },
                };
                const IrisFixture pack = Inline({ { "final", "#version 120\nuniform ivec2 eyeBrightness;\n"
                                                              "uniform float linkspanSkyLight;\nvarying vec2 texcoord;\n"
                                                              "void main(){ gl_FragColor = vec4(float(eyeBrightness.y), "
                                                              "linkspanSkyLight * 240.0, 0.0, 1.0); }\n" } });
                for (const auto& sky : cases) {
                    IrisData data = setParameters(2500.0f, 1.0f, 0.0f, sky.manual);
                    data.sky[0] = sky.openness;
                    std::vector<PassSource> passes;
                    Image image;
                    std::string failure;
                    const bool ran = Translate(pack, passes, failure, sky.announced ? skyHost : noBit) &&
                                     RunGraph(passes, color, depth, moved, data, DefaultScene(), image, failure);
                    Check(ran && std::abs(image[0][0] - sky.expected) <= 1e-4 &&
                                       std::abs(image[0][1] - sky.expected) <= 1e-3,
                          "sky_light: " + sky.name + " (y = " + std::to_string(ran ? image[0][0] : -1.0f) + ") " + failure);
                }
                // The generated shader declares and reads the 13th vector only with the announced bit.
                std::vector<PassSource> withBit, withoutBit;
                std::string failure;
                const bool translated = Translate(pack, withBit, failure, skyHost) && Translate(pack, withoutBit, failure, noBit);
                const auto readsSky = [](const std::vector<PassSource>& list) {
                    return !list.empty() && list.back().source.find("PolicromaIrisSky") != std::string::npos;
                };
                Check(translated && readsSky(withBit) && !readsSky(withoutBit),
                      "o GLSL declara o vetor sky só com o bit de exposição do céu: " + failure);
            }

            // The pack's world reconstruction: the view-relative position scales with k, and the projection
            // undoes the scale (P * P^-1 stays the identity), so depth linearisation keeps working.
            const std::string reconstruction =
                "uniform sampler2D depthtex0;\nuniform mat4 gbufferProjectionInverse, gbufferModelViewInverse;\n";
            const std::string reconstructionBody =
                "float d = texture2D(depthtex0, texcoord).r;\n"
                "vec4 v = gbufferProjectionInverse * vec4(texcoord * 2.0 - 1.0, d * 2.0 - 1.0, 1.0);\n"
                "v /= v.w;\ngl_FragColor = vec4((gbufferModelViewInverse * vec4(v.xyz, 1.0)).xyz, 1.0);";
            Image unit, shrunk;
            ok = run(reconstruction, reconstructionBody, liveHost, defaults, unit, failure) &&
                 run(reconstruction, reconstructionBody, liveHost, scaled, shrunk, failure);
            bool relative = ok;
            for (size_t i = 0; ok && i < 3; ++i) relative &= equal(shrunk[0][i], unit[0][i] / 32.0, 1e-4);
            Check(relative, "playerPos reconstruído em blocos é o de unidades do jogo vezes k " + failure);
            ok = run("uniform mat4 gbufferProjection, gbufferProjectionInverse;\n",
                     probeVector + "gl_FragColor = gbufferProjection * (gbufferProjectionInverse * p);", liveHost, scaled,
                     out, failure);
            Check(ok && equal(out[0][0], 0.3, 1e-4) && equal(out[0][1], -0.2, 1e-4) && equal(out[0][2], 0.5, 1e-4) &&
                      equal(out[0][3], 1.0, 1e-4),
                  "gbufferProjection * gbufferProjectionInverse continua a identidade com k = 1/32 " + failure);
        }

        // PolicromaFeaturePackTextures: shaders.properties textures and the repeat sampler s1.
        const IrisHost textured{ true, true };
        const auto texturedGraph = [&](const IrisFixture& pack, Image& out, std::string& error, std::vector<PassSource>* used,
                                       std::string* manifest = nullptr) {
            std::vector<PassSource> passes;
            const bool ok = Translate(pack, passes, error, textured, manifest) &&
                            RunGraph(passes, color, depth, camera, iris, DefaultScene(), out, error);
            if (used) *used = passes;
            return ok;
        };
        ok = texturedGraph(Inline({ { "final", "#version 120\nuniform sampler2D noisetex;\nvarying vec2 texcoord;\n"
                                               "void main(){ vec2 c = texcoord * 0.03;\n"
                                               "gl_FragColor=abs(texture2D(noisetex, c + vec2(1.0, 2.0)) - texture2D(noisetex, c))"
                                               " + vec4(0.0, 0.0, 0.0, texture2D(noisetex, c).r); }\n" } }),
                           out, error, &passes);
        wrapped = ok;
        for (const auto& p : out)
            wrapped &= equal(p[0], 0.0, 0.0001) && equal(p[1], 0.0, 0.0001) && equal(p[2], 0.0, 0.0001);
        Check(wrapped && passes[0].inputs[0] == SceneNoiseInput && passes[0].source.find("register(s1)") != std::string::npos,
              "com a ponte de texturas, noisetex repete pelo sampler s1 " + error);

        {
            IrisFixture pack = Inline({ { "deferred", "#version 120\nuniform sampler2D colortex3;\nvarying vec2 texcoord;\nvoid main(){\n"
                                               "/* DRAWBUFFERS:1 */\n"
                                               "vec4 a = texture2D(colortex3, vec2(0.125, 0.125) + vec2(1.0, -2.0));\n"
                                               "vec4 b = texture2D(colortex3, vec2(0.375, 0.875));\n"
                                               "vec4 seam = texture2D(colortex3, vec2(0.0, 0.125));\n"
                                               "gl_FragData[0] = vec4(a.r, a.g, b.g, seam.r);\n}\n" },
                                 { "final", "#version 120\nuniform sampler2D colortex1, colortex3, noisetex;\nvarying vec2 texcoord;\n"
                                            "void main(){ vec4 c = texture2D(colortex1, texcoord);\n"
                                            "gl_FragColor = vec4(c.rgb, c.a + texture2D(noisetex, vec2(0.625, 0.125)).r"
                                            " + texture2D(colortex3, texcoord).r); }\n" } });
            for (const auto& [stage, buffer] : { std::pair{ "deferred", "colortex3" }, std::pair{ "noise", "" } })
                pack.textures.push_back({ stage, buffer, "shaders/tex/grid.png", GridPng, false });
            ok = texturedGraph(pack, out, error, &passes, &manifest);
            const bool wired = ok && passes.size() == 2 && passes[0].textures && passes[0].textures->size() == 1 &&
                               passes[0].inputs[0] == PackTextureInputBase &&
                               std::count(passes[1].inputs.begin(), passes[1].inputs.end(), PackTextureInputBase) == 1 &&
                               std::count(passes[1].inputs.begin(), passes[1].inputs.end(), ZeroInput) == 1 &&
                               manifest.find(R"("flip":true)") != std::string::npos &&
                               manifest.find(R"("name":"grid")") != std::string::npos;
            Check(wired, "texture.deferred.colortex3 e texture.noise viram uma textura do manifesto; colortex3 no final segue "
                         "como buffer (" + manifest + ") " + error);
            Check(ok && equal(out[0][0], 40 / 255.0) && equal(out[0][1], 40 / 255.0) && equal(out[0][2], 190 / 255.0) &&
                      equal(out[0][3], 115 / 255.0 + 140 / 255.0),
                  "a textura do pack sai na orientação do Iris (linha de cima em t = 0) e repete com filtro na costura: " +
                      (out.empty() ? std::string() : std::to_string(out[0][0]) + " " + std::to_string(out[0][1]) + " " +
                                                         std::to_string(out[0][2]) + " " + std::to_string(out[0][3])) + " " + error);

            pack.textures[0].clamp = true;
            ok = texturedGraph(pack, out, error, &passes);
            Check(ok && equal(out[0][3], 40 / 255.0 + 140 / 255.0),
                  "clamp do .mcmeta amostra a textura com a borda presa " + error);

            pack.textures.clear();
            for (int i = 0; i < 7; ++i)
                pack.textures.push_back({ "deferred", "colortex3", "shaders/tex/grid.png", GridPng, false });
            ok = texturedGraph(pack, out, error, &passes, &manifest);
            Check(ok && manifest.find(R"("name":"grid")") != std::string::npos &&
                      manifest.find("grid-2") == std::string::npos,
                  "várias declarações do mesmo buffer usam uma textura só no manifesto " + error);
        }

        ok = graph(Inline({ { "final", "#version 120\nuniform sampler2D colortex0;\nvarying vec2 texcoord;\n"
                                       "void main(){ gl_FragColor=textureGather(colortex0, vec2(2.0, 1.0) / vec2(8.0, 4.0)); }\n" } }),
                   out, error, &passes);
        // GL corner between columns 1,2 and GL rows 0,1: (i0,j1) (i1,j1) (i1,j0) (i0,j0) = 11, 12, 2, 1.
        Check(ok && passes[0].shaderModel == 5 && equal(out[0][0], 11) && equal(out[0][1], 12) && equal(out[0][2], 2) &&
                  equal(out[0][3], 1),
              "textureGather na ordem do OpenGL (SM 5): " +
                  (out.empty() ? std::string() : std::to_string(out[0][0]) + " " + std::to_string(out[0][1]) + " " +
                                                     std::to_string(out[0][2]) + " " + std::to_string(out[0][3])) + " " + error);

        ok = graph(Inline({ { "final", "#version 120\nuniform vec3 previousCameraPosition, fogColor;\nuniform float frameTime, fogStart, fogEnd;\n"
                                       "uniform mat4 gbufferProjection;\nvoid main(){\n"
                                       "gl_FragColor = gl_FragCoord.x < 4.0 ? vec4(previousCameraPosition, frameTime)"
                                       " : vec4(fogColor.b, fogStart, fogEnd, gbufferProjection[0][0]); }\n" } }),
                   out, error, &passes);
        Check(ok && equal(out[0][0], 5) && equal(out[0][1], -6) && equal(out[0][2], 7) && equal(out[0][3], 0.05) &&
                  equal(out[7][0], 0.6) && equal(out[7][1], 150) && equal(out[7][2], 900) && equal(out[7][3], 1.5),
              "LinkSpanIris: previousCameraPosition, frameTime, névoa e projeção com o ajuste de aspecto " + error);
        if (ok) Check(Inspect(passes[0]).iris && Inspect(passes[0]).resources, "LinkSpanIris chega em b10 com 176 bytes");

        SceneData scene = DefaultScene();
        scene[4] = 0.0f; scene[5] = 3000.0f; scene[6] = 0.0f; // sun straight up
        scene[9] = 1.0f;                                       // one water box
        scene[80] = -10.0f; scene[81] = -10.0f; scene[82] = 10.0f; scene[83] = 10.0f; // xMin zMin xMax zMax
        scene[112] = 50.0f;                                    // surface above the eye at the origin
        ok = graph(Inline({ { "final", "#version 120\nuniform vec3 sunPosition, upPosition;\nuniform int isEyeInWater;\n"
                                       "uniform float sunAngle, rainStrength;\nvoid main(){\n"
                                       "gl_FragColor = vec4(sunPosition.y / 100.0, upPosition.y / 100.0, float(isEyeInWater), sunAngle + rainStrength); }\n" } }),
                   out, error, &passes, scene);
        Check(ok && equal(out[0][0], 1) && equal(out[0][1], 1) && equal(out[0][2], 1) && equal(out[0][3], 0.5),
              "sunPosition, upPosition, isEyeInWater pela caixa d'água e sunAngle às 18:00 " + error);

        ok = graph(Inline({ { "deferred", "#version 120\nuniform sampler2D colortex0;\nvarying vec2 texcoord;\n"
                                          "void main(){\n/* DRAWBUFFERS:4 */\ngl_FragData[0]=texture2D(colortex0,texcoord)*0.0+vec4(0.125);\n}\n" },
                            { "composite", "#version 120\nuniform sampler2D colortex4;\nvarying vec2 texcoord;\n"
                                           "void main(){\n/* DRAWBUFFERS:02 */\ngl_FragData[0]=texture2D(colortex4,texcoord)*2.0;"
                                           "gl_FragData[1]=vec4(1.0);\n}\n" } }),
                   out, error, &passes);
        Check(ok && passes.size() == 3 && passes[2].file == "shaders/final.fsh" && equal(out[0][0], 0.25),
              "deferred roda antes do composite e o final sintético apresenta o colortex0 de um alvo múltiplo " + error);

        std::vector<IrisProgram> many;
        for (int i = 0; i < 16; ++i)
            many.push_back({ i ? "composite" + std::to_string(i) : "composite", "shaders/c" + std::to_string(i) + ".fsh",
                             "#version 120\nuniform sampler2D colortex0;\nvarying vec2 texcoord;\n"
                             "void main(){ gl_FragColor=texture2D(colortex0,texcoord)+vec4(0.01); }\n" });
        IrisFixture sixteen;
        sixteen.programs = many;
        ok = graph(sixteen, out, error, &passes);
        Check(ok && passes.size() == 16 && equal(out[0][1], 0.66), "16 programas encadeados na V3 " + error);
        many.push_back(many.back());
        many.back().name = "final";
        sixteen.programs = many;
        Check(!graph(sixteen, out, error), "17 programas são recusados: " + error);

        {
            // frameCounter/frameTimeCounter follow the render clock on V3 (Iris counts drawn frames), not the 20 Hz game.
            ok = Translate(Inline({ { "final", "#version 120\nuniform int frameCounter;\nuniform float frameTimeCounter;\n"
                                               "void main(){ gl_FragColor = vec4(float(frameCounter), frameTimeCounter, 0.0, 1.0); }\n" } }),
                           passes, error, v3) &&
                 RunGraph(passes, color, depth, camera, iris, DefaultScene(), out, error, 4);
            Check(ok && equal(out[0][0], 3) && equal(out[0][1], 3 * 0.05),
                  "frameCounter e frameTimeCounter seguem o relógio de render da V3 " + error);
        }

        // colortexNClear = false: a buffer read before this frame writes it holds the last writer's previous frame.
        {
            const std::string accumulate =
                "#version 120\nuniform sampler2D colortex0, colortex2;\nvarying vec2 texcoord;\nvoid main(){\n"
                "vec4 h = texture2D(colortex2, texcoord);\n/* DRAWBUFFERS:02 */\n"
                "gl_FragData[0] = texture2D(colortex0, texcoord) * 0.0 + h + vec4(1.0);\ngl_FragData[1] = h + vec4(1.0);\n}\n";
            const std::string present = "#version 120\nuniform sampler2D colortex0;\nvarying vec2 texcoord;\n"
                                        "void main(){ gl_FragColor = texture2D(colortex0, texcoord); }\n";
            ok = Translate(Inline({ { "composite", "#version 120\nconst bool colortex2Clear = false;\n" + accumulate.substr(13) }, { "final", present } }),
                           passes, error, v3, &manifest) &&
                 RunGraph(passes, color, depth, camera, iris, DefaultScene(), out, error, 3);
            Check(ok && passes[0].persistent && manifest.find("history:composite#1") != manifest.npos && equal(out[0][0], 3),
                  "colortex2Clear = false lê o histórico do último escritor (3 quadros acumulam 3) " + manifest + " " + error);
            ok = Translate(Inline({ { "composite", accumulate }, { "final", present } }), passes, error, v3) &&
                 RunGraph(passes, color, depth, camera, iris, DefaultScene(), out, error, 3);
            Check(ok && !passes[0].persistent && equal(out[0][0], 1), "buffer limpo (padrão do Iris) lê zero a cada quadro " + error);
        }

        // The program's .vsh (V3): compatibility built-ins, flat varyings, texcoord from gl_MultiTexCoord0.
        ok = graph(WithVertex(Inline({ { "final", "#version 120\nuniform sampler2D colortex0;\nvarying vec2 texcoord;\n"
                                                  "flat varying vec3 upVec;\nflat varying int mode;\n"
                                                  "void main(){ gl_FragColor = vec4(upVec * float(mode), "
                                                  "texture2D(colortex0, texcoord).r + texcoord.y * 0.001); }\n" } }),
                              "#version 120\nvarying vec2 texcoord;\nflat varying int mode;\nflat varying vec3 upVec;\n"
                              "void main(){ gl_Position = ftransform();\n"
                              "texcoord = (gl_TextureMatrix[0] * gl_MultiTexCoord0).xy;\n"
                              "upVec = vec3(0.25, 0.5, 0.75); mode = 2; }\n"),
                   out, error, &passes, DefaultScene(), &manifest);
        bool vertexRun = ok;
        for (UINT y = 0; ok && y < RunHeight; ++y)
            for (UINT x = 0; x < RunWidth; ++x) {
                const auto& p = out[y * RunWidth + x];
                const double v = 1.0 - (y + 0.5) / RunHeight;
                vertexRun &= equal(p[0], 0.5) && equal(p[1], 1.0) && equal(p[2], 1.5) &&
                             equal(p[3], x + 10.0 * (RunHeight - 1 - y) + v * 0.001, 0.0005);
            }
        Check(vertexRun && manifest.find("\"vertex\":\"shaders/final.vsh\"") != manifest.npos,
              ".vsh do programa roda: ftransform, gl_MultiTexCoord0 na orientação OpenGL e varyings flat (" + manifest +
                  ") " + error);
        if (ok) {
            const auto shape = Inspect(passes[0]);
            Check(shape.compiled && shape.signature && shape.resources,
                  "o vertex do pack liga com o pixel pelas regras do host " + shape.log);
        }

        ok = graph(WithVertex(Inline({ { "final", "#version 120\nflat varying vec4 corner;\nvarying vec4 missing;\n"
                                                  "void main(){ gl_FragColor = corner + missing; }\n" } }),
                              "#version 120\nuniform sampler2D colortex0;\nflat varying vec4 corner;\n"
                              "void main(){ gl_Position = ftransform(); corner = texelFetch(colortex0, ivec2(2, 1), 0); }\n"),
                   out, error, &passes);
        // GL texel (2, 1) is column 2, one row above the bottom: r = 2 + 10 * 1. missing is never written: zero.
        Check(ok && passes[0].inputs[0] == SceneColorInput && equal(out[0][0], 12) && equal(out[5][0], 12) &&
                  equal(out[5][1], 0.5) && equal(out[5][3], 1),
              "o vertex lê textura (texelFetch na orientação OpenGL) e varying não escrito lê zero " + error);

        ok = graph(WithVertex(Inline({ { "composite", "#version 120\nin vec2 texCoord;\nin float lift;\n"
                                                      "void main(){\n/* DRAWBUFFERS:0 */\ngl_FragData[0] = vec4(texCoord, lift, 1.0);\n}\n" } }),
                              "#version 450 compatibility\nout vec2 texCoord;\nout float lift;\n"
                              "uniform mat4 modelViewMatrix, projectionMatrix;\nin vec3 vaPosition;\nin vec2 vaUV0;\n"
                              "uniform float viewHeight;\n"
                              "void main(){ gl_Position = projectionMatrix * modelViewMatrix * vec4(vaPosition, 1.0);\n"
                              "texCoord = vaUV0; lift = viewHeight; }\n"),
                   out, error, &passes);
        Check(ok && equal(out[0][0], 0.5 / RunWidth) && equal(out[0][1], 1.0 - 0.5 / RunHeight) && equal(out[0][2], RunHeight),
              "atributos do núcleo (vaPosition, vaUV0) e matrizes modelView/projection do composite " + error);

        std::string v2manifest;
        Check(Translate(WithVertex(Inline({ { "final", "#version 120\nuniform sampler2D colortex0;\nvarying vec2 texcoord;\n"
                                                       "void main(){ gl_FragColor=texture2D(colortex0,texcoord); }\n" } }),
                                   "#version 120\nvarying vec2 texcoord;\nvoid main(){ gl_Position = ftransform(); }\n"),
                        passes, error, {}, &v2manifest) &&
                  passes[0].vertexSource.empty() && v2manifest.find("vertex") == v2manifest.npos,
              "a ponte V2 ignora o .vsh e mantém o vertex de tela cheia do host " + error);

        std::string v2error;
        Check(!Translate(Inline({ { "final", "#version 120\nuniform sampler2D noisetex;\nvarying vec2 texcoord;\n"
                                             "void main(){ gl_FragColor=texture2D(noisetex,texcoord); }\n" } }),
                         passes, v2error) && v2error.find("V3") != v2error.npos,
              "noisetex numa ponte V2 pede o host novo: " + v2error);
    }
    {
        // Manifest parser: v3 is refused by a V2 host and the output references are checked.
        std::vector<PassSource> sources(2);
        sources[0].file = "a.hlsl";
        sources[1].file = "b.hlsl";
        const std::string v3 = R"({"version":3,"passes":[{"name":"a","file":"a.hlsl","outputs":2,"inputs":["scene"]},)"
                               R"({"name":"b","file":"b.hlsl","inputs":["a","a#1","scene.noise","zero"]}]})";
        bool refused = false;
        try { ApplyPackManifest(v3, sources); } catch (const std::exception&) { refused = true; }
        Check(refused, "manifesto v3 é recusado pela ponte V2");
        bool parsed = false;
        try {
            ApplyPackManifest(v3, sources, 3);
            parsed = ValidPassSources(sources) && sources[0].outputs == 2 && sources[1].inputs[1] == PassOutputInput(0, 1) &&
                     sources[1].inputs[2] == SceneNoiseInput && sources[1].inputs[3] == ZeroInput;
        } catch (const std::exception&) {}
        Check(parsed, "manifesto v3 com a#1, scene.noise e zero");
        const auto refuses = [&](const std::string& text) {
            try { ApplyPackManifest(text, sources, 3); return !ValidPassSources(sources); } catch (const std::exception&) { return true; }
        };
        Check(refuses(R"({"version":3,"passes":[{"name":"a","file":"a.hlsl","outputs":2,"inputs":["scene"]},)"
                      R"({"name":"b","file":"b.hlsl","inputs":["a#2"]}]})"),
              "saída inexistente (a#2 com 2 saídas) é recusada");
        Check(refuses(R"({"version":3,"passes":[{"name":"a","file":"a.hlsl","inputs":["scene"]},)"
                      R"({"name":"b","file":"b.hlsl","outputs":2,"inputs":["a"]}]})"),
              "o passe final não tem várias saídas");
        Check(refuses(R"({"version":2,"passes":[{"name":"a","file":"a.hlsl","outputs":2,"inputs":["scene"]},)"
                      R"({"name":"b","file":"b.hlsl","inputs":["a"]}]})"),
              "outputs exige manifesto v3");
    }
    {
        // The Link-Span GLSL pack keeps working with the relaxed translation.
        const auto root = packs / "PolicromaGlsl";
        std::vector<PassSource> passes;
        NativePackMetadata metadata;
        std::string error;
        IncludeFiles includes;
        includes.emplace("shaders/lib/grade.glsl", ReadText(root / "shaders" / "lib" / "grade.glsl"));
        bool ok = ParseCatalogManifest(ReadText(root / "shaders" / "linkspan-v2.json"), passes, metadata, error);
        for (auto& pass : passes) {
            if (!ok) break;
            pass.source = ReadText(root / pass.file);
            ok = TranslateGlslFragment(pass, includes, "#define POLICROMA_GRADE 1\n", error) && Inspect(pass).compiled;
        }
        Check(ok, "PolicromaGlsl continua traduzindo e compilando: " + error);
    }


    {
        // F0 gbuffer program (GbufferF0.h): opt-in, V3 only, injected only for the colortex a pack program reads, and the
        // reconstructed world normal checked on WARP against planes of known geometry.
        const auto build = [](const IrisFixture& pack, IrisHost host, std::string& glslAll, std::string& manifest,
                              std::string& error) {
            std::vector<std::string> glsl;
            const bool ok = BuildIrisGraph(pack.programs, pack.includes, glsl, manifest, error, host, nullptr, {}, {},
                                           pack.properties);
            glslAll.clear();
            for (const auto& text : glsl) glslAll += text + "\n//--\n";
            return ok;
        };
        const std::string readNormal = "#version 120\nuniform sampler2D colortex4;\nvarying vec2 texcoord;\n"
                                       "void main(){ gl_FragColor=texture2D(colortex4,texcoord); }\n";
        const std::string readMaterial = "#version 120\nuniform sampler2D colortex6;\nvarying vec2 texcoord;\n"
                                         "void main(){ gl_FragColor=texture2D(colortex6,texcoord); }\n";
        const std::string readNothing = "#version 120\nuniform sampler2D colortex0, colortex4;\nvarying vec2 texcoord;\n"
                                        "void main(){ gl_FragColor=texture2D(colortex0,texcoord); }\n";
        const auto json = [](const std::string& manifest) { return nlohmann::json::parse(manifest); };
        const IrisHost off{ true, true, true };
        IrisHost on = off;
        on.syntheticGbuffer = true;
        std::string glslOff, glslOn, manifestOff, manifestOn, error;

        // Off (the default) is the graph without the option: no extra program, whatever the pack reads.
        bool ok = build(Inline({ { "final", readNormal } }), off, glslOff, manifestOff, error);
        Check(ok && json(manifestOff)["passes"].size() == 1 && manifestOff.find("linkspan-gbuffer-f0") == std::string::npos,
              "F0 desligada (padrão): o grafo não ganha programa " + error);
        // On, but nothing in the pack reads colortex4/6/3 (a declared-only sampler is not a read): same graph, byte for byte.
        ok = build(Inline({ { "final", readNothing } }), off, glslOff, manifestOff, error) &&
             build(Inline({ { "final", readNothing } }), on, glslOn, manifestOn, error);
        Check(ok && manifestOff == manifestOn && glslOff == glslOn,
              "F0 ligada sem leitor de colortex4/6/3 no pack: grafo idêntico ao desligado " + error);
        // On with the V2 bridge: the option needs V3, so the graph is the one without it.
        const IrisHost legacyOn{ false, false, false, DefaultIrisCloudHeight, true };
        ok = build(Inline({ { "final", readNothing } }), IrisHost{}, glslOff, manifestOff, error) &&
             build(Inline({ { "final", readNothing } }), legacyOn, glslOn, manifestOn, error);
        Check(ok && manifestOff == manifestOn && glslOff == glslOn, "F0 ligada na ponte V2: grafo idêntico ao desligado " + error);
        // A pack program that writes the buffer before the reader: nothing to fill.
        ok = build(Inline({ { "composite", "#version 120\nvoid main(){\n/* DRAWBUFFERS:4 */\ngl_FragData[0]=vec4(0.5);\n}\n" },
                            { "final", readNormal } }), on, glslOn, manifestOn, error);
        Check(ok && json(manifestOn)["passes"].size() == 2, "F0 não cobre o colortex que o pack escreve antes de ler " + error);
        // On, a reader of colortex4 only: one program first, one output; reading 4, 6 and 3 gives three.
        ok = build(Inline({ { "final", readNormal } }), on, glslOn, manifestOn, error);
        auto passes = ok ? json(manifestOn)["passes"] : nlohmann::json::array();
        Check(ok && passes.size() == 2 && passes[0]["name"] == "linkspan-gbuffer-f0" && !passes[0].contains("outputs") &&
                  passes[0]["inputs"] == nlohmann::json::array({ "scene.depth" }) &&
                  passes[1]["inputs"] == nlohmann::json::array({ "linkspan-gbuffer-f0" }),
              "F0 ligada, pack lê colortex4: um programa antes, uma saída, lido pelo pack (" + manifestOn + ") " + error);
        ok = build(Inline({ { "final", "#version 120\nuniform sampler2D colortex3, colortex4, colortex6;\nvarying vec2 texcoord;\n"
                                       "void main(){ gl_FragColor=texture2D(colortex3,texcoord)+texture2D(colortex4,texcoord)"
                                       "+texture2D(colortex6,texcoord); }\n" } }), on, glslOn, manifestOn, error);
        passes = ok ? json(manifestOn)["passes"] : nlohmann::json::array();
        Check(ok && passes.size() == 2 && passes[0]["outputs"] == 3,
              "F0 ligada, pack lê colortex4, 6 e 3: três saídas (" + manifestOn + ") " + error);
        // Material buffers of the host (linkspan.materialGbuffer) own their colortex: F0 leaves them alone.
        {
            auto pack = Inline({ { "final", readNormal } });
            pack.properties = "linkspan.materialGbuffer=colortex4 colortex5\n";
            ok = build(pack, on, glslOn, manifestOn, error);
            Check(ok && manifestOn.find("linkspan-gbuffer-f0") == std::string::npos,
                  "F0 não toma o colortex do material do host " + error);
        }

        // World with known planes, run on WARP: 64x48, fovy 60, near 10, far 12800, eye (50, 170, 900) turned 30 degrees
        // and tilted 20 degrees down, so the view rotation (not the identity) must put Y up in the world normal.
        const UINT savedWidth = RunWidth, savedHeight = RunHeight;
        RunWidth = 64;
        RunHeight = 48;
        struct Vec { double x, y, z; };
        const auto dot3 = [](Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; };
        const auto normalize3 = [&](Vec a) { const double n = std::sqrt(dot3(a, a)); return Vec{ a.x / n, a.y / n, a.z / n }; };
        const auto cross3 = [](Vec a, Vec b) { return Vec{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; };
        const double nearPlane = 10.0, farPlane = 12800.0, fy = 1.0 / std::tan(0.5236), yaw = 0.5236, pitch = -0.3491;
        const Vec eye{ 50.0, 170.0, 900.0 };
        const Vec forward{ std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch) };
        const Vec right = normalize3(cross3(forward, { 0, 1, 0 }));
        const Vec up = cross3(right, forward);
        const Vec back{ -forward.x, -forward.y, -forward.z };
        const double p00 = fy / (4.0 / 3.0), p11 = fy, p22 = (nearPlane + farPlane) / (nearPlane - farPlane),
                     p32 = 2.0 * nearPlane * farPlane / (nearPlane - farPlane);
        Camera camera{};
        // Row-vector convention (v_view = v_world * V): the columns of the rotation are right, up and back; the last row
        // is -eye * R, as the host builds it.
        const double rotation[3][3] = { { right.x, up.x, back.x }, { right.y, up.y, back.y }, { right.z, up.z, back.z } };
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) camera.view[i][j] = static_cast<float>(rotation[i][j]);
        for (int j = 0; j < 3; ++j)
            camera.view[3][j] = static_cast<float>(-(eye.x * rotation[0][j] + eye.y * rotation[1][j] + eye.z * rotation[2][j]));
        camera.view[3][3] = 1.0f;
        camera.projection[0][0] = static_cast<float>(p00);
        camera.projection[1][1] = static_cast<float>(p11);
        camera.projection[2][2] = static_cast<float>(p22);
        camera.projection[2][3] = -1.0f;
        camera.projection[3][2] = static_cast<float>(p32);
        camera.info[0] = static_cast<float>(nearPlane);
        camera.info[1] = static_cast<float>(farPlane);
        const auto scene = DefaultScene();
        const auto ray = [&](UINT x, UINT y) { // world direction of the pixel's ray; the view-space point is t * (rx, ry, -1)
            const double ndcX = 2.0 * (x + 0.5) / RunWidth - 1.0, ndcY = 1.0 - 2.0 * (y + 0.5) / RunHeight;
            const double rx = ndcX / p00, ry = ndcY / p11;
            return Vec{ rx * right.x + ry * up.x - back.x, rx * right.y + ry * up.y - back.y, rx * right.z + ry * up.z - back.z };
        };
        // A plane (n . p = offset) seen from the camera side; firstColumn cuts it at a screen column (a depth step).
        struct Plane { Vec normal; double offset; UINT firstColumn = 0; };
        const auto hit = [&](const Plane& plane, UINT x, UINT y) { // view distance t of the pixel's ray, 0 when it misses
            if (x < plane.firstColumn) return 0.0;
            const double denominator = dot3(plane.normal, ray(x, y));
            if (std::abs(denominator) < 1e-9) return 0.0;
            const double t = (plane.offset - dot3(plane.normal, eye)) / denominator;
            return t > 5.0 && t < 1800.0 ? t : 0.0;
        };
        const auto towardEye = [&](Plane plane) {
            if (dot3(plane.normal, eye) - plane.offset < 0.0) {
                plane.normal = { -plane.normal.x, -plane.normal.y, -plane.normal.z };
                plane.offset = -plane.offset;
            }
            return plane;
        };
        const Plane floorPlane = towardEye({ { 0, 1, 0 }, 0.0 });
        const Vec slopeNormal = normalize3({ 0.3, 0.8, 0.5 });
        const Plane slopePlane = towardEye({ slopeNormal, dot3(slopeNormal, { eye.x + 400 * forward.x, eye.y + 400 * forward.y - 100,
                                                                              eye.z + 400 * forward.z }) });
        const Vec wallNormal = normalize3({ 0.2, 0.0, 1.0 });
        const Plane wallPlane = towardEye({ wallNormal, dot3(wallNormal, { eye.x + 260 * forward.x, 170, eye.z + 260 * forward.z }), 32 });
        // Per pixel the nearest plane wins; owner is its index (-1: sky) and depth the hardware depth.
        const auto render = [&](const std::vector<Plane>& planes, Image& depth, std::vector<int>& owner) {
            depth.assign(RunWidth * RunHeight, { 1.0f, 0, 0, 1 });
            owner.assign(RunWidth * RunHeight, -1);
            for (UINT y = 0; y < RunHeight; ++y)
                for (UINT x = 0; x < RunWidth; ++x) {
                    double best = 0.0;
                    for (size_t k = 0; k < planes.size(); ++k) {
                        const double t = hit(planes[k], x, y);
                        if (t > 0.0 && (best == 0.0 || t < best)) {
                            best = t;
                            owner[y * RunWidth + x] = static_cast<int>(k);
                        }
                    }
                    if (best > 0.0) depth[y * RunWidth + x][0] = static_cast<float>((-p22 + p32 / best) * 0.5 + 0.5);
                }
        };
        IrisData iris{};
        for (int i = 0; i < 4; ++i) iris.previousProjection[i][i] = iris.previousView[i][i] = 1.0f;
        iris.info[2] = 1.0f; // aspect factor of the host: none
        const auto reconstruct = [&](const std::string& reader, const Image& depth, float k, float sky, Image& out,
                                     std::string& failure) {
            iris.parameters[1] = k;
            iris.parameters[3] = sky;
            std::vector<PassSource> graphPasses;
            const Image color(RunWidth * RunHeight, { 0.2f, 0.2f, 0.2f, 1.0f });
            return Translate(Inline({ { "final", reader } }), graphPasses, failure, on) && graphPasses.size() == 2 &&
                   RunGraph(graphPasses, color, depth, camera, iris, scene, out, failure);
        };
        const auto distance = [](const std::array<float, 4>& value, Vec expected) {
            return std::sqrt(std::pow(value[0] - expected.x, 2) + std::pow(value[1] - expected.y, 2) +
                             std::pow(value[2] - expected.z, 2));
        };
        const auto planeCase = [&](const std::string& label, const std::vector<Plane>& planes, float k) {
            Image depth, normal, material;
            std::vector<int> owner;
            render(planes, depth, owner);
            std::string failure;
            const bool ran = reconstruct(readNormal, depth, k, 240.0f, normal, failure) &&
                             reconstruct(readMaterial, depth, k, 240.0f, material, failure);
            Check(ran, "F0 " + label + " executa no WARP " + failure);
            if (!ran) return;
            double interior = 0.0, halo = 0.0, sky = 0.0;
            int inside = 0, band = 0, faded = 0, haloCount = 0;
            std::string haloAt;
            for (UINT y = 3; y + 3 < RunHeight; ++y)
                for (UINT x = 3; x + 3 < RunWidth; ++x) {
                    const int own = owner[y * RunWidth + x];
                    const auto& value = normal[y * RunWidth + x];
                    bool uniform = true;
                    for (int dy = -3; dy <= 3; ++dy)
                        for (int dx = -3; dx <= 3; ++dx) uniform &= owner[(y + dy) * RunWidth + x + dx] == own;
                    if (own < 0) {
                        if (!uniform) continue;
                        const auto& smooth = material[y * RunWidth + x];
                        sky = std::max(sky, static_cast<double>(std::abs(value[0]) + std::abs(value[1]) + std::abs(value[2]) +
                                                                std::abs(smooth[0]) + std::abs(smooth[1]) + std::abs(smooth[2])));
                        continue;
                    }
                    const Vec expected = planes[own].normal;
                    const double error = distance(value, expected);
                    if (uniform) {
                        interior = std::max(interior, error);
                        ++inside;
                        continue;
                    }
                    // Next to another surface or the sky: the normal of its own plane, or faded toward the eye (the
                    // pass blends them, so the result stays on the arc between the two) - never a mix of surfaces.
                    ++band;
                    if (error <= 0.02) continue;
                    ++faded;
                    const Vec eyeDirection = normalize3({ -ray(x, y).x, -ray(x, y).y, -ray(x, y).z });
                    const Vec plane = normalize3(cross3(expected, eyeDirection));
                    const double offArc = std::abs(value[0] * plane.x + value[1] * plane.y + value[2] * plane.z);
                    if (offArc > 0.03) {
                        ++haloCount;
                        halo = std::max(halo, offArc);
                        haloAt += " (" + std::to_string(x) + "," + std::to_string(y) + ")";
                    }
                }
            char text[320];
            std::snprintf(text, sizeof(text),
                          "F0 %s (k=%g): %d px de interior, erro máx %.4f (< 0.02); %d px de borda, %d esmaecidos, %d com halo (máx %.3f)%s",
                          label.c_str(), static_cast<double>(k), inside, interior, band, faded, haloCount, halo, haloAt.c_str());
            Check(inside > 60 && interior < 0.02 && haloCount == 0, text);
            Check(sky == 0.0, "F0 " + label + ": céu escreve colortex4 e colortex6 zero");
        };
        for (const float k : { 1.0f, 1.0f / 32.0f }) {
            planeCase("plano horizontal", { floorPlane }, k);
            planeCase("plano inclinado em profundidade", { slopePlane }, k);
        }
        planeCase("degrau de profundidade (chão e parede à frente)", { floorPlane, wallPlane }, 1.0f);
        {
            // Smoothness and sky light of colortex6: the floor reflects, the slope does not; sky light follows the
            // provider (eyeBrightness / 240) and is 1 when the host serves no provider parameters.
            Image depth, material;
            std::vector<int> owner;
            std::string failure;
            render({ floorPlane, slopePlane }, depth, owner);
            const auto sample = [&](int plane) {
                for (UINT y = 6; y + 6 < RunHeight; ++y)
                    for (UINT x = 6; x + 6 < RunWidth; ++x)
                        if (owner[y * RunWidth + x] == plane && owner[(y - 3) * RunWidth + x] == plane &&
                            owner[(y + 3) * RunWidth + x] == plane && owner[y * RunWidth + x - 3] == plane &&
                            owner[y * RunWidth + x + 3] == plane)
                            return material[y * RunWidth + x];
                return std::array<float, 4>{ -1, -1, -1, -1 };
            };
            bool ran = reconstruct(readMaterial, depth, 1.0f, 120.0f, material, failure);
            const auto floorValue = ran ? sample(0) : std::array<float, 4>{};
            const auto slopeValue = ran ? sample(1) : std::array<float, 4>{};
            Check(ran && std::abs(floorValue[0] - 0.75) < 0.01 && floorValue[1] == 0.0f && std::abs(floorValue[2] - 0.5) < 0.01 &&
                      slopeValue[0] == 0.0f && std::abs(slopeValue[2] - 0.5) < 0.01,
                  "F0 colortex6: chão reflete (0.75), inclinação não, máscara 0 e skylight = eyeBrightness/240 (0.5) " + failure);
            std::vector<PassSource> noParameters;
            const IrisHost noProvider{ true, true, false, DefaultIrisCloudHeight, true };
            const Image color(RunWidth * RunHeight, { 0.2f, 0.2f, 0.2f, 1.0f });
            ran = Translate(Inline({ { "final", readMaterial } }), noParameters, failure, noProvider) &&
                  RunGraph(noParameters, color, depth, camera, iris, scene, material, failure);
            const auto openSky = ran ? sample(0) : std::array<float, 4>{};
            Check(ran && openSky[2] == 1.0f, "F0 sem parâmetros do provider: skylight 1 (céu aberto) " + failure);
        }
        RunWidth = savedWidth;
        RunHeight = savedHeight;
    }

    std::printf("%s: %d falha(s)\n", failures ? "FALHOU" : "OK", failures);
    return failures ? 1 : 0;
}
