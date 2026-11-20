// Real operating-system input (SendInput with scan codes, as a keyboard and mouse deliver it) into a real window: fly with the look button and a key held, release the
// key, and the camera must stop. Skipped (nothing is sent) unless the test window really is the foreground window, so no keystroke ever reaches another application.
#include <doctest.h>

#include <QElapsedTimer>

#include "gui_support.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace native;
using namespace guitest;

namespace {

void sendScan(WORD scan, bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wScan = scan;
    in.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &in, sizeof(in));
}

void sendRightButton(bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
    SendInput(1, &in, sizeof(in));
}

bool ourForeground() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

double travelled(const render::Vec3& a, const render::Vec3& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

} // namespace

TEST_CASE("hardware input: releasing the fly key stops the camera" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    {
        ui::Services sv = app.services();
        sv.verifyButtons = true;
        sv.watchInput = true;
        ui::MainWindow window(sv);
        window.resize(900, 600);
        window.show();
        window.raise();
        window.activateWindow();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        SetForegroundWindow(reinterpret_cast<HWND>(window.winId()));
        pumpFor(300);
        if (!ourForeground()) {
            MESSAGE("the test window is not the foreground window: nothing was sent, test skipped");
            return;
        }
        render::ViewportWindow* vp = window.viewport();
        vp->rig().fly().position = {0, 2, 200};
        vp->rig().fly().yaw = 3.14159265358979;
        vp->rig().fly().pitch = 0;
        vp->viewChanged();
        const QPoint mid = vp->mapToGlobal(QPoint(vp->width() / 2, vp->height() / 2));
        SetCursorPos(mid.x(), mid.y());
        pumpFor(100);
        for (int round = 0; round < 3; ++round) {
            const render::Vec3 start = vp->rig().fly().position;
            sendRightButton(true);
            pumpFor(150);
            MESSAGE("after the look button: holding " << window.router().holding() << ", pointer grabbed " << vp->pointerGrabbed() << ", active " << ourForeground());
            sendScan(0x11, true);   // W
            pumpFor(500);
            for (int rep = 0; rep < 20; ++rep) {   // the keyboard's auto-repeat: more key-downs while it is down (30 / s)
                sendScan(0x11, true);
                pumpFor(33);
            }
            const render::Vec3 moving = vp->rig().fly().position;
            sendScan(0x11, false);
            pumpFor(150);
            const render::Vec3 a = vp->rig().fly().position;
            pumpFor(500);
            const render::Vec3 b = vp->rig().fly().position;
            sendRightButton(false);
            pumpFor(200);
            MESSAGE("round " << round << ": flew " << travelled(start, moving) << " m while the key was down, " << travelled(moving, a) << " m after the key went up; " << travelled(a, b) << " m in the next 0.5 s; flying " << vp->rig().flying()
                             << ", holding " << window.router().holding());
            CHECK(travelled(start, moving) > 0.5);   // the key really flew the camera
            CHECK(travelled(a, b) < 0.01);
            CHECK_FALSE(window.router().holding());
        }
    }
}

// A rough hand: random presses and releases of W A S D Q E and Shift while the look button is held and the mouse moves; at the end everything is up and the camera must be at rest.
TEST_CASE("hardware input: a random hand leaves nothing held" * doctest::test_suite("gpu")) {
    App app;
    if (!app.ok) return;
    {
        ui::Services sv = app.services();
        sv.verifyButtons = true;
        sv.watchInput = true;
        ui::MainWindow window(sv);
        window.resize(900, 600);
        window.show();
        window.raise();
        window.activateWindow();
        REQUIRE(waitUntil([&] { return allReady(window); }));
        SetForegroundWindow(reinterpret_cast<HWND>(window.winId()));
        pumpFor(300);
        if (!ourForeground()) {
            MESSAGE("the test window is not the foreground window: nothing was sent, test skipped");
            return;
        }
        render::ViewportWindow* vp = window.viewport();
        const QPoint mid = vp->mapToGlobal(QPoint(vp->width() / 2, vp->height() / 2));
        SetCursorPos(mid.x(), mid.y());
        pumpFor(100);
        const WORD keys[] = {0x11, 0x1E, 0x1F, 0x20, 0x10, 0x12, 0x2A};   // W A S D Q E LShift
        bool down[7] = {};
        uint32_t seed = 12345;
        const auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        int stuck = 0;
        for (int round = 0; round < 12; ++round) {
            vp->rig().fly().position = {0, 2, 200};
            sendRightButton(true);
            pumpFor(100);
            for (int step = 0; step < 14; ++step) {
                const int k = static_cast<int>(rnd() % 7);
                down[k] = !down[k];
                sendScan(keys[k], down[k]);
                if (rnd() % 3 == 0) {
                    INPUT in{};
                    in.type = INPUT_MOUSE;
                    in.mi.dwFlags = MOUSEEVENTF_MOVE;
                    in.mi.dx = static_cast<LONG>(rnd() % 9) - 4;
                    in.mi.dy = static_cast<LONG>(rnd() % 9) - 4;
                    SendInput(1, &in, sizeof(in));
                }
                pumpFor(10 + static_cast<int>(rnd() % 120));
            }
            for (int k = 0; k < 7; ++k)
                if (down[k]) {
                    down[k] = false;
                    sendScan(keys[k], false);
                    pumpFor(5);
                }
            // the order of the releases is the hand's: the look button last in half the rounds, first in the others
            if (round % 2 == 0) pumpFor(60);
            sendRightButton(false);
            pumpFor(300);
            const render::Vec3 a = vp->rig().fly().position;
            pumpFor(300);
            const render::Vec3 b = vp->rig().fly().position;
            const bool bad = vp->rig().flying() || window.router().holding() || travelled(a, b) > 0.01;
            if (bad) ++stuck;
            MESSAGE("round " << round << ": flying " << vp->rig().flying() << ", holding " << window.router().holding() << ", moved " << travelled(a, b) << " m afterwards");
        }
        CHECK(stuck == 0);
    }
}
