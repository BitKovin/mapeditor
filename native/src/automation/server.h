// automation/server -- the automation socket (T9.4): newline-delimited JSON over TCP on 127.0.0.1, for outside scripts only (the editor itself never
// uses it; it is off unless --automation-port is given). One request per line: {"id":1,"cmd":"open","path":"maps/demo.map.json"}; one reply per
// line: {"id":1,"ok":true,...} or {"id":1,"ok":false,"error":"..."}. A socket's requests are answered in order (a request that has to wait for the
// editor -- a load, a converging picture -- holds back the ones behind it).
//
// Commands: open, save, get_document, set_document, camera, settings, screenshot, stats, lighting, help. 08_MILESTONES.md T9.4.
// Owns: the listening socket, its clients and the requests waiting for the editor. Does not own the window. Thread: UI thread.
// While the server runs the editor's errors do not open dialogs (they are answered to the script that caused them) and "unsaved changes" questions
// are answered yes: the script owns the editor.
#pragma once

#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class QTcpServer;
class QTcpSocket;
class QTimer;

namespace native::ui { class MainWindow; }

namespace native::automation {

class Server : public QObject {
public:
    explicit Server(ui::MainWindow& window, QObject* parent = nullptr);
    ~Server() override;

    // Listens on 127.0.0.1:port (0 = any free port, see port()). False (and error()) when the port is taken.
    bool listen(int port);
    int port() const;
    const std::string& error() const { return error_; }

    // One request, without a socket: `respond` is called once, now or when the editor has finished what was asked.
    void dispatch(const nlohmann::json& request, std::function<void(nlohmann::json)> respond);

private:
    struct Client;
    struct Waiting;
    struct Outcome;

    Outcome run(const nlohmann::json& request);
    void onConnection();
    void pump(Client& client);
    void poll();

    Outcome cmdOpen(const nlohmann::json& r);
    Outcome cmdSave(const nlohmann::json& r);
    Outcome cmdGetDocument();
    Outcome cmdSetDocument(const nlohmann::json& r);
    Outcome cmdCamera(const nlohmann::json& r);
    Outcome cmdSettings(const nlohmann::json& r);
    Outcome cmdScreenshot(const nlohmann::json& r);
    Outcome cmdStats();
    Outcome cmdLighting(const nlohmann::json& r);
    Outcome cmdHelp();

    ui::MainWindow& window_;
    QTcpServer* tcp_ = nullptr;
    QTimer* timer_ = nullptr;
    std::string error_;
    std::vector<std::string> errors_;   // what the editor reported since the current request began
    std::vector<std::shared_ptr<Client>> clients_;
    std::vector<std::shared_ptr<Waiting>> waiting_;
};

} // namespace native::automation
