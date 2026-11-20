// light/light_link -- see light_link.h.
#include "light/light_link.h"

namespace native::light {

LightLink::LightLink(gpu::Device& device, uint32_t mailboxEntries, uint32_t mailboxSlots) : mailbox_(device, mailboxEntries, mailboxSlots) {}

void LightLink::publish(std::shared_ptr<const LitSet> set) {
    std::lock_guard<std::mutex> lock(mutex_);
    current_ = std::move(set);
}

void LightLink::setNotify(std::function<void()> fn) {
    std::lock_guard<std::mutex> lock(mutex_);
    notify_ = std::move(fn);
}

void LightLink::notify() {
    std::function<void()> fn;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        fn = notify_;
    }
    if (fn) fn();
}

std::shared_ptr<const LitSet> LightLink::current() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_;
}

} // namespace native::light
