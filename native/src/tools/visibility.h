// tools/visibility -- Hide / Unhide (REQUIREMENTS 24 C4) and the hide-by-class rules (port of shell/src/model/visibility.ts). A view setting, not part of the map
// (not saved with it, not undone). Hidden things are neither drawn nor picked, and cannot stay selected.
// Keys: "e:<entityId>" hides a whole entity, "b:<entityId>|<brushId>" one brush, "i:<instanceId>" a prefab instance with everything in it.
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.3.
#pragma once

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

#include "doc/document.h"
#include "doc/selection.h"
#include "scene/snapshot.h"

namespace native::tools {

std::string entityKey(const std::string& entityId);
std::string instanceKey(const std::string& instanceId);
std::string brushKey(const std::string& entityId, const std::string& brushId);

// "Hide every entity whose class matches": `*` matches any run of characters (trigger_* = every class starting with trigger_), case-insensitive.
struct HideRule {
    std::string pattern;
    bool enabled = false;
    bool custom = false;   // added by the user (can be removed); presets only toggle
    bool operator==(const HideRule& o) const { return pattern == o.pattern && enabled == o.enabled && custom == o.custom; }
};
std::vector<HideRule> defaultHideRules();   // trigger_*, editor_playable_area (both off)
std::string normalizePattern(const std::string& s);
// Tells whether a class name is matched by an enabled rule. worldspawn (the map itself) is never matched.
std::function<bool(const std::string&)> ruleMatcher(const std::vector<HideRule>& rules);
// Rules from storage (anything may be in there): the presets are always present (their on / off kept), well-formed custom rules kept, junk dropped.
std::vector<HideRule> restoreHideRules(const nlohmann::json& raw);
nlohmann::json hideRulesToJson(const std::vector<HideRule>& rules);
// The first enabled rule that hides `classname`, or null.
const HideRule* matchingRule(const std::string& classname, const std::vector<HideRule>& rules);
// Rules after adding `pattern` (enabled): an existing rule with it is enabled instead of duplicated; empty patterns are ignored.
std::vector<HideRule> addRule(const std::vector<HideRule>& rules, const std::string& pattern);

struct VolumeClass {
    std::string classname;
    int count = 0;
    bool operator==(const VolumeClass& o) const { return classname == o.classname && count == o.count; }
};
// The classes of the brush entities (volumes, groups, ...) with how many use each; worldspawn left out; trigger_ and editor_ first.
std::vector<VolumeClass> volumeClasses(const doc::DocumentVersion& doc);

// What is hidden: the keys plus the rules.
struct HideState {
    std::set<std::string> keys;
    std::vector<HideRule> rules = defaultHideRules();
    bool any() const;
    bool entityHidden(const std::string& entityId, const std::string& classname) const;   // by key or by rule
    bool brushHidden(const std::string& entityId, const std::string& brushId, const std::string& classname) const;   // the brush, or its entity
    bool instanceHidden(const std::string& instanceId) const;
};

// Keys that hide a selection: a selected brush hides that brush, an entity selected as a whole hides the entity, an instance hides it.
std::vector<std::string> hideKeys(const std::vector<doc::SelRef>& sels);
// The selection without what is hidden now.
std::vector<doc::SelRef> pruneHidden(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& sels, const HideState& hide);

// The entities as the views see them: hidden entities left out, hidden brushes removed (an entity whose brushes are all hidden goes too).
struct VisibleEntity {
    std::string entityId;
    std::vector<std::string> brushIds;   // the visible ones, in order
};
std::vector<VisibleEntity> visibleEntities(const doc::DocumentVersion& doc, const HideState& hide);

// The objects of a snapshot that are hidden (what the render thread skips and the picker ignores). Content of a hidden prefab instance counts as hidden;
// class rules do not reach into instances.
std::unordered_set<scene::ObjectKey, scene::ObjectKey::Hash> hiddenObjects(const scene::SceneSnapshot& snapshot, const HideState& hide);

} // namespace native::tools
