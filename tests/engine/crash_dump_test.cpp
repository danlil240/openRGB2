// SPDX-License-Identifier: GPL-2.0-or-later
//
// R-9 crash dumps: the shared in-process handler
// (src/supervise/CrashDump) that openrgb2-engine and
// openrgb2-ui install first thing. Drives REAL crashes in a
// stand-in child process (crash_dump_helper) and checks:
//
//   - an access violation, a stack overflow and abort() each
//     leave one minidump (MDMP signature) in the crash dir and
//     the process exits non-zero (the supervisor's restart cue)
//   - the directory keeps the newest 5 dumps per tag: startup
//     prunes to 4, the crash adds the 5th; another tag's dumps
//     are never touched
//   - a clean run leaves no dump
//   - the file name is <tag>-<yyyymmddThhmmssZ>-<pid>.dmp
//
// Usage: crash_dump_test <path-to-crash_dump_helper.exe>

#include "CrashDump.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

static std::string g_helper;

static DWORD RunHelper(const fs::path& dir, const std::string& mode)
{
    std::wstring cmd = L"\"" + fs::path(g_helper).wstring() + L"\"" +
        L" --crash-dir \"" + dir.wstring() + L"\"" +
        L" --tag crash_dump_helper --crash-for-test " +
        fs::path(mode).wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if(!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        std::printf("CreateProcess failed: %lu\n", GetLastError());
        return 0xFFFFFFFFu;
    }
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0xFFFFFFFFu;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

static std::vector<fs::path> Dumps(const fs::path& dir,
                                   const std::string& tag)
{
    std::vector<fs::path> out;
    std::error_code ec;
    for(const auto& e : fs::directory_iterator(dir, ec))
    {
        const std::string n = e.path().filename().string();
        if(n.rfind(tag + "-", 0) == 0 && e.path().extension() == ".dmp")
            out.push_back(e.path());
    }
    std::sort(out.begin(), out.end());
    return out;
}

static bool IsMinidump(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    char sig[4] = {};
    f.read(sig, 4);
    std::error_code ec;
    return f.gcount() == 4 && sig[0] == 'M' && sig[1] == 'D' &&
           sig[2] == 'M' && sig[3] == 'P' &&
           fs::file_size(p, ec) > 1024;
}

static fs::path FreshDir(const fs::path& root, const char* name)
{
    const fs::path d = root / name;
    std::error_code ec;
    fs::remove_all(d, ec);
    fs::create_directories(d, ec);
    return d;
}

static void Touch(const fs::path& p, int age_minutes)
{
    std::ofstream(p, std::ios::binary) << "old";
    std::error_code ec;
    fs::last_write_time(p, fs::file_time_type::clock::now() -
                               std::chrono::minutes(age_minutes), ec);
}

int main(int argc, char** argv)
{
    if(argc < 2)
    {
        std::printf("usage: crash_dump_test <crash_dump_helper.exe>\n");
        return 2;
    }
    g_helper = argv[1];
    using namespace openrgb2::supervise;

    const fs::path root = fs::temp_directory_path() /
        ("openrgb2-crash-dump-test-" +
         std::to_string(GetCurrentProcessId()));

    /*---- file name shape ------------------------------------*/
    {
        const CrashTime st{ 2026, 11, 28, 9, 5, 7 };
        CHECK(CrashDumpFileName("openrgb2-engine", st, 4242) ==
              "openrgb2-engine-20261128T090507Z-4242.dmp");
    }

    /*---- pruning keeps the newest N of ONE tag --------------*/
    {
        const fs::path d = FreshDir(root, "prune");
        for(int i = 0; i < 7; ++i)
            Touch(d / ("t-2020010" + std::to_string(i) + "T000000Z-1.dmp"),
                  100 - i);
        Touch(d / "other-20200101T000000Z-1.dmp", 1000);
        Touch(d / "t-notes.txt", 1000);
        CHECK(PruneCrashDumps(d.string(), "t", 3) == 4);
        const auto left = Dumps(d, "t");
        CHECK(left.size() == 3);
        CHECK(!left.empty() &&
              left.back().filename() == "t-20200106T000000Z-1.dmp");
        CHECK(fs::exists(d / "other-20200101T000000Z-1.dmp"));
        CHECK(fs::exists(d / "t-notes.txt"));
    }

    /*---- clean run: handler installed, no dump --------------*/
    {
        const fs::path d = root / "clean" / "nested";
        std::error_code ec;
        fs::remove_all(root / "clean", ec);
        CHECK(RunHelper(d, "none") == 0);
        CHECK(fs::is_directory(d));          /* created on install */
        CHECK(Dumps(d, "crash_dump_helper").empty());
    }

    /*---- access violation + keep-5 pruning ------------------*/
    {
        const fs::path d = FreshDir(root, "av");
        for(int i = 0; i < 6; ++i)
            Touch(d / ("crash_dump_helper-2020010" + std::to_string(i) +
                       "T000000Z-1.dmp"), 100 - i);
        Touch(d / "openrgb2-ui-20200101T000000Z-1.dmp", 1000);
        const DWORD code = RunHelper(d, "av");
        CHECK(code == (DWORD)EXCEPTION_ACCESS_VIOLATION);
        const auto dumps = Dumps(d, "crash_dump_helper");
        CHECK(dumps.size() == 5);
        /* The two oldest stand-ins are gone, the real dump is in. */
        CHECK(!fs::exists(d / "crash_dump_helper-20200100T000000Z-1.dmp"));
        CHECK(!fs::exists(d / "crash_dump_helper-20200101T000000Z-1.dmp"));
        size_t real = 0;
        for(const auto& p : dumps)
            if(IsMinidump(p)) ++real;
        CHECK(real == 1);
        CHECK(fs::exists(d / "openrgb2-ui-20200101T000000Z-1.dmp"));
    }

    /*---- stack overflow still dumps -------------------------*/
    {
        const fs::path d = FreshDir(root, "stack");
        const DWORD code = RunHelper(d, "stack");
        CHECK(code == (DWORD)EXCEPTION_STACK_OVERFLOW);
        const auto dumps = Dumps(d, "crash_dump_helper");
        CHECK(dumps.size() == 1);
        CHECK(!dumps.empty() && IsMinidump(dumps[0]));
    }

    /*---- abort() (std::terminate path) dumps ----------------*/
    {
        const fs::path d = FreshDir(root, "abort");
        const DWORD code = RunHelper(d, "abort");
        CHECK(code != 0);
        const auto dumps = Dumps(d, "crash_dump_helper");
        CHECK(dumps.size() == 1);
        CHECK(!dumps.empty() && IsMinidump(dumps[0]));
    }

    std::error_code ec;
    fs::remove_all(root, ec);
    std::printf("crash_dump_test: %d checks, %d failures\n",
                checks, failures);
    return failures == 0 ? 0 : 1;
}
