#include "Catalog.h"
#include "GlslFragment.h"
#include "IrisPack.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <zip.h>

namespace ShaderPacks {
namespace {

namespace fs = std::filesystem;
constexpr auto MaxArchiveBytes = 64u * 1024u * 1024u;
constexpr auto MaxEntries = 8192;
constexpr auto MaxShaderBytes = 128u * 1024u;
constexpr auto MaxManifestBytes = 16u * 1024u;
constexpr auto MaxPasses = 8u;
constexpr auto MaxIncludes = 32u;
constexpr auto MaxIncludeBytes = 512u * 1024u;
constexpr size_t MaxPackTextureBytes = 1024u * 1024u; // V3 bridge include limit
constexpr auto NativeShaderEntry = "shaders/linkspan-final-v1.hlsl";
constexpr auto NativeManifestEntry = "shaders/linkspan-v2.json";

using Programs = std::map<std::string, unsigned>;

void RecordProgram(Programs& programs, std::string_view path) {
    constexpr std::string_view root = "shaders/";
    if (!path.starts_with(root) || path.find('\\') != path.npos) return;
    path.remove_prefix(root.size());
    auto name = path;
    if (const auto slash = path.find('/'); slash != path.npos) {
        const auto folder = path.substr(0, slash);
        name = path.substr(slash + 1);
        if (folder.empty() || folder == "." || folder == ".." || name.find('/') != path.npos) return;
    }
    if (!(name.starts_with("gbuffers_") || name.starts_with("shadow") || name.starts_with("deferred") ||
          name.starts_with("composite") || name.starts_with("prepare") || name.starts_with("begin") ||
          name.starts_with("setup") || name.starts_with("final."))) return;

    unsigned stage = 0;
    if (name.ends_with(".vsh")) stage = 1;
    if (name.ends_with(".fsh")) stage = 2;
    if (name.ends_with(".csh")) stage = 4;
    if (stage) programs[std::string(path.substr(0, path.size() - 4))] |= stage;
}

bool ReadFolder(const fs::path& folder, const std::string& prefix, Programs& programs, bool allowSubfolder) {
    std::error_code error;
    const auto status = fs::symlink_status(folder, error);
    if (error == std::errc::no_such_file_or_directory) return true;
    if (error || fs::is_symlink(status)) return false;
    if (!fs::is_directory(status)) return true;

    for (fs::directory_iterator it(folder, error), end; !error && it != end; it.increment(error)) {
        const auto entryStatus = it->symlink_status(error);
        if (error) return false;
        const auto bytes = it->path().filename().u8string();
        const std::string relative = prefix + std::string(bytes.begin(), bytes.end());
        if (fs::is_regular_file(entryStatus)) RecordProgram(programs, relative);
        if (allowSubfolder && fs::is_directory(entryStatus) && !ReadFolder(it->path(), relative + '/', programs, false)) {
            return false;
        }
    }
    return !error;
}

using Archive = std::unique_ptr<zip_t, decltype(&zip_discard)>;

Archive OpenZip(const fs::path& path) {
    std::error_code error;
    Archive zip(nullptr, zip_discard);
    if (fs::file_size(path, error) > MaxArchiveBytes || error) return zip;
#ifdef _WIN32
    std::unique_ptr<zip_source_t, decltype(&zip_source_free)> source(
        zip_source_win32w_create(path.c_str(), 0, -1, nullptr), zip_source_free);
    if (!source) return zip;
    auto* raw = zip_open_from_source(source.get(), ZIP_RDONLY | ZIP_CHECKCONS, nullptr);
    if (raw) source.release(); // Ownership transfers only on success.
#else
    auto* raw = zip_open(path.c_str(), ZIP_RDONLY | ZIP_CHECKCONS, nullptr);
#endif
    zip.reset(raw);
    if (zip) {
        const auto count = zip_get_num_entries(zip.get(), 0);
        if (count < 0 || count > MaxEntries) zip.reset();
    }
    return zip;
}

bool ReadZip(const fs::path& path, Programs& programs, unsigned& nativeVersion) {
    const auto zip = OpenZip(path);
    if (!zip) return false;
    const auto count = zip_get_num_entries(zip.get(), 0);
    if (zip_name_locate(zip.get(), NativeManifestEntry, 0) >= 0) nativeVersion = 2;
    else if (zip_name_locate(zip.get(), NativeShaderEntry, 0) >= 0) nativeVersion = 1;
    for (zip_int64_t i = 0; i < count; ++i) {
        const auto* name = zip_get_name(zip.get(), static_cast<zip_uint64_t>(i), 0);
        if (!name) return false;
        RecordProgram(programs, name);
    }
    return true;
}

Pack Inspect(const fs::path& path, bool directory) {
    Pack pack{path};
    Programs programs;
    if (!(directory ? ReadFolder(path / "shaders", "shaders/", programs, true)
                    : ReadZip(path, programs, pack.nativeVersion))) {
        pack.format = Format::Unreadable;
        return pack;
    }
    for (const auto& program : programs) {
        if ((program.second & 3) == 3) ++pack.graphicsPrograms;
        if (program.second & 4) ++pack.computePrograms;
    }
    if (pack.graphicsPrograms || pack.computePrograms) pack.format = Format::Iris;
    if (pack.nativeVersion) pack.format = Format::LinkSpan;
    return pack;
}

bool ReadEntry(zip_t* zip, const std::string& name, size_t limit, std::string& source, std::string& error,
               bool binary = false) {
    zip_stat_t stats{};
    if (zip_stat(zip, name.c_str(), 0, &stats) != 0 || !(stats.valid & ZIP_STAT_SIZE) ||
        stats.size == 0 || stats.size > limit) {
        error = "Entrada ausente, vazia ou acima do limite: " + name;
        return false;
    }
    const std::unique_ptr<zip_file_t, decltype(&zip_fclose)> file(zip_fopen_index(zip, stats.index, 0),
                                                                zip_fclose);
    std::string candidate(static_cast<size_t>(stats.size), '\0');
    if (!file || zip_fread(file.get(), candidate.data(), candidate.size()) != static_cast<zip_int64_t>(stats.size) ||
        (!binary && candidate.find('\0') != candidate.npos)) {
        error = "Falha de leitura: " + name;
        return false;
    }
    char end;
    if (zip_fread(file.get(), &end, 1) != 0) {
        error = "Falha na verificação da entrada: " + name;
        return false;
    }
    source = std::move(candidate);
    return true;
}

bool ValidResourcePath(std::string_view path) {
    if (path.size() > 160) return false;
    while (!path.empty()) {
        const auto slash = path.find('/');
        const auto part = path.substr(0, slash);
        if (part.empty() || part == "." || part == ".." ||
            part.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-. ") != part.npos) {
            return false;
        }
        if (slash == path.npos) return true;
        path.remove_prefix(slash + 1);
    }
    return false;
}

bool ValidShaderPath(std::string_view path, std::string_view extension) {
    return path.starts_with("shaders/") && path.ends_with(extension) && ValidResourcePath(path);
}

bool ReadIncludes(zip_t* zip, std::string_view extension, std::shared_ptr<IncludeFiles>& includes,
                  std::string& error) {
    auto files = std::make_shared<IncludeFiles>();
    size_t bytes = 0;
    const auto entries = zip_get_num_entries(zip, 0);
    for (zip_int64_t i = 0; i < entries; ++i) {
        const auto* name = zip_get_name(zip, static_cast<zip_uint64_t>(i), 0);
        if (!name || !ValidShaderPath(name, extension)) continue;
        if (files->size() >= MaxIncludes || bytes >= MaxIncludeBytes) {
            error = "O pack excede o limite de includes.";
            return false;
        }
        std::string source;
        if (!ReadEntry(zip, name, std::min<size_t>(MaxShaderBytes, MaxIncludeBytes - bytes), source, error)) return false;
        bytes += source.size();
        files->emplace(name, std::move(source));
    }
    includes = std::move(files);
    return true;
}

bool ReadNativeTextures(zip_t* zip, const nlohmann::json& textures, IncludeFiles& files,
                        std::string& error) {
    if (!textures.is_array() || textures.empty() || textures.size() > MaxPackTextures) {
        error = "textures exige de 1 a 6 texturas PNG.";
        return false;
    }
    constexpr std::string_view pngSignature = "\x89PNG\r\n\x1a\n";
    for (const auto& texture : textures) {
        if (!texture.is_object() || !texture.contains("file") || !texture["file"].is_string()) {
            error = "Declare o arquivo PNG de cada textura.";
            return false;
        }
        const auto file = texture["file"].get<std::string>();
        if (!ValidShaderPath(file, ".png")) {
            error = "Caminho de textura inválido. Use um arquivo .png em shaders/.";
            return false;
        }
        if (files.contains(file)) continue;
        std::string bytes;
        if (!ReadEntry(zip, file, MaxPackTextureBytes, bytes, error, true)) return false;
        if (!bytes.starts_with(pngSignature)) {
            error = "Textura PNG inválida: " + file;
            return false;
        }
        files.emplace(file, std::move(bytes));
    }
    return true;
}

bool ReadTransmissionMaterials(const nlohmann::json& entries,
                               std::shared_ptr<const TransmissionMaterials>& result) {
    if (!entries.is_array() || entries.empty() || entries.size() > MaxTransmissionMaterials) return false;
    auto materials = std::make_shared<TransmissionMaterials>();
    for (const auto& entry : entries) {
        if (!entry.is_object() || !entry.contains("scene") || !entry["scene"].is_number_integer() ||
            entry["scene"] < 0 || entry["scene"] > 127 ||
            !entry.contains("texture") || !entry["texture"].is_string() ||
            !entry.contains("tint") || !entry["tint"].is_array() || entry["tint"].size() != 3 ||
            !entry.contains("strength") || !entry["strength"].is_number() ||
            !entry.contains("light") || !entry["light"].is_array() || entry["light"].size() != 4) return false;
        TransmissionMaterial material{};
        material.scene = entry["scene"].get<unsigned>();
        material.texture = entry["texture"].get<std::string>();
        if (material.texture.find('/') == material.texture.npos || !ValidResourcePath(material.texture) ||
            std::any_of(materials->begin(), materials->end(), [&](const auto& other) {
                return material.scene == other.scene && material.texture == other.texture;
            })) return false;
        for (size_t i = 0; i < 4; ++i) {
            const auto& tint = i < 3 ? entry["tint"][i] : entry["strength"];
            const auto& light = entry["light"][i];
            if (!tint.is_number() || !light.is_number()) return false;
            material.tint[i] = tint.get<float>();
            material.light[i] = light.get<float>();
            if (!std::isfinite(material.tint[i]) || material.tint[i] < 0 || material.tint[i] > 1 ||
                !std::isfinite(material.light[i]) || std::abs(material.light[i]) > 32768) return false;
        }
        if (material.light[3] < 1 || material.light[3] > 2048) return false;
        materials->push_back(std::move(material));
    }
    result = std::move(materials);
    return true;
}

bool ReadProfiles(const nlohmann::json& config, std::string_view requested, ProfileSelection& selection,
                  std::string& preamble, std::string& error) {
    if (!config.contains("profiles") && !config.contains("defaultProfile") && requested.empty()) return true;
    error = "Perfis inválidos. Declare IDs únicos, rótulos e definições inteiras HLSL.";
    if (!config.contains("profiles") || !config["profiles"].is_array() || config["profiles"].empty() ||
        config["profiles"].size() > 8 ||
        (config.contains("defaultProfile") && !config["defaultProfile"].is_string())) return false;
    std::map<std::string, std::string> definitions;
    for (const auto& profile : config["profiles"]) {
        if (!profile.is_object() || !profile.contains("id") || !profile["id"].is_string() ||
            !profile.contains("label") || !profile["label"].is_string() ||
            (profile.contains("defines") && (!profile["defines"].is_object() ||
                                             profile["defines"].size() > 16))) return false;
        const auto id = profile["id"].get<std::string>();
        const auto label = profile["label"].get<std::string>();
        if (id.empty() || id.size() > 32 ||
            id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != id.npos ||
            label.empty() || label.size() > 80 ||
            std::any_of(label.begin(), label.end(), [](unsigned char ch) { return ch < 32 || ch == 127; }) ||
            definitions.contains(id)) return false;
        std::string source;
        const auto defines = profile.value("defines", nlohmann::json::object());
        for (const auto& [key, value] : defines.items()) {
            if (!key.starts_with("POLICROMA_") || key.size() > 48 ||
                key.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") != key.npos ||
                !value.is_number_integer()) return false;
            const auto number = value.get<double>();
            if (number < -1024 || number > 1024) return false;
            source += "#define " + key + " " + std::to_string(static_cast<int>(number)) + "\n";
        }
        definitions.emplace(id, std::move(source));
        selection.choices.emplace_back(id, label);
    }
    const auto defaultProfile = config.value("defaultProfile", selection.choices.front().first);
    if (!definitions.contains(defaultProfile)) return false;
    selection.active = requested.empty() ? defaultProfile : std::string(requested);
    const auto profile = definitions.find(selection.active);
    if (profile == definitions.end()) {
        error = "Perfil ausente no ZIP.";
        return false;
    }
    preamble = profile->second;
    error.clear();
    return true;
}

bool ReadPasses(zip_t* zip, std::vector<PassSource>& passes, std::string& error, std::string_view requested,
                ProfileSelection& selection, NativePackMetadata& metadata) {
    std::string manifest;
    if (!ReadEntry(zip, NativeManifestEntry, MaxManifestBytes, manifest, error)) return false;

    const auto config = nlohmann::json::parse(manifest, nullptr, false);
    if (!config.is_object() || !config.contains("version") || (config["version"] != 2 && config["version"] != 3) ||
        !config.contains("passes") || !config["passes"].is_array() || config["passes"].empty() ||
        config["passes"].size() > (config["version"] == 3 ? ShaderPacks::MaxPasses : MaxPasses)) {
        error = "O manifesto exige de 1 a 8 passes na versão 2, ou de 1 a 16 na versão 3.";
        return false;
    }
    const bool v3 = config["version"] == 3;

    std::shared_ptr<const TransmissionMaterials> materials;
    if (config.contains("transmissionMaterials") && !ReadTransmissionMaterials(config["transmissionMaterials"], materials)) {
        error = "transmissionMaterials exige até quatro materiais válidos e únicos por cena e textura.";
        return false;
    }
    std::string preamble;
    if (!ReadProfiles(config, requested, selection, preamble, error)) return false;

    std::shared_ptr<IncludeFiles> includes;
    if (!ReadIncludes(zip, ".hlsli", includes, error)) return false;
    if (v3 && config.contains("textures") && !ReadNativeTextures(zip, config["textures"], *includes, error))
        return false;
    if (!ParseCatalogManifest(manifest, passes, metadata, error, std::move(includes))) return false;
    for (auto& pass : passes) {
        if (v3 && (!pass.file.ends_with(".hlsl") || !pass.vertexFile.empty())) {
            error = "O manifesto nativo v3 exige passes .hlsl de tela cheia.";
            return false;
        }
        pass.transmissionMaterials = materials;
        if (!ReadEntry(zip, pass.file, MaxShaderBytes, pass.source, error)) return false;
    }
    std::shared_ptr<IncludeFiles> glslIncludes;
    for (auto& pass : passes) {
        if (pass.file.ends_with(".fsh")) {
            if (!glslIncludes && !ReadIncludes(zip, ".glsl", glslIncludes, error)) return false;
            if (!TranslateGlslFragment(pass, *glslIncludes, preamble, error)) {
                error = pass.file + ": " + error;
                return false;
            }
        } else if (!preamble.empty()) {
            constexpr std::string_view lineDirective = "#line 1\n";
            if (pass.source.size() + preamble.size() + lineDirective.size() > MaxShaderBytes) {
                error = "O shader HLSL excede o limite com o perfil selecionado.";
                return false;
            }
            pass.source.insert(0, lineDirective);
            pass.source.insert(0, preamble);
        }
    }
    return true;
}

} // namespace

bool ReadNativePack(const fs::path& path, std::vector<PassSource>& passes, std::string& error,
                    std::string_view requestedProfile, ProfileSelection* selected, NativePackMetadata* metadata) {
    const auto zip = OpenZip(path);
    if (!zip) {
        error = "ZIP ilegível ou limite excedido.";
        return false;
    }
    std::vector<PassSource> candidate;
    ProfileSelection selection;
    NativePackMetadata candidateMetadata;
    if (zip_name_locate(zip.get(), NativeManifestEntry, 0) >= 0) {
        if (!ReadPasses(zip.get(), candidate, error, requestedProfile, selection, candidateMetadata)) return false;
    } else {
        if (!requestedProfile.empty()) {
            error = "Este pack não declara perfis.";
            return false;
        }
        PassSource pass;
        pass.file = NativeShaderEntry;
        pass.inputs[0] = SceneColorInput;
        if (!ReadEntry(zip.get(), pass.file, MaxShaderBytes, pass.source, error)) return false;
        std::shared_ptr<IncludeFiles> includes;
        if (!ReadIncludes(zip.get(), ".hlsli", includes, error)) return false;
        pass.includes = std::move(includes);
        candidate.push_back(std::move(pass));
    }
    passes = std::move(candidate);
    if (selected) *selected = std::move(selection);
    if (metadata) *metadata = std::move(candidateMetadata);
    return true;
}

namespace {

// Iris packs ship hundreds of .glsl files; only the ones a program includes reach the shader.
constexpr auto MaxIrisIncludes = 1024u;
constexpr auto MaxIrisIncludeBytes = 8u * 1024u * 1024u;
constexpr auto MaxIrisFolderDepth = 6;
constexpr size_t MaxIrisIncludeFileBytes = 512u * 1024u; // Complementary has single libraries above 160 KB

constexpr std::string_view IrisProperties = "shaders/shaders.properties";

struct IrisFiles {
    std::vector<IrisProgram> programs;
    IncludeFiles includes;
    size_t includeBytes = 0;
    std::vector<IrisTexture> textures;
    std::string properties; // shaders/shaders.properties, empty when absent
};

// The custom textures of shaders.properties, with their bytes and .mcmeta. A missing or oversized file leaves its
// sampler on the buffer, as Iris does when a texture fails to load. read(name, binary, limit, text) is false when
// the entry is absent or unreadable.
template <typename Read> void ReadIrisTextures(IrisFiles& files, const std::string& properties, Read read) {
    for (auto& texture : ParseIrisTextures(properties)) {
        if (!read(texture.file, true, MaxPackTextureBytes, texture.bytes)) continue;
        std::string mcmeta;
        texture.clamp = read(texture.file + ".mcmeta", false, 64u * 1024u, mcmeta) && IrisTextureClamp(mcmeta);
        files.textures.push_back(std::move(texture));
    }
}

bool AddIrisInclude(IrisFiles& files, std::string name, std::string source, std::string& error) {
    if (files.includes.size() >= MaxIrisIncludes || files.includeBytes + source.size() > MaxIrisIncludeBytes) {
        error = "O pack excede o limite de includes do Iris reduzido (" + std::to_string(MaxIrisIncludes) +
                " arquivos, " + std::to_string(MaxIrisIncludeBytes / (1024 * 1024)) + " MB).";
        return false;
    }
    files.includeBytes += source.size();
    files.includes.emplace(std::move(name), std::move(source));
    return true;
}

bool ReadIrisZip(const fs::path& path, IrisFiles& files, std::string& error) {
    const auto zip = OpenZip(path);
    if (!zip) {
        error = "ZIP ilegível ou limite excedido.";
        return false;
    }
    for (const auto& name : IrisProgramOrder()) {
        for (const auto& file : IrisProgramFiles(name)) {
            if (zip_name_locate(zip.get(), file.c_str(), 0) < 0) continue;
            IrisProgram program{ name, file, {} };
            if (!ReadEntry(zip.get(), file, MaxShaderBytes, program.source, error)) return false;
            if (const auto vertex = IrisVertexFile(file); zip_name_locate(zip.get(), vertex.c_str(), 0) >= 0) {
                if (!ReadEntry(zip.get(), vertex, MaxShaderBytes, program.vertexSource, error)) return false;
                program.vertexFile = vertex;
            }
            files.programs.push_back(std::move(program));
            break;
        }
    }
    const auto entries = zip_get_num_entries(zip.get(), 0);
    for (zip_int64_t i = 0; i < entries; ++i) {
        const auto* name = zip_get_name(zip.get(), static_cast<zip_uint64_t>(i), 0);
        if (!name || !ValidShaderPath(name, ".glsl")) continue;
        // Empty includes are legitimate in Iris packs (placeholders for optional features).
        zip_stat_t stats{};
        const bool empty = zip_stat_index(zip.get(), static_cast<zip_uint64_t>(i), 0, &stats) == 0 &&
                           (stats.valid & ZIP_STAT_SIZE) && stats.size == 0;
        std::string source;
        if ((!empty && !ReadEntry(zip.get(), name, MaxIrisIncludeFileBytes, source, error)) ||
            !AddIrisInclude(files, name, std::move(source), error))
            return false;
    }
    std::string properties, ignored;
    if (zip_name_locate(zip.get(), std::string(IrisProperties).c_str(), 0) >= 0 &&
        ReadEntry(zip.get(), std::string(IrisProperties), MaxIrisIncludeFileBytes, properties, ignored)) {
        files.properties = properties;
        ReadIrisTextures(files, properties, [&](const std::string& name, bool binary, size_t limit, std::string& text) {
            std::string unused;
            return zip_name_locate(zip.get(), name.c_str(), 0) >= 0 &&
                   ReadEntry(zip.get(), name, limit, text, unused, binary);
        });
    }
    return true;
}

bool ReadFolderFile(const fs::path& path, std::string& source, std::string& error, bool include = false,
                    size_t binaryLimit = 0) {
    std::error_code status;
    const auto size = fs::file_size(path, status);
    if (!status && size == 0 && include) {
        source.clear();
        return true;
    }
    if (status || size == 0 ||
        size > (binaryLimit ? binaryLimit : include ? MaxIrisIncludeFileBytes : MaxShaderBytes)) {
        error = "Arquivo ausente, vazio ou acima do limite: " + path.filename().string();
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    std::string candidate(static_cast<size_t>(size), '\0');
    if (!file.read(candidate.data(), static_cast<std::streamsize>(candidate.size())) || file.peek() != EOF ||
        (!binaryLimit && candidate.find('\0') != candidate.npos)) {
        error = "Falha de leitura: " + path.filename().string();
        return false;
    }
    source = std::move(candidate);
    return true;
}

// Unzipped pack in shaderpacks/. Symbolic links are refused, as in the catalog scan.
bool ReadIrisFolder(const fs::path& root, IrisFiles& files, std::string& error) {
    const auto shaders = root / "shaders";
    std::error_code status;
    if (fs::is_symlink(fs::symlink_status(root, status)) || fs::is_symlink(fs::symlink_status(shaders, status)) ||
        !fs::is_directory(fs::symlink_status(shaders, status))) {
        error = "Pasta shaders/ ausente ou inválida.";
        return false;
    }
    for (const auto& name : IrisProgramOrder()) {
        for (const auto& file : IrisProgramFiles(name)) {
            const auto path = root / fs::path(file);
            const auto entry = fs::symlink_status(path, status);
            if (status || !fs::is_regular_file(entry)) continue;
            IrisProgram program{ name, file, {} };
            if (!ReadFolderFile(path, program.source, error)) return false;
            const auto vertex = IrisVertexFile(file);
            const auto vertexPath = root / fs::path(vertex);
            if (const auto vertexEntry = fs::symlink_status(vertexPath, status); !status && fs::is_regular_file(vertexEntry)) {
                if (!ReadFolderFile(vertexPath, program.vertexSource, error)) return false;
                program.vertexFile = vertex;
            }
            status.clear();
            files.programs.push_back(std::move(program));
            break;
        }
    }
    status.clear();
    fs::recursive_directory_iterator it(shaders, fs::directory_options::none, status), end;
    for (; !status && it != end; it.increment(status)) {
        const auto entry = it->symlink_status(status);
        if (status) break;
        if (fs::is_symlink(entry)) {
            it.disable_recursion_pending();
            continue;
        }
        if (fs::is_directory(entry) && it.depth() >= MaxIrisFolderDepth) it.disable_recursion_pending();
        if (!fs::is_regular_file(entry)) continue;
        const auto relative = fs::relative(it->path(), root, status).generic_u8string();
        if (status) break;
        const std::string name(relative.begin(), relative.end());
        if (!ValidShaderPath(name, ".glsl")) continue;
        std::string source;
        if (!ReadFolderFile(it->path(), source, error, true) || !AddIrisInclude(files, name, std::move(source), error))
            return false;
    }
    if (status) {
        error = "Falha ao listar a pasta shaders/.";
        return false;
    }
    const auto readFile = [&](const std::string& name, bool binary, size_t limit, std::string& text) {
        std::error_code missing;
        const auto path = root / fs::path(name);
        if (!fs::is_regular_file(fs::symlink_status(path, missing)) || missing) return false;
        std::string unused;
        // Text files keep the NUL check; the limit only widens for binary ones.
        return ReadFolderFile(path, text, unused, true, binary ? limit : 0);
    };
    std::string properties;
    if (readFile(std::string(IrisProperties), false, MaxIrisIncludeFileBytes, properties)) {
        files.properties = properties;
        ReadIrisTextures(files, properties, readFile);
    }
    return true;
}

} // namespace

bool ReadIrisPack(const fs::path& path, std::vector<PassSource>& passes, NativePackMetadata& metadata,
                  std::string& error, IrisHost host) {
    IrisFiles files;
    std::error_code status;
    const bool folder = fs::is_directory(fs::symlink_status(path, status));
    if (!(folder ? ReadIrisFolder(path, files, error) : ReadIrisZip(path, files, error))) return false;
    const auto& programs = files.programs;
    const auto includes = std::make_shared<const IncludeFiles>(std::move(files.includes));
    std::vector<std::string> glsl, vertexGlsl;
    std::string manifest;
    if (!BuildIrisGraph(programs, *includes, glsl, manifest, error, host, &vertexGlsl, files.textures,
                        ParseIrisUniforms(files.properties), files.properties))
        return false;
    // The custom textures the manifest names travel to the host as includes (PNG bytes, decoded there).
    auto textureFiles = std::make_shared<IncludeFiles>();
    for (const auto& texture : files.textures) textureFiles->emplace(texture.file, texture.bytes);
    std::vector<PassSource> candidate;
    NativePackMetadata candidateMetadata;
    if (!ParseCatalogManifest(manifest, candidate, candidateMetadata, error,
                              textureFiles->empty() ? nullptr : std::shared_ptr<const IncludeFiles>(textureFiles)))
        return false;
    const IncludeFiles none;
    for (size_t i = 0; i < candidate.size(); ++i) {
        candidate[i].source = std::move(glsl[i]);
        // BuildIrisGraph gera este HLSL internamente, sem ler HLSL do ZIP Iris.
        if (candidate[i].file.ends_with(".hlsl")) continue;
        // With a .vsh, both stages translate together and the pass carries the vertex HLSL (sent as an include).
        candidate[i].vertexSource = std::move(vertexGlsl[i]);
        if (!(candidate[i].vertexSource.empty() ? TranslateGlslFragment(candidate[i], none, {}, error)
                                                : TranslateGlslProgram(candidate[i], error))) {
            error = candidate[i].file + ": " + error;
            return false;
        }
    }
    passes = std::move(candidate);
    metadata = std::move(candidateMetadata);
    return true;
}

std::vector<Pack> Scan(const fs::path& root, std::error_code& error) {
    fs::create_directories(root, error);
    if (error) return {};

    std::vector<Pack> packs;
    for (fs::directory_iterator it(root, error), end; !error && it != end; it.increment(error)) {
        const auto status = it->symlink_status(error);
        if (error) return {};
        auto extension = it->path().extension().u8string();
        for (auto& c : extension) {
            if (c >= u8'A' && c <= u8'Z') c += u8'a' - u8'A';
        }
        const bool directory = fs::is_directory(status);
        if (directory || (fs::is_regular_file(status) && extension == u8".zip")) {
            packs.push_back(Inspect(it->path(), directory));
        }
    }
    if (error) return {};
    std::sort(packs.begin(), packs.end(), [](const Pack& a, const Pack& b) { return a.path < b.path; });
    return packs;
}

} // namespace ShaderPacks
