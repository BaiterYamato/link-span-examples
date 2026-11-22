#pragma once

#include "PackManifest.h"
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace ShaderPacks {

struct NativePackMetadata {
    std::string manifest;
    bool requiresV2 = false;
    bool requiresV3 = false; // manifesto versão 3 (RFC 0035)
};

// Parser puro compartilhado pelo carregador ZIP e pelos testes sem libzip/GPU.
// O JSON original é emprestado ao host apenas durante activate().
// includes: the files the manifest "textures" names (an Iris pack's PNG files), shared by the passes.
inline bool ParseCatalogManifest(std::string_view text, std::vector<PassSource>& passes,
                                 NativePackMetadata& metadata, std::string& error,
                                 std::shared_ptr<const IncludeFiles> includes = nullptr) {
    try {
        if (text.empty() || text.size() > 16u * 1024u) throw std::runtime_error("Manifesto acima do limite do catálogo.");
        const auto config = nlohmann::json::parse(text);
        const auto& entries = config.at("passes");
        if (!entries.is_array() || entries.empty() || entries.size() > MaxPasses)
            throw std::runtime_error("O manifesto exige de 1 a 16 passes.");
        std::vector<PassSource> candidate;
        for (const auto& entry : entries) {
            const auto name = entry.at("name").get<std::string>();
            PassSource pass;
            pass.file = entry.at("file").get<std::string>();
            if (name.empty() || name.size() > 32 ||
                name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") != name.npos ||
                !pass.file.starts_with("shaders/") || pass.file.size() > 160 ||
                (!pass.file.ends_with(".hlsl") && !pass.file.ends_with(".fsh")))
                throw std::runtime_error("Nome ou caminho de passe inválido.");
            std::string_view path = pass.file;
            while (!path.empty()) {
                const auto slash = path.find('/');
                const auto part = path.substr(0, slash);
                if (part.empty() || part == "." || part == ".." ||
                    part.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-. ") != part.npos)
                    throw std::runtime_error("Caminho de passe inválido.");
                if (slash == path.npos) break;
                path.remove_prefix(slash + 1);
            }
            pass.includes = includes;
            candidate.push_back(std::move(pass));
        }
        ApplyPackManifest(text, candidate, 3);
        if (!ValidPassSources(candidate)) throw std::runtime_error("Grafo de passes inválido.");
        NativePackMetadata result{std::string(text), false, PackManifestVersion(text) == 3};
        for (const auto& pass : candidate) {
            result.requiresV2 |= pass.width != 0 || pass.persistent ||
                std::any_of(pass.inputs.begin(), pass.inputs.end(), IsHistoryInput);
        }
        passes = std::move(candidate);
        metadata = std::move(result);
        error.clear();
        return true;
    } catch (const std::exception& failure) {
        error = "Manifesto Policroma inválido: " + std::string(failure.what());
        return false;
    }
}

} // namespace ShaderPacks
