/*---------------------------------------------------------*\
||| WinUserActivity.cpp — see WinUserActivity.h (R-4).        ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "WinUserActivity.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>   /* SHQueryUserNotificationState */

namespace openrgb2
{
namespace platform
{

double UserIdleSeconds()
{
    LASTINPUTINFO lii = {};
    lii.cbSize = sizeof(lii);
    if(!GetLastInputInfo(&lii))
    {
        return -1.0;
    }
    /* Both are 32-bit tick counts: unsigned subtraction stays
       correct across the 49.7-day wrap. */
    const DWORD idle_ms = GetTickCount() - lii.dwTime;
    return double(idle_ms) / 1000.0;
}

bool FullscreenAppActive()
{
    QUERY_USER_NOTIFICATION_STATE state = QUNS_ACCEPTS_NOTIFICATIONS;
    if(FAILED(SHQueryUserNotificationState(&state)))
    {
        return false;
    }
    return state == QUNS_BUSY ||
           state == QUNS_RUNNING_D3D_FULL_SCREEN ||
           state == QUNS_PRESENTATION_MODE;
}

} /* namespace platform */
} /* namespace openrgb2 */
