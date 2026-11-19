// app/actions/contexts -- see contexts.h.
#include "app/actions/contexts.h"

#include <algorithm>

namespace native::actions {

void ContextStack::set(const std::vector<std::string>& contexts) {
    list_.assign(1, "global");
    for (const std::string& c : contexts)
        if (!c.empty() && c != "global") list_.push_back(c);
}

void ContextStack::push(const std::string& name) {
    if (!name.empty() && name != "global") list_.push_back(name);
}

void ContextStack::pop(const std::string& name) {
    if (name == "global") return;
    for (size_t i = list_.size(); i-- > 1;)
        if (list_[i] == name) {
            list_.erase(list_.begin() + static_cast<std::ptrdiff_t>(i));
            return;
        }
}

bool ContextStack::has(const std::string& name) const { return std::find(list_.begin(), list_.end(), name) != list_.end(); }

std::string ContextStack::text() const {
    std::string t;
    for (size_t i = 0; i < list_.size(); ++i) t += (i ? " > " : "") + list_[i];
    return t;
}

} // namespace native::actions
