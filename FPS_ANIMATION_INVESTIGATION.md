# FPS Arms Animation Corruption — Investigation Notes

Branch: `feature/fps-first-person-animation`

## RESOLVED — root cause found (read this first)

The "fan of blades" tear described below is **fixed**, and the fix is a three-line
change in one place: `Model::ProcessMesh` skipped the owning node's world transform
for **skinned** meshes (`if (!skinned)`) while Assimp's `mOffsetMatrix` values expect
root-space vertices. Every probe in this document compared **bone transforms** against
Blender and never measured **vertex placement**, which is exactly where the bug was —
so they were all correct and all irrelevant to the actual defect.

```cpp
const glm::mat4& bake = nodeTransform;   // skinned or not, always
```

**Do not "cancel out" `C = RestGlobal · BoneOffset`.** It is the node tree's
pose-versus-bind delta, not a residual error term (bind pose ⇒ `C == I`; the clip FBXs
carry a frame-0 pose ⇒ `C` varies; the weapon's base node tree carries the ADS pose ⇒
`C ≈ translate(-0.069, 1.503, 0.446)`, the gun-socket delta). Applying `C⁻¹` erased the
pose already baked into the node tree and dropped the weapon to the model origin.

Two further first-person-presentation fixes landed on the same branch (camera anchored
on the `head` bone instead of the model root, and a `viewRotation` of `[0, 180, 0]` on
the `.fpsanim` because the rig faces model `+Z` while the camera looks down `-Z`).
Full write-up, asset pipeline, runtime map, probe how-to and known gaps:

> **`FPS_ANIMATION_SYSTEM.md`**

This document exists because an extensive debugging session could not find the
root cause of a real, confirmed visual bug before running out of productive
leads. It's written so a fresh AI session (or human) can pick this up without
re-doing everything already ruled out below. Read the "Ruled out" section
carefully before forming new hypotheses — a lot of the obvious ones are dead
ends that were each checked with hard evidence, not guesswork.

## The bug, precisely

The FPS view-model arms (`SK_Manny_Arms`, a UE5-Manny-rig-based mesh, 20,326
verts / 52 vertex groups) render **correctly** when no animation clip is
playing (bind pose). The moment **any** animation clip is attached and played
— tested primarily with the `Idle` state — the mesh deforms into a severely
torn, "fan of thin blades" shape radiating from roughly the shoulder/elbow
area. This is not a subtle skinning artifact; it's a dramatic, obviously-wrong
tear, confirmed independently by the user (not just an AI misreading a
screenshot) and confirmed from multiple camera distances/angles (not a
near-plane clipping illusion).

The weapon mesh (`aks74u`, driven by its own simple 9-bone `AK` armature —
`root`, `mag_release`, `bolt`, `trigger`, `magazine`, `fire_selector`,
`stock`, `rear_sling_loop`, `mag2`) is **not** affected by this bug and
renders correctly at all times. Only the arms, which use the complex
263-bone Manny rig, are affected.

## Context on the rig

Per the user: this rig ("was made for IK and stuff") was originally used in
**Unity via Kinemation's Character Animation System (CAS) with its FPS
add-on**. The `.blend` source file
(`C:\Users\jacob\OneDrive\Desktop\AKS-74U 60fps (Revised).blend`) contains:

- `SK_Manny_Arms` — the mesh, 52 vertex groups (bare deform-bone names only,
  e.g. `upperarm_twist_01_r`, `lowerarm_r`, `clavicle_r`, finger bones —
  no `CB_`/`REF_` prefixed groups).
- `Armature` — 263 bones total: ~89 bare deform bones (`upperarm_r`,
  `lowerarm_r`, `hand_r`, `spine_01`-`05`, etc. — parented anatomically,
  e.g. `hand_r → lowerarm_r → upperarm_r → clavicle_r → spine_05 → ... →
  root`), plus a matching set of `CB_`-prefixed **control** bones and
  `REF_`-prefixed **reference** bones.
- Deform bones (`upperarm_r`, `lowerarm_r`, `hand_r`, etc.) carry **Copy
  Transforms constraints** targeting the `Armature` object itself (a
  self-referencing control-rig setup). Confirmed directly: `upperarm_r`'s
  `pose.matrix_basis` is identity (zero raw keyframes on the deform bone
  itself) at any frame of `A_FP_ADS`, yet its fully-evaluated `pose.matrix`
  differs drastically from its rest matrix — the pose is 100% coming from
  the constraint solve, not from keys on the deform bone.
- The **actions** (`A_FP_Idle`, `A_FP_ADS`, `A_FP_Walk`, etc.) are keyed
  entirely on `CB_*` control bones (confirmed for `A_FP_Idle`: 31 animated
  bones, every single one prefixed `CB_` — `CB_ik_hand_r`, `CB_ik_hand_l`,
  `CB_pole_elbow_l`, finger `CB_*` bones, `CB_Gun`, etc. — **zero** keys on
  any bare deform bone). The exported per-state `.fbx` files
  (`project/assets/fps/AKS74U/FirstPerson/AKS-74U_A_FP_*.fbx`) only contain
  the 89 bare deform bones with **baked** keyframes (action name
  `Armature|Scene`, the tell-tale sign of Blender's "Bake Animation" export
  option, which flattens constraint-solved motion onto the deform bones and
  drops the control/reference bones entirely).

This means the FBX files the engine actually loads are already a **baked**
representation with no dependency on the original constraint rig — in
principle a self-contained, portable FK animation. Whether that bake was
produced *correctly* is one of the leads that was never fully closed (see
"Most promising next step" below).

## Root causes found and fixed (confirmed real, kept in the code)

### 1. Base mesh/skeleton file baked from a posed action, not rest pose

`AKS74U.fpsanim` uses `AKS-74U_A_FP_ADS.fbx` as the **base** arms
mesh+skeleton file (`armsModel`). It was originally exported as a single
static frame of the `A_FP_ADS` action (frame range `(0,0)` — an
aim-down-sights *pose*, not a neutral rest pose). Because the deform bones
are constraint-driven (see above), that "static" frame actually froze the
**posed** (aiming) skeleton into the file's node hierarchy — while the
mesh's skin-bind data (`aiBone::mOffsetMatrix`, computed by Blender against
the true rest pose) still assumed a neutral rest skeleton. The engine's bind
pose computation
(`Model.cpp`, `BindPoseBones[id] = GlobalInverseTransform * ImportNodeGlobal(bone) * OffsetMatrix`)
was therefore multiplying a **posed** global transform against a
**rest-relative** inverse-bind matrix — garbage, even with zero animation
clips attached.

**Fix applied**: re-exported `AKS-74U_A_FP_ADS.fbx` from Blender with the
armature's `pose_position` forced to `'REST'` and `bake_anim=False` (so the
static node hierarchy now reflects the true rest pose, matching the mesh's
skin bind data). Mesh source object: `SK_Manny_Arms`, no materials (out of
scope per user — untextured rendering is expected and fine for now). This
was done via `mcp__blender__execute_blender_code`, entirely in-memory —
`pose_position` and materials were restored on the live object afterward,
and the `.blend` file was **never saved to disk**.

**Confirmed fixed**: before this fix, even the bind-pose-only render (no
clip attached, via the Inspector's "Stop Animation" button on the
`[Runtime] First Person Arms` entity) was mangled. After this fix, bind
pose renders as two clean, correctly-shaped, recognizable arms/hands.

If this needs to be redone (e.g. a subsequent re-export of the mesh for any
reason), the two flags that matter are `pose_position = 'REST'` on the
`Armature` object's `.data`, and export with `bake_anim=False` — see the
weapon file (`AKS-74U_A_W_ADS.fbx`) too if the weapon side is ever revisited
(it was checked and found NOT to need this fix — its `AK` armature has no
control-rig constraint layer, just simple `LIMIT_LOCATION`/`LIMIT_ROTATION`
constraints on `bolt`/`trigger` for their own local animation).

### 2. Vertex weight truncation without renormalization or largest-weight selection

`Model::ExtractBoneWeights` (`src/Renderer/Model.cpp`) fills each vertex's
`BoneIDs`/`Weights` (`MAX_BONE_INFLUENCE = 4` slots) by iterating
`aiMesh::mBones` and, for each vertex a bone influences, writing into the
**first empty slot**. Two related bugs:

- If a vertex has more than 4 real influences (common right where a twist
  bone blends in — `upperarm_r` and `upperarm_twist_01_r`/`02_r` all
  influence nearby vertices), the 5th+ influence was **silently dropped**
  with no renormalization of the remaining weights. Measured on the arms
  mesh (20,411 verts after import): **1,936 vertices** had a weight sum
  more than 0.02 away from 1.0, worst case summing to only **0.686**.
- Worse: which 4 influences got *kept* depended on `aiMesh::mBones`
  iteration order (essentially arbitrary/alphabetical-ish bone order), not
  weight magnitude — a vertex's **dominant** bone could be arbitrarily
  evicted in favor of a minor one just because it was processed later.

**Fix applied** (`ExtractBoneWeights`, `src/Renderer/Model.cpp`): when a
vertex is already at 4 kept influences and a new weight arrives, it now
replaces the **smallest** currently-kept weight if the new one is larger
(instead of being dropped unconditionally). After all bones are processed,
every vertex's kept weights are renormalized to sum to 1.0.

**This fix is real and independently confirmed** (the badSum/worstSum
numbers above were measured directly), and should stay in the code
regardless of the remaining bug below — but **it had zero visible effect**
on the "fan of blades" corruption. Screenshots taken before and after this
fix, at the same camera position, were pixel-identical. So this is not (or
not solely) the cause of the remaining bug.

## The unresolved bug

With fix #1 applied, bind pose is clean. The moment the `Idle` clip plays
(attached via `Model::AttachClip` from `AKS-74U_A_FP_Idle.fbx` onto the
now-fixed `AKS-74U_A_FP_ADS.fbx` base), the mesh tears into the same
"fan of blades" shape as before fix #1 — visually similar to the original
bug report, but now proven **not** to be the base-file rest-pose issue,
since bind pose is independently verified clean on the exact same loaded
model.

## Things verified correct / ruled out (do not re-check these without new evidence)

All of the following were checked via temporary `Log::Warn` diagnostics
added to `src/Renderer/Model.cpp`, rebuilding, running in the editor's Play
mode, and reading the Console panel (`Window → Console` in the editor — far
better than the notification bell popover, which truncates long lines).
**All diagnostic code has since been removed**; the file only contains the
two real fixes described above. `grep -c "DIAG" src/Renderer/Model.cpp`
should return `0`.

- **Every individual bone's computed GLOBAL transform during `Idle`
  playback is numerically sane.** Checked: `root`, `pelvis`, `spine_01`
  through `spine_05`, `clavicle_r`, `upperarm_r`, `upperarm_twist_01_r`,
  `upperarm_twist_02_r`, `lowerarm_r`, `lowerarm_twist_01_r`, `hand_r`,
  `index_metacarpal_r`, `index_01_r`. Every one has an orthonormal rotation
  submatrix (all three column lengths == 1.0 — no scale corruption, no
  NaN) and forms a smooth, continuous, anatomically-correct positional
  chain (e.g. `upperarm_twist_01_r`'s global position sits positionally
  between `upperarm_r` and `lowerarm_r`, exactly as expected for a twist
  bone at roughly the midpoint of the upper arm segment).
- **The node hierarchy is complete.** `m_D->Nodes` has 266 entries for the
  arms model (263 armature bones + a few wrapper nodes), `BoneCounter=52`
  matching the mesh's 52 vertex groups exactly. (An earlier false alarm —
  "only 12 nodes, twist bones missing entirely" — was a diagnostic-scoping
  bug: a `static bool` latch shared across *all* `Model` instances got set
  by the **weapon**'s 12-node/9-bone `AK` rig before the arms model's own
  data could log. Once correctly scoped to `nodes.size() > 50`, both
  `upperarm_twist_01_r` (`boneId=50`) and `upperarm_twist_02_r`
  (`boneId=51`) were confirmed present with valid `hasChan=Y`.)
- **`Model::AttachClip`'s name-based channel matching is the exact same
  code path already used successfully by the pre-existing, working
  Mixamo/ybot character animations** (`project/animations/ybot_showcase.controller`,
  `assets/characters/ybot/*.fbx`, gitignored per Mixamo license so not
  directly byte-inspectable in this repo, but the code path is provably
  identical). This is not a structural difference between the working and
  broken pipelines.
- **`AnimationCount()` correctly includes attached (external) clips**, so
  `Model::UploadBoneMatrices`'s `posed` flag correctly evaluates `true` and
  selects the animated palette (`m_FinalBoneMatrices`), not the bind-pose
  one. Confirmed via diagnostic at the actual upload call site:
  `posed=Y usingFinal=Y count=52`, with a sane (non-garbage, small-magnitude)
  uploaded skinning matrix for bone id 50.
- **Not a camera near-plane-clipping artifact.** Verified by viewing the
  mesh in the Scene editor from multiple distances, including well outside
  any plausible clipping range — the torn shape persists identically at
  all distances/angles tested. (Earlier in this same session, a *different*
  screenshot was misdiagnosed as clipping and the user rightly pushed back
  hard on that — this specific "fan of blades" shape has since been
  re-verified as real corruption, not a repeat of that mistake.)
- **`ModelVertex::BoneIDs` defaults correctly** to `{-1,-1,-1,-1}`
  (`src/Renderer/ModelVertex.h`) — ruling out a "slot 0 gets spurious
  default weight" theory.
- **The initial bind-pose → `Idle` crossfade genuinely completes and
  clears.** `AKS74U.fpsanim`'s `Idle` state has `"fade": 0.12` (seconds).
  Diagnostic confirmed, after 2 full real seconds of Play time:
  `fadeDuration=0.000000 fadeElapsed=0.132545 fromClip=-1` — not stuck in
  a permanent/partial blend between bind pose and the animated pose.
- **The vertex shader's skinning code**
  (`src/Renderer/shaders/ModelVertex.glsl`) is standard, correct
  weighted-blend skinning (`skinMat += uBones[...] * weight`, with a sane
  `totalWeight <= 0.0001 → identity` fallback). Nothing suspicious on
  inspection.
- **`UploadBoneMatrices` and the corresponding `Draw()` calls are tightly
  paired per-model** at every call site (`src/Renderer/Model.cpp:1211/1220`,
  `1328/1345`, `1350/1385` roughly — line numbers will have shifted after
  edits, search for `UploadBoneMatrices`) — ruling out a shared-SSBO
  interleaving race between the arms and weapon models (they share one
  process-lifetime SSBO, `g_BoneSsbo`, uploaded fresh immediately before
  each model's own draw).

## UPDATE: the decisive test was run — the bone/skeleton pipeline is proven correct end-to-end

Two follow-up tests were run after the section below was originally written
(it's left intact below since the reasoning is still valid background).

**Test 1 — play `A_FP_Idle` live on the rig in Blender, zero export/import
involved.** Result: clean. At frame 64 (mid-clip), the deformed mesh has a
normal bounding box for an arm (55.6 × 81.5 × 33.4 cm) and no wildly
stretched edges anywhere (max edge length 2.3cm, 99th percentile 1.4cm,
mean 0.5cm, across the whole mesh). This rules out "the original bake is
broken" — the source animation data is genuinely fine.

**Test 2 — byte-level comparison between Assimp's parse of the exported FBX
and Blender's live evaluation.** A standalone tool
(`work/assimp_probe.cpp`) was written that links directly against the
engine's own already-built Assimp static library
(`build/_deps/assimp-build/lib/Release/assimp-vc145-mt.lib`), loads
`AKS-74U_A_FP_ADS.fbx` (base) and `AKS-74U_A_FP_Idle.fbx` (clip) with the
exact same import flags `Model.cpp` uses (`GlobalScale=1.0`,
`AI_CONFIG_IMPORT_FBX_PRESERVE_PIVOTS=false`, `Triangulate | FlipUVs |
GlobalScale | GenSmoothNormals | CalcTangentSpace | LimitBoneWeights |
JoinIdenticalVertices | OptimizeMeshes`), and replicates
`Model::AttachClip`/`Model::EvaluatePose`'s exact math (name-match
channels onto the base hierarchy, sample at a given tick, compose through
the parent chain) — entirely independent of the running engine, editor, or
any UI interaction. It was compiled directly with MSVC against the
project's existing build artifacts:

```
"C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cl /EHsc /std:c++17 /MD ^
  /I"build\_deps\assimp-src\include" /I"build\_deps\assimp-build\include" /I"build\_deps\glm-src" ^
  work\assimp_probe.cpp /Fe:work\assimp_probe.exe ^
  /link /LIBPATH:"build\_deps\assimp-build\lib\Release" /LIBPATH:"build\_deps\assimp-build\contrib\zlib\Release" ^
  assimp-vc145-mt.lib zlibstatic.lib
```

(Note: must be `/MD`, not `/MT` — the prebuilt Assimp lib is `MD_DynamicRelease`
despite the `-mt` in its filename; that suffix is just Assimp's own naming
convention and does not indicate the runtime library it was built against.)

It was sampled at tick=64 (matching Blender's frame 64, since this rig's
exported `mTicksPerSecond=60` = 1 tick per frame) for the same 10 bones
checked earlier in the session (`root`, `pelvis`, `spine_05`, `clavicle_r`,
`upperarm_r`, `upperarm_twist_01_r`, `upperarm_twist_02_r`, `lowerarm_r`,
`lowerarm_twist_01_r`, `hand_r`). Blender's live rig evaluation at the same
frame was fetched separately (`arm.matrix_world @ pose_bone.matrix`,
world-space translation) and converted from Blender's native Z-up/cm
convention to the engine's Y-up/m convention
(`(bx, by, bz)_blender_cm → (bx, bz, -by) / 100`, matching Blender's
default FBX export axis/unit convention) for direct comparison.

**Every single bone matched to 3-4 decimal places.** E.g. `clavicle_r`:
Blender gives `(-0.0447, 1.5056, 0.0141)`, the standalone Assimp probe
gives `(-0.0447, 1.5056, 0.0141)`. Same order-of-magnitude agreement for
`upperarm_r`, both twist bones, `lowerarm_r`, and `hand_r`.

**This proves, independent of the running engine, that the source bake,
the FBX export, and Assimp's parsing of it are all correct** — the same
conclusion the earlier bone-by-bone `Log::Warn` diagnostics inside the
running engine had already reached (see "Things verified correct" below),
now independently corroborated by a completely separate code path (a
standalone program, not the engine) and an external ground truth
(Blender's own live evaluation, not just Blender's reconstruction of the
exported file).

> **AMENDED (resolved):** the conclusion in this paragraph — "very likely not in
> bone-transform math at all" — was right, but both remaining candidates were the
> wrong shape. The bug was in `Model::ProcessMesh`: it baked `nodeTransform` into
> static meshes only, so skinned vertices stayed in mesh-local space while the
> palette expected root space. See `FPS_ANIMATION_SYSTEM.md` §3.

**What this means for whoever continues:** the skeletal transform pipeline
— hierarchy, channel matching, per-bone global composition — is about as
thoroughly proven correct as is practical without a GPU capture. The
remaining bug is very likely **not** in bone-transform math at all. The
two live candidates left are:

1. Something in the real `Model.cpp`/`Model.h` code path that this
   standalone replica didn't capture (it's a simplified reimplementation,
   not the actual engine code — e.g. it doesn't replicate blending,
   `BoneOffset`/`BindPoseBones` computation, or anything past the raw
   global-transform composition). Worth literally instrumenting the real
   `EvaluatePose()` to log the same 10 bones at the same tick and diffing
   against these numbers directly, rather than trusting the replica any
   further.
2. Something in the **vertex/mesh data** pipeline rather than skeletal
   math — i.e. not "is bone X's matrix correct" (proven yes) but "does
   vertex Y actually get multiplied by the *right* bone's matrix, with the
   *right* weight." The two weight-truncation fixes already applied
   (largest-weight selection + renormalization) were verified real bugs
   but had zero visible effect on the corruption — so if it is a
   vertex-weight issue, it's a different one than those two.

The standalone probe tool is left at `work/assimp_probe.cpp` (not built by
the main CMake project — compile it ad hoc as shown above) since it's a
fast, UI-free way to sanity-check any bone's transform against Blender
without touching the running engine at all.

## UPDATE 2: three more hypotheses tested and killed; the vertex shader was inspected and looks clean

Three additional standalone probes (same technique as `work/assimp_probe.cpp` —
link directly against the project's prebuilt Assimp static lib, no engine/editor
involved) were written and run to close the remaining gaps UPDATE 1 left open.
All three came back clean, further narrowing (but not yet finding) the bug.

**`work/bone_match_probe.cpp`** — checks `Model::AttachClip`'s name-matching
rule (exact string match between a clip channel's bone name and a node name in
the base skeleton) for **every** channel in 5 different clips (`Idle`, `Walk`,
`Sprint`, `Fire`, `Draw`), not just a 10-bone sample. Result: **89/89 channels
matched in all 5 clips, zero unmatched names.** Rules out a partial/silent
name-mismatch (e.g. a handful of bones — say, specific twist bones — quietly
falling back to bind pose while everything else animates, which would tear the
mesh exactly at the seam and wouldn't trip the existing `matched == 0` or
`matched * 2 < total` warnings in `AttachClip`).

**`work/dup_name_probe.cpp`** — checks for duplicate node names in the base
skeleton's 266-node tree. This was a serious hypothesis: `Model::ImportFromFile`
builds *two* independent name-keyed representations of the same tree —
`m_ImportNodeGlobals` (a `std::unordered_map`, populated via `emplace`, which
**keeps the first occurrence** of a duplicate key) for `BindPoseBones`, versus
`m_D->Nodes` (a flat array, walked in the same DFS order, but where
`EvaluatePose` **overwrites** `m_FinalBoneMatrices[boneId]` on every matching
node) for the animated palette. If any bone name were duplicated anywhere in
the 266-node tree, bind pose and animated pose could legitimately read two
different nodes' transforms for the "same" bone ID — bind pose using the first
occurrence, animated pose using the last. This would exactly explain "bind pose
clean, animated pose torn" without contradicting the earlier tick=64 spot-check
(if the specific bones sampled then happened not to collide). Result: **266
total nodes, 266 unique names, zero duplicates.** Hypothesis dead.

**`work/full_skin_probe.cpp`** — extends the byte-level comparison from 10
sampled bones to **all 52 actually-skinned bones** (the full set referenced by
`aiMesh::mBones` across the model, i.e. every bone `ExtractBoneWeights` would
register), computing the *complete* final skin matrix
(`GlobalInverseTransform * NodeGlobal * BoneOffset` — not just the raw node
global checked in UPDATE 1) at tick=64. Result: **all 52 bones produce clean
rigid transforms** — every rotation submatrix has all three column lengths
exactly `1.0000` (no scale corruption, no shear, no NaN/Inf), and every
translation is a small, sane, anatomically-plausible number. Notably,
`upperarm_l`/`upperarm_r` themselves carry **no vertex weights at all** — only
`clavicle_*`, the twist bones, `lowerarm_*`, `hand_*`, and finger bones do —
so the upper-arm segment's shape is controlled entirely by the twist bones,
which were already confirmed matching Blender exactly in UPDATE 1. Full output
saved at `work/full_skin_probe_out.txt` for reference.

Taken together with UPDATE 1's Blender-live-rig test and 10-bone comparison,
this means: the source bake is clean, every clip's every channel resolves to
the correct bone by name, there are no hidden name collisions, and the fully
composed CPU-side skin matrix for *every single skinned bone* is numerically
correct at the sampled tick. The CPU-side skeletal pipeline is about as
exhaustively verified as it can be without literally running the real engine's
own `EvaluatePose()` and diffing its output byte-for-byte (candidate #1 from
UPDATE 1 — still not done; would still be worthwhile if everything else below
comes up empty, since it's the one remaining "is the standalone replica
faithful to the real code" gap).

**The vertex shader (`src/Renderer/shaders/ModelVertex.glsl`) was read in
full** as the next place to look, since the CPU-side matrices feeding it are
now heavily verified. On inspection it looks correct: standard weighted-blend
skinning (`skinMat += uBones[clamp(aBoneIDs[i], 0, uBones.length()-1)] *
aWeights[i]`, summed across 4 slots, `totalWeight <= 0.0001 → identity`
fallback so unweighted vertices don't collapse to the origin), matching
`ModelVertex.h`'s documented default of `BoneIDs = {-1,-1,-1,-1}` (a slot with
`aBoneIDs[i] < 0` is simply skipped in the shader loop — no out-of-bounds read,
no silent garbage term). Nothing suspicious was found by inspection alone; this
has **not** been confirmed against an actual GPU capture, so it's "looks
correct on read" rather than "proven correct" the way the CPU-side bone math
now is.

> **AMENDED (resolved):** "the CPU-side skeletal pipeline is about as exhaustively
> verified as it can be" overstated what was covered. What was exhaustively verified
> was **bone transforms**: hierarchy, channel matching, and every composed skin matrix.
> **Vertex placement** was never measured by any probe — no probe checked where a
> vertex ends up after `ProcessMesh`. That was the defect: skinned meshes skipped
> `nodeTransform`, so vertices stayed in mesh-local space while Assimp's offsets
> assume root space. It is silent at bind (the palette collapses to one matrix) and
> catastrophic under any clip. Fixed as `bake = nodeTransform`; see
> `FPS_ANIMATION_SYSTEM.md` §3.

**What's left, ranked by how promising it looks right now:**

1. **The SSBO upload/binding path** (`Model::UploadBoneMatrices`,
   `src/Renderer/Model.cpp` — search for `g_BoneSsbo`). This is the one
   remaining link between "CPU has the right matrices" (proven) and "shader
   reads the right matrices" (assumed, not proven) that hasn't been directly
   instrumented or captured. It's a single process-lifetime SSBO shared by
   every model, re-uploaded via `glNamedBufferSubData(g_BoneSsbo, 0, count *
   sizeof(mat4), palette.data())` immediately before each model's draw call,
   with `count = clamp(BoneCounter, 1, MAX_BONES)`. This was reasoned about
   (not GPU-captured) in UPDATE 1's "ruled out" list as "tightly paired
   per-model at every call site" — that reasoning holds for *ordering*
   (upload-then-draw, no interleaving), but the actual **byte contents** of
   what lands in the buffer at draw time have never been directly captured
   and compared against the CPU-side `m_FinalBoneMatrices` this session
   computed by hand. Worth a RenderDoc capture (see "Other leads" below,
   already flagged but still not done) or a direct post-`glNamedBufferSubData`
   readback+log for one frame.
2. Actually instrumenting the real `Model::EvaluatePose()` to log the same
   52-bone table `full_skin_probe.cpp` produces, at the same tick, from
   *inside the running engine*, and diffing byte-for-byte against
   `work/full_skin_probe_out.txt`. This closes candidate #1 from UPDATE 1
   for good — either it matches (pipeline fully exonerated, bug is 100%
   downstream of `m_FinalBoneMatrices`) or it doesn't (found it).
3. A GPU capture remains the most direct way to settle this once and for
   all — see "Other leads" below.

## UPDATE 3: a second, unrelated bug — the left hand swings during `A_FP_Idle`

Reported after the socket/FOV work: *"the left hand is moving very strange during idle."*
This is **not** the tear above — the skeleton, skinning and pose evaluation are all
correct (re-verified: every one of the 52 skinned bones is driven, no frozen child of a
moving parent, loop seam ≤0.06°). It is an **asset-side** defect introduced at export
time, and it has nothing to do with `Model::ProcessMesh`.

**Mechanism.** `CB_ik_hand_l` carries `CHILD_OF -> AK:magazine` at influence 1.0, so the
whole left arm is parented to the **weapon** armature's magazine bone. `bake_anim`
therefore samples the weapon as much as the arms — but `work/export_clip.py` only ever
assigned the *arms* action, so every export ran with the weapon still on whatever the
`.blend` was last saved on: `A_W_Tac_Reload`. Its magazine starts moving around frame 44,
inside the idle's 0..128 range, so the baked left hand chased a magazine being pulled out
of the gun while the right hand (parented to `CB_Gun`, keyed directly in the action) sat
perfectly still.

**Evidence.** `work/bl_idle_probe.py` / `bl_idle_probe2.py` evaluate `A_FP_Idle` inside
Blender and measure world-space path per bone, left vs right:

| weapon action during the bake | `lowerarm_l` path | `lowerarm_r` path |
|---|---|---|
| `A_W_Tac_Reload` (the file's saved state) | **25.175** | 0.184 |
| `A_W_ADS`, or weapon animation off | **0.141** | 0.184 |

The shipped FBX matched the first row, not the second (`work/hand_probe.cpp`:
`lowerarm_l` 266.2 mm against `lowerarm_r` 0.0 mm — roughly 100:1 left/right, with
`upperarm_l` carrying 68 rotation keys against `upperarm_r`'s 2). The right arm being
*dead still* while the left travels 71 cm in aggregate is what made it read as "strange"
rather than merely asymmetric.

**Fix.** `work/export_clip.py` derives the weapon action from the arms action
(`A_FP_x -> A_W_x`, falling back to `A_W_ADS` when no weapon counterpart exists — which
is precisely what `AKS74U.fpsanim` implies for Idle/Walk/Aim/Draw/Regrip), pins the
weapon's NLA off for the export, and restores both armatures in memory. Four clips were
re-exported armature-only: **Idle, Empty_Reload, Inspect, Melee**. `Mag_Check` and the
other twelve measured identical under both pairings and were left untouched.

**Verification** — `work/hand_probe.cpp`, 240 uniform samples, byte-identical code path
for before and after:

- Idle `hand_l` **223.6 mm → 2.1 mm**, against `hand_r` 2.6 mm; L/R aggregate path ratio
  **0.00 → 1.12**. Blender independently predicted 1.4 / 1.8 mm — exact agreement.
- Empty_Reload 2324 → 3062 mm, Inspect 1140 → 881 mm, Melee 1057 → 999 mm.
- Unit tests **800 checks, 0 failures**; smoke `FPS_Animation_smoke_play.json` **PASS**
  with identical `drawCalls=1 playCycles=2 variants: 0x00=220`.

**Do not re-derive this.** If a left hand ever looks wrong again, run
`work/bl_pair_check.py` first: it evaluates every `A_FP_*` action under both the saved
and the correct weapon pairing and prints the divergence per clip. `work/hand_probe.cpp`
does the same on the shipped FBXs, and `work/fbx_info.cpp` shows what a clip actually
contains (meshes, takes, channel count).

**Method note.** The smoke suite's `Apartment`/`Sandbox` `newLogErrors` and the
`undo round-trip OK (... models ...)` counts shift depending on whether the editor is
running alongside the smoke test. This was A/B-proved innocent by restoring the original
Idle FBX and re-running: **identical numbers either way.** Only the per-scene asset
content matters; the totals are environment, not the change under test.

## UPDATE 4: the arms and the AK blink out of frame whenever a one-shot ends

Reported after the socket/FOV work: *"the arms and ak mesh disappear at moments during
the idle animation, or i guess more like when it switches animation."* Engine-side this
time — not the rig, not the skinning, not the re-exported clips.

**Mechanism.** `FirstPersonPresentation::Play()` asked for `AnimationWrapMode::Once` on
every non-looping state. `Model::UpdateAnimation` clears `Clip` the moment a `Once`
clip reaches its end:

```cpp
if (s.Wrap == AnimationWrapMode::Once && (s.Time >= len || s.Time < 0.0f)) s.Clip = -1;
```

From that frame two things fall back to the base FBX:

- `Model::UploadBoneMatrices`'s `posed` test goes false, so the model is skinned with
  `m_D->BindPoseBones` instead of `m_FinalBoneMatrices`;
- `Model::NodeTransform`'s identical test goes false, so it answers from the **bind
  walk** rather than `m_NodeGlobals`.

For these rigs the base FBX's bind pose is the **Manny T-pose**, so the arms leave the
view — *and* `FirstPersonPresentation::Update()` solves the weapon's entity transform
from `NodeTransform("ik_hand_gun")`, which has just moved to the right **hip**, so the
gun leaves with it. Both meshes vanish on the same frame, which is why they read as one
symptom.

Then the next state's `PlayAnimation` stores that `Clip = -1` state as `m_AnimFrom`, and
`EvaluatePose` reads an absent source clip as the bind pose — so the crossfade **travels
through the T-pose** for the full `fade` (0.12 s on Idle ≈ 7 frames) on the way back.

**Which states.** Every one-shot: Fire, TacReload, EmptyReload, Inspect, MagCheck,
Melee, Draw, Holster, Regrip — **and `IdleToSprint` / `SprintToIdle`**, because
`FirstPersonTransitionVia` routes Idle↔Sprint through them. Just starting and stopping
a sprint was enough to blink. Transitions between two *looping* states (Idle↔Walk↔Aim)
never hit this path, which is why it looked intermittent.

**Evidence** — `work/bind_probe.cpp`, which replicates `Model::EvaluatePose` exactly and
prints the base FBX's bind pose against each clip:

| node | bind pose (arms base) | Idle clip | delta |
|---|---|---|---|
| `hand_l` | (0.478, 1.045, 0.157) — hip, arm down | — | **768 mm / 109°** |
| `hand_r` | (−0.478, 1.045, 0.157) | — | **547 mm / 74°** |
| `ik_hand_gun` | (−0.478, 1.045, 0.157) — right hip | — | **678 mm / 105°** |
| `head` | (0, 1.626, 0.007) | — | 120 mm / 7° |
| worst bone | `CB_Gun` | — | **1569 mm / 177°** |

The weapon base alone is innocent: `AKS-74U_A_W_ADS.fbx`'s `root` sits within 0.0 mm of
every weapon clip's t=0, so the weapon rig's own bind pose is a correct on-screen pose.
The gun only disappears because its **socket** moves.

**Fix.** One-shot states now request `AnimationWrapMode::ClampForever` — the mode
`AnimatorController` already uses for non-looping clips — which keeps the clip alive and
holding its last frame, so both the skinned palette and `NodeTransform` stay posed and
the next state crossfades **from the real end pose**. Because a held clip never stops
reporting `IsPlayingAnimation()`, completion is now read through the new
`Model::AnimationFinished()`, which uses the *same* predicate `UpdateAnimation` applies
to a `Once` clip. `AnimationWrapMode::Once`'s documented "returns to bind pose"
behaviour is deliberately **unchanged** for every other caller.

**Verification** — build exit 0; unit tests **800 checks, 0 failures**;
smoke `FPS_Animation_smoke_play.json` **PASS**, `newLogErrors=0 drawCalls=1
playCycles=2 variants: 0x00=220` — byte-for-byte the same counters as before the change.
`Apartment`=6 / `Sandbox`=1 are the known missing-HDR environment noise (see UPDATE 3's
method note).

**Follow-up found while probing — since fixed:** the weapon rig's `mag2` bone sits
**121.8 mm from bind at t=0 *and* tEnd of every weapon clip** (305 mm / 40° under
Sprint), and `Play()` used to cut the weapon to bind with `StopAnimation()` — i.e.
fade 0 — whenever it entered a state with no weapon clip (Idle/Walk/Aim/Draw/Regrip).
That was a hard snap on the magazine at both ends of every weapon clip, and while the
spare magazine's weights were being dropped (UPDATE 5) it was invisible, because a
frozen mesh can't snap. `Play()` now crossfades with
`PlayAnimation(-1, clip.Fade, wrap)` instead of cutting; the `.blend` and every clip
file are untouched, so if that snap turns out to be authored behaviour the data still
matches the original export. See `FPS_ANIMATION_SYSTEM.md` §8 item 7.

## UPDATE 5: the spare magazine exists in Blender but not in the engine — assimp's vertex join

Reported after the disappearance fix: *"the reload should show a 2nd magazine in the hand
being swapped out — visible in Blender but not in the engine."* Root cause found, and it
is **not** in the engine's animation path — it happens three layers down, inside assimp's
`aiProcess_JoinIdenticalVertices`.

**The asset.** The weapon is a single object `aks74u` with 9 disjoint vertex groups
summing exactly to its vertex count: `magazine` 3190, `mag2` 3190, `root` 12469, `stock`
2590, `bolt` 670, `fire_selector` 523, `rear_sling_loop` 420, `mag_release` 82,
`trigger` 76. The spare magazine is **not a separate object** — it is a second mesh
island bound to the `mag2` bone, and it is a *perfect geometric duplicate of the installed
magazine*: both islands have 17594 raw verts and an identical rest bounding box
`(0.0111, −0.0131, −0.2062)..(0.1404, 0.0131, −0.0019)`. `magazine` and `mag2` share the
same rest head `(5.0641, −1.3933, 0.0)`. At rest the two islands occupy **the same
space**; only the `mag2` bone's animation ever separates them — ~66 cm at rest (parked at
the hip pouch), ~5 cm at the midpoint of the reload (in the hand).

So "identical vertices, different binding" is exactly the situation the asset is built
around, and it is precisely what `aiProcess_JoinIdenticalVertices` does not understand.

**Mechanism.** `build/_deps/assimp-src/code/PostProcessing/JoinVerticesProcess.cpp` keys
its dedup map on a `Vertex` whose `std::hash` hashes **position only**, and
`areVerticesEqual` compares position/normal/uv/color — **bone weights are not part of the
key in either direction**. The two islands therefore hash to the same buckets and compare
equal. One island wins (in practice `magazine`, which reads first in bone-array order),
the other's verts all get `JOINED_VERTICES_MARK`, and then:

```cpp
if (newWeights.size() > 0) {          // <-- the second, independent defect
    delete [] bone->mWeights;
    bone->mNumWeights = (unsigned int)newWeights.size();
    ...
}
```

When *every* one of a bone's weights was filtered out, this guard skipped the rewrite, so
the bone kept `mNumWeights = 17594` **and the original vertex ids** — indices into a
buffer that had already been shrunk to `magazine`'s vertices. `Model::ExtractBoneWeights`
then hit its `if (vertexId >= vertices.size()) continue;` and dropped all 17594 silently.
With zero vertices carrying a `mag2` influence, `ModelVertex.glsl`'s
`totalWeight <= 0.0001 → skinMat = mat4(1.0)` fallback leaves those verts at rest — frozen
**exactly underneath** the installed magazine, hence invisible.

**Bisect (assimp v5.4.3).** On `AKS-74U_A_W_Tac_Reload.fbx`, counting `mag2` weights
whose vertex id survives the flag set:

| flags | `mag2` weights in range |
|---|---|
| base (no post-processing) | 17594 |
| base + `JoinIdenticalVertices` | **0** |
| base + `OptimizeMeshes` | 17594 |
| base + `LimitBoneWeights` | 17594 |
| **engine exact (all three)** | **0** |

`aiProcess_JoinIdenticalVertices` alone is sufficient and necessary.

**Blast radius** (`work/mag_probe.exe --scan`, out-of-range weights across every shipped
FBX under the engine's exact flags): **all 11 weapon FBXs** dropped 17594 weights each —
**193,534 dropped over 27 files** — while **all 16 FirstPerson/arms FBXs were clean**.
This was weapon-specific because only the weapon models a duplicate skinned island.
Only `AKS-74U_A_W_ADS.fbx` supplies geometry; the other 10 weapon FBXs are channel-only.

**Fix — a local assimp patch**, not a `.blend` change and not a per-asset importer flag.
Two defects, both needed:

1. **Identity must include the skin binding.** `JoinVerticesProcess.cpp` now builds a
   canonical per-vertex `(bone index, weight)` table and keys the dedup map on a local
   `SkinnedVertex` wrapper carrying it, so differently-skinned coincident vertices are
   never merged. The functors live in the same TU as the existing `std::hash<Vertex>`
   specializations, so the public `assimp/Vertex.h` every other process depends on is
   untouched. Skinned meshes only pay for it when `mNumBones > 0`.
2. **Always rewrite `bone->mNumWeights`**, even down to zero, so a bone can never expose
   stale ids into a shrunken buffer.

Carried as `tools/assimp_patches/0001-join-vertices-keep-skin-bindings-apart.patch` and
applied at configure time by `tools/apply_assimp_patches.cmake` (hooked into
`CMakeLists.txt` right after `FetchContent_MakeAvailable(assimp)`), because assimp is
fetched from github at a pinned `GIT_TAG v5.4.3` and `build/_deps` does not survive a
clean configure. The applier probes with `git apply --check`, falls back to
`--check --reverse` for "already applied", and **fails the configure** if neither
matches — so a tag bump that invalidates the patch cannot silently build assimp without
the fix. Round-trip verified: pristine v5.4.3 (`c35200e38`) → applied → idempotent skip.
`Model::ExtractBoneWeights`'s out-of-range drop still exists as a safety net, but it is no
longer reached silently.

**Verification** (`work/mag_probe.exe`, engine-exact flags, patched assimp):

| | before | after |
|---|---|---|
| dropped weights, all 27 FBXs | 193,534 | **0** |
| `mag2` weights in range | 0 of 17594 | **5469 of 5469** (mesh0 3773 + mesh1 1696 — exactly mirroring `magazine`) |
| weapon verts | 29,215 | **34,684** (+18.7%) |
| weights per vertex | — | 1.000 across 31292 verts |
| spare mag vs installed mag @ rest | n/a | **0.551 m** (vertex space) |
| @ tick 85 (mid-reload) | n/a | **0.059 m** (in the hand) |
| @ tick 170 (end) | n/a | **0.551 m** (returned) |

> **Read those last three rows with care.** They come from `mag_probe`'s *vertex-space*
> forward-skin, which skins raw mesh vertices with `globalInverse·world·mOffsetMatrix`
> and never bakes each mesh's node transform the way `Model::ProcessMesh` does. A
> constant offset-matrix mismatch between `magazine` and `mag2` therefore shows up here
> as extra separation the engine does not render — note the two figures are identical at
> tick 0 and tick 170, i.e. a fixed bias, not motion. The number that actually describes
> *how far the rig jumps* is **`work/bind_probe.cpp`'s 121.8 mm** (node globals vs node
> globals), and the authoritative check is the visual one: the magazine appears in the
> hand and travels the swap, confirmed by the user in Play mode.

Both islands now survive with their own bindings, and the spare magazine tracks the clip
where it used to sit frozen underneath the installed one. Note the vertex cost: **+18.7%,
not the +348%** the rejected per-asset `optimizeGraph=false` workaround would have paid.

Also confirmed: build exit 0; unit tests **800 checks, 0 failures**; smoke
`FPS_Animation_smoke_play.json` **PASS**, `newGlErrors=0 newLogErrors=0 drawCalls=1
playCycles=2 variants: 0x00=220` — identical to the pre-change baseline. `Apartment`=6 /
`Sandbox`=1 remain the missing-asset environment noise; all five files those runs complain
about (`…/kloofendal_48d_partly_cloudy_puresky_4k.hdr`, `Y Bot.fbx`, `walking.fbx`, and
the two `Desktop/AKS-74U Textures/*.png`) were checked with `Test-Path` and **do not
exist on disk**, so those failures are pre-existing and unrelated.

**Consequence for UPDATE 4's follow-up:** that `mag2` snap was *latent* while the weights
were being dropped — a frozen mesh can't snap — and became real the moment `mag2` started
driving its island again. It is now fixed engine-side by the fade-to-bind described in
UPDATE 4's follow-up; no animation data changed.

## UPDATE 6: MagCheck's floating magazine — the stale export batch, and the pairing that was wrong all along

Reported alongside UPDATE 5, from a Play-mode recording: **during `MagCheck` the magazine
floats in space, detached, while the left hand stays planted** somewhere else. The right
hand and the weapon looked fine.

**Where the data came from.** Every arm clip in `project/assets/fps/AKS74U/FirstPerson/`
falls into one of two batches by file timestamp:

| batch | clips | channels |
|---|---|---|
| re-exported 8:41–8:51 | `Idle`, `Aim`, `Empty_Reload`, `Inspect`, `Melee` | 264 |
| shipped 4:00:42 | `ADS`, `Draw`, `Fire`, `Holster`, `IdleToSprint`, **`Mag_Check`**, `Regrip`, `Sprint`, `SprintToIdle`, `Tac_Reload`, `Walk` | 89 |

The 4:00:42 batch **predates the `A_FP_x → A_W_x` pairing rule** described in
`FPS_ANIMATION_SYSTEM.md` §2. And the `.blend` is *still saved* with the weapon parked on
`A_W_Tac_Reload` (with arm action `A_FP_Tac_Reload`), which is exactly what an
export that does not run `export_clip.py` will bake against. So every clip in that batch
was baked with the magazine being pulled out from under it.

**Which made `A_FP_Tac_Reload` correct by accident** — it was baked against the very
action it is meant to pair with (and the user had already verified TacReload looks right),
while `A_FP_Mag_Check` chased `A_W_Tac_Reload`'s magazine instead of `A_W_Mag_Check`'s.

**Why only MagCheck ever showed it.** `A_W_Tac_Reload`'s magazine starts moving around
**frame 44**, and *every other stale clip's frame range ends at 46 or earlier* — Draw
0..46, Fire 0..36, Holster 0..28, IdleToSprint 0..16, Regrip 0..40, Sprint 0..44,
SprintToIdle 0..44, Walk 0..44. Across their whole range the wrongly-paired magazine was
sitting still, so which action it was parked on made no measurable difference. `Mag_Check`
runs **0..260**, deep into TacReload's mag swap, and is the only clip in the batch long
enough for the bad pairing to actually bite.

**The earlier claim was wrong, and here is the A/B that proves it.** `FPS_ANIMATION_SYSTEM.md`
§2 used to assert that `Mag_Check` "measured identical under both pairings and was left
alone". That measurement was bad. Same action, same `0..260` range, same `meshes=0`, only
the paired weapon action changed:

| MagCheck bake | `hand_l` world path |
|---|---|
| shipped (as found) | 1085.7 mm |
| rebaked against `A_W_Tac_Reload` | **1086.3 mm** — Δ **0.6** vs shipped |
| rebaked against `A_W_Mag_Check` | **1062.9 mm** — Δ **−22.8** vs shipped |

Pairing alone swings the hand **23.4 mm**, and the shipped file lands on the
`A_W_Tac_Reload` bake to within noise. Root cause confirmed by controlled experiment, not
inference.

**What changed, and what did not.** All ten stale clips were re-exported with correct
pairing; `ADS` was **deliberately excluded** (it is the armsModel — geometry, bind pose
and node tree that every clip attaches to, and its animation is never played, so rebaking
it was the only move here that could actually break everything). `Idle`, `Aim`,
`Empty_Reload`, `Inspect` and `Melee` were already correct.

`hand_l` world-path delta vs what shipped, and `lowerarm_l` where it moved:

| clip | range | `lowerarm_l` | `hand_l` | `hand_r` |
|---|---|---|---|---|
| **`Mag_Check`** | 0..260 | **−67.0 mm** | **−22.8 mm** | +1.2 |
| Draw | 0..46 | +0.2 | −1.6 | −0.4 |
| Fire | 0..36 | +1.0 | −2.8 | +1.5 |
| Holster | 0..28 | 0.0 | 0.0 | −0.1 |
| IdleToSprint | 0..16 | 0.0 | 0.0 | 0.0 |
| Regrip | 0..40 | +1.2 | +0.8 | +3.2 |
| Sprint | 0..44 | +0.2 | −0.5 | +1.1 |
| SprintToIdle | 0..44 | +0.4 | −0.6 | −1.3 |
| Walk | 0..44 | +0.9 | −0.9 | +6.4 |
| `Tac_Reload` | 0..170 | −0.4 | −0.9 | +3.5 |

The nine non-MagCheck rows are ≤3.2 mm — exporter-version noise, not motion. They were
rebaked anyway for uniformity (one exporter run, one format) rather than left as a
mixed batch.

**A second trap on the way: `meshes`.** The first rebake passed `meshes=1`, which produced
267 nodes and bolted on two stray meshes named `Mesh.001` / `Mesh.003` (5–8 MB files).
Structural comparison against the clips that demonstrably work:

| file | nodes | meshes |
|---|---|---|
| `Inspect` / `Idle` / `Melee` (work) | 265 | **0** |
| first rebake (`meshes=1`) | 267 | 2 — `Mesh.001`, `Mesh.003` |
| old 4:00 batch | 92 | 1 — `SK_FP_Arms_Manneguin` |
| `ADS` (base, untouched) | 266 | 1 |

Geometry belongs to `ADS`; clip FBXs are channel-only. Re-running with `meshes=0` gives
265 nodes / 0 meshes — an exact structural match to the working clips — and drops files
back to ~1.9–2.3 MB. Motion is provably unaffected by the flag: the `meshes=0` export
produces identical deltas to the `meshes=1` export on every clip measured.

**Verification.** Unit tests **804 checks, 0 failures**; smoke
`FPS_Animation_smoke_play.json` **PASS**, `newGlErrors=0 newLogErrors=0 drawCalls=1
playCycles=2 variants: 0x00=220` — identical to the pre-change baseline (`Apartment`=6 /
`Sandbox`=1 remain the pre-existing missing-asset noise). One operational note: the first
run reported *2 failures*, which is the known spurious signature of running `--unit-tests`
with the editor open (they race on the scene round-trip files) — closing `TartarusEngine.exe`
restored 804/0. **Confirmed by the user in Play mode:** the magazine now meets the left
hand.

**Rollback:** every file is git-tracked, so `git checkout -- project/assets/fps/AKS74U/FirstPerson/`
restores all ten; the original `Mag_Check` is also kept as `work/AKS-74U_A_FP_Mag_Check_ORIGINAL.fbx`.

**Open follow-ups:** (a) the real fix for `EmptyReload` is an ammo system — `G` is a
temporary debug binding, and it only reached Play mode after `InputMap::MergeDefaults`
stopped `project/settings.json`'s `input` array from replacing `Defaults()` wholesale
(see `FPS_ANIMATION_SYSTEM.md` §8 item 9); (b) `export_clip.py` always prints
`weapon action paired = …` — read it, because a wrong pairing is invisible until someone
watches the clip at a frame range long enough to expose it.

## Original next-step writeup (now executed — see UPDATE above; left for the reasoning/code)

**Play the `A_FP_Idle` action directly on the live rig in Blender, with
zero export/import round-trip involved at all**, and check whether the
mesh deforms cleanly or is *already* torn in Blender itself:

```python
import bpy, bmesh
arm = bpy.data.objects.get("Armature")
mesh = bpy.data.objects.get("SK_Manny_Arms")
action = bpy.data.actions.get("A_FP_Idle")

arm.data.pose_position = 'POSE'
if not arm.animation_data:
    arm.animation_data_create()
arm.animation_data.action = action
fs, fe = action.frame_range
bpy.context.scene.frame_set(int((fs + fe) / 2))
bpy.context.view_layer.update()

deps = bpy.context.evaluated_depsgraph_get()
mesh_eval = mesh.evaluated_get(deps)
bm = bmesh.new()
bm.from_object(mesh_eval, deps)
bm.verts.ensure_lookup_table()
coords = [tuple(v.co) for v in bm.verts]
# inspect coords / bounding box for a torn/exploded shape vs a normal arm
bm.free()
```

(Restore `arm.animation_data.action` and `pose_position` afterward; do not
save the `.blend`. Per the user's standing rule, **ask before making any
change to animation data** in the `.blend` file — this specific check is
read-only/evaluation-only and doesn't modify keyframes, but the courtesy
heads-up was the working norm all session.)

- If Blender **also** shows the same torn shape playing this action live,
  that conclusively proves the bug is in the **source bake** of
  `A_FP_Idle` itself (most likely something about how the
  constraint-driven Kinemation/Unity-oriented control-rig motion was
  originally baked down onto the plain deform bones — possibly incorrect
  bake settings, a bone-space issue during baking, or the bake genuinely
  requires something Kinemation's runtime does at playback time that a
  static bake can't capture). The fix would then need to happen in
  Blender (re-baking the action correctly), not in engine code, and it
  would likely affect **all 15** per-state clip files, not just `Idle`.
- If Blender shows it **clean**, the bug is still somewhere in the
  export → import → engine pipeline that this session's checks didn't
  reach — worth then diffing the *exported* FBX's baked keyframe values
  directly (not Blender's own re-import reconstruction of them, which was
  checked and looked sane, but the raw Assimp-parsed values in the actual
  running engine) against Blender's live in-memory evaluation at the same
  frame, bone by bone, starting with whichever bone's vertices sit at the
  base of the visible "blades" in the torn render.

## Other leads not fully exhausted

- Consider a GPU-side capture (RenderDoc or similar) to see the *actual*
  per-vertex positions reaching the rasterizer, to close the loop between
  "CPU data and shader logic are individually correct" (verified) and
  "the actual pixels on screen" (still wrong).
- Double-check that the running engine is definitely loading the freshly
  re-exported `AKS-74U_A_FP_ADS.fbx` and not any stale cached geometry —
  each test in this session used a fresh process launch, which should rule
  this out, but it was never independently verified via e.g. a file-size
  or checksum check against what got loaded.
- `src/Game/FirstPersonPresentation.cpp/h`'s `Tick()`/state-transition
  logic (the priority-tiered FSM wrapping arms+weapon) was not stress
  tested beyond the initial bind→Idle fade — worth ruling out if the
  Blender-side check above comes back clean.

## Build/run instructions

- This worktree builds directly (no path-length workaround needed, despite
  an unrelated stale memory note suggesting otherwise for other worktrees):
  `cmake --build . --config Release --target TartarusEngine` from `build/`.
- Launch `build\Release\TartarusEngine.exe`. The untracked scene
  `project/scenes/FPS_Animation_smoke_play.json` is already set up — hit
  Play and the `Idle` clip starts automatically on the runtime-created
  `[Runtime] First Person Arms` / `[Runtime] First Person Weapon` entities.
- Use `Window → Console` in the editor (not the notification bell) to read
  any `Log::Warn`/`Log::Info` output — it doesn't truncate long lines and
  has a filter box.
- Blender MCP (`mcp__blender__execute_blender_code`) targets the running
  Blender instance with `C:\Users\jacob\OneDrive\Desktop\AKS-74U 60fps (Revised).blend`
  open. **Standing user rule: always ask before making any change to
  animation data in this file.** Export operations and read-only
  inspection were fine to do directly in this session but were still
  communicated to the user beforehand as a courtesy.
- Materials/textures are explicitly out of scope for now per the user —
  untextured rendering is expected and correct; the "Texture: failed to
  load" console errors are known/expected noise, not a bug.
