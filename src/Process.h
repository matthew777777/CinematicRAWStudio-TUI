// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdio>
#include <string>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <cerrno>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>
#endif

namespace craw::tui::process {
// Windows commands go straight to CreateProcess, never through cmd.exe.
// POSIX commands use popen, with each user-supplied argument shell-quoted.
inline std::string quote(const std::string& s) {
#ifdef _WIN32
    std::string result = "\"";
    size_t slashes = 0;
    for (char c : s) {
        if (c == '\\') { ++slashes; continue; }
        if (c == '"') result.append(slashes * 2 + 1, '\\');
        else result.append(slashes, '\\');
        slashes = 0;
        result += c;
    }
    result.append(slashes * 2, '\\');
    return result + '"';
#else
    std::string result = "'";
    for (char c : s) result += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return result + "'";
#endif
}

#ifdef _WIN32
inline std::map<std::FILE*, HANDLE> children;
inline std::mutex childrenMutex;
inline std::wstring wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring result(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), result.data(), n);
    return result;
}
#endif

inline std::FILE* open(const std::string& command, bool write, const std::string& log = {}) {
#ifdef _WIN32
    std::lock_guard lock(childrenMutex); // Keep inheritable pipe handles private during process creation.
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return nullptr;
    const HANDLE parent = write ? writePipe : readPipe;
    const HANDLE child = write ? readPipe : writePipe;
    if (!SetHandleInformation(parent, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readPipe); CloseHandle(writePipe); return nullptr;
    }
    HANDLE nullFile = CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE errorFile = log.empty() ? nullFile : CreateFileW(wide(log).c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullFile == INVALID_HANDLE_VALUE || errorFile == INVALID_HANDLE_VALUE) {
        CloseHandle(readPipe); CloseHandle(writePipe);
        if (nullFile != INVALID_HANDLE_VALUE) CloseHandle(nullFile);
        if (!log.empty() && errorFile != INVALID_HANDLE_VALUE) CloseHandle(errorFile);
        return nullptr;
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof startup;
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = write ? child : nullFile;
    startup.hStdOutput = write ? nullFile : child;
    startup.hStdError = errorFile;
    PROCESS_INFORMATION process{};
    auto cmd = wide(command);
    const BOOL ok = !cmd.empty() && CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(child);
    CloseHandle(nullFile);
    if (!log.empty()) CloseHandle(errorFile);
    if (!ok) { CloseHandle(parent); return nullptr; }
    CloseHandle(process.hThread);
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(parent), _O_BINARY | (write ? _O_WRONLY : _O_RDONLY));
    std::FILE* stream = fd < 0 ? nullptr : _fdopen(fd, write ? "wb" : "rb");
    if (!stream) {
        if (fd >= 0) _close(fd); else CloseHandle(parent);
        TerminateProcess(process.hProcess, 1); CloseHandle(process.hProcess);
        return nullptr;
    }
    children.emplace(stream, process.hProcess);
    return stream;
#else
    const std::string cmd = command + " 2> " + (log.empty() ? "/dev/null" : quote(log));
    return ::popen(cmd.c_str(), write ? "w" : "r");
#endif
}

inline int close(std::FILE* stream) {
#ifdef _WIN32
    HANDLE child;
    {
        std::lock_guard lock(childrenMutex);
        const auto it = children.find(stream);
        if (it == children.end()) return -1;
        child = it->second;
        children.erase(it);
    }
    std::fclose(stream); // EOF before waiting for FFmpeg.
    WaitForSingleObject(child, INFINITE);
    DWORD status = 1;
    GetExitCodeProcess(child, &status);
    CloseHandle(child);
    return static_cast<int>(status);
#else
    return ::pclose(stream);
#endif
}
} // namespace craw::tui::process
