/*---------------------------------------------------------*\
| LianLiWirelessCodec.cpp                                   |
|                                                           |
|   tinyuz wrapper — memory streams around tuz_compress /   |
|   tuz_decompress_mem. Replaces yuz.dll (same codec).      |
|                                                           |
|   This file is part of the OpenRGB project                |
|   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#include "LianLiWirelessCodec.h"
#include <cstring>
#include <stdexcept>

extern "C"
{
#include "compress/tuz_enc.h"
#include "decompress/tuz_dec.h"
}

namespace
{
    constexpr size_t DICT_SIZE = 4096;   /* vendor uploads use a 4KB dict */

    struct MemInput
    {
        const uint8_t* data;
        size_t         size;
    };

    struct MemOutput
    {
        uint8_t* buf;
        size_t   capacity;
        size_t   written;
    };

    hpatch_BOOL mem_read(const hpatch_TStreamInput* stream,
                         hpatch_StreamPos_t readFromPos,
                         unsigned char* out_data,
                         unsigned char* out_data_end)
    {
        const MemInput* ms = (const MemInput*)stream->streamImport;
        size_t len = (size_t)(out_data_end - out_data);
        if(readFromPos + len > ms->size)
        {
            return hpatch_FALSE;
        }
        memcpy(out_data, ms->data + readFromPos, len);
        return hpatch_TRUE;
    }

    hpatch_BOOL mem_write(const hpatch_TStreamOutput* stream,
                          hpatch_StreamPos_t writeToPos,
                          const unsigned char* data,
                          const unsigned char* data_end)
    {
        MemOutput* ms = (MemOutput*)stream->streamImport;
        size_t len = (size_t)(data_end - data);
        if(writeToPos + len > ms->capacity)
        {
            return hpatch_FALSE;
        }
        memcpy(ms->buf + writeToPos, data, len);
        if(writeToPos + len > ms->written)
        {
            ms->written = writeToPos + len;
        }
        return hpatch_TRUE;
    }
}

size_t LianLiWirelessCodec::MaxCompressedSize(size_t input_len)
{
    return (size_t)tuz_maxCompressedSize(input_len);
}

std::vector<uint8_t> LianLiWirelessCodec::Compress(const uint8_t* input, size_t len)
{
    if(input == nullptr || len == 0)
    {
        throw std::invalid_argument("tinyuz: cannot compress empty input");
    }

    MemInput  in_mem  = { input, len };
    MemOutput out_mem = { nullptr, 0, 0 };

    std::vector<uint8_t> output(MaxCompressedSize(len));
    out_mem.buf      = output.data();
    out_mem.capacity = output.size();

    hpatch_TStreamInput in_stream;
    memset(&in_stream, 0, sizeof(in_stream));
    in_stream.streamImport = (void*)&in_mem;
    in_stream.streamSize   = len;
    in_stream.read         = mem_read;

    hpatch_TStreamOutput out_stream;
    memset(&out_stream, 0, sizeof(out_stream));
    out_stream.streamImport = (void*)&out_mem;
    out_stream.streamSize   = out_mem.capacity;
    out_stream.read_writed  = nullptr;
    out_stream.write        = mem_write;

    tuz_TCompressProps props = tuz_kDefaultCompressProps;
    props.dictSize  = DICT_SIZE;
    props.threadNum = 1;

    hpatch_StreamPos_t result = tuz_compress(&out_stream, &in_stream, &props);
    if(result == 0 || result > out_mem.capacity)
    {
        throw std::runtime_error("tinyuz compression failed");
    }

    output.resize((size_t)result);
    return output;
}

bool LianLiWirelessCodec::Decompress(const uint8_t* input, size_t input_len,
                                     uint8_t* output, size_t output_len)
{
    tuz_size_t out_size = (tuz_size_t)output_len;
    tuz_TResult r = tuz_decompress_mem(input, (tuz_size_t)input_len, output, &out_size);
    return (r == tuz_OK || r == tuz_STREAM_END) && out_size == output_len;
}
