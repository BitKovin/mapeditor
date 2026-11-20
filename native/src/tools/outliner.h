// tools/outliner -- the rows of the Outliner panel as data (port of the model part of shell/src/ui/panels.ts OutlinerPanel): entities with their brushes folded
// under them, prefab instances, the search filter, the eye state of each row, which rows light up for the selection and which row to scroll to.
// Row keys are the hide keys of tools/visibility: "e:<entityId>", "b:<entityId>|<brushId>", "i:<instanceId>".
// Thread: any (pure).
// Spec: 08_MILESTONES.md T4.4.
#pragma once

#include <optional>
#include <set>
#include <string>
#include <vector>

#include "doc/document.h"
#include "doc/selection.h"
#include "tools/visibility.h"

namespace native::tools {

// The eye of a row: open (No), hidden by the row's own key (Self, clicking shows it again), hidden by a Hide rule (Rule) or through its entity (Parent): the last two
// cannot be switched on the row.
enum class EyeState { No, Self, Rule, Parent };
inline bool eyeHidden(EyeState s) { return s != EyeState::No; }
inline bool eyeLocked(EyeState s) { return s == EyeState::Rule || s == EyeState::Parent; }

// The state of an entity row (brushId empty) or of one of its brushes.
EyeState eyeState(const HideState& hide, const std::string& entityId, const std::string& classname, const std::string& brushId = {});

struct OutlinerRow {
    enum class Kind { Entity, Brush, Instance };
    Kind kind = Kind::Entity;
    std::string key;                 // "e:..." / "b:..." / "i:..."
    std::string entityId, brushId, instanceId;
    std::string label;               // the class, "+ brush" / "- brush", the instance's name
    std::string target;              // the targetname (entities)
    std::string detail;              // the id (and "· 2 brushes" while folded), the prefab file
    bool foldable = false;           // an entity with brushes: has the fold arrow
    bool open = false;               // its brushes are listed
    bool selected = false;
    EyeState eye = EyeState::No;
    bool hidden() const { return eyeHidden(eye); }
};

struct OutlinerState {
    std::string query;               // words that must all be in a row's text
    std::set<std::string> expanded;  // entities whose brushes are listed (everything starts folded)
};

struct OutlinerModel {
    std::vector<OutlinerRow> rows;
    std::string message;             // "Empty map. ..." / "Nothing matches “q”." when there are no rows to show
};

OutlinerModel buildOutliner(const doc::DocumentVersion& doc, const std::vector<doc::SelRef>& selection, const HideState& hide, const OutlinerState& state);

// The keys of the rows the outliner scrolls to for the primary selection, best first (a brush: its own row, then its entity's; an instance: its row).
std::vector<std::string> revealKeys(const doc::SelRef& primary);

// Remembers the selection the outliner last scrolled to: only a changed selection moves the list (an unrelated rebuild must not take the scroll position away).
class RevealTracker {
public:
    // The row keys to scroll to when the primary selection (null = nothing) is a different thing than last time; empty when nothing should move.
    std::vector<std::string> update(const doc::SelRef* primary);
    void reset() { last_.clear(); }

private:
    std::string last_;
};

// The message of a click on a hidden row.
inline const char* hiddenRowMessage() { return "Hidden: click its eye in the Outliner to show it again"; }

} // namespace native::tools
