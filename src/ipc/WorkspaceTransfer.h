/*---------------------------------------------------------*\
||| WorkspaceTransfer.h                                       ||
|||                                                           ||
|||   Bounded, session-bound staging for the workspace     ||
|||   transfer family (studio-import Task 3, plan section  ||
|||   3.3). The 1 MiB message envelope stays fixed — large ||
|||   documents and bundles move through chunked           ||
|||   transfers instead:                                   ||
|||                                                           ||
|||     upload:   transfer_begin{direction:"upload",       ||
|||                 kind,length,sha256} -> transfer_id       ||
|||             transfer_chunk{transfer_id,offset,data}    ||
|||                 (base64, <=192 KiB decoded, strictly   ||
|||                 ordered: offset must equal bytes so    ||
|||                 far)                                   ||
|||             transfer_finish -> verifies length + hash, ||
|||                 marks the stage consumable and returns ||
|||                 the staged token the commit verbs      ||
|||                 reference                              ||
|||     download: the server materializes bounded bytes    ||
|||             (document/type/look JSON, an export        ||
|||             result) -> transfer_id + declared length   ||
|||             and SHA-256; transfer_chunk pulls ordered  ||
|||             slices; transfer_finish closes.            ||
|||                                                           ||
|||   Bounds (plan 3.3, verbatim): 192 KiB decoded bytes   ||
|||   per chunk; 4 MiB per document/type/look JSON;        ||
|||   16 MiB per asset; 64 MiB per imported bundle; at     ||
|||   most 2 transfers per session; 128 MiB staged across  ||
|||   the server; idle transfers expire after 60 s. Disk   ||
|||   staging lives INSIDE the verified managed workspace  ||
|||   root (<root>/transfers/) — clients never supply      ||
|||   paths; server-generated opaque ids only.             ||
|||                                                           ||
|||   Qt-free. All methods are mutex-serialized; chunk     ||
|||   writes/reads are bounded file I/O on the caller's    ||
|||   thread (the IPC session reader — bounded per the     ||
|||   IIpcService contract: a blocking handler pins only   ||
|||   its own session slot).                               ||
|||                                                           ||
|||   SPDX-License-Identifier: GPL-2.0-or-later               ||
\*---------------------------------------------------------*/

#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace openrgb2
{
namespace ipc
{

/* Lowercase-hex SHA-256 — the declared content hash of the
   transfer contract (docs/ipc-protocol.md). */
std::string Sha256Hex(const void* data, size_t len);
std::string Sha256Hex(const std::string& s);

/* Chunk payloads on the wire are base64 inside the JSON body.
   Decode refuses output past max_decoded (bounded buffers). */
std::string Base64Encode(const void* data, size_t len);
bool        Base64Decode(const std::string& s,
                         std::vector<uint8_t>& out,
                         size_t max_decoded);

class WorkspaceTransfer
{
public:
    struct Options
    {
        uint64_t max_chunk_decoded_bytes = 192ull * 1024;
        uint64_t max_document_bytes      = 4ull  * 1024 * 1024;
        uint64_t max_asset_bytes         = 16ull * 1024 * 1024;
        uint64_t max_bundle_bytes        = 64ull * 1024 * 1024;
        size_t   max_per_session         = 2;
        uint64_t max_staged_total_bytes  = 128ull * 1024 * 1024;
        uint32_t idle_expiry_ms          = 60000;
    };

    /* staging_root must be inside the verified managed
       workspace root; orphan files matching this layer's own
       naming are cleaned on construction. */
    explicit WorkspaceTransfer(std::string staging_root,
                               Options options = Options{});
    ~WorkspaceTransfer();   /* removes this layer's staging files */

    /* Every failure carries a stable machine token in `detail`
       (docs/ipc-protocol.md §workspace): "bad_request",
       "unknown_transfer", "transfer_limit", "aggregate_cap",
       "too_large", "chunk_order", "hash_mismatch",
       "length_mismatch", "transfer_expired". */
    struct BeginResult
    {
        bool        ok = false;
        std::string transfer_id;
        uint64_t    length = 0;      /* downloads: the declared size */
        std::string sha256;          /* downloads: the declared hash */
        std::string error;
        std::string detail;
    };

    static bool IsKnownKind(const std::string& kind);
    /* The per-kind content cap (plan 3.3). */
    uint64_t    KindCap(const std::string& kind) const;

    /* Upload side: the client declares kind + exact length +
       SHA-256. Reserved length counts against the aggregate
       cap at begin. */
    BeginResult BeginUpload(uint64_t session_id,
                            const std::string& kind,
                            uint64_t declared_length,
                            const std::string& sha256_hex);

    /* Download side: stage server-produced bytes for `session`.
       The transfer is immediately readable; length/hash are
       computed and returned for the client to verify. */
    BeginResult StageDownload(uint64_t session_id,
                              const std::string& kind,
                              const std::string& bytes);

    struct ChunkResult
    {
        bool        ok = false;
        uint64_t    offset = 0;
        uint64_t    received = 0;    /* upload: bytes staged so far */
        std::string data_b64;        /* download: base64 payload    */
        bool        eof = false;     /* download: end reached       */
        std::string error;
        std::string detail;
    };

    /* Upload chunk: decoded bytes are appended at `offset`,
       which MUST equal bytes received so far (ordered offsets —
       a gap or replay is rejected, never silently skipped). */
    ChunkResult WriteChunk(uint64_t session_id,
                           const std::string& transfer_id,
                           uint64_t offset,
                           const std::vector<uint8_t>& decoded);

    /* Download chunk: reads at `offset` (must equal the read
       cursor — ordered), at most max_bytes (clamped to the
       chunk bound). */
    ChunkResult ReadChunk(uint64_t session_id,
                          const std::string& transfer_id,
                          uint64_t offset,
                          uint64_t max_bytes);

    struct FinishResult
    {
        bool        ok = false;
        std::string staged_token;    /* upload: consumable token   */
        uint64_t    bytes = 0;
        std::string error;
        std::string detail;
    };

    /* Upload: verify length + declared SHA-256, then mark the
       stage consumable (the returned token is what
       commit/validate/import_* reference as `staged`).
       Download: close the reader side. */
    FinishResult Finish(uint64_t session_id,
                        const std::string& transfer_id);

    bool Cancel(uint64_t session_id,
                const std::string& transfer_id);

    /* A finished upload is consumable while it lives; reads
       are bounded by the kind cap. Session end, expiry, or
       cancel removes it — ReadStaged then fails honestly. */
    bool ReadStaged(const std::string& staged_token,
                    std::string& bytes,
                    std::string& err) const;
    /* Ownership + readiness check for the command layer —
       a staged token may only be consumed by a job of the
       session that uploaded it. */
    bool StagedFor(uint64_t session_id,
                   const std::string& staged_token) const;

    void CancelSession(uint64_t session_id);
    /* Drop transfers idle past options.idle_expiry_ms — called
       by the IPC surface on every workspace command and on the
       owner loop's PollEvents tick. Mutex-serialized like every
       other public op. */
    void SweepExpired();

    uint64_t       StagedBytes() const;
    size_t         LiveCount(uint64_t session_id) const;
    const Options& TransferOptions() const { return options_; }

private:
    struct Transfer
    {
        std::string id;
        uint64_t    session_id = 0;
        std::string kind;
        bool        upload   = false;
        bool        staged   = false;   /* upload: finish-verified  */
        bool        closed   = false;   /* download: finished       */
        uint64_t    declared = 0;       /* upload declared length   */
        std::string sha256;
        uint64_t    received = 0;       /* upload bytes so far      */
        uint64_t    length   = 0;       /* download byte length     */
        uint64_t    read_pos = 0;       /* download cursor          */
        std::string path;
        uint64_t    last_ms  = 0;
    };

    std::string NewId();
    void        Drop(Transfer& t);      /* erase record + file      */
    std::string PathFor(const std::string& id) const;
    static uint64_t NowMs();
    /* Under-lock variants — public ops call these while already
       holding mutex_; NEVER call the locking forms there. */
    void        SweepExpiredLocked();
    size_t      LiveCountLocked(uint64_t session_id) const;

    std::string          root_;
    Options              options_;
    mutable std::mutex   mutex_;
    std::map<std::string, Transfer> transfers_;
    uint64_t             staged_bytes_ = 0;
    uint64_t             next_id_      = 1;
};

} /* namespace ipc */
} /* namespace openrgb2 */
