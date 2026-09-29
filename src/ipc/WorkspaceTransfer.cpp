// SPDX-License-Identifier: GPL-2.0-or-later
//
// WorkspaceTransfer — session-bound bounded staging for
// WORKSPACE_COMMAND (studio-import Task 3, plan section 3.3).
//
// Large content never travels inside one IPC envelope: clients
// open a transfer, stream <=192 KiB decoded chunks in offset
// order, and finish to receive an opaque `staged` token (or the
// inline staged bytes for a download). Tokens are session-local
// and id-keyed — a client can never name, traverse, or guess a
// filesystem path. Chunk payloads spill to disk under a
// server-chosen dir inside the verified managed root so staged
// bytes never accumulate in RAM, and a lazy sweep drops idle
// transfers so bounded memory stays bounded even when clients
// vanish mid-stream.

#include "WorkspaceTransfer.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

namespace fs = std::filesystem;

namespace openrgb2 { namespace ipc {

/*---------------------------------------------------------*\
||| SHA-256 (self-contained; staged hash is part of the wire   |
||| contract so it lives here, not behind a crypto dependency) |
\*---------------------------------------------------------*/
namespace
{
struct Sha256
{
    uint32_t h[8];
    uint8_t  buf[64];
    size_t   buflen;
    uint64_t total;

    Sha256()
        : h{0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u,
            0xa54ff53au, 0x510e527fu, 0x9b05688cu,
            0x1f83d9abu, 0x5be0cd19u}
        , buflen(0), total(0)
    {
    }

    static uint32_t rotr(uint32_t x, int n)
    {
        return (x >> n) | (x << (32 - n));
    }

    void block(const uint8_t* p)
    {
        static const uint32_t K[64] = {
            0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,
            0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
            0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,
            0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
            0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,
            0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
            0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,
            0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
            0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,
            0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
            0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,
            0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
            0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,
            0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
            0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,
            0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u,
        };
        uint32_t w[64];
        for(int i = 0; i < 16; ++i)
        {
            w[i] = ((uint32_t)p[i * 4] << 24) |
                   ((uint32_t)p[i * 4 + 1] << 16) |
                   ((uint32_t)p[i * 4 + 2] << 8) |
                   ((uint32_t)p[i * 4 + 3]);
        }
        for(int i = 16; i < 64; ++i)
        {
            const uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18)
                                ^ (w[i-15] >> 3);
            const uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19)
                                ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],
                 e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int i = 0; i < 64; ++i)
        {
            const uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }

    void update(const void* data, size_t len)
    {
        const uint8_t* p = (const uint8_t*)data;
        total += len;
        while(len)
        {
            const size_t take = std::min(len, 64 - buflen);
            std::memcpy(buf + buflen, p, take);
            buflen += take; p += take; len -= take;
            if(buflen == 64)
            {
                block(buf);
                buflen = 0;
            }
        }
    }

    void finish(uint8_t out[32])
    {
        const uint64_t bits = total * 8;
        const uint8_t one = 0x80;
        update(&one, 1);
        const uint8_t zero = 0;
        while(buflen != 56)
        {
            update(&zero, 1);
        }
        uint8_t lenb[8];
        for(int i = 0; i < 8; ++i)
        {
            lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
        }
        update(lenb, 8);
        for(int i = 0; i < 8; ++i)
        {
            out[i * 4]     = (uint8_t)(h[i] >> 24);
            out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
            out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
            out[i * 4 + 3] = (uint8_t)(h[i]);
        }
    }
};

std::string Hex(const uint8_t* d, size_t n)
{
    std::string out(n * 2, '0');
    for(size_t i = 0; i < n; ++i)
    {
        std::snprintf(&out[i * 2], 3, "%02x", d[i]);
    }
    return out;
}

bool HexOk(const std::string& s)
{
    if(s.size() != 64)
    {
        return false;
    }
    for(char c : s)
    {
        if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
        {
            return false;
        }
    }
    return true;
}
} // namespace

std::string Sha256Hex(const void* data, size_t len)
{
    Sha256 s;
    s.update(data, len);
    uint8_t d[32];
    s.finish(d);
    return Hex(d, 32);
}

std::string Sha256Hex(const std::string& s)
{
    return Sha256Hex(s.data(), s.size());
}

namespace
{
std::string HashFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if(!f.is_open())
    {
        return {};
    }
    Sha256 s;
    char buf[64 * 1024];
    while(f)
    {
        f.read(buf, (std::streamsize)sizeof(buf));
        const std::streamsize n = f.gcount();
        if(n > 0)
        {
            s.update(buf, (size_t)n);
        }
    }
    uint8_t d[32];
    s.finish(d);
    return Hex(d, 32);
}
} // namespace

/*---------------------------------------------------------*\
||| base64                                                    |
\*---------------------------------------------------------*/
namespace
{
const char kB64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

int B64Val(char c)
{
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a' + 26;
    if(c >= '0' && c <= '9') return c - '0' + 52;
    if(c == '+') return 62;
    if(c == '/') return 63;
    return -1;
}
} // namespace

std::string Base64Encode(const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for(size_t i = 0; i < len; i += 3)
    {
        const uint32_t a = p[i];
        const uint32_t b = (i + 1 < len) ? p[i + 1] : 0;
        const uint32_t c = (i + 2 < len) ? p[i + 2] : 0;
        out += kB64[a >> 2];
        out += kB64[((a & 3) << 4) | (b >> 4)];
        out += (i + 1 < len) ? kB64[((b & 15) << 2) | (c >> 6)]
                             : '=';
        out += (i + 2 < len) ? kB64[c & 63] : '=';
    }
    return out;
}

bool Base64Decode(const std::string& s, std::vector<uint8_t>& out,
                  size_t max_decoded)
{
    out.clear();
    if(s.empty() || s.size() % 4 != 0)
    {
        return false;
    }
    out.reserve((s.size() / 4) * 3);
    for(size_t i = 0; i < s.size(); i += 4)
    {
        const int v0 = B64Val(s[i]);
        const int v1 = B64Val(s[i + 1]);
        const int v2 = (s[i + 2] == '=') ? -2 : B64Val(s[i + 2]);
        const int v3 = (s[i + 3] == '=') ? -2 : B64Val(s[i + 3]);
        if(v0 < 0 || v1 < 0 || v2 == -1 || v3 == -1)
        {
            out.clear();
            return false;
        }
        if((v2 == -2 && v3 != -2) ||
           (v2 == -2 && i + 4 != s.size()))
        {
            out.clear();
            return false;   /* '=' only inside the tail quad */
        }
        const uint32_t trip = ((uint32_t)v0 << 18) |
                              ((uint32_t)v1 << 12) |
                              ((uint32_t)(v2 < 0 ? 0 : v2) << 6) |
                              (uint32_t)(v3 < 0 ? 0 : v3);
        out.push_back((uint8_t)(trip >> 16));
        if(v2 >= 0) out.push_back((uint8_t)(trip >> 8));
        if(v3 >= 0) out.push_back((uint8_t)trip);
        if(out.size() > max_decoded)
        {
            out.clear();
            return false;
        }
    }
    return true;
}

/*---------------------------------------------------------*\
||| WorkspaceTransfer                                           |
\*---------------------------------------------------------*/

WorkspaceTransfer::WorkspaceTransfer(std::string staging_root,
                                     Options options)
    : root_(std::move(staging_root))
    , options_(options)
{
    /* Orphans from a previous run are ours — sweep the exact
       naming this layer owns, nothing else. */
    std::error_code ec;
    fs::create_directories(root_, ec);
    for(const auto& e : fs::directory_iterator(root_, ec))
    {
        const std::string n = e.path().filename().string();
        if(n.rfind("tr-", 0) == 0)
        {
            fs::remove(e.path(), ec);
        }
    }
}

WorkspaceTransfer::~WorkspaceTransfer()
{
    std::lock_guard<std::mutex> lock(mutex_);
    for(auto& kv : transfers_)
    {
        Drop(kv.second);
    }
    transfers_.clear();
}

bool WorkspaceTransfer::IsKnownKind(const std::string& kind)
{
    return kind == "document" || kind == "type" ||
           kind == "look"     || kind == "asset" ||
           kind == "bundle"   || kind == "export";
}

uint64_t WorkspaceTransfer::KindCap(const std::string& kind) const
{
    if(kind == "asset")
    {
        return options_.max_asset_bytes;
    }
    if(kind == "bundle" || kind == "export")
    {
        return options_.max_bundle_bytes;
    }
    /* document / type / look JSON share the 4 MiB bound */
    return options_.max_document_bytes;
}

std::string WorkspaceTransfer::NewId()
{
    return "tr-" + std::to_string(next_id_++) + "-" +
           Sha256Hex(std::to_string(
               std::chrono::steady_clock::now()
                   .time_since_epoch().count())).substr(0, 16);
}

std::string WorkspaceTransfer::PathFor(const std::string& id) const
{
    /* server-chosen name under the verified staging root —
       the client only ever sees the opaque id. */
    return (fs::path(root_) / (id + ".bin")).string();
}

uint64_t WorkspaceTransfer::NowMs()
{
    return (uint64_t)std::chrono::duration_cast<
        std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void WorkspaceTransfer::Drop(Transfer& t)
{
    staged_bytes_ -= std::min(staged_bytes_, t.declared);
    if(!t.path.empty())
    {
        std::error_code ec;
        fs::remove(t.path, ec);
        t.path.clear();
    }
}

WorkspaceTransfer::BeginResult WorkspaceTransfer::BeginUpload(
    uint64_t session_id, const std::string& kind,
    uint64_t declared_length, const std::string& sha256_hex)
{
    std::lock_guard<std::mutex> lock(mutex_);
    SweepExpired();

    BeginResult r;
    if(!IsKnownKind(kind))
    {
        r.error = "unknown kind"; r.detail = "bad_request";
        return r;
    }
    if(!HexOk(sha256_hex))
    {
        r.error = "bad sha256"; r.detail = "bad_request";
        return r;
    }
    if(declared_length > KindCap(kind))
    {
        r.error = "declared length exceeds the kind cap";
        r.detail = "too_large";
        return r;
    }
    if(LiveCount(session_id) >= options_.max_per_session)
    {
        r.error = "too many open transfers on this session";
        r.detail = "transfer_limit";
        return r;
    }
    if(staged_bytes_ + declared_length >
       options_.max_staged_total_bytes)
    {
        r.error = "aggregate staging cap reached";
        r.detail = "aggregate_cap";
        return r;
    }

    Transfer t;
    t.id         = NewId();
    t.session_id = session_id;
    t.kind       = kind;
    t.upload     = true;
    t.declared   = declared_length;
    t.sha256     = sha256_hex;
    t.path       = PathFor(t.id);
    t.last_ms    = NowMs();

    /* spill to disk immediately — the file exists from begin so
       a pathological chunk storm never grows RAM. */
    {
        std::ofstream f(t.path,
                        std::ios::binary | std::ios::trunc);
        if(!f.is_open())
        {
            r.error = "staging unavailable";
            r.detail = "internal";
            return r;
        }
    }
    staged_bytes_ += declared_length;
    r.ok = true;
    r.transfer_id = t.id;
    transfers_[t.id] = std::move(t);
    return r;
}

WorkspaceTransfer::BeginResult WorkspaceTransfer::StageDownload(
    uint64_t session_id, const std::string& kind,
    const std::string& bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    SweepExpired();

    BeginResult r;
    if(!IsKnownKind(kind))
    {
        r.error = "unknown kind"; r.detail = "bad_request";
        return r;
    }
    if(bytes.size() > KindCap(kind))
    {
        r.error = "content exceeds the kind cap";
        r.detail = "too_large";
        return r;
    }
    if(LiveCount(session_id) >= options_.max_per_session)
    {
        r.error = "too many open transfers on this session";
        r.detail = "transfer_limit";
        return r;
    }
    if(staged_bytes_ + bytes.size() >
       options_.max_staged_total_bytes)
    {
        r.error = "aggregate staging cap reached";
        r.detail = "aggregate_cap";
        return r;
    }

    Transfer t;
    t.id         = NewId();
    t.session_id = session_id;
    t.kind       = kind;
    t.upload     = false;
    t.staged     = true;          /* server-produced = verified */
    t.declared   = bytes.size();
    t.length     = bytes.size();
    t.sha256     = Sha256Hex(bytes);
    t.path       = PathFor(t.id);
    t.last_ms    = NowMs();
    {
        std::ofstream f(t.path,
                        std::ios::binary | std::ios::trunc);
        if(!f.is_open())
        {
            r.error = "staging unavailable";
            r.detail = "internal";
            return r;
        }
        f.write(bytes.data(), (std::streamsize)bytes.size());
    }
    staged_bytes_ += bytes.size();
    r.ok          = true;
    r.transfer_id = t.id;
    r.length      = t.length;
    r.sha256      = t.sha256;
    transfers_[t.id] = std::move(t);
    return r;
}

WorkspaceTransfer::ChunkResult WorkspaceTransfer::WriteChunk(
    uint64_t session_id, const std::string& transfer_id,
    uint64_t offset, const std::vector<uint8_t>& decoded)
{
    std::lock_guard<std::mutex> lock(mutex_);
    SweepExpired();
    ChunkResult r;
    auto it = transfers_.find(transfer_id);
    if(it == transfers_.end() || it->second.session_id != session_id)
    {
        r.error = "unknown transfer"; r.detail = "unknown_transfer";
        return r;
    }
    Transfer& t = it->second;
    t.last_ms = NowMs();
    if(!t.upload || t.staged)
    {
        r.error = "not an open upload"; r.detail = "bad_request";
        return r;
    }
    if(decoded.size() > options_.max_chunk_decoded_bytes)
    {
        r.error = "chunk exceeds 192 KiB decoded";
        r.detail = "too_large";
        return r;
    }
    if(offset != t.received)
    {
        r.error = "offset must equal bytes received so far";
        r.detail = "chunk_order";
        return r;
    }
    if(t.received + decoded.size() > t.declared)
    {
        r.error = "chunk runs past the declared length";
        r.detail = "length_mismatch";
        return r;
    }
    /* bounded append at the declared offset — the file position
       equals `received` because offsets are strictly ordered */
    {
        std::ofstream f(t.path,
                        std::ios::binary | std::ios::app);
        if(!f.is_open())
        {
            r.error = "staging write failed"; r.detail = "internal";
            return r;
        }
        f.write((const char*)decoded.data(),
                (std::streamsize)decoded.size());
    }
    t.received += decoded.size();
    r.ok       = true;
    r.offset   = t.received;
    r.received = t.received;
    return r;
}

WorkspaceTransfer::ChunkResult WorkspaceTransfer::ReadChunk(
    uint64_t session_id, const std::string& transfer_id,
    uint64_t offset, uint64_t max_bytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    SweepExpired();
    ChunkResult r;
    auto it = transfers_.find(transfer_id);
    if(it == transfers_.end() || it->second.session_id != session_id)
    {
        r.error = "unknown transfer"; r.detail = "unknown_transfer";
        return r;
    }
    Transfer& t = it->second;
    t.last_ms = NowMs();
    if(t.upload || t.closed)
    {
        r.error = "not an open download"; r.detail = "bad_request";
        return r;
    }
    if(offset != t.read_pos)
    {
        r.error = "offset must equal the read cursor";
        r.detail = "chunk_order";
        return r;
    }
    const uint64_t want =
        std::min<uint64_t>(max_bytes ? max_bytes
                                     : options_.max_chunk_decoded_bytes,
                           options_.max_chunk_decoded_bytes);
    const uint64_t avail = t.length - t.read_pos;
    const size_t   n = (size_t)std::min<uint64_t>(want, avail);

    std::vector<char> buf(n);
    {
        std::ifstream f(t.path, std::ios::binary);
        if(!f.is_open())
        {
            r.error = "staging read failed"; r.detail = "internal";
            return r;
        }
        f.seekg((std::streamoff)t.read_pos);
        f.read(buf.data(), (std::streamsize)n);
        if((size_t)f.gcount() != n)
        {
            r.error = "staging read failed"; r.detail = "internal";
            return r;
        }
    }
    t.read_pos += n;
    r.ok       = true;
    r.offset   = offset;        /* echoes the served offset   */
    r.received = t.read_pos;    /* cursor after this chunk    */
    r.data_b64 = Base64Encode(buf.data(), n);
    r.eof      = t.read_pos >= t.length;
    return r;
}

WorkspaceTransfer::FinishResult WorkspaceTransfer::Finish(
    uint64_t session_id, const std::string& transfer_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    SweepExpired();
    FinishResult r;
    auto it = transfers_.find(transfer_id);
    if(it == transfers_.end() || it->second.session_id != session_id)
    {
        r.error = "unknown transfer"; r.detail = "unknown_transfer";
        return r;
    }
    Transfer& t = it->second;
    t.last_ms = NowMs();

    if(t.upload)
    {
        if(t.received != t.declared)
        {
            r.error = "received bytes do not match the declared "
                      "length";
            r.detail = "length_mismatch";
            return r;
        }
        const std::string got = HashFile(t.path);
        if(got != t.sha256)
        {
            r.error = "staged bytes do not match the declared "
                      "sha256";
            r.detail = "hash_mismatch";
            return r;
        }
        /* verified — consumable by a job of the owning session */
        t.staged = true;
        r.ok           = true;
        r.staged_token = t.id;
        r.bytes        = t.received;
        return r;
    }

    /* download: finish closes the reader; an unread tail is
       discarded with the staging file. */
    t.closed = true;
    Transfer dead = std::move(t);
    transfers_.erase(it);
    Drop(dead);
    r.ok = true;
    return r;
}

bool WorkspaceTransfer::Cancel(uint64_t session_id,
                               const std::string& transfer_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = transfers_.find(transfer_id);
    if(it == transfers_.end() || it->second.session_id != session_id)
    {
        return false;
    }
    Transfer dead = std::move(it->second);
    transfers_.erase(it);
    Drop(dead);
    return true;
}

bool WorkspaceTransfer::ReadStaged(const std::string& staged_token,
                                   std::string& bytes,
                                   std::string& err) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = transfers_.find(staged_token);
    if(it == transfers_.end() || !it->second.staged)
    {
        err = "unknown_transfer";
        return false;
    }
    const Transfer& t = it->second;
    std::ifstream f(t.path, std::ios::binary);
    if(!f.is_open())
    {
        err = "staging read failed";
        return false;
    }
    bytes.resize((size_t)t.length ? t.length : t.declared);
    f.read(&bytes[0], (std::streamsize)bytes.size());
    if((size_t)f.gcount() != bytes.size())
    {
        bytes.clear();
        err = "staging read failed";
        return false;
    }
    return true;
}

bool WorkspaceTransfer::StagedFor(uint64_t session_id,
                                  const std::string& staged_token) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = transfers_.find(staged_token);
    return it != transfers_.end() && it->second.staged &&
           it->second.upload &&
           it->second.session_id == session_id;
}

void WorkspaceTransfer::CancelSession(uint64_t session_id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for(auto it = transfers_.begin(); it != transfers_.end();)
    {
        if(it->second.session_id == session_id)
        {
            Transfer dead = std::move(it->second);
            it = transfers_.erase(it);
            Drop(dead);
        }
        else
        {
            ++it;
        }
    }
}

void WorkspaceTransfer::SweepExpired()
{
    const uint64_t now = NowMs();
    for(auto it = transfers_.begin(); it != transfers_.end();)
    {
        if(now - it->second.last_ms > options_.idle_expiry_ms)
        {
            Transfer dead = std::move(it->second);
            it = transfers_.erase(it);
            Drop(dead);
        }
        else
        {
            ++it;
        }
    }
}

uint64_t WorkspaceTransfer::StagedBytes() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return staged_bytes_;
}

size_t WorkspaceTransfer::LiveCount(uint64_t session_id) const
{
    size_t n = 0;
    for(const auto& kv : transfers_)
    {
        n += kv.second.session_id == session_id ? 1u : 0u;
    }
    return n;
}

}} // namespace openrgb2::ipc
