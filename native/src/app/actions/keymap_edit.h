// app/actions/keymap_edit -- everything the binding editor (Settings > Keymap) decides, without a widget: which contexts can be active together, which
// bindings an input would clash with, the table rows with their filters, the reverse lookup ("what does this key do"), editing with undo / redo and the
// files of the user keymaps (several, one active; import / export).
// Qt-free; the keymap and the registry are the ones the router reads, so a change applies at the next input event.
// Thread: UI thread.
// Spec: 10_UI_AND_INPUT.md §4.2, §4.3, §5; 08_MILESTONES.md T4.6.
#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "app/actions/keymap.h"
#include "app/actions/registry.h"

namespace native::actions {

// ---- contexts ----------------------------------------------------------------------------------------------------------------------
// 0 global, 1 window:*, 2 area:*, 3 view:*, 4 mode:*, 5 tool:*, 6 modal:* / hold:*. A higher level is more specific.
int contextLevel(const std::string& context);
// Can both contexts be active at once (so a chord bound in both is decided by specificity)? Different levels: yes; the same name: yes; the same level with
// different names: no (two areas, two tools), except the views (perspective with fly / orbit) and the hold / modal contexts.
bool contextsOverlap(const std::string& a, const std::string& b);
// The contexts offered when recording: the ones the actions and the keymap use, outermost first.
std::vector<std::string> knownContexts(const Registry& registry, const Keymap& keymap);
// Where a new binding of the action starts: its narrowest context ("global" for an action available everywhere).
std::string defaultContextFor(const ActionDef& def);
// A chord the editor accepts: no Win / Meta key (the system's), at least one step.
bool bindableChord(const Chord& chord, std::string* reason = nullptr);

// ---- conflicts ---------------------------------------------------------------------------------------------------------------------
struct Conflict {
    enum class Kind {
        Same,      // the same input in the same context
        Shadows,   // the candidate's context is more specific: it wins where both are active, the existing binding loses there
        Shadowed,  // the existing binding's context is more specific: it wins, the candidate would never run there
    };
    Kind kind = Kind::Same;
    Binding existing;
    std::string actionLabel;   // "Dissolve"
};
// The effective bindings of other actions (or the same action with other parameters) that `candidate` would clash with. Empty = free.
std::vector<Conflict> conflictsFor(const Registry& registry, const Keymap& keymap, const Binding& candidate);
// "Already used by Dissolve in mode:mesh.edge" for the dialog.
std::string describeConflict(const Conflict& c);

// ---- the table -----------------------------------------------------------------------------------------------------------------------
struct ActionRow {
    const ActionDef* def = nullptr;
    std::vector<Binding> bindings;   // effective, preset order then user (Binding::fromUser tells which)
    bool changed = false;            // a user change concerns this action (added, removed, or taking a preset chord over)
    bool unbound = false;
    bool conflicting = false;        // one of its bindings is bound to another action in the same context with the same input
};
struct RowFilter {
    std::string text;                // words (all must occur) in the id, label, category, description, bindings or contexts; case-insensitive
    bool changed = false, unbound = false, conflicts = false;
    std::string context;             // actions with a binding in it, or available in it
    std::string category;            // exact
};
std::vector<ActionRow> buildRows(const Registry& registry, const Keymap& keymap, const RowFilter& filter = {});
// The categories of the registry, in order of first use.
std::vector<std::string> categories(const Registry& registry);
// "Ctrl+Z (global)": one binding for the chip text.
std::string bindingText(const Binding& b);

// ---- reverse lookup -------------------------------------------------------------------------------------------------------------------
struct Lookup {
    std::string action, label, context;
    Binding binding;
};
// What `chord` does in every context (effective bindings with the same chord). A chord that does not parse gives an empty list.
std::vector<Lookup> lookupInput(const Registry& registry, const Keymap& keymap, const std::string& chordText);

// ---- editing with undo -------------------------------------------------------------------------------------------------------------------
class BindingEditor {
public:
    BindingEditor(const Registry& registry, Keymap& keymap) : registry_(registry), keymap_(keymap) {}

    enum class Resolve { Cancel, Replace, KeepBoth };
    struct Result {
        bool ok = false;
        std::string error;                 // bad action / chord, or "conflict" with `conflicts` filled when Cancel was chosen
        std::vector<Conflict> conflicts;   // what the binding clashed with (also when it was added anyway)
    };
    // Adds the binding to the user layer. With conflicts: Cancel adds nothing and returns them, Replace unbinds every binding it clashed with (they lose
    // the input), KeepBoth adds it next to them (in different contexts the more specific one decides; in the same context both stay and show as a conflict).
    Result add(const Binding& binding, Resolve onConflict = Resolve::Cancel);
    // Keep both only works where both can stay: a preset binding of the same input in the same context is replaced by any user binding there (the keymap has one
    // action per input per context), so `add(..., KeepBoth)` refuses that case with a message.
    static bool canKeepBoth(const std::vector<Conflict>& conflicts);
    // Builds the binding from text first (error text on a bad chord).
    Result add(const std::string& action, const std::string& context, const std::string& input, const nlohmann::json& params, Resolve onConflict = Resolve::Cancel);
    bool remove(const Binding& binding);
    bool resetAction(const std::string& action);
    bool resetAll();

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool undo();
    bool redo();
    // Bumped by every change (the page and the window know when to save and refresh).
    int revision() const { return revision_; }

private:
    void push();
    bool restore(const std::string& userJson);

    const Registry& registry_;
    Keymap& keymap_;
    std::vector<std::string> undo_, redo_;   // user layer JSON before each change
    int revision_ = 0;
};

// ---- user keymap files ------------------------------------------------------------------------------------------------------------------
// <dir>/<name>.json, one per name; the active one is the application's choice (a setting).
class KeymapStore {
public:
    explicit KeymapStore(std::filesystem::path dir) : dir_(std::move(dir)) {}
    const std::filesystem::path& dir() const { return dir_; }
    std::filesystem::path pathOf(const std::string& name) const { return dir_ / (name + ".json"); }
    // The names that have a file, sorted.
    std::vector<std::string> names() const;
    // A name that can be a file name: letters, digits, space, - _ . ; no path parts. Empty = ok, else the reason.
    static std::string checkName(const std::string& name);
    // Loads the user layer of `name` into `keymap` (a missing file = an empty layer). Empty = ok, else the error (the keymap is untouched).
    std::string load(const std::string& name, Keymap& keymap) const;
    // Writes the user layer atomically (the folder is created). Empty = ok.
    std::string save(const std::string& name, const Keymap& keymap) const;
    std::string remove(const std::string& name) const;
    // A keymap file anywhere: checks that it parses against `keymap`'s grammar and stores it under `name` (the keymap is not changed).
    std::string importFile(const std::filesystem::path& file, const std::string& name) const;
    std::string exportFile(const std::filesystem::path& file, const Keymap& keymap) const;

private:
    std::filesystem::path dir_;
};

} // namespace native::actions
