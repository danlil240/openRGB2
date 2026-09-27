/*---------------------------------------------------------*\
|| SingleInstance.h                                          ||
||                                                           ||
||   The engine's single-owner guard (plan section 4,      ||
||   process ownership: "Test the fork's single-instance   ||
||   policy ... duplicate hardware owners are never        ||
||   valid"). Exactly ONE engine process may own the        ||
||   managed configuration at a time — a second engine      ||
||   would be a second scene writer on the same hardware,   ||
||   which the section-4 ownership rules forbid.            ||
||                                                           ||
||   The guard is a named kernel object (Windows) — it is   ||
||   released automatically by the OS if the owner dies, so||
||   a crashed engine never wedges the next start, and     ||
||   there is no stale lock-file to misread. The name is   ||
||   injectable so the offline suite exercises collision   ||
||   handling without touching the production name.        ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <string>

namespace openrgb2
{

/* The production instance name. Local\ scope binds the guard to
   the interactive session — two different logged-on users do not
   share a hardware-ownership claim, and a service-session engine
   is a separate owner by design (plan section 4's single-owner
   rule applies per managed configuration/session). */
constexpr const char* kEngineInstanceName = "Local\\openrgb2-engine";

class SingleInstanceGuard
{
public:
    explicit SingleInstanceGuard(std::string name = kEngineInstanceName);
    ~SingleInstanceGuard();                                  /* Release() */

    SingleInstanceGuard(const SingleInstanceGuard&)            = delete;
    SingleInstanceGuard& operator=(const SingleInstanceGuard&) = delete;

    /* Acquires the ownership claim. Returns false + err when the
       name is already owned (another live engine instance) — the
       caller reports the conflict and refuses to double-own;
       it NEVER attempts to take over or kill the holder. */
    bool Acquire(std::string& err);
    void Release();
    bool Held() const;

private:
    std::string name_;
    void*       handle_ = nullptr;    /* Win32 HANDLE, hidden */
};

} /* namespace openrgb2 */
