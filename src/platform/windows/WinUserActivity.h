/*---------------------------------------------------------*\
||| WinUserActivity.h                                         ||
|||                                                           ||
|||   R-4: the production sources behind user-idle dimming. ||
|||   Both are plain polls on the owner loop — no thread, no ||
|||   hook. They work because the engine runs in the         ||
|||   interactive session (packaging/windows/RELEASE.md).    ||
|||                                                           ||
|||   - UserIdleSeconds: GetLastInputInfo — the session-wide||
|||     last keyboard/mouse input. Gamepad input probably    ||
|||     does not move it (unverified), which is why the idle ||
|||     rule skips fullscreen apps. < 0 when the call fails. ||
|||   - FullscreenAppActive: SHQueryUserNotificationState —  ||
|||     QUNS_BUSY (a fullscreen app), QUNS_RUNNING_D3D_FULL_ ||
|||     SCREEN, QUNS_PRESENTATION_MODE count as fullscreen.  ||
|||                                                           ||
|||   Platform code: not loaded by the offline suites (they  ||
|||   inject EngineHostOptions::idle_seconds / fullscreen).  ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

namespace openrgb2
{
namespace platform
{

double UserIdleSeconds();
bool   FullscreenAppActive();

} /* namespace platform */
} /* namespace openrgb2 */
