#pragma once
#include <string_view>

namespace core {

// Browsers let ANY web page open a WebSocket to ws://localhost. Since Core can
// write files, it must not accept connections from arbitrary origins (cross-
// site WebSocket hijacking). Allowed: no Origin header at all (non-browser
// clients: tests, CLI tools) and http(s)://localhost | 127.0.0.1 | [::1] with
// an optional numeric port. Everything else -- including the literal "null"
// origin of file:// pages -- is refused. Revisit when packaging (Electron).
bool isLocalOrigin(std::string_view origin);

}  // namespace core
