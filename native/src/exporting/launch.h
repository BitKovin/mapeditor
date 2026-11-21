// exporting/launch -- the start line of the game for File > Play map (the Shell Settings > Run game): placeholders expanded, arguments split.
// Thread: any. Spec: 08_MILESTONES.md T9.3.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace native::exporting {

struct LaunchSettings {
    std::string exe;        // absolute path of the game executable
    std::string args = "--map {mapname}.bsp";
    std::string workingDir; // empty = the executable folder
};
struct LaunchCommand {
    std::string exe;
    std::vector<std::string> args;
    std::string workingDir;
};
// {map} = `bspRelative` (assets relative), {mapname} = its name without folder and extension, {bsp} = its absolute path, {game} = the game (assets) folder. Arguments split at spaces
// outside double quotes.
LaunchCommand launchCommand(const LaunchSettings& s, const std::filesystem::path& assetsRoot, const std::string& bspRelative);

} // namespace native::exporting
