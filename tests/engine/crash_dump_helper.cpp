// SPDX-License-Identifier: GPL-2.0-or-later
//
// R-9 crash-dump stand-in: installs the shared crash handler
// exactly as openrgb2-engine / openrgb2-ui do, then crashes on
// purpose. crash_dump_test runs it as a child process and
// inspects the dump directory it leaves behind.
//
//   crash_dump_helper --crash-dir <dir> --tag <tag>
//                     --crash-for-test <av|stack|abort|none>

#include "CrashDump.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv)
{
    std::string dir;
    std::string tag  = "crash_dump_helper";
    std::string mode = "none";
    for(int i = 1; i < argc; ++i)
    {
        if(std::strcmp(argv[i], "--crash-dir") == 0 && i + 1 < argc)
            dir = argv[++i];
        else if(std::strcmp(argv[i], "--tag") == 0 && i + 1 < argc)
            tag = argv[++i];
        else if(std::strcmp(argv[i], "--crash-for-test") == 0 &&
                i + 1 < argc)
            mode = argv[++i];
    }
    std::string err;
    if(!openrgb2::supervise::InstallCrashDumpHandler(dir, tag, 5, &err))
    {
        std::fprintf(stderr, "install failed: %s\n", err.c_str());
        return 10;
    }
    if(mode == "none")
    {
        return 0;
    }
    openrgb2::supervise::CrashForTest(mode);
    return 11;  /* unreachable unless the mode is unknown */
}
