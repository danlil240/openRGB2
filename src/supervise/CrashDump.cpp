/*---------------------------------------------------------*\
|| CrashDump.cpp — see CrashDump.h.                          ||
||                                                           ||
||   Everything the crashing process does is preallocated  ||
||   at install time (directory + tag in fixed wide         ||
||   buffers): the filter only spawns the writer thread and ||
||   waits. No heap, no std::filesystem, no pruning after   ||
||   the fault — the heap may be what broke.                ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "CrashDump.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <system_error>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#include <csignal>
#include <cstdlib>
#include <cwchar>
#endif

namespace fs = std::filesystem;

namespace openrgb2
{
namespace supervise
{

std::string CrashDumpFileName(const std::string& tag,
                              const CrashTime& t,
                              unsigned long pid)
{
    char stamp[64];
    std::snprintf(stamp, sizeof(stamp),
                  "-%04d%02d%02dT%02d%02d%02dZ-%lu.dmp",
                  t.year, t.month, t.day, t.hour, t.minute, t.second,
                  pid);
    return tag + stamp;
}

namespace
{

/* "<tag>-<digit>...dmp" — the digit check keeps a tag that is
   a prefix of another ("openrgb2" vs "openrgb2-ui") from
   pruning the other's dumps. */
bool IsDumpOf(const fs::path& p, const std::string& tag)
{
    if(p.extension() != ".dmp")
    {
        return false;
    }
    const std::string n = p.filename().string();
    const std::string pre = tag + "-";
    return n.size() > pre.size() &&
           n.compare(0, pre.size(), pre) == 0 &&
           std::isdigit((unsigned char)n[pre.size()]);
}

} /* anonymous namespace */

size_t PruneCrashDumps(const std::string& dir, const std::string& tag,
                       size_t keep)
{
    struct Entry
    {
        fs::path            path;
        fs::file_time_type  when;
    };
    std::vector<Entry> found;
    std::error_code ec;
    for(fs::directory_iterator it(fs::u8path(dir), ec), end;
        !ec && it != end; it.increment(ec))
    {
        std::error_code fec;
        if(!it->is_regular_file(fec) || !IsDumpOf(it->path(), tag))
        {
            continue;
        }
        found.push_back({ it->path(), it->last_write_time(fec) });
    }
    if(found.size() <= keep)
    {
        return 0;
    }
    std::sort(found.begin(), found.end(),
              [](const Entry& a, const Entry& b)
              {
                  if(a.when != b.when) return a.when < b.when;
                  return a.path < b.path;
              });
    size_t removed = 0;
    for(size_t i = 0; i + keep < found.size(); ++i)
    {
        std::error_code rec;
        if(fs::remove(found[i].path, rec))
        {
            ++removed;
        }
    }
    return removed;
}

#ifdef _WIN32

namespace
{

/* Preallocated crash-time state. */
wchar_t            g_dir[1024]  = {};
wchar_t            g_tag[128]   = {};
std::string        g_dir_utf8;
volatile LONG      g_in_filter  = 0;
bool               g_installed  = false;

struct WriterArgs
{
    EXCEPTION_POINTERS* ep        = nullptr;
    DWORD               thread_id = 0;
};
WriterArgs g_writer;

constexpr DWORD kAbortException = 0xE0524941u;   /* 'R9' 'A' */

DWORD WINAPI WriterThread(LPVOID)
{
    SYSTEMTIME st;
    GetSystemTime(&st);
    wchar_t path[1400];
    const int n = std::swprintf(path, sizeof(path) / sizeof(path[0]),
        L"%ls\\%ls-%04u%02u%02uT%02u%02u%02uZ-%lu.dmp",
        g_dir, g_tag,
        (unsigned)st.wYear, (unsigned)st.wMonth, (unsigned)st.wDay,
        (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond,
        (unsigned long)GetCurrentProcessId());
    if(n <= 0)
    {
        return 1;
    }
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if(f == INVALID_HANDLE_VALUE)
    {
        return 2;
    }
    MINIDUMP_EXCEPTION_INFORMATION mei{};
    mei.ThreadId          = g_writer.thread_id;
    mei.ExceptionPointers = g_writer.ep;
    mei.ClientPointers    = FALSE;
    const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(
        MiniDumpWithThreadInfo |
        MiniDumpWithIndirectlyReferencedMemory);
    const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(),
        GetCurrentProcessId(), f, type,
        g_writer.ep ? &mei : nullptr, nullptr, nullptr);
    FlushFileBuffers(f);
    CloseHandle(f);
    if(!ok)
    {
        DeleteFileW(path);
        return 3;
    }
    return 0;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep)
{
    /* One dump per process: a fault inside the writer (or a
       second faulting thread) goes straight to termination. */
    if(InterlockedExchange(&g_in_filter, 1) != 0)
    {
        return EXCEPTION_EXECUTE_HANDLER;
    }
    g_writer.ep        = ep;
    g_writer.thread_id = GetCurrentThreadId();
    /* A helper thread has a full stack of its own — the
       faulting thread may have none left (stack overflow). */
    HANDLE t = CreateThread(nullptr, 256 * 1024, WriterThread, nullptr,
                            0, nullptr);
    if(t != nullptr)
    {
        WaitForSingleObject(t, 60000);
        CloseHandle(t);
    }
    /* End the process with the exception code: no WER dialog
       and no WerFault delay, so the supervisor sees the exit
       (non-zero) immediately. */
    return EXCEPTION_EXECUTE_HANDLER;
}

void RaiseForDump()
{
    RaiseException(kAbortException, EXCEPTION_NONCONTINUABLE, 0, nullptr);
}

void __cdecl OnAbortSignal(int)
{
    RaiseForDump();
}

void __cdecl OnPureCall()
{
    RaiseForDump();
}

void __cdecl OnInvalidParameter(const wchar_t*, const wchar_t*,
                                const wchar_t*, unsigned int, uintptr_t)
{
    RaiseForDump();
}

#pragma warning(push)
#pragma warning(disable : 4717)   /* recursive on all paths: intended */
__declspec(noinline) int Recurse(volatile int* p)
{
    volatile char buf[4096];
    buf[0] = (char)*p;
    buf[4095] = buf[0];
    /* Work after the call defeats tail-call elimination. */
    return Recurse(p) + buf[4095];
}
#pragma warning(pop)

bool CopyWide(const std::wstring& in, wchar_t* out, size_t cap)
{
    if(in.size() + 1 > cap)
    {
        return false;
    }
    std::wmemcpy(out, in.c_str(), in.size() + 1);
    return true;
}

} /* anonymous namespace */

std::string DefaultCrashDir()
{
    wchar_t env[1024];
    const DWORD en = GetEnvironmentVariableW(L"OPENRGB2_CRASH_DIR", env,
                                             (DWORD)(sizeof(env) /
                                                     sizeof(env[0])));
    if(en > 0 && en < sizeof(env) / sizeof(env[0]))
    {
        return fs::path(env).u8string();
    }
    wchar_t exe[1024];
    const DWORD n = GetModuleFileNameW(nullptr, exe,
                                       (DWORD)(sizeof(exe) /
                                               sizeof(exe[0])));
    if(n == 0 || n >= sizeof(exe) / sizeof(exe[0]))
    {
        return std::string();
    }
    return (fs::path(exe).parent_path() / L"state" / L"crash").u8string();
}

bool InstallCrashDumpHandler(const std::string& dir_in,
                             const std::string& tag,
                             size_t keep,
                             std::string* err)
{
    const std::string dir = dir_in.empty() ? DefaultCrashDir() : dir_in;
    if(dir.empty())
    {
        if(err) *err = "no crash directory";
        return false;
    }
    const fs::path p = fs::u8path(dir);
    std::error_code ec;
    fs::create_directories(p, ec);
    if(!fs::is_directory(p, ec))
    {
        if(err) *err = "cannot create crash directory " + dir;
        return false;
    }
    /* Leave room for this run's dump: at most `keep` after a
       crash, pruned here while the process is healthy. */
    if(keep > 0)
    {
        PruneCrashDumps(dir, tag, keep - 1);
    }
    const std::wstring wtag = fs::u8path(tag).wstring();
    if(!CopyWide(p.wstring(), g_dir, sizeof(g_dir) / sizeof(g_dir[0])) ||
       !CopyWide(wtag, g_tag, sizeof(g_tag) / sizeof(g_tag[0])))
    {
        if(err) *err = "crash directory path too long";
        return false;
    }
    g_dir_utf8 = dir;

    if(!g_installed)
    {
        g_installed = true;
        SetUnhandledExceptionFilter(CrashFilter);
        /* Room for the filter on a stack overflow. */
        ULONG guarantee = 64 * 1024;
        SetThreadStackGuarantee(&guarantee);
        /* abort() raises SIGABRT before it would report to WER;
           std::terminate ends in abort(). */
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        std::signal(SIGABRT, OnAbortSignal);
        _set_purecall_handler(OnPureCall);
        _set_invalid_parameter_handler(OnInvalidParameter);
    }
    return true;
}

std::string InstalledCrashDir()
{
    return g_dir_utf8;
}

void CrashForTest(const std::string& mode)
{
    if(mode == "av")
    {
        *(volatile int*)nullptr = 0x52394156;
    }
    else if(mode == "stack")
    {
        volatile int seed = 1;
        std::printf("%d\n", Recurse(&seed));
    }
    else if(mode == "abort")
    {
        std::abort();
    }
}

#else  /* !_WIN32 */

std::string DefaultCrashDir() { return std::string(); }
bool InstallCrashDumpHandler(const std::string&, const std::string&,
                             size_t, std::string* err)
{
    if(err) *err = "crash dumps are Windows-only";
    return false;
}
std::string InstalledCrashDir() { return std::string(); }
void CrashForTest(const std::string&) {}

#endif

} /* namespace supervise */
} /* namespace openrgb2 */
