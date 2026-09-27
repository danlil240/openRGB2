/*---------------------------------------------------------*\
|| SingleInstance.cpp                                        ||
||                                                           ||
||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "SingleInstance.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace openrgb2
{

SingleInstanceGuard::SingleInstanceGuard(std::string name)
    : name_(std::move(name))
{
}

SingleInstanceGuard::~SingleInstanceGuard()
{
    Release();
}

bool SingleInstanceGuard::Acquire(std::string& err)
{
    if(handle_ != nullptr)
    {
        return true;        /* already held — idempotent */
    }
#ifdef _WIN32
    HANDLE h = CreateMutexA(nullptr, FALSE, name_.c_str());
    if(h == nullptr)
    {
        err = "cannot create instance guard '" + name_ +
              "' (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    if(GetLastError() == ERROR_ALREADY_EXISTS)
    {
        CloseHandle(h);
        err = "another engine instance already owns '" + name_ +
              "' — refusing to double-own the managed "
              "configuration (section-4 single-owner rule)";
        return false;
    }
    handle_ = h;
    return true;
#else
    /* The engine is a Windows deliverable; on other platforms the
       guard is a honest no-op rather than a false sense of
       exclusivity. */
    (void)err;
    handle_ = this;
    return true;
#endif
}

void SingleInstanceGuard::Release()
{
#ifdef _WIN32
    if(handle_ != nullptr)
    {
        CloseHandle((HANDLE)handle_);
        handle_ = nullptr;
    }
#else
    handle_ = nullptr;
#endif
}

bool SingleInstanceGuard::Held() const
{
    return handle_ != nullptr;
}

} /* namespace openrgb2 */
