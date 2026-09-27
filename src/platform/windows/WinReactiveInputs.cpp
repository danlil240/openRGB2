/*---------------------------------------------------------*\
||| WinReactiveInputs.cpp                                     ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#include "WinReactiveInputs.h"

namespace openrgb2
{
namespace platform
{

std::unique_ptr<IReactiveSource> MakeReactiveSource(
    const std::string& id)
{
    if(id == "audio")
        return MakeAudioLoopbackSource();
    if(id == "screen")
        return MakeScreenSamplerSource();
    if(id == "key")
        return MakeKeyHookSource();
    return nullptr;
}

} /* namespace platform */
} /* namespace openrgb2 */
