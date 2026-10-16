#include "core/origin.h"
#include <algorithm>
#include <cctype>

namespace core {

bool isLocalOrigin(std::string_view o) {
    if (o.empty()) return true;

    std::string_view rest;
    if (o.substr(0, 7) == "http://") rest = o.substr(7);
    else if (o.substr(0, 8) == "https://") rest = o.substr(8);
    else return false;

    std::string_view host;
    if (!rest.empty() && rest.front() == '[') {
        const auto end = rest.find(']');
        if (end == std::string_view::npos) return false;
        host = rest.substr(0, end + 1);
        rest = rest.substr(end + 1);
    } else {
        const auto colon = rest.find(':');
        host = rest.substr(0, colon);
        rest = (colon == std::string_view::npos) ? std::string_view{} : rest.substr(colon);
    }
    if (host != "localhost" && host != "127.0.0.1" && host != "[::1]") return false;

    if (rest.empty()) return true;
    if (rest.front() != ':' || rest.size() == 1) return false;
    rest.remove_prefix(1);
    return std::all_of(rest.begin(), rest.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

}  // namespace core
