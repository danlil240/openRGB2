/*---------------------------------------------------------*\
|| CrashDump.h                                               ||
||                                                           ||
||   R-9: in-process crash dumps for openrgb2-engine and    ||
||   openrgb2-ui (Qt-free, engine-free — the UI links it    ||
||   without carrying the engine/SDK boundary).             ||
||                                                           ||
||   InstallCrashDumpHandler is called first thing in main: ||
||     - creates the crash dir and prunes it to keep-1 dumps ||
||       of this tag (so the next crash leaves at most      ||
||       `keep`; nothing is pruned inside the crashing      ||
||       process, whose heap may be corrupt)                ||
||     - SetUnhandledExceptionFilter -> MiniDumpWriteDump   ||
||       (MiniDumpWithThreadInfo |                          ||
||        MiniDumpWithIndirectlyReferencedMemory) into      ||
||       <dir>\<tag>-<yyyymmddThhmmssZ>-<pid>.dmp, written ||
||       from a helper thread so a stack overflow still     ||
||       dumps; the filter then ends the process with the   ||
||       exception code (no WER dialog, no delay — the      ||
||       supervisor sees a non-zero exit at once)           ||
||     - SetThreadStackGuarantee on the calling thread      ||
||     - abort(), std::terminate, pure-virtual calls and    ||
||       CRT invalid-parameter reports are routed into the  ||
||       same filter                                        ||
||   Not covered: __fastfail (/GS, CFG) bypasses every      ||
||   in-process handler — only WER sees those.             ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <string>

namespace openrgb2
{
namespace supervise
{

/* UTC wall-clock parts for the dump file name. */
struct CrashTime
{
    int year   = 0;
    int month  = 0;
    int day    = 0;
    int hour   = 0;
    int minute = 0;
    int second = 0;
};

/* <tag>-<yyyymmddThhmmssZ>-<pid>.dmp */
std::string CrashDumpFileName(const std::string& tag,
                              const CrashTime& utc,
                              unsigned long pid);

/* Deletes the oldest (by last-write time) "<tag>-*.dmp"
   files in `dir` until at most `keep` remain. Other tags and
   other files are never touched. Returns the number deleted. */
size_t PruneCrashDumps(const std::string& dir, const std::string& tag,
                       size_t keep);

/* The default crash directory: OPENRGB2_CRASH_DIR when set,
   else <directory of this executable>\state\crash — in the
   installed layout that is <install>\state\crash, the
   user-writable state tree update/rollback preserve. */
std::string DefaultCrashDir();

/* See the header comment. `dir` empty -> DefaultCrashDir().
   Idempotent per process (a second call retargets the dir).
   False + err when the dir cannot be created — the caller
   logs and runs on without dumps. */
bool InstallCrashDumpHandler(const std::string& dir,
                             const std::string& tag,
                             size_t keep,
                             std::string* err);

/* The directory the installed handler writes to ("" before
   InstallCrashDumpHandler). */
std::string InstalledCrashDir();

/* Test/diagnostic hook: crash on purpose. "av" = access
   violation, "stack" = stack overflow, "abort" = abort().
   Returns only for an unknown mode. */
void CrashForTest(const std::string& mode);

} /* namespace supervise */
} /* namespace openrgb2 */
