#pragma once
#include "core/profile.h"
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace core {

// Texture groups (REQUIREMENTS 20): rule sets that lay textures out over a face. One file per group,
// `<assetsRoot>/<profile.texGroupRoot>/<name>.texgroup.json`, any folder depth. Names are relative, forward slashes, NO extension
// ("buildings/apartment_block"). Core only stores and lists them: it checks that a file is a JSON object of a sane size with a
// `root` node (rule groups) or a `kit` (simple texture sets); the Shell (model/texgroup.ts) interprets the rules and reports what is wrong with them.

inline constexpr size_t kTexGroupMaxBytes = 1u << 20;

/// Every group under the group root, sorted. `limit` bounds the scan.
std::vector<std::string> listTexGroups(const std::filesystem::path& assetsRoot, const GameProfile& profile, size_t limit = 5000);

/// nullopt: unsafe name, no such group, unreadable, too large, or not a JSON object.
std::optional<nlohmann::json> readTexGroup(const std::filesystem::path& assetsRoot, const GameProfile& profile, const std::string& name);

/// Replaces (or creates) a group, atomically. Returns "" on success, else the reason.
std::string writeTexGroup(const std::filesystem::path& assetsRoot, const GameProfile& profile, const std::string& name, const nlohmann::json& group);

/// Returns "" on success (also when the file was already gone), else the reason.
std::string deleteTexGroup(const std::filesystem::path& assetsRoot, const GameProfile& profile, const std::string& name);

}  // namespace core
