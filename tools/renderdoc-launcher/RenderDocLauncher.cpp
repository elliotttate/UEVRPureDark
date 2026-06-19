#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct Options {
    std::wstring exe;
    std::wstring args;
    std::wstring cwd;
    std::wstring backend;
    std::wstring renderdoc;
    DWORD ready_timeout_ms{30000};
    DWORD defer_backend_ms{0};
    bool wait{};
    bool inject_renderdoc_first{true};
    bool allow_vr_runtime{};
    bool allow_openvr{};
};

std::wstring quote_arg(const std::wstring& arg) {
    if (arg.empty()) {
        return L"\"\"";
    }
    if (arg.find_first_of(L" \t\"") == std::wstring::npos) {
        return arg;
    }

    std::wstring out{L"\""};
    size_t backslashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++backslashes;
        } else if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(c);
            backslashes = 0;
        } else {
            out.append(backslashes, L'\\');
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::filesystem::path launcher_dir() {
    wchar_t path[MAX_PATH * 2]{};
    const DWORD len = GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    if (len == 0 || len >= std::size(path)) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path{path}.parent_path();
}

std::filesystem::path absolute_existing_or_default(const std::wstring& value, const std::filesystem::path& fallback) {
    if (!value.empty()) {
        return std::filesystem::absolute(std::filesystem::path{value});
    }
    return fallback;
}

bool exists_file(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::exists(path, ec) && std::filesystem::is_regular_file(path, ec);
}

void usage() {
    std::wcerr
        << L"Usage:\n"
        << L"  UEVRRenderDocLauncher.exe --exe <game.exe> [--args \"...\"] [--cwd <dir>]\n"
        << L"      [--backend <UEVRBackend.dll>] [--renderdoc <renderdoc.dll>]\n"
        << L"      [--ready-timeout-ms <ms>] [--defer-backend-ms <ms>]\n"
        << L"      [--backend-load-renderdoc] [--allow-vr-runtime]\n"
        << L"      [--allow-openvr] [--wait]\n\n"
        << L"Examples:\n"
        << L"  UEVRRenderDocLauncher.exe --exe \"C:\\Games\\Game\\Game.exe\" --args \"-dx12\"\n"
        << L"  UEVRRenderDocLauncher.exe --exe \"C:\\Games\\Game.exe\" -- --dx12 -log\n";
}

bool parse_args(int argc, wchar_t** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        auto need_value = [&](const wchar_t* name) -> std::wstring {
            if (i + 1 >= argc) {
                std::wcerr << L"Missing value for " << name << L"\n";
                return {};
            }
            return argv[++i];
        };

        if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            usage();
            ExitProcess(0);
        } else if (arg == L"--exe") {
            options.exe = need_value(L"--exe");
        } else if (arg == L"--args") {
            options.args = need_value(L"--args");
        } else if (arg == L"--cwd") {
            options.cwd = need_value(L"--cwd");
        } else if (arg == L"--backend") {
            options.backend = need_value(L"--backend");
        } else if (arg == L"--renderdoc") {
            options.renderdoc = need_value(L"--renderdoc");
        } else if (arg == L"--ready-timeout-ms") {
            const auto value = need_value(L"--ready-timeout-ms");
            try {
                options.ready_timeout_ms = static_cast<DWORD>(std::stoul(value));
            } catch (...) {
                std::wcerr << L"Invalid --ready-timeout-ms value: " << value << L"\n";
                return false;
            }
        } else if (arg == L"--defer-backend-ms") {
            const auto value = need_value(L"--defer-backend-ms");
            try {
                options.defer_backend_ms = static_cast<DWORD>(std::stoul(value));
            } catch (...) {
                std::wcerr << L"Invalid --defer-backend-ms value: " << value << L"\n";
                return false;
            }
        } else if (arg == L"--wait") {
            options.wait = true;
        } else if (arg == L"--inject-renderdoc-first") {
            options.inject_renderdoc_first = true;
        } else if (arg == L"--backend-load-renderdoc") {
            options.inject_renderdoc_first = false;
        } else if (arg == L"--allow-vr-runtime") {
            options.allow_vr_runtime = true;
        } else if (arg == L"--allow-openvr") {
            options.allow_openvr = true;
        } else if (arg == L"--") {
            std::wstring tail;
            for (++i; i < argc; ++i) {
                if (!tail.empty()) {
                    tail.push_back(L' ');
                }
                tail += quote_arg(argv[i]);
            }
            options.args = std::move(tail);
            break;
        } else {
            std::wcerr << L"Unknown option: " << arg << L"\n";
            return false;
        }
    }

    return !options.exe.empty();
}

bool inject_dll(HANDLE process, const std::filesystem::path& dll_path, const wchar_t* label) {
    const std::wstring path = dll_path.wstring();
    const SIZE_T bytes = (path.size() + 1) * sizeof(wchar_t);

    void* remote = VirtualAllocEx(process, nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (remote == nullptr) {
        std::wcerr << L"VirtualAllocEx failed for " << label << L" err=" << GetLastError() << L"\n";
        return false;
    }

    bool ok = false;
    SIZE_T written{};
    if (!WriteProcessMemory(process, remote, path.c_str(), bytes, &written) || written != bytes) {
        std::wcerr << L"WriteProcessMemory failed for " << label << L" err=" << GetLastError() << L"\n";
    } else {
        auto* kernel32 = GetModuleHandleW(L"kernel32.dll");
        auto* load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(GetProcAddress(kernel32, "LoadLibraryW"));
        HANDLE thread = CreateRemoteThread(process, nullptr, 0, load_library, remote, 0, nullptr);
        if (thread == nullptr) {
            std::wcerr << L"CreateRemoteThread failed for " << label << L" err=" << GetLastError() << L"\n";
        } else {
            WaitForSingleObject(thread, INFINITE);
            DWORD exit_code{};
            if (GetExitCodeThread(thread, &exit_code) && exit_code != 0) {
                ok = true;
            } else {
                std::wcerr << L"Remote LoadLibraryW failed for " << label << L" err=" << GetLastError() << L"\n";
            }
            CloseHandle(thread);
        }
    }

    VirtualFreeEx(process, remote, 0, MEM_RELEASE);
    return ok;
}

std::wstring build_command_line(const Options& options) {
    std::wstring command = quote_arg(options.exe);
    if (!options.args.empty()) {
        command.push_back(L' ');
        command += options.args;
    }
    return command;
}

std::wstring ready_event_name() {
    return L"Local\\UEVRRenderDocReady_" +
           std::to_wstring(GetCurrentProcessId()) + L"_" +
           std::to_wstring(GetTickCount64());
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    Options options{};
    if (!parse_args(argc, argv, options)) {
        usage();
        return 2;
    }

    const auto dir = launcher_dir();
    auto backend = absolute_existing_or_default(options.backend, dir / L"UEVRBackend.dll");
    auto renderdoc = absolute_existing_or_default(options.renderdoc, dir / L"renderdoc.dll");
    if (!exists_file(renderdoc)) {
        const auto source_tree_renderdoc = std::filesystem::path{L"E:\\Github\\renderdoc\\x64\\Development\\renderdoc.dll"};
        if (exists_file(source_tree_renderdoc)) {
            renderdoc = source_tree_renderdoc;
        }
    }

    const auto exe = std::filesystem::absolute(std::filesystem::path{options.exe});
    const auto cwd = options.cwd.empty()
        ? exe.parent_path()
        : std::filesystem::absolute(std::filesystem::path{options.cwd});

    if (!exists_file(exe)) {
        std::wcerr << L"Game executable not found: " << exe.wstring() << L"\n";
        return 2;
    }
    if (!exists_file(renderdoc)) {
        std::wcerr << L"renderdoc.dll not found: " << renderdoc.wstring() << L"\n";
        return 2;
    }
    if (!exists_file(backend)) {
        std::wcerr << L"UEVRBackend.dll not found: " << backend.wstring() << L"\n";
        return 2;
    }

    const auto ready_name = ready_event_name();
    HANDLE ready_event = CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str());
    if (ready_event == nullptr) {
        std::wcerr << L"CreateEventW failed err=" << GetLastError() << L"\n";
        return 1;
    }

    SetEnvironmentVariableW(L"UEVR_RENDERDOC_BOOTSTRAP", L"1");
    SetEnvironmentVariableW(L"UEVR_RENDERDOC_TRACK_ACTIVE_PAIR", L"1");
    SetEnvironmentVariableW(L"UEVR_RENDERDOC_STRICT_ORIGINALS", L"1");
    SetEnvironmentVariableW(L"UEVR_RENDERDOC_DXGI_FACTORY_PROOF", L"0");
    SetEnvironmentVariableW(L"UEVR_RENDERDOC_PREHOOK_D3D12", options.defer_backend_ms > 0 ? L"0" : L"1");
    SetEnvironmentVariableW(L"UEVR_RENDERDOC_LAUNCHED_SUSPENDED", options.defer_backend_ms > 0 ? L"0" : L"1");
    SetEnvironmentVariableW(L"UEVR_RENDERDOC_READY_EVENT", options.defer_backend_ms > 0 ? L"" : ready_name.c_str());
    SetEnvironmentVariableW(L"UEVR_RENDERDOC_DLL", renderdoc.wstring().c_str());
    SetEnvironmentVariableW(L"UEVR_LOAD_RENDERDOC_DLL", options.inject_renderdoc_first ? L"0" : L"1");
    SetEnvironmentVariableW(L"UEVR_DISABLE_PIX_BOOTSTRAP", L"1");
    SetEnvironmentVariableW(L"UEVR_RENDERDOC_CAPTURE_ONLY", options.allow_vr_runtime ? L"0" : L"1");
    SetEnvironmentVariableW(L"UEVR_RENDERDOC_SKIP_OPENVR", options.allow_openvr ? L"0" : L"1");

    std::wstring command_line = build_command_line(options);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};

    if (!CreateProcessW(
            exe.wstring().c_str(),
            command_line.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_SUSPENDED,
            nullptr,
            cwd.wstring().c_str(),
            &startup,
            &process)) {
        std::wcerr << L"CreateProcessW failed err=" << GetLastError() << L"\n";
        CloseHandle(ready_event);
        return 1;
    }

    std::wcout << L"Created suspended process pid=" << process.dwProcessId << L"\n";
    if (options.inject_renderdoc_first) {
        std::wcout << L"Injecting RenderDoc first: " << renderdoc.wstring() << L"\n";
        if (!inject_dll(process.hProcess, renderdoc, L"renderdoc.dll")) {
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(ready_event);
            return 1;
        }
    } else {
        std::wcout << L"UEVR backend will load RenderDoc before signaling ready: "
                   << renderdoc.wstring() << L"\n";
    }

    if (options.defer_backend_ms > 0) {
        ResumeThread(process.hThread);
        std::wcout << L"Resumed process with RenderDoc resident only. Deferring UEVR backend injection for "
                   << options.defer_backend_ms << L" ms.\n";

        Sleep(options.defer_backend_ms);

        DWORD live_exit_code{};
        if (GetExitCodeProcess(process.hProcess, &live_exit_code) && live_exit_code != STILL_ACTIVE) {
            std::wcerr << L"Game exited before deferred UEVR injection, exit_code=" << live_exit_code << L"\n";
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(ready_event);
            return 1;
        }

        std::wcout << L"Injecting deferred UEVR backend: " << backend.wstring() << L"\n";
        if (!inject_dll(process.hProcess, backend, L"UEVRBackend.dll")) {
            TerminateProcess(process.hProcess, 1);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
            CloseHandle(ready_event);
            return 1;
        }

        std::wcout << L"Deferred UEVR backend injected. RenderDoc was resident before the game main thread ran.\n";

        DWORD exit_code = 0;
        if (options.wait) {
            WaitForSingleObject(process.hProcess, INFINITE);
            GetExitCodeProcess(process.hProcess, &exit_code);
        }

        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(ready_event);
        return static_cast<int>(exit_code);
    }

    std::wcout << L"Injecting UEVR backend: " << backend.wstring() << L"\n";
    if (!inject_dll(process.hProcess, backend, L"UEVRBackend.dll")) {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(ready_event);
        return 1;
    }

    std::wcout << L"Waiting for UEVR early RenderDoc/D3D12 ready event: " << ready_name << L"\n";
    const DWORD ready_wait = WaitForSingleObject(ready_event, options.ready_timeout_ms);
    if (ready_wait != WAIT_OBJECT_0) {
        std::wcerr << L"Timed out waiting for UEVR ready event after "
                   << options.ready_timeout_ms << L" ms\n";
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        CloseHandle(ready_event);
        return 1;
    }

    ResumeThread(process.hThread);
    std::wcout << L"Resumed process. UEVR/RenderDoc were resident before the game main thread ran.\n";

    DWORD exit_code = 0;
    if (options.wait) {
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, &exit_code);
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    CloseHandle(ready_event);
    return static_cast<int>(exit_code);
}
