#pragma once
#include <string>
#include <utility>
#include <vector>

namespace core {

// The operating system's own file / folder dialogs, shown by Core on the desktop it runs on (a browser page cannot get a real
// path from its file inputs). Windows only for now; elsewhere `available()` is false and the Shell uses its in-page browser.
struct NativeDialogRequest {
    enum class Kind { Folder, Open, Save } kind = Kind::Open;
    std::string title;
    std::string startDir;  // absolute (UTF-8); empty = the system's choice
    std::string fileName;  // Save: the suggested name
    std::vector<std::pair<std::string, std::string>> filters;  // {"Maps", "*.map.json"}; "All files" is added
    std::string defaultExt;  // Save: appended when the typed name has none (without the dot)
};

struct NativeDialogResult {
    bool cancelled = false;
    std::string path;   // absolute (UTF-8) when not cancelled and no error
    std::string error;  // empty = fine
};

bool nativeDialogsAvailable();
/// Blocks until the person closes the dialog: call it on a thread of its own. The dialog is brought in front of the browser.
NativeDialogResult showNativeDialog(const NativeDialogRequest& request);

/// Starts `exe` with a command line made from `args` in `workingDir` and does not wait for it. Empty string = started, else the reason.
std::string launchProcess(const std::string& exe, const std::string& args, const std::string& workingDir);

}  // namespace core
