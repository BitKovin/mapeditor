#pragma once
#include <array>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace core {

// Entity definitions in the Quake 3 / GtkRadiant / NetRadiant `.ent` dialect: an XML-like file of
//
//   <point name="light" color="1 1 0" box="-8 -8 -8 8 8 8">   point entity (a marker in the map)
//   <group name="func_door" color="0 .5 .8">                   brush entity (owns brushes)
//     Free text = the class description.
//     <real key="light" name="Intensity">description</real>    a key; the element name is its type
//     <list key="style" name="Style"><item name="Off" value="0"/></list>
//     <real key="radius" name="Radius" default="10" min="0" max="1000" step="0.5">...</real>   typed editor hints (default / min / max / step)
//     <flag key="START_OPEN" name="Start open" bit="0">description</flag>   a spawnflags bit
//   </point>
//
// The parser is deliberately forgiving: it is a tag scanner, not an XML parser. Comments, `<?xml ?>`
// and unknown elements are skipped, attribute quotes may be ' or ", and a broken class produces a
// warning instead of failing the whole file. A stray definition must never make the editor unusable.

struct EntKey {
    std::string key;
    std::string type;  // element name: string, integer, real, angle, angles, color255, boolean, list, ...
    std::string name;  // human label
    std::string description;
    std::vector<std::pair<std::string, std::string>> items;  // <list>: (label, value)
    // Optional typed-editor hints (attributes of the key element): the value the game uses when the key is absent, and the numeric range / step.
    // A class with defaults gets a Properties section in the Shell that shows every defined key, with its default, instead of only the keys that are set.
    std::optional<std::string> defaultValue;
    std::optional<double> min, max, step;
};

struct EntFlag {
    int bit = 0;
    std::string key;
    std::string name;
    std::string description;
};

struct EntClass {
    enum class Kind { Point, Group };
    std::string name;
    Kind kind = Kind::Point;
    std::array<float, 3> color{0.6f, 0.6f, 0.6f};  // 0..1
    std::optional<std::array<float, 6>> box;         // min xyz, max xyz (game units)
    std::string description;
    std::vector<EntKey> keys;
    std::vector<EntFlag> flags;
};

struct EntityDefinitions {
    std::vector<EntClass> classes;
    std::vector<std::string> warnings;

    const EntClass* find(const std::string& classname) const;
    bool isBrushClass(const std::string& classname) const;  // false for unknown classes
    nlohmann::json toJson() const;                          // {"type":"entity_defs","classes":[...],"warnings":[...]}
};

/// Parses the text of a .ent file. Never throws on malformed content (see warnings).
EntityDefinitions parseEntityDefinitions(std::string_view text);

/// Reads and parses a file. A missing file yields no classes and no warnings (definitions are optional);
/// an unreadable or oversized one yields no classes and a warning.
EntityDefinitions loadEntityDefinitions(const std::filesystem::path& file);

}  // namespace core
