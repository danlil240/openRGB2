/*---------------------------------------------------------*\
|| StudioScenePreparation.h                                  |
||                                                           |
||   Engine-side prepared scene (studio-import Task 6):     |
||   the shared, hardware-aware scene state BOTH the        |
||   SceneWorker output path and the Task 7 preview drive.  |
||                                                           |
||   PrepareStudioScene turns the authored workspace        |
||   candidate into the resolved runtime form:              |
||     - hardware-dependent geometry: "matrix_map" device   |
||       objects get emitters rebuilt from the BOUND zone's |
||       live matrix map (the source plugin's              |
||       SceneBridge::rebuildMatrixLayouts)                 |
||     - runtime effect stack: the authored inline layers   |
||       or the named look resolved through the candidate's |
||       own effect-registry snapshot, with global          |
||       speed/intensity and the persisted ripple-decay     |
||       applied to the COPY (SceneBridge::rebuildEffect)   |
||     - key_positions: VK -> emitter world position, from  |
||       the bound keyboard's real LED names                |
||       (SceneBridge::rebuildKeyLookup)                    |
||                                                           |
||   Authored data is NEVER mutated: emitters are expanded  |
||   on a document copy and layers decay on a vector copy,  |
||   so repeated prepares of the same candidate cannot      |
||   compound the decay multiplier.                         |
||                                                           |
||   SPDX-License-Identifier: GPL-2.0-or-later               |
\*---------------------------------------------------------*/

#pragma once

#include "EngineTypes.h"

#include "config/WorkspaceCandidate.h"
#include "scene/SceneTypes.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace openrgb2
{

struct PreparedStudioScene
{
    /* The scene the worker/preview evaluate. Aliases the
       candidate's resolved document when no hardware-
       dependent geometry needed expanding; otherwise a copy
       with matrix emitters rebuilt against the bound zone's
       live matrix. */
    std::shared_ptr<const studio::SceneDocument> doc;

    /* Runtime layer stack — authored layers or the resolved
       look, WITH global speed/intensity and inputs.decay_pct
       ripple-density scaling already applied. Never the
       authored objects. */
    std::vector<studio::EffectLayer>             layers;

    /* VK code -> emitter world position, built from the bound
       keyboards' real LED names ("Key: Q" ...) — the hardware
       layout, not a guessed grid. First emitter wins when two
       LEDs share a name (source first-match parity via
       std::map::emplace). */
    std::map<int, studio::Vec3>                  key_positions;
};

/*---------------------------------------------------------*\
|| PrepareStudioScene — build the shared prepared form.   ||
||                                                          ||
||   `cand` supplies the authored document (inputs.decay_  ||
||   pct, effect registry snapshot) and the resolved scene ||
||   (bindings, objects, effect state). `snapshots` are the||
||   current device enumeration — bindings resolve by      ||
||   persisted identity (studio::ResolveBinding), never by ||
||   controller index.                                     ||
||                                                          ||
||   Behavior (source parity):                             ||
||   - matrix_map objects whose binding resolves get       ||
||     emitters from layout::KeyboardMatrix over the bound ||
||     zone's live matrix map; empty cells (0xFFFFFFFF)    ||
||     produce no emitter; an unresolved/missing keyboard  ||
||     leaves the authored emitters untouched              ||
||   - matrix cells naming a LED index >= the zone's       ||
||     led_count reject the prepare (malformed hardware    ||
||     data is never silently truncated)                   ||
||   - key_positions transform emitter locals through      ||
||     ResolveWorldMatrices — a moved/rotated keyboard or  ||
||     a moved parent group ripples keys from where they   ||
||     actually are; rerun after any transform/topology    ||
||     change                                              ||
||   - runtime layers take the authored inline stack first,||
||     else cand.effect_registry resolves `effect.preset`; ||
||     ApplyGlobalParams runs BEFORE the decay multiplier  ||
||     (decay_pct/100 scales ONLY ripple-layer density)    ||
||                                                          ||
||   Returns false + error on a null resolved document or  ||
||   malformed hardware matrix data. Never throws; never   ||
||   mutates `cand`.                                       ||
\*---------------------------------------------------------*/
bool PrepareStudioScene(const WorkspaceCandidate& cand,
                        const std::vector<DeviceSnapshot>& snapshots,
                        PreparedStudioScene& out,
                        std::string& error);

/*---------------------------------------------------------*\
|| PositionStudioInput — enrich transient key events with ||
|| the prepared scene's key_positions. Unmatched key       ||
|| events keep has_pos == false so the layer-origin        ||
|| fallback stays active (source parity). Other event      ||
|| fields pass through untouched; nothing is retained —    ||
|| typed-text history never leaves the returned copy.     ||
\*---------------------------------------------------------*/
studio::InputState PositionStudioInput(
    const studio::InputState& in,
    const PreparedStudioScene& scene);

} /* namespace openrgb2 */
