// app/actions/contexts -- the context stack of 10_UI_AND_INPUT.md §3.
//
// A context is a named state ("area:viewport", "mode:mesh.edge", "tool:clipper", "modal:grab", "hold:camera.fly"). The active ones
// form a stack, most specific last; "global" is always at the bottom. The router resolves an input by walking the stack from the
// top down. Qt-free.
#pragma once

#include <string>
#include <vector>

namespace native::actions {

class ContextStack {
public:
    ContextStack() { list_.push_back("global"); }

    // Replaces everything above "global" (outermost first, most specific last).
    void set(const std::vector<std::string>& contexts);
    void push(const std::string& name);
    // Removes the most recent entry with that name (not "global").
    void pop(const std::string& name);
    bool has(const std::string& name) const;
    // Outermost first; always starts with "global".
    const std::vector<std::string>& list() const { return list_; }
    std::string text() const;   // "global > window:main > area:viewport", for logs and tests

private:
    std::vector<std::string> list_;
};

} // namespace native::actions
