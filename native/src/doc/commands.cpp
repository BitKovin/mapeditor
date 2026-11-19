// doc/commands -- see commands.h.
#include "doc/commands.h"

namespace native::doc {

History::History(std::shared_ptr<const DocumentVersion> initial) : current_(std::move(initial)) {}

void History::announce(const std::shared_ptr<const DocumentVersion>& previous, ChangeCause cause) {
    if (!listener_) return;
    ChangeSet cs = (!previous || !current_ || cause == ChangeCause::Reset) ? allChanged(previous ? previous->version : 0, current_ ? current_->version : 0)
                                                                          : diff(*previous, *current_);
    listener_(current_, cs, cause);
}

void History::reset(std::shared_ptr<const DocumentVersion> version) {
    auto previous = current_;
    undo_.clear();
    redo_.clear();
    interacting_ = false;
    interactionDirty_ = false;
    current_ = std::move(version);
    announce(previous, ChangeCause::Reset);
}

void History::push(Command c) {
    undo_.push_back(std::move(c));
    while (undo_.size() > kMaxCommands) undo_.pop_front();
    redo_.clear();
}

bool History::commit(Command c) {
    if (!c.after || !c.before || c.before != current_ || c.before == c.after) return false;
    auto previous = current_;
    current_ = c.after;
    if (interacting_) {
        interaction_.after = c.after;
        interactionDirty_ = true;
    } else {
        push(std::move(c));
    }
    announce(previous, ChangeCause::Commit);
    return true;
}

void History::beginInteraction(std::string label) {
    if (interacting_) endInteraction();
    interacting_ = true;
    interactionDirty_ = false;
    interaction_ = Command{std::move(label), current_, current_};
}

void History::endInteraction() {
    if (!interacting_) return;
    interacting_ = false;
    if (interactionDirty_ && interaction_.before != interaction_.after) push(std::move(interaction_));
    interaction_ = Command{};
    interactionDirty_ = false;
}

bool History::undo() {
    if (!canUndo()) return false;
    Command c = std::move(undo_.back());
    undo_.pop_back();
    auto previous = current_;
    current_ = c.before;
    redo_.push_back(std::move(c));
    announce(previous, ChangeCause::Undo);
    return true;
}

bool History::redo() {
    if (!canRedo()) return false;
    Command c = std::move(redo_.back());
    redo_.pop_back();
    auto previous = current_;
    current_ = c.after;
    undo_.push_back(std::move(c));
    announce(previous, ChangeCause::Redo);
    return true;
}

} // namespace native::doc
