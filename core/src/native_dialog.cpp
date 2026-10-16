#include "core/native_dialog.h"
#include "core/settings.h"
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#include <shobjidl.h>
#else
#include <cstdlib>
#endif

namespace fs = std::filesystem;

namespace core {

#ifdef _WIN32
namespace {

std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::string utf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}

template <class T> struct Com {
    T* p = nullptr;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
};

// An invisible owner window in the middle of the window that has the focus (the browser), made the foreground window, so the
// dialog opens centred over the editor and in front of it instead of behind the browser.
HWND makeOwner() {
    HWND fg = GetForegroundWindow();
    RECT r{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    if (fg) GetWindowRect(fg, &r);
    HWND owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"STATIC", L"", WS_POPUP, (r.left + r.right) / 2 - 1, (r.top + r.bottom) / 2 - 120, 1, 1,
                                 nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!owner) return nullptr;
    const DWORD fgThread = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    const bool attached = fgThread && fgThread != me && AttachThreadInput(fgThread, me, TRUE);
    ShowWindow(owner, SW_SHOWNA);
    SetForegroundWindow(owner);
    BringWindowToTop(owner);
    if (attached) AttachThreadInput(fgThread, me, FALSE);
    return owner;
}

}  // namespace

bool nativeDialogsAvailable() { return true; }

NativeDialogResult showNativeDialog(const NativeDialogRequest& req) {
    NativeDialogResult res;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    {
        Com<IFileDialog> dlg;
        const bool save = req.kind == NativeDialogRequest::Kind::Save;
        HRESULT hr = CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg));
        if (FAILED(hr)) { res.error = "cannot create the file dialog"; if (SUCCEEDED(init)) CoUninitialize(); return res; }
        FILEOPENDIALOGOPTIONS opts = 0;
        dlg->GetOptions(&opts);
        opts |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
        if (req.kind == NativeDialogRequest::Kind::Folder) opts |= FOS_PICKFOLDERS | FOS_PATHMUSTEXIST;
        else if (save) opts |= FOS_OVERWRITEPROMPT | FOS_PATHMUSTEXIST;
        else opts |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        dlg->SetOptions(opts);
        if (!req.title.empty()) dlg->SetTitle(wide(req.title).c_str());
        std::vector<std::wstring> names, patterns;
        if (req.kind != NativeDialogRequest::Kind::Folder && !req.filters.empty()) {
            for (const auto& [n, p] : req.filters) { names.push_back(wide(n)); patterns.push_back(wide(p)); }
            names.push_back(L"All files"); patterns.push_back(L"*.*");
            std::vector<COMDLG_FILTERSPEC> spec;
            for (size_t i = 0; i < names.size(); ++i) spec.push_back({names[i].c_str(), patterns[i].c_str()});
            dlg->SetFileTypes(static_cast<UINT>(spec.size()), spec.data());
            dlg->SetFileTypeIndex(1);
        }
        if (!req.defaultExt.empty()) dlg->SetDefaultExtension(wide(req.defaultExt).c_str());
        if (!req.fileName.empty()) dlg->SetFileName(wide(req.fileName).c_str());
        if (!req.startDir.empty()) {
            Com<IShellItem> folder;
            std::error_code ec;
            fs::path start = pathFromUtf8(req.startDir);
            start.make_preferred();
            if (fs::is_directory(start, ec) && SUCCEEDED(SHCreateItemFromParsingName(start.wstring().c_str(), nullptr, IID_PPV_ARGS(&folder))))
                dlg->SetFolder(folder.p);
        }
        HWND owner = makeOwner();
        hr = dlg->Show(owner);
        if (owner) DestroyWindow(owner);
        if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) res.cancelled = true;
        else if (FAILED(hr)) res.error = "the file dialog failed";
        else {
            Com<IShellItem> item;
            PWSTR name = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name))) {
                res.path = utf8(name);
                CoTaskMemFree(name);
            } else res.error = "that is not a file system location";
        }
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return res;
}

std::string launchProcess(const std::string& exe, const std::string& args, const std::string& workingDir) {
    std::error_code ec;
    const fs::path exePath = pathFromUtf8(exe);
    if (exe.empty()) return "no game executable is set (Settings > Game)";
    if (!fs::is_regular_file(exePath, ec)) return "the game executable was not found: " + exe;
    fs::path wd = workingDir.empty() ? exePath.parent_path() : pathFromUtf8(workingDir);
    if (!fs::is_directory(wd, ec)) return "the working folder does not exist: " + pathToUtf8(wd);
    std::wstring cmd = L"\"" + exePath.wstring() + L"\"";
    if (!args.empty()) cmd += L" " + wide(args);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exePath.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE | CREATE_NEW_PROCESS_GROUP, nullptr,
                        wd.wstring().c_str(), &si, &pi))
        return "could not start the game (Windows error " + std::to_string(GetLastError()) + ")";
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {};
}

#else

bool nativeDialogsAvailable() { return false; }

NativeDialogResult showNativeDialog(const NativeDialogRequest&) {
    NativeDialogResult r;
    r.error = "native dialogs are not available on this platform";
    return r;
}

std::string launchProcess(const std::string& exe, const std::string& args, const std::string& workingDir) {
    std::error_code ec;
    if (exe.empty()) return "no game executable is set (Settings > Game)";
    if (!fs::is_regular_file(exe, ec)) return "the game executable was not found: " + exe;
    const std::string wd = workingDir.empty() ? fs::path(exe).parent_path().string() : workingDir;
    const std::string cmd = "cd '" + wd + "' && '" + exe + "' " + args + " >/dev/null 2>&1 &";
    return std::system(cmd.c_str()) == 0 ? std::string() : "could not start the game";
}

#endif

}  // namespace core
