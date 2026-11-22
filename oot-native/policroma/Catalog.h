#pragma once

#include "CatalogManifest.h"
#include "IrisPack.h"

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace ShaderPacks {

enum class Format { Iris, LinkSpan, Unknown, Unreadable };

struct Pack {
    std::filesystem::path path;
    Format format = Format::Unknown;
    unsigned graphicsPrograms = 0;
    unsigned computePrograms = 0;
    unsigned nativeVersion = 0;
};

// Creates the catalog directory and reads entry names only. The caller owns the rendering policy.
std::vector<Pack> Scan(const std::filesystem::path& root, std::error_code& error);

struct ProfileSelection {
    std::string active;
    std::vector<std::pair<std::string, std::string>> choices;
};

bool ReadNativePack(const std::filesystem::path& path, std::vector<PassSource>& passes, std::string& error,
                    std::string_view requestedProfile = {}, ProfileSelection* selected = nullptr, NativePackMetadata* metadata = nullptr);

// Reduced Iris subset (IrisPack.h): deferred*, composite* and final, translated to a Link-Span pass graph (v3 with
// the V3 bridge, else v2).
bool ReadIrisPack(const std::filesystem::path& path, std::vector<PassSource>& passes, NativePackMetadata& metadata,
                  std::string& error, IrisHost host = {});

} // namespace ShaderPacks
