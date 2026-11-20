// app/actions/palette -- what the command palette decides, without a widget: the entries (every enabled action, one per variant), fuzzy matching, typed arguments
// ("grid 0.25"), the recently used list; and the two discoverability texts generated from the keymap: the status bar hints of the active tool / modal operator
// and the sections of the key help overlay.
// Qt-free. The registry and keymap are the ones the router reads, so the texts follow a rebinding at once.
// Thread: UI thread.
// Spec: 10_UI_AND_INPUT.md §6, 08_MILESTONES.md T4.7.
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "app/actions/keymap.h"
#include "app/actions/registry.h"

namespace native::actions {

// ---- the palette ---------------------------------------------------------------------------------------------------------------------
struct PaletteEntry {
    std::string id;
    std::string label;          // "Smaller grid", "Camera mode: Top"
    std::string category;
    std::string description;
    nlohmann::json params;      // what to run it with (null = the action's own default)
    std::string keys;           // "Ctrl+Z" (the first plain key binding), empty when unbound
    int score = 0;              // match quality for the query, higher is better
};

// Hold, axis and modal actions need an input stream, so only triggers and toggles are offered; the palette itself and the other entries that are disabled are left out.
bool paletteRunnable(const ActionDef& def);

// Every action the user could run now: enabled for `env`, one entry per menu variant of a parameterised action ("Camera mode: Top"). Registry order.
std::vector<PaletteEntry> paletteEntries(const Registry& registry, const Keymap& keymap, const ActionEnv& env);

// Fuzzy match of `query` against `text`: every character of the query occurs in order (case-insensitive); consecutive characters and word starts score more. Negative = no match.
int fuzzyScore(const std::string& query, const std::string& text);

// The entries that match, best first. An empty query lists all entries, the recently used ones first (most recent first), then the rest by category and label. Words of the
// query may match in any order against label, category and id ("save as", "file save"). A typed argument adds one entry at the top: "grid 0.25" -> Set grid size with size 0.25.
struct Recent {
    std::string id;
    nlohmann::json params;
    bool operator==(const Recent&) const = default;
};
std::vector<PaletteEntry> paletteSearch(const std::vector<PaletteEntry>& entries, const Registry& registry, const std::string& query, const std::vector<Recent>& recent);

// The recently used list as JSON text (settings) and back; bad text gives an empty list. A new use moves to the front; at most `limit` are kept.
std::string recentToJson(const std::vector<Recent>& list);
std::vector<Recent> recentFromJson(const std::string& text);
std::vector<Recent> recentUsed(std::vector<Recent> list, const Recent& used, size_t limit = 12);

// ---- hints and key help ---------------------------------------------------------------------------------------------------------------
// "LMB / Enter confirm  ·  RMB / Esc cancel  ·  X / Y / Z constrain": the keys of the actions that live in `context`, grouped by action, with the action's hint word.
// At most three inputs per action ("Numpad Enter" is left out next to "Enter"). Empty when the context has no bound actions.
std::string hintFor(const Registry& registry, const Keymap& keymap, const std::string& context);

struct HelpLine {
    std::string input;    // "Ctrl+Z" or "X / Y / Z"
    std::string label;    // "Undo"
};
struct HelpSection {
    std::string context;           // "tool:box"
    std::vector<HelpLine> lines;
};
// The bindings of the active contexts, the most specific context first ("global" last), each action once per context with its inputs joined.
std::vector<HelpSection> keyHelpSections(const Registry& registry, const Keymap& keymap, const std::vector<std::string>& activeContexts);

} // namespace native::actions
