/*---------------------------------------------------------*\
||| WinReactiveInputs.h                                       ||
|||                                                           ||
|||   Factory for the Windows reactive capture sources     ||
|||   (Task 4.6): WASAPI loopback audio, GDI screen        ||
|||   sampler, WH_KEYBOARD_LL key hook — Qt-free ports of  ||
|||   the preserved plugin's inputs                        ||
|||   (openrgb-unified/plugins/DesktopLightingStudio/      ||
|||   inputs/). Each returns an IReactiveSource the engine  ||
|||   ReactiveInputs manager start/stops per user toggle.  ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include "ReactiveInputs.h"

#include <memory>
#include <string>
#include <vector>

namespace openrgb2
{
namespace platform
{

/* The capture source for one input id ("audio", "screen",
   "key") — nullptr for an unknown id. On non-Windows builds
   every factory returns a source that reports
   "unsupported platform" on Start. */
std::unique_ptr<IReactiveSource> MakeAudioLoopbackSource();
std::unique_ptr<IReactiveSource> MakeScreenSamplerSource();
std::unique_ptr<IReactiveSource> MakeKeyHookSource();
std::unique_ptr<IReactiveSource> MakeReactiveSource(
    const std::string& id);

/* Enumerated displays for the screen-source selector —
   index order matches the sampler's SetScreenIndex. */
std::vector<ReactiveDisplay>     EnumerateDisplays();

} /* namespace platform */
} /* namespace openrgb2 */
