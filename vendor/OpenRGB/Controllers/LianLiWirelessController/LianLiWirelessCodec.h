/*---------------------------------------------------------*\
| LianLiWirelessCodec.h                                     |
|                                                           |
|   Memory-to-memory wrapper around tinyuz (MIT,            |
|   sisong/tinyuz — vendored at vendor/tinyuz). The fan     |
|   firmware decompresses RGB frame data with this codec;   |
|   it replaces the proprietary yuz.dll used by L-Connect.  |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace LianLiWirelessCodec
{
    /*---------------------------------------------------------*\
    | Compress using tinyuz with the vendor's 4KB dictionary.   |
    | Throws std::runtime_error on failure.                     |
    \*---------------------------------------------------------*/
    std::vector<uint8_t> Compress(const uint8_t* input, size_t len);

    /*---------------------------------------------------------*\
    | Decompress. Used by tests for independent decode          |
    | validation — the driver itself never decodes.             |
    | Returns false on malformed input or wrong output size.    |
    \*---------------------------------------------------------*/
    bool Decompress(const uint8_t* input, size_t input_len,
                    uint8_t* output, size_t output_len);

    size_t MaxCompressedSize(size_t input_len);
}
