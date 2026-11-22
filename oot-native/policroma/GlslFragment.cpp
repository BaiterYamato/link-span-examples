#include "GlslFragment.h"

#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <SPIRV/GlslangToSpv.h>
#include <spirv_hlsl.hpp>

#include <algorithm>
#include <cctype>
#include <map>
#include <string_view>
#include <utility>
#include <vector>

namespace ShaderPacks {
namespace {

constexpr size_t MaxExpandedIncludes = 512 * 1024;
// The V3 bridge takes 1 MiB of HLSL per pass (V2 128 KiB, checked by the host).
constexpr size_t MaxTranslatedBytes = 1024 * 1024;
// Link-Span v2 GLSL packs put the shared sampler at binding 4; the Iris rewrite uses 16, clear of the 8 textures.
constexpr unsigned GlslSamplerBinding = 4;
constexpr unsigned IrisSamplerBinding = 16;
// The repeat sampler (host s1, bridge feature PolicromaFeaturePackTextures) for noisetex and pack textures.
constexpr unsigned IrisWrapSamplerBinding = 17;

class ZipIncluder final : public glslang::TShader::Includer {
  public:
    explicit ZipIncluder(const IncludeFiles& files) : files(files) {}

    IncludeResult* includeLocal(const char* header, const char* from, size_t depth) override {
        const std::string_view name(header);
        if (name.starts_with('/')) return Open("shaders/" + std::string(name.substr(1)), depth);
        const std::string_view path(from);
        const auto slash = path.rfind('/');
        if (slash == path.npos) return nullptr;
        return Open(std::string(path.substr(0, slash + 1)) + header, depth);
    }

    IncludeResult* includeSystem(const char* header, const char*, size_t depth) override {
        const std::string_view name(header);
        return Open("shaders/" + std::string(name.starts_with('/') ? name.substr(1) : name), depth);
    }

    void releaseInclude(IncludeResult* result) override {
        delete result;
    }

  private:
    IncludeResult* Open(const std::string& path, size_t depth) {
        if (depth > 16) return nullptr;
        const auto found = files.find(path);
        if (found == files.end() || found->second.size() > remaining) return nullptr;
        remaining -= found->second.size();
        return new IncludeResult(found->first, found->second.data(), found->second.size(), nullptr);
    }

    const IncludeFiles& files;
    size_t remaining = MaxExpandedIncludes;
};

struct GlslangProcess {
    bool ready = glslang::InitializeProcess();
    ~GlslangProcess() {
        if (ready) glslang::FinalizeProcess();
    }
};

bool GlslangReady() {
    static const GlslangProcess process;
    return process.ready;
}

bool IsVector(const spirv_cross::CompilerHLSL& compiler, const spirv_cross::Resource& resource,
              unsigned components, unsigned location = 0) {
    const auto& type = compiler.get_type(resource.type_id);
    return compiler.has_decoration(resource.id, spv::DecorationLocation) &&
           compiler.get_decoration(resource.id, spv::DecorationLocation) == location &&
           !compiler.has_decoration(resource.id, spv::DecorationComponent) &&
           !compiler.has_decoration(resource.id, spv::DecorationIndex) && type.array.empty() &&
           type.basetype == spirv_cross::SPIRType::Float && type.width == 32 && type.columns == 1 &&
           type.vecsize == components;
}

// SPIRV-Cross keys its register remapping by (set, binding), one entry for every resource kind. A texture slot and a
// host buffer may share a GLSL binding (16 texture slots against the buffers at 8, 9 and 10), so the entries are
// merged per binding and added once, before compiling.
using Bindings = std::map<unsigned, spirv_cross::HLSLResourceBinding>;

spirv_cross::HLSLResourceBinding& Entry(Bindings& bindings, unsigned binding, spv::ExecutionModel stage) {
    auto& entry = bindings[binding];
    entry.stage = stage;
    entry.desc_set = 0;
    entry.binding = binding;
    return entry;
}

void Commit(spirv_cross::CompilerHLSL& compiler, const Bindings& bindings) {
    for (const auto& [binding, entry] : bindings) compiler.add_hlsl_resource_binding(entry);
}

bool BindTexture(spirv_cross::CompilerHLSL& compiler, Bindings& bindings, const spirv_cross::Resource& resource,
                 const PassSource& pass, spv::ExecutionModel stage = spv::ExecutionModelFragment) {
    const auto& type = compiler.get_type(resource.type_id);
    if (!type.array.empty() || type.image.dim != spv::Dim2D || type.image.arrayed || type.image.ms ||
        type.image.depth || compiler.get_type(type.image.type).basetype != spirv_cross::SPIRType::Float ||
        compiler.get_decoration(resource.id, spv::DecorationDescriptorSet) != 0 ||
        !compiler.has_decoration(resource.id, spv::DecorationBinding)) return false;

    const auto slot = compiler.get_decoration(resource.id, spv::DecorationBinding);
    if (slot >= pass.inputs.size() || pass.inputs[slot] == UnboundInput) return false;

    Entry(bindings, slot, stage).srv.register_binding = slot;
    return true;
}

bool BindSampler(spirv_cross::CompilerHLSL& compiler, Bindings& bindings, const spirv_cross::Resource& sampler,
                 spv::ExecutionModel stage = spv::ExecutionModelFragment) {
    if (!compiler.get_type(sampler.type_id).array.empty() ||
        compiler.get_decoration(sampler.id, spv::DecorationDescriptorSet) != 0 ||
        !compiler.has_decoration(sampler.id, spv::DecorationBinding) ||
        (compiler.get_decoration(sampler.id, spv::DecorationBinding) != GlslSamplerBinding &&
         compiler.get_decoration(sampler.id, spv::DecorationBinding) != IrisSamplerBinding &&
         compiler.get_decoration(sampler.id, spv::DecorationBinding) != IrisWrapSamplerBinding)) return false;
    const auto binding = compiler.get_decoration(sampler.id, spv::DecorationBinding);
    Entry(bindings, binding, stage).sampler.register_binding = binding == IrisWrapSamplerBinding ? 1 : 0;
    return true;
}

// The host constant buffers a GLSL pass may read, by block name. The GLSL binding is the pack's choice inside
// set 0 (Iris rewrites use 8 and 9); the HLSL register is the one the host validates.
struct HostBuffer {
    const char* block;
    unsigned binding;
    unsigned registerIndex;
};
constexpr HostBuffer HostBuffers[] = { { "LinkSpanCamera", 8, 0 }, { "LinkSpanScene", 9, 1 }, { "LinkSpanIris", 10, 10 } };

bool BindHostBuffer(spirv_cross::CompilerHLSL& compiler, Bindings& bindings, const spirv_cross::Resource& buffer,
                    spv::ExecutionModel stage = spv::ExecutionModelFragment) {
    if (compiler.get_decoration(buffer.id, spv::DecorationDescriptorSet) != 0 ||
        !compiler.has_decoration(buffer.id, spv::DecorationBinding)) return false;
    const auto binding = compiler.get_decoration(buffer.id, spv::DecorationBinding);
    const auto& name = compiler.get_name(buffer.base_type_id);
    for (const auto& host : HostBuffers) {
        if (name != host.block || binding != host.binding) continue;
        Entry(bindings, binding, stage).cbv.register_binding = host.registerIndex;
        return true;
    }
    return false;
}

// Field of SPIRV_Cross_Input that carries a semantic, read from the generated HLSL.
std::string InputField(const std::string& hlsl, std::string_view semantic) {
    const auto start = hlsl.find("struct SPIRV_Cross_Input");
    if (start == hlsl.npos) return {};
    const auto end = hlsl.find("};", start);
    const auto block = std::string_view(hlsl).substr(start, end - start);
    const auto at = block.find(std::string(" : ") + std::string(semantic) + ";");
    if (at == block.npos) return {};
    auto name_end = at;
    auto name_start = block.find_last_of(" \t", name_end - 1);
    if (name_start == block.npos) return {};
    return std::string(block.substr(name_start + 1, name_end - name_start - 1));
}

// A varying between the pack's own vertex and fragment shaders: a 32-bit scalar or vector at a location.
bool IsVarying(const spirv_cross::CompilerHLSL& compiler, const spirv_cross::Resource& resource) {
    const auto& type = compiler.get_type(resource.type_id);
    return compiler.has_decoration(resource.id, spv::DecorationLocation) &&
           !compiler.has_decoration(resource.id, spv::DecorationComponent) && type.array.empty() &&
           type.columns == 1 && type.width == 32 &&
           (type.basetype == spirv_cross::SPIRType::Float || type.basetype == spirv_cross::SPIRType::Int ||
            type.basetype == spirv_cross::SPIRType::UInt);
}

// Fragment stage. With raw (the pass has its own vertex shader), the inputs are that shader's varyings and the
// generated HLSL goes to raw without the entry point, which TranslateGlslProgram writes once it knows both stages.
bool TranslateSpirv(std::vector<unsigned> spirv, PassSource& pass, std::string& error, std::string* raw = nullptr) {
    spirv_cross::CompilerHLSL compiler(std::move(spirv));
    auto active = compiler.get_active_interface_variables();
    const auto resources = compiler.get_shader_resources(active);
    bool fragCoord = false;
    for (const auto& builtin : resources.builtin_inputs) {
        if (builtin.builtin != spv::BuiltInFragCoord) {
            error = "GLSL aceita só gl_FragCoord entre as entradas embutidas.";
            return false;
        }
        fragCoord = true;
    }
    // One vec4 output per pass output (RFC 0035), each at its own location 0..outputs-1.
    bool outputs = !resources.stage_outputs.empty() && resources.stage_outputs.size() <= pass.outputs;
    std::vector<bool> locations(pass.outputs, false);
    for (const auto& output : resources.stage_outputs) {
        const auto location = compiler.has_decoration(output.id, spv::DecorationLocation)
                                  ? compiler.get_decoration(output.id, spv::DecorationLocation) : pass.outputs;
        outputs = outputs && location < pass.outputs && !locations[location] &&
                  IsVector(compiler, output, 4, location);
        if (outputs) locations[location] = true;
    }
    const bool inputs = raw ? std::all_of(resources.stage_inputs.begin(), resources.stage_inputs.end(),
                                          [&](const auto& input) { return IsVarying(compiler, input); })
                            : resources.stage_inputs.size() <= 1 &&
                                  (resources.stage_inputs.empty() || IsVector(compiler, resources.stage_inputs.front(), 2));
    if (!inputs || !outputs || !resources.builtin_outputs.empty()) {
        error = "GLSL exige saídas vec4 nas locations 0.." + std::to_string(pass.outputs - 1) +
                (raw ? " e varyings escalares ou vetores de 32 bits." : " e aceita UV vec2 location=0 como única entrada.");
        return false;
    }
    if (!resources.storage_buffers.empty() ||
        !resources.storage_images.empty() || !resources.push_constant_buffers.empty() ||
        !resources.subpass_inputs.empty() || !resources.atomic_counters.empty() ||
        resources.sampled_images.size() + resources.separate_samplers.size() > 2) {
        error = "GLSL aceita texturas 2D e até dois samplers por passe (o comum e o de repetição).";
        return false;
    }
    Bindings bindings;
    for (const auto& buffer : resources.uniform_buffers) {
        if (!BindHostBuffer(compiler, bindings, buffer)) {
            error = "GLSL aceita só os blocos LinkSpanCamera (binding 8), LinkSpanScene (binding 9) e LinkSpanIris "
                    "(binding 10).";
            return false;
        }
    }
    for (const auto& image : resources.sampled_images) {
        if (!BindTexture(compiler, bindings, image, pass)) {
            error = "A textura GLSL precisa de binding de uma entrada 2D declarada no passe.";
            return false;
        }
    }
    for (const auto& image : resources.separate_images) {
        if (!BindTexture(compiler, bindings, image, pass)) {
            error = "A textura GLSL precisa de binding de uma entrada 2D declarada no passe.";
            return false;
        }
    }
    for (const auto& sampler : resources.separate_samplers) {
        if (!BindSampler(compiler, bindings, sampler)) {
            error = "Use layout(binding=4) (ou 16, e 17 para o de repetição) para o sampler GLSL.";
            return false;
        }
    }
    Commit(compiler, bindings);

    compiler.set_enabled_interface_variables(std::move(active));
    spirv_cross::CompilerHLSL::Options options;
    options.shader_model = pass.shaderModel == 5 ? 50 : 40;
    options.use_entry_point_name = true;
    compiler.set_hlsl_options(options);
    compiler.rename_entry_point("main", "PolicromaGlslMain", spv::ExecutionModelFragment);
    auto hlsl = compiler.compile();
    if (raw) {
        *raw = std::move(hlsl);
        return true;
    }
    // Match the fullscreen vertex output of the DX11 bridge: SV_Position in register 0, then TEXCOORD0. The
    // generated input struct may order its fields differently, so it is filled by field name.
    // A UV that is declared but never read can still leave the input struct in the generated entry point, so the
    // wrapper follows the generated signature rather than the active resources.
    const bool takesInput = hlsl.find("PolicromaGlslMain(SPIRV_Cross_Input") != hlsl.npos;
    if (!takesInput) {
        hlsl += "\nSPIRV_Cross_Output main(float4 position : SV_Position) { return PolicromaGlslMain(); }\n";
    } else {
        std::string body = "\nSPIRV_Cross_Output main(float4 position : SV_Position, float2 uv : TEXCOORD0) {\n"
                           "    SPIRV_Cross_Input input = (SPIRV_Cross_Input)0;\n";
        const auto uvField = InputField(hlsl, "TEXCOORD0");
        if (!resources.stage_inputs.empty() && uvField.empty()) {
            error = "Tradução GLSL sem o campo TEXCOORD0 esperado.";
            return false;
        }
        if (!uvField.empty()) body += "    input." + uvField + " = uv;\n";
        if (fragCoord) {
            const auto field = InputField(hlsl, "SV_Position");
            if (field.empty()) {
                error = "Tradução GLSL sem o campo SV_Position esperado.";
                return false;
            }
            body += "    input." + field + " = position;\n";
        }
        hlsl += body + "    return PolicromaGlslMain(input);\n}\n";
    }
    if (hlsl.size() > MaxTranslatedBytes) {
        error = "O HLSL traduzido excede o limite por passe.";
        return false;
    }
    pass.source = std::move(hlsl);
    return true;
}

// The pack's vertex shader for a pass: only gl_VertexIndex comes in (the host draws one fullscreen triangle), and
// it reads the same textures, sampler and host buffers the fragment stage may.
bool TranslateVertexSpirv(std::vector<unsigned> spirv, const PassSource& pass, std::string& hlsl, std::string& error) {
    spirv_cross::CompilerHLSL compiler(std::move(spirv));
    auto active = compiler.get_active_interface_variables();
    const auto resources = compiler.get_shader_resources(active);
    for (const auto& builtin : resources.builtin_inputs) {
        if (builtin.builtin != spv::BuiltInVertexIndex) {
            error = "O vertex shader aceita só gl_VertexIndex entre as entradas embutidas.";
            return false;
        }
    }
    if (!resources.stage_inputs.empty() ||
        !std::all_of(resources.stage_outputs.begin(), resources.stage_outputs.end(),
                     [&](const auto& output) { return IsVarying(compiler, output); })) {
        error = "O vertex shader não recebe atributos e escreve varyings escalares ou vetores de 32 bits.";
        return false;
    }
    if (!resources.storage_buffers.empty() || !resources.storage_images.empty() ||
        !resources.push_constant_buffers.empty() || !resources.atomic_counters.empty() ||
        resources.sampled_images.size() + resources.separate_samplers.size() > 2) {
        error = "O vertex shader aceita texturas 2D e até dois samplers.";
        return false;
    }
    constexpr auto vertex = spv::ExecutionModelVertex;
    Bindings bindings;
    for (const auto& buffer : resources.uniform_buffers) {
        if (!BindHostBuffer(compiler, bindings, buffer, vertex)) {
            error = "O vertex shader aceita só os blocos LinkSpanCamera, LinkSpanScene e LinkSpanIris.";
            return false;
        }
    }
    for (const auto* images : { &resources.sampled_images, &resources.separate_images }) {
        for (const auto& image : *images) {
            if (!BindTexture(compiler, bindings, image, pass, vertex)) {
                error = "A textura do vertex shader precisa de binding de uma entrada 2D declarada no passe.";
                return false;
            }
        }
    }
    for (const auto& sampler : resources.separate_samplers) {
        if (!BindSampler(compiler, bindings, sampler, vertex)) {
            error = "Use layout(binding=16) (ou 17, o de repetição) para o sampler do vertex shader.";
            return false;
        }
    }
    Commit(compiler, bindings);
    compiler.set_enabled_interface_variables(std::move(active));
    spirv_cross::CompilerHLSL::Options options;
    options.shader_model = pass.shaderModel == 5 ? 50 : 40;
    options.use_entry_point_name = true;
    compiler.set_hlsl_options(options);
    // OpenGL clip z runs from -w to w; D3D's from 0 to w.
    auto common = compiler.get_common_options();
    common.vertex.fixup_clipspace = true;
    compiler.set_common_options(common);
    compiler.rename_entry_point("main", "PolicromaGlslVertex", vertex);
    hlsl = compiler.compile();
    return true;
}

struct HlslField {
    std::string declaration; // modifiers and type
    std::string name;
    std::string semantic;
};

// Fields of a struct SPIRV-Cross generated, one per line ("    nointerpolation float3 upVec : TEXCOORD1;").
std::vector<HlslField> StructFields(std::string_view hlsl, std::string_view name) {
    std::vector<HlslField> fields;
    const auto start = hlsl.find("struct " + std::string(name) + "\n");
    if (start == hlsl.npos) return fields;
    const auto end = hlsl.find("};", start);
    auto block = hlsl.substr(start, end == hlsl.npos ? hlsl.npos : end - start);
    block.remove_prefix(std::min(block.size(), block.find('{') + 1));
    while (!block.empty()) {
        const auto newline = block.find('\n');
        auto line = block.substr(0, newline);
        block.remove_prefix(newline == block.npos ? block.size() : newline + 1);
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front()))) line.remove_prefix(1);
        while (!line.empty() && (std::isspace(static_cast<unsigned char>(line.back())) || line.back() == ';'))
            line.remove_suffix(1);
        const auto colon = line.find(" : ");
        if (colon == line.npos) continue;
        const auto left = line.substr(0, colon);
        const auto space = left.rfind(' ');
        if (space == left.npos) continue;
        fields.push_back({ std::string(left.substr(0, space)), std::string(left.substr(space + 1)),
                           std::string(line.substr(colon + 3)) });
    }
    return fields;
}

const HlslField* FindField(const std::vector<HlslField>& fields, std::string_view semantic) {
    for (const auto& field : fields)
        if (field.semantic == semantic) return &field;
    return nullptr;
}

// Both entry points pass one struct, written identically in the two HLSL files, so the vertex outputs and the
// pixel inputs get the same registers: SV_Position, then each varying the vertex shader writes, by location. A pixel
// input the vertex shader does not write reads zero; interpolation follows the pixel declaration, as in OpenGL.
bool LinkVaryings(std::string& vertexHlsl, std::string& pixelHlsl, std::string& error) {
    const auto outputs = StructFields(vertexHlsl, "SPIRV_Cross_Output");
    const auto pixelInputs = StructFields(pixelHlsl, "SPIRV_Cross_Input");
    const auto* position = FindField(outputs, "SV_Position");
    if (!position) {
        error = "O vertex shader não escreve gl_Position.";
        return false;
    }
    std::vector<std::pair<int, const HlslField*>> varyings;
    for (const auto& output : outputs) {
        if (!output.semantic.starts_with("TEXCOORD")) continue;
        varyings.emplace_back(std::stoi(output.semantic.substr(8)), &output);
    }
    std::sort(varyings.begin(), varyings.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::string block = "\nstruct PolicromaVaryings\n{\n    float4 position : SV_Position;\n";
    for (const auto& [location, output] : varyings) {
        const auto type = output->declaration.substr(output->declaration.rfind(' ') + 1);
        const auto* input = FindField(pixelInputs, output->semantic);
        std::string modifiers;
        if (input && input->declaration.find(' ') != std::string::npos)
            modifiers = input->declaration.substr(0, input->declaration.rfind(' ') + 1);
        else if (!type.starts_with("float"))
            modifiers = "nointerpolation "; // integers do not interpolate
        block += "    " + modifiers + type + " policroma_" + output->semantic + " : " + output->semantic + ";\n";
    }
    block += "};\n";

    std::string vertex = block + "\nPolicromaVaryings main(uint policroma_id : SV_VertexID)\n{\n";
    std::string call = "PolicromaGlslVertex()";
    if (vertexHlsl.find("PolicromaGlslVertex(SPIRV_Cross_Input") != vertexHlsl.npos) {
        const auto inputs = StructFields(vertexHlsl, "SPIRV_Cross_Input");
        const auto* id = FindField(inputs, "SV_VertexID");
        vertex += "    SPIRV_Cross_Input input = (SPIRV_Cross_Input)0;\n";
        if (id) vertex += "    input." + id->name + " = policroma_id;\n";
        call = "PolicromaGlslVertex(input)";
    }
    vertex += "    SPIRV_Cross_Output stage = " + call + ";\n    PolicromaVaryings v;\n    v.position = stage." +
              position->name + ";\n";
    for (const auto& [location, output] : varyings)
        vertex += "    v.policroma_" + output->semantic + " = stage." + output->name + ";\n";
    vertexHlsl += vertex + "    return v;\n}\n";

    std::string pixel = block + "\nSPIRV_Cross_Output main(PolicromaVaryings v)\n{\n";
    if (pixelHlsl.find("PolicromaGlslMain(SPIRV_Cross_Input") != pixelHlsl.npos) {
        pixel += "    SPIRV_Cross_Input input = (SPIRV_Cross_Input)0;\n";
        for (const auto& input : pixelInputs) {
            if (input.semantic == "SV_Position") {
                pixel += "    input." + input.name + " = v.position;\n";
            } else if (FindField(outputs, input.semantic)) {
                pixel += "    input." + input.name + " = v.policroma_" + input.semantic + ";\n";
            }
        }
        pixel += "    return PolicromaGlslMain(input);\n}\n";
    } else {
        pixel += "    return PolicromaGlslMain();\n}\n";
    }
    pixelHlsl += pixel;
    return true;
}

} // namespace

bool TranslateGlslFragment(PassSource& pass, const IncludeFiles& includes, const std::string& preamble,
                           std::string& error) {
    try {
        if (!GlslangReady()) {
            error = "Falha ao inicializar glslang.";
            return false;
        }
        const char* source = pass.source.data();
        const char* name = pass.file.c_str();
        const int size = static_cast<int>(pass.source.size());
        const std::string defines = "#extension GL_GOOGLE_include_directive : enable\n" + preamble;
        ZipIncluder includer(includes);
        glslang::TShader shader(EShLangFragment);
        shader.setStringsWithLengthsAndNames(&source, &size, &name, 1);
        shader.setPreamble(defines.c_str());
        shader.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
        // Iris programs declare "in vec2 texcoord" without a location; there is a single varying, so 0.
        shader.setAutoMapLocations(true);
        constexpr auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
        if (!shader.parse(GetDefaultResources(), 450, false, messages, includer)) {
            error = "GLSL inválido.\n" + std::string(std::string_view(shader.getInfoLog()).substr(0, 2048));
            return false;
        }
        glslang::TProgram program;
        program.addShader(&shader);
        if (!program.link(messages)) {
            error = "Falha ao vincular o GLSL.\n" +
                    std::string(std::string_view(program.getInfoLog()).substr(0, 2048));
            return false;
        }
        // setAutoMapLocations only takes effect in mapIO.
        if (!program.mapIO()) {
            error = "Falha ao mapear as entradas e saídas do GLSL.\n" +
                    std::string(std::string_view(program.getInfoLog()).substr(0, 2048));
            return false;
        }
        std::vector<unsigned> spirv;
        glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), spirv);
        return TranslateSpirv(std::move(spirv), pass, error);
    } catch (const std::exception& failure) {
        error = "Falha na tradução GLSL.\n" + std::string(std::string_view(failure.what()).substr(0, 2048));
        return false;
    }
}

bool TranslateGlslProgram(PassSource& pass, std::string& error) {
    try {
        if (!GlslangReady()) {
            error = "Falha ao inicializar glslang.";
            return false;
        }
        constexpr auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
        const std::string vertexName = pass.vertexFile.empty() ? pass.file + ".vsh" : pass.vertexFile;
        glslang::TShader vertex(EShLangVertex), fragment(EShLangFragment);
        const struct {
            glslang::TShader& shader;
            EShLanguage stage;
            const std::string& source;
            const std::string& name;
        } stages[] = { { vertex, EShLangVertex, pass.vertexSource, vertexName },
                       { fragment, EShLangFragment, pass.source, pass.file } };
        for (const auto& stage : stages) {
            const char* source = stage.source.data();
            const char* name = stage.name.c_str();
            const int size = static_cast<int>(stage.source.size());
            stage.shader.setStringsWithLengthsAndNames(&source, &size, &name, 1);
            stage.shader.setEnvInput(glslang::EShSourceGlsl, stage.stage, glslang::EShClientVulkan, 100);
            stage.shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
            stage.shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
            stage.shader.setAutoMapLocations(true);
            glslang::TShader::ForbidIncluder includer;
            if (!stage.shader.parse(GetDefaultResources(), 450, false, messages, includer)) {
                error = std::string(stage.stage == EShLangVertex ? "GLSL de vértice inválido.\n" : "GLSL inválido.\n") +
                        std::string(std::string_view(stage.shader.getInfoLog()).substr(0, 2048));
                return false;
            }
        }
        glslang::TProgram program;
        program.addShader(&vertex);
        program.addShader(&fragment);
        if (!program.link(messages) || !program.mapIO()) {
            error = "Falha ao vincular o vertex e o fragment shader.\n" +
                    std::string(std::string_view(program.getInfoLog()).substr(0, 2048));
            return false;
        }
        std::vector<unsigned> vertexSpirv, fragmentSpirv;
        glslang::GlslangToSpv(*program.getIntermediate(EShLangVertex), vertexSpirv);
        glslang::GlslangToSpv(*program.getIntermediate(EShLangFragment), fragmentSpirv);
        std::string vertexHlsl, pixelHlsl;
        if (!TranslateVertexSpirv(std::move(vertexSpirv), pass, vertexHlsl, error)) return false;
        if (!TranslateSpirv(std::move(fragmentSpirv), pass, error, &pixelHlsl)) return false;
        if (!LinkVaryings(vertexHlsl, pixelHlsl, error)) return false;
        if (vertexHlsl.size() > MaxTranslatedBytes || pixelHlsl.size() > MaxTranslatedBytes) {
            error = "O HLSL traduzido excede o limite por passe.";
            return false;
        }
        pass.source = std::move(pixelHlsl);
        pass.vertexSource = std::move(vertexHlsl);
        return true;
    } catch (const std::exception& failure) {
        error = "Falha na tradução GLSL.\n" + std::string(std::string_view(failure.what()).substr(0, 2048));
        return false;
    }
}

bool PreprocessGlsl(const std::string& source, const std::string& name, std::string& output, std::string& error) {
    try {
        if (!GlslangReady()) {
            error = "Falha ao inicializar glslang.";
            return false;
        }
        const char* text = source.data();
        const char* file = name.c_str();
        const int size = static_cast<int>(source.size());
        glslang::TShader shader(EShLangFragment);
        shader.setStringsWithLengthsAndNames(&text, &size, &file, 1);
        shader.setEnvInput(glslang::EShSourceGlsl, EShLangFragment, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
        glslang::TShader::ForbidIncluder includer;
        constexpr auto messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
        if (!shader.preprocess(GetDefaultResources(), 450, ENoProfile, false, false, messages, &output, includer)) {
            error = "Pré-processador GLSL falhou.\n" + std::string(std::string_view(shader.getInfoLog()).substr(0, 2048));
            return false;
        }
        return true;
    } catch (const std::exception& failure) {
        error = "Falha no pré-processador GLSL.\n" + std::string(std::string_view(failure.what()).substr(0, 2048));
        return false;
    }
}

} // namespace ShaderPacks
