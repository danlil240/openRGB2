// SPDX-License-Identifier: GPL-2.0-or-later
//
// Contract tests for device mapping and identity resolution
// (src/engine/DeviceMapper.h) against the documented
// daniel-pc topology (docs/hardware/daniel-pc.md,
// plan section 2.1):
//   - 120-LED wireless group = ONE complete-controller write
//     per scene update, never one per zone
//   - unmapped LEDs (LED_C / Chipset Accent, keyboard Extras,
//     unbound zones) keep their colors in the assembled frame
//   - mirrored fans are Linked copies — they never multiply
//     the 8/16/8 addressable counts
//   - identical DIMM names resolve by stable identity
//     (serial/location), never enumeration order
//   - reordered enumeration cannot misroute colors
//   - stale-generation frames are rejected; a vanished bound
//     device keeps its binding, never reassigned
// Fake snapshots/sinks only — no hardware, deterministic.

#include "DeliveryPipeline.h"
#include "DeviceMapper.h"
#include "FrameSink.h"
#include "ProfileStore.h"        /* AugmentSceneBindings */

#include "scene/SceneTypes.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace openrgb2;

static int checks   = 0;
static int failures = 0;
#define CHECK(x) do { ++checks; if(!(x)) { ++failures; \
    std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while(0)

/*---------------------------------------------------------*\
|| Fake daniel-pc snapshots (identifier-free placeholders). |
\*---------------------------------------------------------*/
static const char* kWirelessMac = "FA:KE:00:00:00:01";
static const char* kDimmASerial = "SN-FAKE-DIMM-A";
static const char* kDimmBSerial = "SN-FAKE-DIMM-B";
static const char* kDimmALoc    = "smbus:0-0053";
static const char* kDimmBLoc    = "smbus:0-0057";

static const RgbColor kEnumerated{ 7, 6, 5 }; /* baseline sentinel */

static DeviceSnapshot WirelessGroup(uint64_t gen = 1)
{
    /* One controller: 3 x 40-LED zones = 120 logical LEDs. */
    DeviceSnapshot s;
    s.key.device   = "Lian Li:UNI FAN SL V3 Wireless 0001";
    s.key.serial   = kWirelessMac;
    s.key.location = std::string("Wireless: ") + kWirelessMac;
    s.name         = "UNI FAN SL V3 Wireless 0001";
    s.vendor       = "Lian Li";
    s.device_type  = 5; /* DEVICE_TYPE_COOLER */
    s.topology_generation = gen;
    s.zones = { { "Fan 1", 40 }, { "Fan 2", 40 }, { "Fan 3", 40 } };
    s.connection = ConnectionState::Connected;
    s.colors.assign(s.LedCount(), kEnumerated);
    return s;
}

static DeviceSnapshot Motherboard(uint64_t gen = 1)
{
    /* 8 / 16 / 8 headers plus the deliberately unmapped
       LED_C / Chipset Accent zones — 34 LEDs total. */
    DeviceSnapshot s;
    s.key.device   = "GIGABYTE:X870E AORUS ELITE WIFI7 ICE";
    s.key.serial   = "MB-FAKE-1";
    s.key.location = "smbus:0-motherboard";
    s.name         = "X870E AORUS ELITE WIFI7 ICE";
    s.vendor       = "GIGABYTE";
    s.device_type  = 2; /* motherboard-ish */
    s.topology_generation = gen;
    s.zones = { { "ARGB_V2_1", 8 }, { "ARGB_V2_2", 16 },
                { "ARGB_V2_3", 8 }, { "LED_C", 1 },
                { "Chipset Accent", 1 } };
    s.connection = ConnectionState::Connected;
    s.colors.assign(s.LedCount(), kEnumerated);
    return s;
}

static DeviceSnapshot Dimm(const std::string& serial,
                           const std::string& location,
                           uint64_t gen = 1)
{
    /* Both DIMMs report the SAME name — identity comes only
       from serial/location (plan section 6 review focus). */
    DeviceSnapshot s;
    s.key.device   = "Corsair:Corsair Vengeance RGB DDR5";
    s.key.serial   = serial;
    s.key.location = location;
    s.name         = "Corsair Vengeance RGB DDR5";
    s.vendor       = "Corsair";
    s.topology_generation = gen;
    s.zones = { { "DRAM", 10 } };
    s.connection = ConnectionState::Connected;
    s.colors.assign(s.LedCount(), kEnumerated);
    return s;
}

static DeviceSnapshot Gpu(uint64_t gen = 1)
{
    DeviceSnapshot s;
    s.key.device   = "GIGABYTE:AORUS RTX 5080 MASTER ICE";
    s.key.serial   = "GPU-FAKE-1";
    s.key.location = "pci:gpu";
    s.name         = "AORUS RTX 5080 MASTER ICE";
    s.vendor       = "GIGABYTE";
    s.topology_generation = gen;
    s.zones = { { "Logo", 26 } };
    s.connection = ConnectionState::Connected;
    s.colors.assign(s.LedCount(), kEnumerated);
    return s;
}

static DeviceSnapshot Keyboard(uint64_t gen = 1)
{
    /* 107 per-key LEDs + the non-visible Extras zone. */
    DeviceSnapshot s;
    s.key.device   = "Logitech:Logitech G512";
    s.key.serial   = "KB-FAKE-1";
    s.key.location = "hid:kb";
    s.name         = "Logitech G512";
    s.vendor       = "Logitech";
    s.topology_generation = gen;
    s.zones = { { "Keys", 107 }, { "Extras", 1 } };
    s.connection = ConnectionState::Connected;
    s.colors.assign(s.LedCount(), kEnumerated);
    return s;
}

static std::vector<DeviceSnapshot> FullTopology(uint64_t gen = 1)
{
    return { WirelessGroup(gen), Motherboard(gen),
             Dimm(kDimmASerial, kDimmALoc, gen),
             Dimm(kDimmBSerial, kDimmBLoc, gen),
             Gpu(gen), Keyboard(gen) };
}

/*---------------------------------------------------------*\
|| Scene document fixtures                                  |
\*---------------------------------------------------------*/
static studio::DeviceBinding Binding(const std::string& id,
                                     const std::string& ctrl,
                                     const std::string& serial,
                                     const std::string& location,
                                     const std::string& zone,
                                     unsigned zone_leds = 0)
{
    studio::DeviceBinding b;
    b.id              = id;
    b.controller_name = ctrl;
    b.serial          = serial;
    b.location        = location;
    b.zone_name       = zone;
    b.zone_leds       = zone_leds;
    return b;
}

static studio::SceneObject DevObj(const std::string& id,
                                  const std::string& binding,
                                  int first_addr, int count)
{
    studio::SceneObject o;
    o.id      = id;
    o.kind    = studio::ObjectKind::Device;
    o.binding = binding;
    o.verified = true;
    for(int i = 0; i < count; ++i)
    {
        studio::Emitter e;
        e.address = first_addr + i;
        o.emitters.push_back(e);
    }
    return o;
}

static studio::SceneObject LinkedCopy(const std::string& id,
                                      const std::string& source,
                                      int emitter_count)
{
    /* Mirrored fan: shares the source object's emitter group;
       every emitter is render-only (address == -1). */
    studio::SceneObject o;
    o.id        = id;
    o.kind      = studio::ObjectKind::Linked;
    o.mirror_of = source;
    o.verified  = true;
    for(int i = 0; i < emitter_count; ++i)
    {
        studio::Emitter e;
        e.address = -1;
        o.emitters.push_back(e);
    }
    return o;
}

/* The daniel-pc scene: every bound device from the hardware
   profile; unmapped zones have no bindings at all. */
static std::shared_ptr<studio::SceneDocument> DanielPcDoc()
{
    auto doc = std::make_shared<studio::SceneDocument>();
    doc->brightness = 1.0f;

    doc->bindings = {
        Binding("b-fan1", "UNI FAN SL", kWirelessMac, "", "Fan 1", 40),
        Binding("b-fan2", "UNI FAN SL", kWirelessMac, "", "Fan 2", 40),
        Binding("b-fan3", "UNI FAN SL", kWirelessMac, "", "Fan 3", 40),
        Binding("b-argb1", "X870E", "", "", "ARGB_V2_1", 8),
        Binding("b-argb2", "X870E", "", "", "ARGB_V2_2", 16),
        Binding("b-argb3", "X870E", "", "", "ARGB_V2_3", 8),
        Binding("b-dimm-a", "Vengeance RGB", kDimmASerial, "", "", 10),
        Binding("b-dimm-b", "Vengeance RGB", kDimmBSerial, "", "", 10),
        Binding("b-gpu", "5080 MASTER", "", "", "", 0),
        Binding("b-kb", "G512", "", "", "Keys", 107),
    };

    doc->objects = {
        DevObj("o-fan1", "b-fan1", 0, 40),
        DevObj("o-fan2", "b-fan2", 0, 40),
        DevObj("o-fan3", "b-fan3", 0, 40),
        /* 4 mirrored case fans: ONE bound object + 3 Linked
           copies — the 8 addressable LEDs are shared. */
        DevObj("o-case1", "b-argb1", 0, 8),
        LinkedCopy("o-case2", "o-case1", 8),
        LinkedCopy("o-case3", "o-case1", 8),
        LinkedCopy("o-case4", "o-case1", 8),
        /* ARGB_V2_2 regions: radiator fans 0-7 (3 mirrored),
           pump lighting 8-15. */
        DevObj("o-rad1", "b-argb2", 0, 8),
        LinkedCopy("o-rad2", "o-rad1", 8),
        LinkedCopy("o-rad3", "o-rad1", 8),
        DevObj("o-pump", "b-argb2", 8, 8),
        DevObj("o-topgpu", "b-argb3", 0, 8),
        DevObj("o-dimm-a", "b-dimm-a", 0, 10),
        DevObj("o-dimm-b", "b-dimm-b", 0, 10),
        DevObj("o-gpu", "b-gpu", 0, 26),
        DevObj("o-kb", "b-kb", 0, 107),
    };
    return doc;
}

static studio::SceneColor SC(int r, int g, int b)
{
    return studio::MakeSceneColor((unsigned)r, (unsigned)g, (unsigned)b);
}

static RgbColor RGB(int r, int g, int b)
{
    RgbColor c;
    c.r = (uint8_t)r; c.g = (uint8_t)g; c.b = (uint8_t)b;
    return c;
}

/* Fill eval colors for every Device object from a per-object
   color table; objects absent from the table get none. */
static EvaluatedFrame EvalAll(
    const studio::SceneDocument& doc,
    const std::map<std::string, studio::SceneColor>& colors)
{
    EvaluatedFrame eval;
    eval.sequence    = 1;
    eval.sample_time = Clock::now();
    for(const studio::SceneObject& obj : doc.objects)
    {
        if(obj.kind != studio::ObjectKind::Device)
        {
            continue;
        }
        const auto it = colors.find(obj.id);
        if(it != colors.end())
        {
            eval.colors[obj.id].assign(obj.emitters.size(), it->second);
        }
    }
    return eval;
}

static const RgbFrame* FrameFor(const std::vector<RgbFrame>& frames,
                                const DeviceKey& key)
{
    for(const RgbFrame& f : frames)
    {
        if(f.controller == key)
        {
            return &f;
        }
    }
    return nullptr;
}

static bool AllEqual(const std::vector<RgbColor>& v, size_t first,
                     size_t count, const RgbColor& want)
{
    for(size_t i = first; i < first + count; ++i)
    {
        if(i >= v.size() || v[i] != want)
        {
            return false;
        }
    }
    return true;
}

class FakeTransport final : public IFrameSink
{
public:
    SubmitResult SubmitLatest(const RgbFrame& f) override
    {
        std::lock_guard<std::mutex> lock(m_);
        delivered_[f.controller].push_back(f);
        return SubmitResult::Accept();
    }
    DeliveryStats Stats() const override { return DeliveryStats{}; }

    std::vector<RgbFrame> Delivered(const DeviceKey& key) const
    {
        std::lock_guard<std::mutex> lock(m_);
        const auto it = delivered_.find(key);
        return it == delivered_.end() ? std::vector<RgbFrame>{}
                                      : it->second;
    }
    size_t TotalDelivered() const
    {
        std::lock_guard<std::mutex> lock(m_);
        size_t n = 0;
        for(const auto& kv : delivered_) { n += kv.second.size(); }
        return n;
    }

private:
    mutable std::mutex m_;
    std::map<DeviceKey, std::vector<RgbFrame>> delivered_;
};

/*---------------------------------------------------------*\
|| Tests                                                    |
\*---------------------------------------------------------*/

/* The whole 120-LED wireless group updates as ONE
   complete-controller write — three zone edits must never
   become three uploads (plan section 4). */
static void TestWirelessGroupBatchesToOneWrite()
{
    DeviceMapper mapper;
    const auto doc = DanielPcDoc();
    mapper.SetScene(doc);
    const auto snaps = FullTopology();
    mapper.SyncDevices(snaps);

    const EvaluatedFrame eval = EvalAll(*doc, {
        { "o-fan1", SC(255, 0, 0) },
        { "o-fan2", SC(0, 255, 0) },
        { "o-fan3", SC(0, 0, 255) },
    });
    const std::vector<RgbFrame> frames = mapper.MapFrame(eval);

    /* Exactly one frame for the wireless controller. */
    const DeviceSnapshot w = snaps[0];
    int wireless_frames = 0;
    const RgbFrame* wf = nullptr;
    for(const RgbFrame& f : frames)
    {
        if(f.controller == w.key) { ++wireless_frames; wf = &f; }
    }
    CHECK(wireless_frames == 1);
    CHECK(wf != nullptr && wf->colors.size() == 120);
    CHECK(wf->topology_generation == w.topology_generation);
    CHECK(wf->sequence == 1);

    /* Fan zones land at their documented offsets. */
    CHECK(AllEqual(wf->colors, 0, 40, RGB(255, 0, 0)));
    CHECK(AllEqual(wf->colors, 40, 40, RGB(0, 255, 0)));
    CHECK(AllEqual(wf->colors, 80, 40, RGB(0, 0, 255)));

    /* End-to-end through the pipeline: exactly ONE controller
       write reaches the transport for the 3-zone update. */
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    pipe.SyncDevices(snaps);
    for(const RgbFrame& f : frames)
    {
        pipe.SubmitLatest(f);
    }
    pipe.DrainAllDue();
    CHECK(transport->Delivered(w.key).size() == 1);
    CHECK(transport->Delivered(w.key)[0].colors.size() == 120);
}

/* Reordered enumeration and identical DIMM names never
   misroute colors (plan section 6 review focus). */
static void TestReorderedEnumerationKeepsRouting()
{
    const auto doc = DanielPcDoc();
    const auto snaps = FullTopology();
    std::vector<DeviceSnapshot> reversed(snaps.rbegin(), snaps.rend());

    const EvaluatedFrame eval = EvalAll(*doc, {
        { "o-dimm-a", SC(10, 0, 0) },
        { "o-dimm-b", SC(0, 20, 0) },
        { "o-fan1", SC(0, 0, 30) },
    });

    DeviceMapper fwd;
    fwd.SetScene(doc);
    fwd.SyncDevices(snaps);
    const std::vector<RgbFrame> fwd_frames = fwd.MapFrame(eval);

    DeviceMapper rev;
    rev.SetScene(doc);
    rev.SyncDevices(reversed);
    const std::vector<RgbFrame> rev_frames = rev.MapFrame(eval);

    const DeviceSnapshot a = snaps[2];   /* dimm A */
    const DeviceSnapshot b = snaps[3];   /* dimm B */
    const DeviceSnapshot w = snaps[0];

    const RgbFrame* fa = FrameFor(fwd_frames, a.key);
    const RgbFrame* fb = FrameFor(fwd_frames, b.key);
    CHECK(fa != nullptr && fb != nullptr);
    CHECK(AllEqual(fa->colors, 0, 10, RGB(10, 0, 0)));
    CHECK(AllEqual(fb->colors, 0, 10, RGB(0, 20, 0)));

    /* Same routing under reversed enumeration. */
    const RgbFrame* ra = FrameFor(rev_frames, a.key);
    const RgbFrame* rb = FrameFor(rev_frames, b.key);
    CHECK(ra != nullptr && rb != nullptr);
    CHECK(ra->colors == fa->colors);
    CHECK(rb->colors == fb->colors);
    CHECK(FrameFor(rev_frames, w.key) != nullptr);
}

/* Two DIMMs with identical display names resolve separately
   by serial; a serial-less binding on a duplicated name is
   ambiguous and gets NO writes — never silently picked. */
static void TestIdenticalDimmNamesIdentityResolution()
{
    auto doc = std::make_shared<studio::SceneDocument>();
    doc->bindings = {
        Binding("b-a", "Vengeance RGB", kDimmASerial, "", "", 0),
        Binding("b-b", "Vengeance RGB", kDimmBSerial, "", "", 0),
        /* Serial-less bindings disambiguate by location. */
        Binding("b-la", "Vengeance RGB", "", kDimmALoc, "", 0),
        Binding("b-lb", "Vengeance RGB", "", kDimmBLoc, "", 0),
        /* Nothing to disambiguate with -> ambiguous. */
        Binding("b-x", "Vengeance RGB", "", "", "", 0),
    };
    doc->objects = {
        DevObj("o-a", "b-a", 0, 10),
        DevObj("o-b", "b-b", 0, 10),
        DevObj("o-la", "b-la", 0, 10),
        DevObj("o-lb", "b-lb", 0, 10),
        DevObj("o-x", "b-x", 0, 10),
    };

    DeviceMapper mapper;
    mapper.SetScene(doc);
    const DeviceSnapshot da = Dimm(kDimmASerial, kDimmALoc);
    const DeviceSnapshot db = Dimm(kDimmBSerial, kDimmBLoc);
    mapper.SyncDevices({ db, da });   /* reversed on purpose */

    studio::BindingStatus st;
    CHECK(mapper.BindingState("b-a", st) &&
          st == studio::BindingStatus::Resolved);
    CHECK(mapper.BindingState("b-b", st) &&
          st == studio::BindingStatus::Resolved);
    CHECK(mapper.BindingState("b-la", st) &&
          st == studio::BindingStatus::Resolved);
    CHECK(mapper.BindingState("b-lb", st) &&
          st == studio::BindingStatus::Resolved);
    CHECK(mapper.BindingState("b-x", st) &&
          st == studio::BindingStatus::Ambiguous);

    ZonePlacement za, zb, zla, zlb;
    CHECK(mapper.BoundZone("b-a", za) && za.controller == da.key);
    CHECK(mapper.BoundZone("b-b", zb) && zb.controller == db.key);
    CHECK(mapper.BoundZone("b-la", zla) && zla.controller == da.key);
    CHECK(mapper.BoundZone("b-lb", zlb) && zlb.controller == db.key);

    const EvaluatedFrame eval = EvalAll(*doc, {
        { "o-a", SC(1, 0, 0) },
        { "o-b", SC(2, 0, 0) },
        { "o-la", SC(3, 0, 0) },
        { "o-lb", SC(4, 0, 0) },
        { "o-x", SC(9, 9, 9) },   /* ambiguous — must not land */
    });
    const std::vector<RgbFrame> frames = mapper.MapFrame(eval);
    const RgbFrame* fa = FrameFor(frames, da.key);
    const RgbFrame* fb = FrameFor(frames, db.key);
    CHECK(fa != nullptr && fb != nullptr);
    /* Document precedence: later objects overwrite the same
       addresses — o-lb is last for B, o-la last for A. */
    CHECK(AllEqual(fa->colors, 0, 10, RGB(3, 0, 0)));
    CHECK(AllEqual(fb->colors, 0, 10, RGB(4, 0, 0)));
    /* The ambiguous binding's color appears nowhere. */
    for(const RgbFrame& f : frames)
    {
        CHECK(!AllEqual(f.colors, 0, f.colors.size(), RGB(9, 9, 9)));
    }
}

/* Mirrored fans never multiply addressable counts: the
   motherboard frame is exactly 8+16+8 (+2 unmapped) LEDs. */
static void TestMirroredFansKeepExactCounts()
{
    DeviceMapper mapper;
    const auto doc = DanielPcDoc();
    mapper.SetScene(doc);
    const auto snaps = FullTopology();
    mapper.SyncDevices(snaps);

    const EvaluatedFrame eval = EvalAll(*doc, {
        { "o-case1", SC(50, 0, 0) },   /* 4 mirrored fans     */
        { "o-rad1",  SC(60, 0, 0) },   /* 3 mirrored radiators*/
        { "o-pump",  SC(70, 0, 0) },
        { "o-topgpu", SC(80, 0, 0) },
    });
    const std::vector<RgbFrame> frames = mapper.MapFrame(eval);
    const DeviceSnapshot mb = snaps[1];
    const RgbFrame* f = FrameFor(frames, mb.key);
    CHECK(f != nullptr);
    CHECK(f->colors.size() == 34);      /* NOT 8*4 + ... */

    /* ARGB_V2_1 offset 0: 8 shared colors for 4 fans. */
    CHECK(AllEqual(f->colors, 0, 8, RGB(50, 0, 0)));
    /* ARGB_V2_2 offset 8: 0-7 radiator, 8-15 pump — separate
       regions inside the same zone. */
    CHECK(AllEqual(f->colors, 8, 8, RGB(60, 0, 0)));
    CHECK(AllEqual(f->colors, 16, 8, RGB(70, 0, 0)));
    /* ARGB_V2_3 offset 24: top-GPU fan. */
    CHECK(AllEqual(f->colors, 24, 8, RGB(80, 0, 0)));
    /* Linked copies generated no extra writes — verified by
       the exact buffer size and per-region colors above. */
}

/* Unmapped zones and unbound regions keep their colors in a
   complete-controller write (plan section 4): baseline =
   enumerated colors first, then the last-emitted frame. */
static void TestUnmappedLedsKeepColors()
{
    /* Bindings cover headers 1-3, the keyboard Keys zone, and
       wireless fans 1-2 — but objects only write ARGB_V2_1,
       Keys, and the two fans. Zones/regions with no object are
       preserved, not blacked. */
    auto doc = std::make_shared<studio::SceneDocument>();
    doc->bindings = {
        Binding("b-h1", "X870E", "", "", "ARGB_V2_1", 8),
        Binding("b-h2", "X870E", "", "", "ARGB_V2_2", 16),
        Binding("b-h3", "X870E", "", "", "ARGB_V2_3", 8),
        Binding("b-kb", "G512", "", "", "Keys", 107),
        Binding("b-f1", "UNI FAN SL", kWirelessMac, "", "Fan 1", 40),
        Binding("b-f2", "UNI FAN SL", kWirelessMac, "", "Fan 2", 40),
    };
    doc->objects = {
        DevObj("o-h1", "b-h1", 0, 8),
        DevObj("o-kb", "b-kb", 0, 107),
        DevObj("o-f1", "b-f1", 0, 40),
        DevObj("o-f2", "b-f2", 0, 40),
    };

    DeviceMapper mapper;
    mapper.SetScene(doc);
    const auto snaps = FullTopology();
    const DeviceSnapshot mb = snaps[1];
    const DeviceSnapshot kb = snaps[5];
    const DeviceSnapshot w  = snaps[0];
    mapper.SyncDevices(snaps);

    const EvaluatedFrame eval = EvalAll(*doc, {
        { "o-h1", SC(11, 0, 0) },
        { "o-kb", SC(12, 0, 0) },
        { "o-f1", SC(13, 0, 0) },
        { "o-f2", SC(14, 0, 0) },
    });
    std::vector<RgbFrame> frames = mapper.MapFrame(eval);

    const RgbFrame* mf = FrameFor(frames, mb.key);
    CHECK(mf != nullptr && mf->colors.size() == 34);
    CHECK(AllEqual(mf->colors, 0, 8, RGB(11, 0, 0)));
    /* Bound zones with no object mapped keep enumeration —
       writes never reach LEDs nobody mapped. */
    CHECK(AllEqual(mf->colors, 8, 16, kEnumerated));  /* V2_2  */
    CHECK(AllEqual(mf->colors, 24, 8, kEnumerated));  /* V2_3  */
    /* LED_C (32) / Chipset Accent (33): unmapped, preserved. */
    CHECK(mf->colors[32] == kEnumerated);
    CHECK(mf->colors[33] == kEnumerated);

    const RgbFrame* kf = FrameFor(frames, kb.key);
    CHECK(kf != nullptr && kf->colors.size() == 108);
    CHECK(AllEqual(kf->colors, 0, 107, RGB(12, 0, 0)));
    CHECK(kf->colors[107] == kEnumerated);   /* Extras zone   */

    /* Wireless fan 3 has no binding at all — its 40 LEDs keep
       the enumerated colors inside the single group frame. */
    const RgbFrame* wf = FrameFor(frames, w.key);
    CHECK(wf != nullptr && wf->colors.size() == 120);
    CHECK(AllEqual(wf->colors, 0, 40, RGB(13, 0, 0)));
    CHECK(AllEqual(wf->colors, 40, 40, RGB(14, 0, 0)));
    CHECK(AllEqual(wf->colors, 80, 40, kEnumerated));

    /* Scene swap drops the fan-2 object: its region keeps the
       LAST EMITTED colors (not enumeration, not black). */
    auto doc2 = std::make_shared<studio::SceneDocument>();
    doc2->bindings = {
        Binding("b-f1", "UNI FAN SL", kWirelessMac, "", "Fan 1", 40),
    };
    doc2->objects = { DevObj("o-f1", "b-f1", 0, 40) };
    mapper.SetScene(doc2);
    mapper.SyncDevices(snaps);
    frames = mapper.MapFrame(EvalAll(*doc2, { { "o-f1", SC(15, 0, 0) } }));
    wf = FrameFor(frames, w.key);
    CHECK(wf != nullptr);
    CHECK(AllEqual(wf->colors, 0, 40, RGB(15, 0, 0)));  /* new    */
    CHECK(AllEqual(wf->colors, 40, 40, RGB(14, 0, 0))); /* kept   */
    CHECK(AllEqual(wf->colors, 80, 40, kEnumerated));   /* kept   */
}

/* Stale-generation frames are rejected/dropped (plan section
   6): the mapper stamps the CURRENT generation; the pipeline
   refuses anything stamped older. */
static void TestStaleGenerationRejected()
{
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    DeviceMapper mapper;
    const auto doc = DanielPcDoc();
    mapper.SetScene(doc);

    const auto gen1 = FullTopology(1);
    pipe.SyncDevices(gen1);
    mapper.SyncDevices(gen1);

    const EvaluatedFrame eval = EvalAll(*doc, {
        { "o-case1", SC(1, 2, 3) },
    });
    std::vector<RgbFrame> frames = mapper.MapFrame(eval);
    const DeviceSnapshot mb = gen1[1];
    const RgbFrame* mf = FrameFor(frames, mb.key);
    CHECK(mf != nullptr && mf->topology_generation == 1);

    /* Topology change: generation 2. The pipeline drops the
       pending gen-1 frame on re-registration; a late stale
       submission is rejected outright. */
    const auto gen2 = FullTopology(2);
    pipe.SyncDevices(gen2);
    mapper.SyncDevices(gen2);
    CHECK(pipe.SubmitLatest(*mf).status == SubmitStatus::Rejected);
    CHECK(pipe.PendingCount(mb.key) == 0);

    frames = mapper.MapFrame(eval);
    mf = FrameFor(frames, mb.key);
    CHECK(mf != nullptr && mf->topology_generation == 2);
    CHECK(pipe.SubmitLatest(*mf).status == SubmitStatus::Accepted);
    pipe.DrainAllDue();
    CHECK(transport->Delivered(mb.key).size() == 1);
}

/* A bound device that disappears keeps its binding — never
   reassigned to a same-named unit — and re-resolves when the
   same identity returns (plan section 6). */
static void TestMissingDeviceBindingPreserved()
{
    auto doc = std::make_shared<studio::SceneDocument>();
    doc->bindings = {
        Binding("b-a", "Vengeance RGB", kDimmASerial, "", "", 0),
        Binding("b-b", "Vengeance RGB", kDimmBSerial, "", "", 0),
    };
    doc->objects = {
        DevObj("o-a", "b-a", 0, 10),
        DevObj("o-b", "b-b", 0, 10),
    };

    const DeviceSnapshot da = Dimm(kDimmASerial, kDimmALoc);
    const DeviceSnapshot db = Dimm(kDimmBSerial, kDimmBLoc);
    /* Imposter: same display name, different identity. */
    const DeviceSnapshot imposter = Dimm("SN-FAKE-DIMM-C",
                                         "smbus:0-0099");

    DeviceMapper mapper;
    mapper.SetScene(doc);
    studio::BindingStatus st;

    mapper.SyncDevices({ da, db });
    CHECK(mapper.BindingState("b-a", st) &&
          st == studio::BindingStatus::Resolved);

    /* Device A unplugged: binding goes unresolved, emits no
       writes — but is NOT reassigned to B. */
    mapper.SyncDevices({ db });
    CHECK(mapper.BindingState("b-a", st) &&
          st == studio::BindingStatus::Unresolved);
    std::vector<RgbFrame> frames = mapper.MapFrame(
        EvalAll(*doc, { { "o-a", SC(9, 0, 0) },
                        { "o-b", SC(5, 0, 0) } }));
    CHECK(FrameFor(frames, da.key) == nullptr);
    const RgbFrame* fb = FrameFor(frames, db.key);
    CHECK(fb != nullptr && AllEqual(fb->colors, 0, 10, RGB(5, 0, 0)));

    /* A same-named imposter must not capture the binding. */
    mapper.SyncDevices({ db, imposter });
    CHECK(mapper.BindingState("b-a", st) &&
          st != studio::BindingStatus::Resolved);
    frames = mapper.MapFrame(
        EvalAll(*doc, { { "o-a", SC(9, 0, 0) } }));
    CHECK(FrameFor(frames, imposter.key) == nullptr);

    /* The original identity returns -> re-resolved. */
    mapper.SyncDevices({ imposter, db, da });
    CHECK(mapper.BindingState("b-a", st) &&
          st == studio::BindingStatus::Resolved);
    frames = mapper.MapFrame(
        EvalAll(*doc, { { "o-a", SC(9, 0, 0) } }));
    const RgbFrame* fa = FrameFor(frames, da.key);
    CHECK(fa != nullptr && AllEqual(fa->colors, 0, 10, RGB(9, 0, 0)));
}

/* Task 5.4 — missing-binding integrity through the FULL
   offline-binding chain: a committed name-only scene binding
   is completed by the persisted resolved-identity record at
   apply time (AugmentSceneBindings — the EngineService
   prepare_scene hook); afterwards a same-named device with a
   DIFFERENT serial can never capture the binding or its
   frames, and the record itself is not rewritten by the
   failed resolution. A name-only binding with NO recorded
   identity is the documented first-bind exception: it may
   resolve to a single same-named candidate, never to one of
   several (Ambiguous, never a silent pick). */
static void TestAugmentedIdentityImposterRejected()
{
    /* The persisted identity record, as the engine profile
       holds it after a real session. */
    std::map<std::string, studio::DeviceBinding> record;
    {
        studio::DeviceBinding b;
        b.id              = "b-a";
        b.controller_name = "Corsair Vengeance RGB DDR5";
        b.vendor          = "Corsair";
        b.serial          = kDimmASerial;
        b.location        = kDimmALoc;
        b.zone_name       = "DRAM";
        b.zone_leds       = 10;
        record[b.id]      = b;
    }

    /* The committed scene ships name-only bindings. The
       controller name must equal the recorded identity's —
       the augment refuses to touch a re-purposed binding
       id. b-b carries a committed serial so it stays
       resolved throughout (a second same-named name-only
       binding would sit Ambiguous and pollute the frame
       assertions). */
    auto doc = std::make_shared<studio::SceneDocument>();
    doc->bindings = {
        Binding("b-a", "Corsair Vengeance RGB DDR5",
                "", "", "DRAM", 10),
        Binding("b-b", "Corsair Vengeance RGB DDR5",
                kDimmBSerial, "", "DRAM", 10),
    };
    doc->objects = {
        DevObj("o-a", "b-a", 0, 10),
        DevObj("o-b", "b-b", 0, 10),
    };

    /* Only b-a has a recorded identity — and only b-a is
       name-only, so only b-a is augmented. */
    CHECK(AugmentSceneBindings(*doc, record));
    CHECK(doc->bindings[0].serial == kDimmASerial);
    CHECK(doc->bindings[1].serial == kDimmBSerial);

    const DeviceSnapshot da = Dimm(kDimmASerial, kDimmALoc);
    const DeviceSnapshot db = Dimm(kDimmBSerial, kDimmBLoc);
    /* Imposter: same display name, different identity — the
       unit swapped in for the missing DIMM A. */
    const DeviceSnapshot imposter = Dimm("SN-FAKE-DIMM-C",
                                         "smbus:0-0099");

    DeviceMapper mapper;
    mapper.SetScene(doc);
    studio::BindingStatus st;

    /* The real device present: resolved, frames flow to its
       key. */
    mapper.SyncDevices({ da, db });
    CHECK(mapper.BindingState("b-a", st) &&
          st == studio::BindingStatus::Resolved);
    CHECK(mapper.BindingState("b-b", st) &&
          st == studio::BindingStatus::Resolved);

    /* DIMM A absent, imposter in its slot: b-a goes
       Unresolved — the same-named different-serial device
       never inherits the binding — and emits ZERO frames to
       the imposter's key. */
    mapper.SyncDevices({ db, imposter });
    CHECK(mapper.BindingState("b-a", st) &&
          st == studio::BindingStatus::Unresolved);
    std::vector<RgbFrame> frames = mapper.MapFrame(
        EvalAll(*doc, { { "o-a", SC(9, 9, 9) },
                        { "o-b", SC(4, 4, 4) } }));
    CHECK(FrameFor(frames, imposter.key) == nullptr);
    CHECK(FrameFor(frames, db.key) != nullptr);

    /* The identity record itself is unchanged by the failed
       resolution — a re-apply still pins the ORIGINAL
       serial. */
    CHECK(record["b-a"].serial == kDimmASerial);
    auto doc2 = std::make_shared<studio::SceneDocument>();
    doc2->bindings = {
        Binding("b-a", "Corsair Vengeance RGB DDR5",
                "", "", "DRAM", 10),
    };
    CHECK(AugmentSceneBindings(*doc2, record));
    CHECK(doc2->bindings[0].serial == kDimmASerial);

    /* The real device returns: the same identity re-resolves
       and frames route to it again. */
    mapper.SyncDevices({ imposter, db, da });
    CHECK(mapper.BindingState("b-a", st) &&
          st == studio::BindingStatus::Resolved);
    frames = mapper.MapFrame(EvalAll(*doc, { { "o-a", SC(9, 9, 9) } }));
    const RgbFrame* fa = FrameFor(frames, da.key);
    CHECK(fa != nullptr && AllEqual(fa->colors, 0, 10,
                                    RGB(9, 9, 9)));

    /* Documented exception — first-bind with NO recorded
       identity: the name-only binding resolves to a SINGLE
       same-named candidate (there is nothing stronger to
       pin to; the resolution is then recorded, pinning it
       from the next apply on). Two candidates is Ambiguous —
       never a silent pick. */
    auto doc3 = std::make_shared<studio::SceneDocument>();
    doc3->bindings = {
        Binding("b-x", "Corsair Vengeance RGB DDR5",
                "", "", "", 0),
    };
    doc3->objects = { DevObj("o-x", "b-x", 0, 10) };
    DeviceMapper mapper3;
    mapper3.SetScene(doc3);
    mapper3.SyncDevices({ db });
    CHECK(mapper3.BindingState("b-x", st) &&
          st == studio::BindingStatus::Resolved);
    mapper3.SyncDevices({ da, db });
    CHECK(mapper3.BindingState("b-x", st) &&
          st == studio::BindingStatus::Ambiguous);
    frames = mapper3.MapFrame(EvalAll(*doc3, { { "o-x", SC(1, 1, 1) } }));
    CHECK(FrameFor(frames, da.key) == nullptr);
    CHECK(FrameFor(frames, db.key) == nullptr);
}

/* Per-controller sequence counters are independent and
   monotonic; sample_time passes through from the eval. */
static void TestSequencesAndSampleTime()
{
    DeviceMapper mapper;
    const auto doc = DanielPcDoc();
    mapper.SetScene(doc);
    const auto snaps = FullTopology();
    mapper.SyncDevices(snaps);

    const TimePoint t0 = Clock::now();
    EvaluatedFrame eval = EvalAll(*doc, {
        { "o-fan1", SC(1, 0, 0) },
        { "o-case1", SC(2, 0, 0) },
    });
    eval.sample_time = t0;

    const std::vector<RgbFrame> f1 = mapper.MapFrame(eval);
    const std::vector<RgbFrame> f2 = mapper.MapFrame(eval);

    const DeviceSnapshot w  = snaps[0];
    const DeviceSnapshot mb = snaps[1];
    CHECK(FrameFor(f1, w.key)->sequence == 1);
    CHECK(FrameFor(f1, mb.key)->sequence == 1);
    CHECK(FrameFor(f2, w.key)->sequence == 2);
    CHECK(FrameFor(f2, mb.key)->sequence == 2);
    CHECK(FrameFor(f1, w.key)->sample_time == t0);

    /* Generation change restarts the sequence space — the
       pipeline's guard resets with it. */
    mapper.SyncDevices(FullTopology(2));
    const std::vector<RgbFrame> f3 = mapper.MapFrame(eval);
    CHECK(FrameFor(f3, w.key)->sequence == 1);
}

/* Disconnected controllers are skipped (obsolete frames stop
   on disconnect); the binding stays resolved for return.
   Unbound controllers never get frames at all. */
static void TestDisconnectedAndUnboundSkipped()
{
    DeviceMapper mapper;
    const auto doc = DanielPcDoc();
    mapper.SetScene(doc);
    auto snaps = FullTopology();
    snaps[0].connection = ConnectionState::Disconnected; /* wireless */
    mapper.SyncDevices(snaps);

    studio::BindingStatus st;
    CHECK(mapper.BindingState("b-fan1", st) &&
          st == studio::BindingStatus::Resolved);

    const EvaluatedFrame eval = EvalAll(*doc, {
        { "o-fan1", SC(1, 0, 0) },
        { "o-case1", SC(2, 0, 0) },
        { "o-gpu", SC(3, 0, 0) },
    });
    const std::vector<RgbFrame> frames = mapper.MapFrame(eval);
    CHECK(FrameFor(frames, snaps[0].key) == nullptr);  /* wireless */
    CHECK(FrameFor(frames, snaps[1].key) != nullptr);  /* mb */
    CHECK(FrameFor(frames, snaps[4].key) != nullptr);  /* gpu */

    /* A controller with no bindings produces nothing — the
       mouse appears in snapshots but gets no writes. */
    DeviceSnapshot mouse;
    mouse.key.device   = "Razer:Razer Basilisk V3 Pro";
    mouse.key.location = "hid:mouse";
    mouse.name         = "Razer Basilisk V3 Pro";
    mouse.zones = { { "Wheel", 1 }, { "Logo", 1 }, { "Strip", 11 } };
    mouse.connection = ConnectionState::Connected;
    snaps.push_back(mouse);
    mapper.SyncDevices(snaps);
    const std::vector<RgbFrame> frames2 = mapper.MapFrame(eval);
    CHECK(FrameFor(frames2, mouse.key) == nullptr);
}

/* F04 — auto-verify on resolve: the authored `verified` bit
   does NOT gate output. A binding resolved against live
   hardware makes the object writable (runtime grant — the
   stored document is never mutated); an unresolved binding
   stays unwritable regardless of the flag. */
static studio::SceneObject UnverifiedObj(const std::string& id,
                                         const std::string& binding,
                                         int count)
{
    studio::SceneObject o = DevObj(id, binding, 0, count);
    o.verified = false;
    return o;
}

static void TestAutoVerifyOnResolve()
{
    std::vector<std::string> logs;

    auto doc = std::make_shared<studio::SceneDocument>();
    doc->brightness = 1.0f;
    doc->bindings = {
        Binding("b-h1", "X870E", "", "", "ARGB_V2_1", 8),
        /* Nothing in the fake topology matches this. */
        Binding("b-gone", "No Such Controller", "", "",
                "Zone", 8),
    };
    doc->objects = {
        UnverifiedObj("o-h1",   "b-h1",   8),  /* resolves   */
        UnverifiedObj("o-gone", "b-gone", 8),  /* unresolved */
        DevObj("o-vx",          "b-gone", 0, 8),  /* authored
                                                  verified but
                                                  unresolved —
                                                  still dark */
    };

    DeviceMapper mapper;
    mapper.SetLogger([&logs](const std::string& l)
                     { logs.push_back(l); });
    mapper.SetScene(doc);
    const DeviceSnapshot mb = Motherboard();
    mapper.SyncDevices({ mb });

    /* One apply-summary line: "3 bound device objects,
       1 writable". */
    CHECK(logs.size() == 1);
    CHECK(!logs.empty() &&
          logs[0].find("3 bound device objects") !=
              std::string::npos &&
          logs[0].find("1 writable") != std::string::npos);

    const EvaluatedFrame eval = EvalAll(*doc, {
        { "o-h1",   SC(10, 20, 30) },
        { "o-gone", SC(9, 9, 9) },
        { "o-vx",   SC(9, 9, 9) },
    });
    const std::vector<RgbFrame> frames = mapper.MapFrame(eval);

    /* The resolved-but-authored-unverified object emits. */
    const RgbFrame* f = FrameFor(frames, mb.key);
    CHECK(f != nullptr);
    CHECK(AllEqual(f->colors, 0, 8, RGB(10, 20, 30)));
    /* The unresolved bindings emitted nothing — exactly one
       frame total, and the (9,9,9) color landed nowhere. */
    CHECK(frames.size() == 1);
    CHECK(!AllEqual(f->colors, 0, f->colors.size(), RGB(9, 9, 9)));

    /* The stored document is untouched — the grant is runtime
       mapper state, not a document mutation. */
    CHECK(!doc->objects[0].verified);
    CHECK(!doc->objects[1].verified);
    CHECK(doc->objects[2].verified);            /* o-vx authored true */

    /* LastMapStats accounts the pass. */
    const MapStats st = mapper.LastMapStats();
    CHECK(st.objects_total == 3);
    CHECK(st.objects_bound == 3);
    CHECK(st.objects_writable == 1);
    CHECK(st.objects_emitted == 1);
    CHECK(st.skipped_unverified_or_unresolved == 2);
    CHECK(st.frames_emitted == 1);
    CHECK(st.frames_dropped == 0);

    /* An unresolved-only doc produces zero frames. */
    auto doc2 = std::make_shared<studio::SceneDocument>();
    doc2->bindings = { Binding("b-gone", "No Such Controller",
                               "", "", "Zone", 8) };
    doc2->objects  = { UnverifiedObj("o-gone", "b-gone", 8) };
    DeviceMapper mapper2;
    mapper2.SetScene(doc2);
    mapper2.SyncDevices({ mb });
    CHECK(mapper2.MapFrame(EvalAll(*doc2, {
        { "o-gone", SC(1, 1, 1) } })).empty());
    CHECK(mapper2.LastMapStats().frames_emitted == 0);
    CHECK(mapper2.LastMapStats().skipped_unverified_or_unresolved == 1);
}

/* F04 regression: brightness 0 produces a BLACK frame, not no
   frame — the lights must go dark, never hold the old colors. */
static void TestZeroBrightnessEmitsBlackFrame()
{
    auto doc = std::make_shared<studio::SceneDocument>();
    doc->brightness = 0.0f;
    doc->bindings = {
        Binding("b-h1", "X870E", "", "", "ARGB_V2_1", 8),
    };
    doc->objects = { UnverifiedObj("o-h1", "b-h1", 8) };

    DeviceMapper mapper;
    mapper.SetScene(doc);
    const DeviceSnapshot mb = Motherboard();
    mapper.SyncDevices({ mb });

    const EvaluatedFrame eval = EvalAll(*doc, {
        { "o-h1", SC(200, 100, 50) },
    });
    const std::vector<RgbFrame> frames = mapper.MapFrame(eval);
    const RgbFrame* f = FrameFor(frames, mb.key);
    CHECK(f != nullptr);                        /* frame emitted */
    CHECK(f->colors.size() == 34);
    CHECK(AllEqual(f->colors, 0, 8, RGB(0, 0, 0))); /* bound = black */
    CHECK(f->colors[32] == kEnumerated);        /* unmapped kept */
    CHECK(mapper.LastMapStats().frames_emitted == 1);
}

/* Scene brightness scales mapped writes; preserved colors
   are untouched (they are already device-space). */
static void TestBrightnessScalesMappedOnly()
{
    auto doc = std::make_shared<studio::SceneDocument>();
    doc->brightness = 0.5f;
    doc->bindings = {
        Binding("b-h1", "X870E", "", "", "ARGB_V2_1", 8),
    };
    doc->objects = { DevObj("o-h1", "b-h1", 0, 8) };

    DeviceMapper mapper;
    mapper.SetScene(doc);
    const DeviceSnapshot mb = Motherboard();
    mapper.SyncDevices({ mb });

    const EvaluatedFrame eval = EvalAll(*doc, {
        { "o-h1", SC(200, 100, 50) },
    });
    const std::vector<RgbFrame> frames = mapper.MapFrame(eval);
    const RgbFrame* f = FrameFor(frames, mb.key);
    CHECK(f != nullptr);
    CHECK(AllEqual(f->colors, 0, 8, RGB(100, 50, 25)));
    CHECK(f->colors[32] == kEnumerated);  /* unmapped untouched */
}

/* Out-of-range brightness clamps instead of wrapping: 1.5 used
   to give (unsigned)(200*1.5) & 0xFF = 44; negative was UB. */
static void TestBrightnessClamps()
{
    struct Case { float b; RgbColor want; };
    const Case cases[] = {
        { 1.5f,  RGB(200, 100, 50) },
        { -2.0f, RGB(0, 0, 0) },
        { 1e30f, RGB(200, 100, 50) },
    };
    for(const Case& c : cases)
    {
        auto doc = std::make_shared<studio::SceneDocument>();
        doc->brightness = c.b;
        doc->bindings = {
            Binding("b-h1", "X870E", "", "", "ARGB_V2_1", 8),
        };
        doc->objects = { DevObj("o-h1", "b-h1", 0, 8) };

        DeviceMapper mapper;
        mapper.SetScene(doc);
        const DeviceSnapshot mb = Motherboard();
        mapper.SyncDevices({ mb });
        const std::vector<RgbFrame> frames = mapper.MapFrame(
            EvalAll(*doc, { { "o-h1", SC(200, 100, 50) } }));
        const RgbFrame* f = FrameFor(frames, mb.key);
        CHECK(f != nullptr);
        if(f)
        {
            CHECK(AllEqual(f->colors, 0, 8, c.want));
        }
    }
}

/* R-1: the runtime master scale (OutputControl's effective
   atomic) multiplies into the document brightness on the NEXT
   map — no document edit, no re-apply. Blackout (scale 0)
   still emits a frame per tick (black, same sequence cadence),
   so the LEDs go dark instead of holding the last colors. */
static void TestOutputScaleSource()
{
    auto doc = std::make_shared<studio::SceneDocument>();
    doc->brightness = 1.0f;
    doc->bindings = {
        Binding("b-h1", "X870E", "", "", "ARGB_V2_1", 8),
    };
    doc->objects = { DevObj("o-h1", "b-h1", 0, 8) };

    std::atomic<float> scale{ 0.5f };
    DeviceMapper mapper;
    mapper.SetOutputScaleSource(&scale);
    mapper.SetScene(doc);
    const DeviceSnapshot mb = Motherboard();
    mapper.SyncDevices({ mb });
    const EvaluatedFrame eval = EvalAll(*doc, {
        { "o-h1", SC(200, 100, 50) },
    });

    std::vector<RgbFrame> frames = mapper.MapFrame(eval);
    const RgbFrame* f = FrameFor(frames, mb.key);
    CHECK(f != nullptr);
    uint64_t seq = 0;
    if(f)
    {
        CHECK(AllEqual(f->colors, 0, 8, RGB(100, 50, 25)));
        CHECK(f->colors[32] == kEnumerated);   /* unmapped kept */
        seq = f->sequence;
    }

    /* Blackout: black frame, next sequence number. */
    scale.store(0.0f);
    frames = mapper.MapFrame(eval);
    f = FrameFor(frames, mb.key);
    CHECK(f != nullptr);
    if(f)
    {
        CHECK(AllEqual(f->colors, 0, 8, RGB(0, 0, 0)));
        CHECK(f->sequence == seq + 1);
        seq = f->sequence;
    }
    frames = mapper.MapFrame(eval);
    f = FrameFor(frames, mb.key);
    CHECK(f != nullptr && f->sequence == seq + 1);

    /* Release: full colors on the very next map. */
    scale.store(1.0f);
    frames = mapper.MapFrame(eval);
    f = FrameFor(frames, mb.key);
    CHECK(f != nullptr);
    if(f)
    {
        CHECK(AllEqual(f->colors, 0, 8, RGB(200, 100, 50)));
    }

    /* Document brightness and master multiply; an out-of-range
       document value clamps BEFORE the master applies (1e30 x
       0.5 must still halve). */
    doc->brightness = 0.5f;
    scale.store(0.5f);
    frames = mapper.MapFrame(eval);
    f = FrameFor(frames, mb.key);
    CHECK(f != nullptr);
    if(f)
    {
        CHECK(AllEqual(f->colors, 0, 8, RGB(50, 25, 12)));
    }
    doc->brightness = 1e30f;
    frames = mapper.MapFrame(eval);
    f = FrameFor(frames, mb.key);
    CHECK(f != nullptr);
    if(f)
    {
        CHECK(AllEqual(f->colors, 0, 8, RGB(100, 50, 25)));
    }

    /* Detaching the source restores the identity scale. */
    doc->brightness = 1.0f;
    mapper.SetOutputScaleSource(nullptr);
    frames = mapper.MapFrame(eval);
    f = FrameFor(frames, mb.key);
    CHECK(f != nullptr);
    if(f)
    {
        CHECK(AllEqual(f->colors, 0, 8, RGB(200, 100, 50)));
    }
}

/* E-1 regression: a scene apply must not freeze output until
   the host's next SyncDevices (the 500 ms maintenance poll).
   SetScene resolves the new bindings against the snapshots it
   already holds; a revision-stamped apply keeps mapping the
   previous document until the worker's frame carries the new
   revision, so no tick pairs one document's colors with the
   other's objects. */
static void TestSceneApplyResolvesWithoutResync()
{
    const DeviceSnapshot mb  = Motherboard();
    const DeviceSnapshot gpu = Gpu();

    auto docA = std::make_shared<studio::SceneDocument>();
    docA->brightness = 1.0f;
    docA->bindings = { Binding("b-h1", "X870E", "", "", "ARGB_V2_1", 8) };
    docA->objects  = { DevObj("o-a", "b-h1", 0, 8) };

    auto docB = std::make_shared<studio::SceneDocument>();
    docB->brightness = 1.0f;
    docB->bindings = { Binding("b-gpu", "5080 MASTER", "", "", "", 0) };
    docB->objects  = { DevObj("o-b", "b-gpu", 0, 26) };

    auto docC = std::make_shared<studio::SceneDocument>();
    docC->brightness = 1.0f;
    docC->bindings = { Binding("b-h1", "X870E", "", "", "ARGB_V2_1", 8) };
    docC->objects  = { DevObj("o-c", "b-h1", 0, 8) };

    std::vector<std::string> logs;
    DeviceMapper mapper;
    mapper.SetLogger([&logs](const std::string& l)
                     { logs.push_back(l); });
    mapper.SetScene(docA);
    mapper.SyncDevices({ mb, gpu });
    CHECK(logs.size() == 1);
    CHECK(FrameFor(mapper.MapFrame(EvalAll(*docA,
              { { "o-a", SC(1, 2, 3) } })), mb.key) != nullptr);

    /* Immediate apply (revision 0), NO SyncDevices: the very
       next frame already reaches the new binding. */
    mapper.SetScene(docB);
    studio::BindingStatus bs = studio::BindingStatus::Unresolved;
    CHECK(mapper.BindingState("b-gpu", bs) &&
          bs == studio::BindingStatus::Resolved);
    {
        const std::vector<RgbFrame> frames =
            mapper.MapFrame(EvalAll(*docB, { { "o-b", SC(4, 5, 6) } }));
        const RgbFrame* f = FrameFor(frames, gpu.key);
        CHECK(f != nullptr);
        CHECK(f != nullptr && AllEqual(f->colors, 0, 26, RGB(4, 5, 6)));
        CHECK(mapper.LastMapStats().objects_writable == 1);
    }
    /* One apply-summary line per apply, even with no resync. */
    CHECK(logs.size() == 2);

    /* Staged apply: the control plane sees docC at once, but
       a frame the worker evaluated against the OLD revision
       still maps through docB. */
    mapper.SetScene(docC, /*revision*/ 7);
    CHECK(mapper.Scene() == docC);
    CHECK(mapper.BindingState("b-h1", bs) &&
          bs == studio::BindingStatus::Resolved);
    {
        EvaluatedFrame old = EvalAll(*docB, { { "o-b", SC(8, 8, 8) } });
        old.scene_revision = 6;
        const std::vector<RgbFrame> frames = mapper.MapFrame(old);
        CHECK(frames.size() == 1);
        const RgbFrame* f = FrameFor(frames, gpu.key);
        CHECK(f != nullptr && AllEqual(f->colors, 0, 26, RGB(8, 8, 8)));
    }
    {
        EvaluatedFrame cur = EvalAll(*docC, { { "o-c", SC(9, 1, 2) } });
        cur.scene_revision = 7;
        const std::vector<RgbFrame> frames = mapper.MapFrame(cur);
        CHECK(frames.size() == 1);
        const RgbFrame* f = FrameFor(frames, mb.key);
        CHECK(f != nullptr && AllEqual(f->colors, 0, 8, RGB(9, 1, 2)));
    }
    /* A topology change still re-resolves the swapped scene. */
    mapper.SyncDevices({ gpu });
    CHECK(mapper.BindingState("b-h1", bs) &&
          bs != studio::BindingStatus::Resolved);
}

/* Audit E-4: identify on an UNBOUND controller spends sequence
   numbers in the lane's live space (overlay-only frames). A
   binding applied while the blink runs must continue that
   space — restarting it at 1 in the same topology generation
   made the lane reject every frame as out-of-order until the
   scene caught up with the identify's frame count. */
static void TestIdentifyThenBindKeepsSequence()
{
    auto transport = std::make_shared<FakeTransport>();
    DeliveryPipeline pipe(transport);
    DeviceMapper mapper;

    const DeviceSnapshot mb  = Motherboard();
    const DeviceSnapshot gpu = Gpu();
    pipe.SyncDevices({ mb, gpu });

    /* Scene binds the motherboard only: the GPU is unbound. */
    auto docA = std::make_shared<studio::SceneDocument>();
    docA->brightness = 1.0f;
    docA->bindings = { Binding("b-h1", "X870E", "", "", "ARGB_V2_1", 8) };
    docA->objects  = { DevObj("o-a", "b-h1", 0, 8) };
    mapper.SetScene(docA);
    mapper.SyncDevices({ mb, gpu });

    const TimePoint t0 = Clock::now();
    std::string err;
    CHECK(mapper.StartIdentify(gpu.key, 0, 26,
                               std::chrono::seconds(10), t0, err));

    /* 20 identify frames: the GPU gets overlay-only frames. */
    TimePoint t = t0;
    for(int i = 0; i < 20; ++i)
    {
        t += std::chrono::milliseconds(16);
        EvaluatedFrame eval = EvalAll(*docA, { { "o-a", SC(1, 1, 1) } });
        eval.sample_time = t;
        for(const RgbFrame& f : mapper.MapFrame(eval))
        {
            CHECK(pipe.SubmitLatest(f).status != SubmitStatus::Rejected);
        }
        pipe.DrainAllDue(t);
    }
    /* Identical blink phases are suppressed downstream; the
       lane's sequence high-water is what matters here. */
    CHECK(!transport->Delivered(gpu.key).empty());

    /* The user binds the blinking GPU (set_binding re-applies
       the scene, no topology change), then cancels identify. */
    auto docB = std::make_shared<studio::SceneDocument>();
    docB->brightness = 1.0f;
    docB->bindings = { Binding("b-h1", "X870E", "", "", "ARGB_V2_1", 8),
                       Binding("b-gpu", "5080 MASTER", "", "", "", 0) };
    docB->objects  = { DevObj("o-a", "b-h1", 0, 8),
                       DevObj("o-b", "b-gpu", 0, 26) };
    mapper.SetScene(docB);

    /* Still identifying: the scene frame for the GPU carries the
       blink and must stay admissible. */
    t += std::chrono::milliseconds(16);
    {
        EvaluatedFrame eval = EvalAll(*docB, { { "o-a", SC(1, 1, 1) },
                                               { "o-b", SC(4, 5, 6) } });
        eval.sample_time = t;
        const std::vector<RgbFrame> frames = mapper.MapFrame(eval);
        const RgbFrame* g = FrameFor(frames, gpu.key);
        CHECK(g != nullptr);
        CHECK(g != nullptr && g->sequence > 20);
        CHECK(g != nullptr &&
              pipe.SubmitLatest(*g).status != SubmitStatus::Rejected);
        pipe.DrainAllDue(t);
    }

    CHECK(mapper.CancelIdentify(gpu.key, t));
    for(int i = 0; i < 3; ++i)
    {
        t += std::chrono::milliseconds(16);
        EvaluatedFrame eval = EvalAll(*docB, { { "o-a", SC(1, 1, 1) },
                                               { "o-b", SC(4, 5, 6) } });
        eval.sample_time = t;
        const std::vector<RgbFrame> frames = mapper.MapFrame(eval);
        const RgbFrame* g = FrameFor(frames, gpu.key);
        CHECK(g != nullptr &&
              pipe.SubmitLatest(*g).status != SubmitStatus::Rejected);
        pipe.DrainAllDue(t);
    }
    /* The bound scene colors reached the hardware at once. */
    const std::vector<RgbFrame> sent = transport->Delivered(gpu.key);
    CHECK(!sent.empty() &&
          AllEqual(sent.back().colors, 0, 26, RGB(4, 5, 6)));
}

int main()
{
    TestWirelessGroupBatchesToOneWrite();
    TestReorderedEnumerationKeepsRouting();
    TestIdenticalDimmNamesIdentityResolution();
    TestMirroredFansKeepExactCounts();
    TestUnmappedLedsKeepColors();
    TestStaleGenerationRejected();
    TestMissingDeviceBindingPreserved();
    TestAugmentedIdentityImposterRejected();
    TestSequencesAndSampleTime();
    TestDisconnectedAndUnboundSkipped();
    TestAutoVerifyOnResolve();
    TestZeroBrightnessEmitsBlackFrame();
    TestBrightnessScalesMappedOnly();
    TestBrightnessClamps();
    TestOutputScaleSource();
    TestSceneApplyResolvesWithoutResync();
    TestIdentifyThenBindKeepsSequence();

    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
