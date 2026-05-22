// PlatformSpawn.cpp — POSIX (posix_spawn) + Windows (CreateProcessA)
// implementations, selected at compile time. Single TU; the unused
// branch compiles to nothing on the wrong platform.

#include "PlatformSpawn.h"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#if defined(_WIN32)
#    define WIN32_LEAN_AND_MEAN
#    include <windows.h>
#elif defined(__APPLE__)
#    include <mach-o/dyld.h>
#    include <spawn.h>
#    include <sys/types.h>
#    include <unistd.h>
extern "C" char** environ;
#else  // Linux / generic POSIX
#    include <spawn.h>
#    include <sys/types.h>
#    include <unistd.h>
extern "C" char** environ;
#endif

namespace ocs::spawn
{

std::string selfExecutablePath()
{
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    DWORD n = ::GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (n == 0 || n >= sizeof(buf)) return {};
    return std::string(buf, n);
#elif defined(__APPLE__)
    char buf[PATH_MAX] = {};
    uint32_t len = sizeof(buf);
    if (_NSGetExecutablePath(buf, &len) != 0) return {};
    return std::string(buf);
#else
    char buf[PATH_MAX] = {};
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    return std::string(buf, static_cast<std::size_t>(n));
#endif
}

#if defined(_WIN32)
// Quote a single argv element per the CommandLineToArgvW rules:
// backslashes preceding a quote get doubled; embedded quotes escape;
// element wrapped in quotes if it contains whitespace or is empty.
static std::string quoteWin32Arg(const std::string& arg)
{
    if (!arg.empty() &&
        arg.find_first_of(" \t\n\v\"") == std::string::npos)
        return arg;

    std::string out;
    out.push_back('"');
    for (std::size_t i = 0; i < arg.size(); ++i)
    {
        std::size_t bs = 0;
        while (i < arg.size() && arg[i] == '\\') { ++bs; ++i; }
        if (i == arg.size())
        {
            out.append(bs * 2, '\\');  // trailing backslashes: double
            break;
        }
        if (arg[i] == '"')
        {
            out.append(bs * 2 + 1, '\\');
            out.push_back('"');
        }
        else
        {
            out.append(bs, '\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back('"');
    return out;
}
#endif

int launchDetached(const std::string& exePath,
                   const std::vector<std::string>& argv)
{
    if (exePath.empty()) return 0;

#if defined(_WIN32)
    std::string cmdline = quoteWin32Arg(exePath);
    for (std::size_t i = 1; i < argv.size(); ++i)
    {
        cmdline.push_back(' ');
        cmdline.append(quoteWin32Arg(argv[i]));
    }
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    // CreateProcessA's lpCommandLine must be writable.
    std::vector<char> cmdbuf(cmdline.begin(), cmdline.end());
    cmdbuf.push_back('\0');
    BOOL ok = ::CreateProcessA(
        exePath.c_str(),
        cmdbuf.data(),
        nullptr, nullptr, FALSE,
        0,           // no special flags; detached not strictly needed
        nullptr,     // inherit environment
        nullptr,
        &si, &pi);
    if (!ok) return 0;
    const int pid = static_cast<int>(pi.dwProcessId);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return pid;
#else
    // posix_spawn — argv is null-terminated char* array. Build a
    // writable vector pointing at each std::string's data.
    std::vector<char*> ptrs;
    ptrs.reserve(argv.size() + 1);
    for (auto& s : argv)
        ptrs.push_back(const_cast<char*>(s.c_str()));
    ptrs.push_back(nullptr);
    pid_t pid = 0;
    const int r = ::posix_spawn(&pid, exePath.c_str(),
                                nullptr, nullptr,
                                ptrs.data(), environ);
    if (r != 0) return 0;
    return static_cast<int>(pid);
#endif
}

}  // namespace ocs::spawn
