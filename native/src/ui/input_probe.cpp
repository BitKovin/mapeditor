// ui/input_probe -- see input_probe.h.
#include "ui/input_probe.h"

#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace native::ui {

#ifdef _WIN32
namespace {

int virtualKeyOf(const std::string& name) {
    if (name.size() == 1) {
        const char c = name[0];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
    }
    struct Named { const char* name; int vk; };
    static const Named table[] = {{"Shift", VK_SHIFT},   {"Ctrl", VK_CONTROL}, {"Alt", VK_MENU},     {"Space", VK_SPACE}, {"Escape", VK_ESCAPE}, {"Tab", VK_TAB},
                                  {"Enter", VK_RETURN},  {"Backspace", VK_BACK}, {"Delete", VK_DELETE}, {"Left", VK_LEFT},   {"Right", VK_RIGHT},   {"Up", VK_UP},
                                  {"Down", VK_DOWN},     {"Home", VK_HOME},    {"End", VK_END},      {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}};
    for (const Named& n : table)
        if (name == n.name) return n.vk;
    if (name.size() >= 2 && name[0] == 'F') {
        const int n = std::atoi(name.c_str() + 1);
        if (n >= 1 && n <= 24) return VK_F1 + n - 1;
    }
    return 0;
}

} // namespace
#endif

bool hardwareTriggerDown(const std::string& trigger, bool& known) {
    known = false;
#ifdef _WIN32
    int vk = 0;
    if (trigger.rfind("btn:", 0) == 0) {
        const int button = std::atoi(trigger.c_str() + 4);
        const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
        switch (button) {
            case 1: vk = swapped ? VK_RBUTTON : VK_LBUTTON; break;
            case 2: vk = swapped ? VK_LBUTTON : VK_RBUTTON; break;
            case 3: vk = VK_MBUTTON; break;
            case 4: vk = VK_XBUTTON1; break;
            case 5: vk = VK_XBUTTON2; break;
            default: break;
        }
    } else if (trigger.rfind("key:", 0) == 0) {
        vk = virtualKeyOf(trigger.substr(4));
    }
    if (vk == 0) return true;
    known = true;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
#else
    (void)trigger;
    return true;
#endif
}

} // namespace native::ui
