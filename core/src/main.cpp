#include "core/router.h"
#include "core/scene.h"
#include "core/settings.h"
#include "core/ws_server.h"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <thread>

namespace fs = std::filesystem;

int main(int argc, char** argv) {
    // usage: mapeditor_core [port] [assetsRoot] [initialMap]
    // With no assetsRoot the persisted setting (settings.json next to the executable, changed from the
    // Shell) is used, then ./assets. An explicit argument wins for this run.
    uint16_t port = 8787;
    if (argc > 1) port = static_cast<uint16_t>(std::stoi(argv[1]));
    const bool rootFromArg = argc > 2;
    const bool mapFromArg = argc > 3;

    std::error_code ec;
    fs::path exeDir = fs::absolute(fs::path(argc > 0 ? argv[0] : ""), ec).parent_path();
    if (ec || exeDir.empty()) exeDir = fs::current_path();
    const fs::path settingsFile = exeDir / "settings.json";
    core::Settings saved = core::loadSettings(settingsFile);

    fs::path root = "assets";
    if (rootFromArg) root = core::pathFromUtf8(argv[2]);
    else if (!saved.assetsRoot.empty()) {
        if (fs::is_directory(core::pathFromUtf8(saved.assetsRoot), ec)) root = core::pathFromUtf8(saved.assetsRoot);
        else std::cerr << "saved assets folder '" << saved.assetsRoot << "' no longer exists; using ./assets" << std::endl;
    }
    root = fs::absolute(root, ec).lexically_normal();
    if (core::pathFromUtf8(saved.assetsRoot) != root) {  // another game folder than last time: its own last map (REQUIREMENTS §32)
        saved.lastMap.clear();
        core::loadGameSettings(saved, root);
    }

    core::Scene scene(root);
    try {
        scene.loadGameProfile();
    } catch (const std::exception& e) {
        std::cerr << e.what() << " (assets folder: " << core::pathToUtf8(root) << ")" << std::endl;
        return 1;
    }
    for (const auto& w : scene.entityDefinitions().warnings) std::cerr << "entity definitions: " << w << std::endl;

    // Which map to open first: an explicit argument must work; otherwise try the last-used map, then the
    // demo, then fall back to an empty document (the Shell can Open something).
    if (mapFromArg) {
        try {
            scene.openDocument(argv[3]);
        } catch (const std::exception& e) {
            std::cerr << "failed to open '" << argv[3] << "' under '" << core::pathToUtf8(root) << "': " << e.what()
                      << "\nusage: mapeditor_core [port] [assetsRoot] [initialMap]" << std::endl;
            return 1;
        }
    } else {
        for (const std::string& candidate : {saved.lastMap, std::string("maps/demo.map.json")}) {
            if (candidate.empty()) continue;
            try {
                scene.openDocument(candidate);
                break;
            } catch (const std::exception&) {
            }
        }
        if (scene.currentPath().empty()) std::cerr << "no map opened; starting with an empty document" << std::endl;
    }
    std::cout << "assets: " << core::pathToUtf8(root) << "   settings: " << core::pathToUtf8(settingsFile) << std::endl;
    // Lighting quality / load settings: lighting.json next to settings.json (written with the defaults when missing, so there is something to edit)
    // in the game folder (REQUIREMENTS §32); a lighting.json left next to the executable by an older version is copied there once
    const fs::path lightingFile = root / "lighting.json";
    if (!fs::exists(lightingFile, ec) && !settingsFile.empty() && fs::exists(settingsFile.parent_path() / "lighting.json", ec))
        fs::copy_file(settingsFile.parent_path() / "lighting.json", lightingFile, ec);
    core::LightingService::writeDefaultOptions(lightingFile);
    std::string lightingWarning;
    const core::LightingService::Options lightingOptions = core::LightingService::loadOptions(lightingFile, &lightingWarning);
    if (!lightingWarning.empty()) std::cerr << lightingWarning << std::endl;
    std::cout << "lighting settings: " << core::pathToUtf8(lightingFile) << std::endl;
    core::Router router(scene, settingsFile, lightingOptions);
    router.setLightingFile(lightingFile);
    router.lighting().setLog([](const std::string& line) { std::cout << line << std::endl; });

    // The server's callbacks need the server itself to send replies, but they
    // are constructed as part of it. Nothing invokes a callback before run(),
    // by which point the pointer is set.
    core::WsServer* serverPtr = nullptr;
    auto dispatch = [&](uint64_t sessionId, const core::RouterOutput& out) {
        using To = core::RouterOutput::To;
        for (const auto& msg : out.inDeliveryOrder()) {
            switch (msg.to) {
                case To::Sender: serverPtr->sendTo(sessionId, *msg.text); break;
                case To::Others: serverPtr->broadcastExcept(sessionId, *msg.text); break;
                case To::All: serverPtr->broadcast(*msg.text); break;
            }
        }
        // native dialogs wait for the person: off the server thread, the answer is handed back with post()
        for (const auto& job : out.background)
            std::thread([job, sessionId, server = serverPtr] {
                std::string reply = job();
                server->post([server, sessionId, reply = std::move(reply)] { server->sendTo(sessionId, reply); });
            }).detach();
    };

    std::unique_ptr<core::WsServer> serverOwner;
    try {
        serverOwner = std::make_unique<core::WsServer>(
            port, [&](uint64_t id) { dispatch(id, router.onConnect()); },
            [&](uint64_t id, const std::string& text) { dispatch(id, router.onMessage(text)); }, [](uint64_t) {});
    } catch (const std::exception& e) {
        std::cerr << "cannot listen on 127.0.0.1:" << port << ": " << e.what()
                  << "\nIs another mapeditor_core already running on this port? Close it, or start this one with another port." << std::endl;
        return 1;
    }
    core::WsServer& server = *serverOwner;
    serverPtr = &server;
    server.setTick(std::chrono::seconds(1), [&] { dispatch(0, router.onTick()); });  // only toAll is used by onTick
    // Live lighting runs on its own thread: it never waits for the socket thread and the socket thread never waits for it. The bake thread hands its
    // messages over with post(); delivered() tells it the batch left for every Shell (it holds tiles back while a Shell is behind). That is when the
    // frames were written to the sockets, not when they were queued: a batch is hundreds of kB, and a Shell that reads slower than the bake produces
    // would otherwise fall minutes behind (every edit waiting for the ones before it).
    router.lighting().startWorker([&server, &router](std::vector<std::string>&& messages) {
        server.post([&server, &router, batch = std::move(messages)] {
            const std::shared_ptr<void> written(nullptr, [&router](void*) { router.lighting().delivered(); });
            for (const auto& m : batch) server.broadcast(m, written);
        });
    });
    server.run();
    router.lighting().stopWorker();
    return 0;
}
