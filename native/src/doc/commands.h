// doc/commands -- undo / redo as version switches.
//
// Owns: Command (label + the versions before and after an edit) and History, the stack of commands with the current version. Because
// versions share their nodes, keeping both sides of every command costs only the changed nodes. Undo and redo switch the current
// version and report the ChangeSet between the two versions (computed by diff), so lighting and the scene update exactly as for a
// forward edit. A drag is an "interaction": its commits show up at once (listeners are told) but end up as one undo step.
// Thread: UI thread only.
// Spec: 03_DOCUMENT.md §5, 08_MILESTONES.md T2.4.
#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>

#include "doc/changeset.h"
#include "doc/document.h"

namespace native::doc {

struct Command {
    std::string label;                                 // "Move", "Clip", "Set texture" ... (Edit menu: "Undo Move")
    std::shared_ptr<const DocumentVersion> before, after;
};

enum class ChangeCause { Commit, Undo, Redo, Reset };

class History {
public:
    static constexpr size_t kMaxCommands = 500;        // the oldest commands are dropped first

    // Called after every change of the current version: the new version, the change set from the previous current version, the cause.
    // Reset (a new document) reports `all`.
    using Listener = std::function<void(const std::shared_ptr<const DocumentVersion>& current, const ChangeSet&, ChangeCause)>;

    explicit History(std::shared_ptr<const DocumentVersion> initial = nullptr);

    void setListener(Listener l) { listener_ = std::move(l); }

    // A new document: the stacks are cleared, the listener hears Reset.
    void reset(std::shared_ptr<const DocumentVersion> version);

    const std::shared_ptr<const DocumentVersion>& current() const { return current_; }

    // Pushes the command and clears redo. Refused (false) when c.before is not the current version (a stale edit) or nothing changed
    // (before == after). During an interaction the command only updates the current version; it joins the interaction's step at end.
    bool commit(Command c);

    // A drag starts: the commits until endInteraction() merge into one command labelled `label`. Nesting is not supported (a second
    // begin ends the first).
    void beginInteraction(std::string label);
    void endInteraction();                             // pushes the merged command when something was committed
    bool interacting() const { return interacting_; }

    bool canUndo() const { return !interacting_ && !undo_.empty(); }
    bool canRedo() const { return !interacting_ && !redo_.empty(); }
    // The label of what undo / redo would do ("" when none).
    std::string undoLabel() const { return undo_.empty() ? std::string() : undo_.back().label; }
    std::string redoLabel() const { return redo_.empty() ? std::string() : redo_.back().label; }
    size_t undoCount() const { return undo_.size(); }
    size_t redoCount() const { return redo_.size(); }

    bool undo();
    bool redo();

private:
    void announce(const std::shared_ptr<const DocumentVersion>& previous, ChangeCause cause);
    void push(Command c);

    std::shared_ptr<const DocumentVersion> current_;
    std::deque<Command> undo_, redo_;                  // back = the next to undo / redo
    bool interacting_ = false;
    Command interaction_;                              // before = at begin, after = latest; label = the interaction's
    bool interactionDirty_ = false;
    Listener listener_;
};

} // namespace native::doc
