# P-13: Test and build gaps

Status: proposed · Size: M · Source: audit 2026-11-28 P-13

## Goal

Close the five gaps the audit listed:
- the real `TcpSocket` is untested
- unsolicited SDK packets are untested
- `LianLiWirelessTransport.cpp` is in no test target
- nothing exercises `stage-release.ps1`
- the build has no `/W4` or ASan configuration

Each gap becomes a CTest target or a configure preset, so later fixes (P-5, P-10,
P-12, P-2, P-14) have somewhere to put their regression tests.

## Current state

- **TcpSocket.** `src/backends/openrgb/TcpSocket.cpp` (300 lines; Winsock,
  non-blocking connect with `select`, `SO_RCVTIMEO`/`SO_SNDTIMEO`, `TCP_NODELAY`)
  implements `IStreamTransport` (`Transport.h`). Every `sdk` suite injects
  `FakeStream` (`tests/sdk/fake_sdk_server.h:220`), so the partial-read Timeout
  versus Error contract (`Transport.h`, `RecvExact`) is never exercised on a real
  socket.
- **Unsolicited packets.** `OpenRgbClient::Request` drops up to 64 unrelated
  packets (`OpenRgbClient.cpp:198-224`), and `DrainPending` (`:226-252`)
  consumes them. `FakeStream::InjectServerBytes` exists (`fake_sdk_server.h:231`)
  but is used only for delayed wireless replies (`sdk_client_test.cpp:781-861`).
  No test sends `kPktDeviceListUpdated` (`SdkWire.h:53`) or a junk packet
  mid-request.
- **Lian Li transport.** `tests/backend/CMakeLists.txt:6-15` builds `LLW_SOURCES`
  without `LianLiWirelessTransport.cpp`. libusb headers are vendored under
  `vendor/OpenRGB/dependencies/libusb-1.0.27/include`.
- **stage-release.ps1.** `tests/packaging/CMakeLists.txt` drives the startup
  task, launcher, GUI launcher, shortcut and lifecycle scripts, but not
  `packaging/windows/stage-release.ps1` (726 lines). That script needs the built
  exes, the pinned backend (`OpenRGB.exe`) and `windeployqt` (`:216-220`).
- **Build flags.** `CMakeLists.txt:20-22` adds `/W3` (no `/WX`). There is no
  `CMakePresets.json`, the standard is C++17 (`:11-13`), and nothing sets
  `/fsanitize=address`. The MSVC 14.44 toolset here ships the ASan runtime
  (`clang_rt.asan_dbg_dynamic-x86_64.dll` under `bin/Hostx64/x64`).

## Design

1. **`sdk_tcp_socket_test`** (label `sdk`). A loopback listener on port 0 in a
   helper thread. It covers:
   - connect timeout to a closed port
   - `SendAll` and `RecvExact` round-trips
   - Timeout with zero bytes, and Error after a partial read (server sends 3 of
     16 bytes, then stalls)
   - `PeerClosed` after a graceful close and after an abortive close (`SO_LINGER` 0)
   - `WaitReadable`

   It uses real Winsock but only 127.0.0.1, so it stays offline.
2. **Unsolicited-packet cases** in `sdk_client_test`:
   - inject `DEVICE_LIST_UPDATED` before a reply
   - inject 64 junk packets, which hits the bound error
   - inject one between frames, then `DrainPending`
   - a torn header, which must report "mid-packet"

   The assertions pin today's behaviour; P-5 then changes the first one into
   "topology generation bumped".
3. **`lianli_transport_test`** (label `backend`). It compiles
   `LianLiWirelessTransport.cpp` against a test-only `fake_libusb.cpp` that
   defines the handful of `libusb_*` symbols the file uses (init,
   get_device_list, descriptor, open, close, claim and release, bulk_transfer,
   set_auto_detach) over a scripted queue. This is a link-time seam: no
   production change, no real USB. It serves P-10 (timeouts, close, reopen) and
   P-12 (chunked reads).
4. **`package_stage_release_test`** (label `packaging`). A PowerShell test that
   stages into a temp `-OutDir` from the real build tree:
   - It runs only when `OpenRGB.exe` is present (`OPENRGB2_BACKEND_DIR` or the
     vendor output). Otherwise it exits 77, with `SKIP_RETURN_CODE 77`.
   - It asserts the manifest, the layout, the `windeployqt` output and
     `-VerifyOnly` success.
   - It is the home for P-2 (preserve `state\` and `profiles\local\`) and P-14
     (`-VerifyOnly` hashes).
5. **Configure presets.** Add `CMakePresets.json` with:
   - `default` (today's flags)
   - `w4`: `/W4 /permissive-` on first-party targets only, through an
     `openrgb2_warnings(target)` function. Vendor sources compiled into tests stay
     at `/W3`. Qt headers go behind `/external:anglebrackets /external:W0`.
   - `asan`: `/fsanitize=address /Zi`, no `/RTC`, `/INCREMENTAL:NO`, Debug or
     RelWithDebInfo, and a test preset that puts the ASan runtime directory on
     `PATH`.

   Stay on C++17. Qt 6.8 needs only C++17, and C++20 is a separate decision.

**Rejected alternatives:**
- A mock `IStreamTransport` for the TcpSocket test: that is what exists already;
  the gap is the real socket.
- A libusb shim interface inside the transport: it changes vendor code for
  testability that the link seam gives for free.
- Running stage-release in every CTest run with a fake backend:
  `windeployqt` on a non-Qt stub exe is unlikely to behave (guess), and the real
  backend is the thing worth checking.
- `/WX`: not until the `/W4` backlog is at zero.

## Steps

1. `sdk_tcp_socket_test`. Test: itself, plus the full `sdk` label.
2. Unsolicited-packet cases in `sdk_client_test`.
3. `fake_libusb.cpp` and `lianli_transport_test` with open, close, write and
   read happy paths, plus `NeedsReopen` errors closing and bumping the
   generation.
4. `package_stage_release_test` with the skip path. Test: run it with and
   without the backend present.
5. `CMakePresets.json` (`default`, `w4`, `asan`) and `openrgb2_warnings`. Test:
   the `default` preset gives an identical build. Then build `w4` and record the
   warning count in the PR. Fixing them is a later series.
6. ASan run of the Qt-free labels (`core`, `engine`, `sdk`, `backend`), with
   findings filed as new audit items. Test: the run itself. See the
   static-analysis hygiene plan for the procedure.

## Tests

The new targets above, under their CTest labels. No hardware is needed. Step 4
needs a built backend on the machine.

## Risks and open questions

- The `/W4` warning count is unknown and may be large. Should a burn-down target
  exist before `/WX` is turned on per target? Daniel's call.
- Can the UI tests run under ASan at all? Qt release DLLs are not instrumented,
  which is fine, but QML JIT and ASan on MSVC are untested here. Start with the
  Qt-free labels.
- `fake_libusb.cpp` duplicates libusb symbol signatures. Pin it to the vendored
  1.0.27 header so drift fails the compile.
- Port-0 listeners can still be blocked by local firewall policy (guess). The
  test should report "skipped" and not fail if `bind` is refused.

## Results (2026-11-28, branch `test/p-13-test-and-build-gaps`)

- **New tests.** `sdk_tcp_socket_test` (`sdk`, exits 77 if the loopback
  bind is refused), four unsolicited-packet cases in `sdk_client_test`,
  `lianli_transport_test` (`backend`; the stub API is the header comment
  of `tests/backend/fake_libusb.h`), `package_stage_release_test`
  (`packaging`, exits 77 without a built backend or in Debug). Each was
  shown to fail against a deliberately broken product file, then
  restored.
- **Current state was ahead of this plan.** P-5 (`DEVICE_LIST_UPDATED`
  bumps the topology generation), P-2 and P-14 had already landed, so the
  unsolicited-packet cases pin the P-5 behaviour, and the real-tree stage
  test sits next to the existing fake-input stage tests.
- **`default` preset.** Identical build: all 141 generated `.vcxproj`
  files match the pre-change configure, except the regeneration-input
  lists of 7 utility projects (they now list the new `.cmake` file).
- **`w4` preset.** 102 first-party targets at `/W4 /permissive-`; builds
  clean and passes the full suite (94 tests, the stage test skipped
  without a backend). 77 unique warnings:
  - 33 first-party: 15 that `/W3` also reports, plus C4456 x6, C4505 x3,
    C4189 x2, C4127, C4458 and more C4996;
  - 31 vendor: the unchanged `/W3` set (vendor sources stay at `/W3`);
  - 13 C4702 in Qt headers (`qmetatype.h`, `qvariant.h`, `qjsengine.h`).
    C4702 is a code-generation warning that `/external:W0` does not
    silence.
- **`asan` preset.** Qt-free (`OPENRGB2_BUILD_UI=OFF`), RelWithDebInfo.
  The four labels (51 tests) ran with no AddressSanitizer report. A
  canary heap overflow built with the same flags is reported, so the
  runtime is live. One timing failure: `e2e_lifecycle_test:924`, 3 of 14
  ASan runs against 0 of 6 Release runs, filed as audit
  [E-13](../audit-2026-11-28-open-findings.md#e-13). Not fixed here.
- **Open for Daniel.** Whether to add a `/W4` burn-down target before
  `/WX` per target; whether to suppress C4702 from Qt headers (for
  example with `#pragma warning` around the includes) or leave it.
