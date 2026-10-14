# Job system and multithreaded frames (design)

Status: proposal. No engine code changes yet; this document is the plan the implementation PRs follow.

## Why

Every frame-critical system runs on the main thread today, one after another, in `src/main.cpp`'s loop:

1. Gameplay: `PhysicsWorld::Step` (with the game module's `FixedTick` and C# `Scripting::Tick` inside the
   sub-steps), player and first-person update, enemy AI, animators, animator controllers, IK, combat FX,
   particles (`main.cpp` ~2386-2780).
2. Animation: one `Model::TickAnimationOnce` per renderable (`main.cpp` ~2787), then
   `OutfitSystem::UpdateAttachments` and `World::RebuildWorldTransformCache` (~2822).
3. Asset pump and outfit hiding (~2829).
4. Rendering: the sun shadow caster gather and pass (~3109), the local shadow gather (~3288) and the spot and point
   passes, the SSAO depth pre-pass gather (~3760), then `SceneRenderer::RenderScene` for the Scene view and again
   for the Game view, which each build, sort and submit their draw lists.

The only parallelism is PhysX's own dispatcher (`PxDefaultCpuDispatcherCreate` with up to 4 threads,
`PhysicsWorld.cpp:1080`) plus ad-hoc `std::thread` / `std::async` use for editor and asset work
(`AssetLibrary.cpp:191`, `AssetThumbnailLoader`, `OutfitSystem.cpp:1115`, `OutfitAudit.cpp:379`).

The same visibility work is also done four times per view: `GatherVisible` (`SceneRenderer.cpp:108`) walks every
`TransformComponent + RenderableComponent` for the camera (`SceneRenderer.cpp:491`), the sun casters
(`main.cpp:3136`), the local-light casters (`main.cpp:3292`) and the SSAO pre-pass (`main.cpp:3760`). Each walk
does the same registry lookups, transform fetches and `Model::PosedBounds` calls before applying its own filter.

The goal is to move the CPU work that is per-entity and independent onto worker threads, while keeping all OpenGL
calls, all registry structural changes, PhysX writes and C# scripting on the main thread.

## Goals and non-goals

Goals:

* A small engine-wide job system that every subsystem shares (one pool, sized to the machine).
* Culling, animation pose evaluation and skinning prep, and draw-list building running in parallel.
* Output identical to today: same draw order, same pixels. Every phase is checked against the single-threaded path.
* A switch that runs every job inline on the main thread, so any regression can be bisected to "threading or not".

Non-goals for this plan:

* A separate render thread or pipelining frame N+1's simulation against frame N's rendering. It is the next big
  win after this plan but needs a double-buffered render snapshot of the world; see Phase 5.
* Multithreaded GL submission. OpenGL 4.6 has one useful context per thread and draw submission stays on the main
  thread. Reducing submit cost (multi-draw indirect, bindless textures) is a separate track.
* Parallel gameplay scripting. C# scripts, the game module, the player and first-person stack stay serial.

## The job system

### Shape

`src/Core/JobSystem.h/.cpp`, C++17 (the project's standard; nothing below needs C++20).

```cpp
namespace Jobs {
struct Counter;                           // atomic count of unfinished jobs; a job group waits on one
using JobFn = void (*)(void* data, std::uint32_t begin, std::uint32_t end);

void Init(unsigned workerCount);          // see "Pool size" below; main thread is worker 0
void Shutdown();

void Run(Counter& c, std::function<void()> fn);                       // one job
void ParallelFor(Counter& c, std::uint32_t count, std::uint32_t grain, // [0, count) in chunks of `grain`
                 const std::function<void(std::uint32_t, std::uint32_t)>& fn);
void Wait(Counter& c);                    // the caller runs queued jobs until c reaches zero (never sleeps idle)

unsigned ThisWorker();                    // 0 = main thread, 1..N = workers; indexes per-worker scratch
bool OnMainThread();
void SetInline(bool on);                  // debug: every job runs immediately on the caller, in order
}
```

Implementation choices:

* **Fixed pool, one worker per spare hardware thread.** The main thread is worker 0 and helps while it waits, so
  a `Wait` never parks the thread that owns the GL context.
* **Pool size.** The dev machine is an i9-7980XE (18 cores / 36 threads). Default to one worker per physical
  core minus one (17), not per hardware thread: culling and pose evaluation are memory-bound, and two
  hyper-threads on one core share its caches and load ports, so the second thread adds little. The cap and the
  SMT choice are settings, tuned with Phase 0's numbers.
* **Per-worker work-stealing deques** (Chase-Lev). The first version can use one mutex-guarded queue; the API does
  not change when the queue does, and with tens of jobs per frame a single queue is not the bottleneck. Move to
  deques once profiling shows contention.
* **No fibers.** Jobs run to completion. A job that needs another job's result is split at that point and the
  dependency is a `Counter` the scheduler waits on. This keeps stack traces, the crash handler
  (`CrashHandler.cpp`) and the sampling profiler (`SamplingProfiler.cpp`) working unchanged.
* **No per-job heap allocation.** Job records come from a fixed ring; `ParallelFor` writes one record per chunk.
  The `std::function` in the sketch above is for readability; the real entry point takes a function pointer plus
  a user pointer so nothing allocates on the hot path.
* **Per-worker frame scratch.** A linear allocator per worker, reset at `Profiler::BeginFrame` time. Gather and
  draw-list jobs write their chunk outputs there and the main thread concatenates them.
* **Workers sleep on a condition variable / `WaitOnAddress` when the queues are empty**, with a short spin first.
  They must not busy-spin across the whole frame: the GPU driver thread and the PhysX workers need those cores.

### Shared by PhysX

PhysX's `PxDefaultCpuDispatcher` owns up to four more threads (`PhysicsWorld.cpp:1080`). With a job pool of
`hardware - 1` workers on top, an 8-thread CPU would run 11 busy threads during a physics step. Phase 1 replaces
it with a `PxCpuDispatcher` adapter whose `submitTask` pushes a job that calls `task.run()` then `task.release()`,
and whose `getWorkerCount` returns the pool size. PhysX then shares the pool and the main thread helps run physics
tasks while `fetchResults(true)` blocks today.

### Profiler and logging

* `Profiler::PushSample` appends to one vector and is main-thread only (`Profiler.h`). Phase 1 gives each worker
  its own sample buffer, merged at `BeginFrame`, and tags samples with the worker index so the Stats panel can
  show a per-thread breakdown. `PROFILE_GPU_SCOPE` stays main-thread only because it issues GL queries.
* `Log` is already safe from any thread (`Log.cpp:35-60` queues non-main-thread entries), so jobs may log.

### Thread-role assertions

Debug builds get `JOBS_ASSERT_MAIN()` at the few choke points that must never run on a worker: `Shader::Bind`,
`ShaderAsset::Variant` / `CompileVariant`, `Model::UploadBoneMatrices`, `Texture` uploads,
`GpuScopeTimer`, and a wrapper used for registry `emplace` / `remove` / `create` / `destroy` during parallel
phases. These catch the hazards below the first time they happen instead of as a rare corruption.

## Ownership rules

These hold during any parallel phase of the frame.

### OpenGL

The one GL context (`Window.cpp:200`) stays current on the main thread for the life of the process. Workers never
call GL, directly or indirectly. That rules out more than it looks like, because some CPU-looking code compiles or
uploads lazily:

* `ShaderAsset::Variant` compiles a missing variant on first use (`ShaderAsset.cpp:515`). The draw-list loop calls
  it through `programFor` while classifying each renderable (`SceneRenderer.cpp:561`). A parallel draw-list build
  must record the variant key only, collect misses, and let the main thread compile them before submission
  (`SceneRenderer::WarmShaderVariants`, `SceneRenderer.cpp:321`, already does this for a whole scene).
* `Model::UploadBoneMatrices` writes the bone ring with `glNamedBufferSubData` (`Model.cpp:2025`). See Phase 3 for
  how workers write palettes without GL.
* Profiler GPU scopes, texture loads, framebuffer queries.

Shared contexts for background uploads are deliberately left out: on Windows WGL they work, but they add fences
and driver-specific behaviour for little gain while `AssetLibrary::PumpAsync` already budgets uploads per frame.

### EnTT (v3.13.2)

An `entt::registry` is not thread-safe for structural changes, and a structural change on one pool can reallocate
storage another thread is reading. The rules:

1. **The registry is structurally frozen while jobs run.** No `create`, `destroy`, `emplace`, `remove`, `clear`
   or tag toggling (`InactiveTag`, `LodCulledTag`, ...) from any thread, the main thread included, between kicking
   a parallel phase and waiting on it. Jobs that need a structural change append it to a per-worker command list
   that the main thread applies after the wait.
2. **Jobs iterate a snapshot, not a view.** The main thread copies the view's entities into a frame-scratch array
   (or takes the smallest pool's contiguous `data()`), and `ParallelFor` splits that array. Iterating a live view
   from several threads is fine while frozen, but splitting a view into ranges is awkward and the snapshot also
   fixes the order.
3. **Reads anywhere, writes only to what the job owns.** `get`, `try_get`, `all_of`, `any_of` are safe in parallel
   while frozen. A job writes only to components of the entities in its own range, or to its own output buffer.
4. **No `static` locals or globals touched from jobs** unless they are per-worker or atomic.

### PhysX

No PhysX API calls from jobs in Phases 1-4. Physics stays a serial step whose internal tasks run on the shared
pool. Scene queries from worker threads (for example parallel AI line-of-sight) would need
`PxSceneFlag::eREQUIRE_RW_LOCK` and `PxSceneReadLock`, or a guarantee that no simulate or write is in flight; that
is a later, separate change.

### C# scripting and the game module

Managed callbacks (`Scripting::Tick`, `gameModule.FixedTick`) only run on the main thread. A job never invokes
user script, and animation events or similar callbacks raised in a job are queued and delivered on the main
thread after the wait.

## Hazards found in the current code

These are the data races a straight `ParallelFor` would introduce today. Each phase fixes the ones it touches
before going parallel.

| Where | What is shared | Fix |
| --- | --- | --- |
| `Model::PosedBounds` (`Model.cpp:1786`) | Lazily writes `m_PosedMin/Max`, `m_PosedBoundsVersion` and may call `ResolvePalette()` (mutable) on read. Two cull jobs touching one model race. | Compute posed bounds eagerly at the end of the animation step (Phase 3), or in a serial pre-pass, so culling only reads. |
| `Model::ResolvePalette` (`Model.cpp:1775`) | `m_PaletteDirty` lazy resolve from const readers (`FinalBoneMatrix`, `UploadBoneMatrices`, `PosedBounds`). | Resolve eagerly in the animation job. |
| `Material::Hash`, `CachedVariantKey` / `StoreVariantKey` (`Material.h` ~125-145, `ShaderVariant.cpp:29`) | Per-frame mutable caches written on first read; two draw-list jobs sharing a material race. | Warm the frame's hashes and keys in one serial pass over the materials in use, or make the cache word atomic. |
| `ShaderAsset::Variant` (`ShaderAsset.cpp:515`) | Compiles on miss: GL plus `m_Variants` map insert. | Lookup-only on workers; misses compiled on main before submit. |
| `World::GetCachedWorldTransform` fallback (`World.cpp:337`) | Falls back to `ComposeWorldTransform`, which inserts into `m_CycleWarned` (`World.cpp:124`). | The cache must cover every entity before culling starts; the fallback's warning set becomes a main-thread-only path or an atomic flag. |
| `RenderScene` and the shadow gathers | `static std::vector` scratch (`drawList`, `visible`, `sortKeys`, `casting`, ...). | Per-view, per-worker scratch owned by a prepare context. |
| `UpdateAnimatorControllers` (`AnimatorController.cpp:1355`) | `static` group map; followers depend on their driver's state lengths. | Serial grouping, then parallel per group. |
| `g_variantDrawCounts` (`SceneRenderer.cpp`) | Global map incremented in `selectProgram`. | Stays in the serial submit; no change. |
| Bone ring cursor (`Model.cpp:1984-2060`) | `g_BoneCursor`, `g_BoneEpoch`, `m_BoneUpload`. | Atomic reservation, see Phase 3. |

## Phases

Each phase is its own PR, lands behind `Jobs::SetInline` (an `EditorSettings` toggle plus a `--jobs=0` launch
option), and is checked against the single-threaded path before merge.

### Phase 0: baseline and safety net

* Record per-scope CPU times from the existing profiler for two reference scenes: a heavy NPC scene (the NPC test
  from `NpcTest.cpp`) and a static-heavy smoke scene. These are the numbers each later phase is measured against.
* Add an image comparison to the `--smoke-test` run: render a fixed set of frames and diff them. Every later phase
  must match bit-for-bit with jobs on, jobs inline, and jobs in a debug "shuffle" mode that runs chunks in a random
  order on one thread (this flushes out order dependence without real races).
* There is no ThreadSanitizer on MSVC. Clang-cl's `-fsanitize=thread` does not support Windows either, so the
  safety net is the thread-role asserts above, the shuffle mode, and code review against the ownership rules.

### Phase 1: the job system itself

* `JobSystem.h/.cpp` as above, unit tests in `UnitTests_Engine.cpp` (counters, nested `ParallelFor`, inline mode,
  shutdown with pending jobs).
* Per-worker profiler buffers and a per-thread view in the Stats panel.
* The PhysX dispatcher adapter.
* Optional in the same phase: move `AssetLibrary`'s and `AssetThumbnailLoader`'s private thread pools onto
  long-running jobs, or at least cap them so the machine is not oversubscribed. These are editor-time and lower
  priority than PhysX.

No behaviour change. Expected gain: none on its own, except that PhysX stops competing with the main thread for
cores and the main thread helps run its tasks.

### Phase 2: parallel culling

1. Make `GatherVisible` read-only: posed bounds resolved before it runs (a serial pass over skinned models until
   Phase 3 makes it eager), transform cache complete.
2. Replace the four walks with one **visibility build** kicked right after `RebuildWorldTransformCache` and the
   outfit hiding update (`main.cpp` ~2833), before the shadow passes:
   * a `ParallelFor` over the renderable snapshot computes, per entity, the filter bits `GatherVisible` checks
     (inactive, LOD-culled, owner-view tags, shadow-casting mode, transparent slot), the cached transform and the
     world-space bounds, once;
   * then each consumer (camera, SSAO, sun casters, local casters, and the Game view's set) is a `ParallelFor`
     filter plus frustum test over that shared array, all running concurrently.
3. Chunk outputs are concatenated in chunk order, so every list comes out in registry order exactly as today.
   This matters: the opaque sort breaks ties by gather order (`SceneRenderer.cpp:658-680`), so a different gather
   order would change draw order.
4. `World::ApplyLod` can fold into the same pass later; it writes `LodCulledTag`, so for now it stays serial and
   runs before the visibility build.

Expected gain: the four gathers' combined cost drops to roughly one walk's cost divided by the worker count, plus
a merge. How much frame time that is depends on the scene; Phase 0's numbers for the `Local Shadow Gather` scope
and the gather part of `Sun Shadow Pass` and `Scene Draw` say how big the prize is. This is inferred from the
code, not measured.

### Phase 3: animation and skinning prep

1. **Split resolution from evaluation.** `UpdateSkeletalAnimations` resolves clips through `AssetLibrary`
   (`AnimationSystem.cpp:264`) and may trigger loads; that stays serial. Pose evaluation is then parallel.
2. **Parallel `TickAnimationOnce`.** The loop at `main.cpp` ~2787 becomes a `ParallelFor` over the snapshot of
   models. Scene entities each own their `Model` instance (`AssetLibrary::InstantiateModel`), and
   `TickAnimationOnce`'s frame index guard stays as the dedupe, made atomic so a shared model can't be ticked twice.
   Each job also resolves the palette and the posed bounds eagerly, which removes two of the hazards above.
3. **Animator controllers in parallel per group.** Grouping (drivers and followers) stays serial; each group's
   state machine update is a job. Events into scripts are queued.
4. **Bone palettes written by workers.** Change the bone ring (`Model.cpp:1984`) to a persistently mapped buffer
   (`GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT`, mapped once on main). Each slice gets a `glFenceSync` at the end
   of the frame that used it, waited on (normally already signalled) before the slice is reused. Workers reserve
   a range with an atomic add on the slice cursor and `memcpy` their palette into the mapped pointer;
   `UploadBoneMatrices` on main only binds the range. A slice that fills up falls back to the next frame's slice
   behaviour today (`g_BoneEpoch` bump), reserved by the main thread only.
5. **Stays serial:** first-person body, presentation, IK, ADS and arms (`main.cpp` ~2543-2560). They are a handful
   of rigs with strict ordering between them, and they touch the player and camera.

Expected gain: scales with the number of animated characters. With a few NPCs it is small; with crowds the
animation scopes (`Animators`, `Animator Controllers`, `Animation Update`) approach their single-thread cost
divided by the worker count. The bone upload moves from many small `glNamedBufferSubData` calls during submission
to plain memory writes, which also trims driver time on the main thread.

### Phase 4: parallel draw-list building

1. **Split `RenderScene` into Prepare and Submit.** Prepare is everything up to the draw loop that does not touch
   GL: per-entity classification, `DrawItem` construction (`SceneRenderer.cpp` ~492-650), sort keys and sorts.
   Submit is the existing draw loop, `ApplyFrameState`, program binding and material binding.
2. **Prepare in parallel.** A `ParallelFor` over the camera's visible set writes `DrawItem`s into per-chunk lists
   (opaque, transparent, view-model, view-model transparent), concatenated in chunk order. The four lists then sort
   as four concurrent jobs. Variant misses are collected and compiled on main before Submit.
3. **Shadow caster lists** (`SunCaster`, `LocalCaster` in `main.cpp`) are built the same way; the light-to-caster
   assignment for spot and point lights is a `ParallelFor` over lights.
4. **Scene and Game views prepare concurrently** when both are visible, each with its own prepare context
   (replacing the `static` scratch vectors).
5. While the workers prepare, the main thread is not idle: it runs GL work that does not depend on the lists
   (cluster cull dispatch, sky aerial perspective, IBL and probe updates).

Expected gain: Prepare leaves the main thread almost entirely. Submit remains, and after this phase it is most of
the main thread's render cost. That is the point where multi-draw indirect and bindless materials (a separate
plan) become the next lever.

### Phase 5: overlap (later, separate plan)

Two larger steps, listed so the earlier phases do not paint them into a corner:

* **Async physics.** `simulate()` without blocking, run animation and culling for entities physics does not
  move, then `fetchResults`. Complicated by C# `FixedTick` riding the sub-steps and by ragdolls feeding animation.
* **Render thread / frame pipelining.** Simulation of frame N+1 overlaps rendering of frame N. Needs an
  immutable render snapshot (transforms, palettes, draw lists) produced at the end of simulation, and the editor's
  mid-frame edits (`main.cpp` ~2810 comment) routed through it. Typically the largest CPU win, and the most
  invasive change in this list.

Phases 2-4 are designed so their outputs (the visibility array, palettes in the ring, prepared draw lists) are
exactly what such a snapshot would contain.

## Expected gains, summarised

No numbers here are measured; Phase 0 produces them. As a rule of thumb, the speedup on the moved work is close to
the worker count for culling and animation (independent per-entity work, little shared memory traffic) and lower
for draw-list prep (bigger records, a merge and sorts). Total frame time improves only by the share of the frame
those systems take: if culling, animation and prep are 40% of main-thread CPU and they run 4x faster, the frame
is about 30% shorter on the CPU side, and GL submission, gameplay and scripts set the new floor. If the frame is
GPU-bound (the scene renders at the GPU's limit), none of this shows in FPS until the GPU side is cheaper too.

| Phase | Moves off the main thread | Main risks |
| --- | --- | --- |
| 1 | PhysX tasks share the pool | Dispatcher adapter correctness, PhysX task lifetime |
| 2 | All visibility gathers | Lazy caches in `PosedBounds`, order of the gathered lists |
| 3 | Pose evaluation, palette build, posed bounds, bone ring writes | Asset loads during clip resolution, script events, fence handling on the persistent ring |
| 4 | Draw item construction and sorting for every view and shadow pass | Lazy material and variant caches, shader compiles on miss, per-view scratch |

## Risks

* **Silent data races.** The biggest risk, made worse by MSVC having no race detector. Mitigation: the ownership
  rules, the thread-role asserts, the hazard table above, shuffle mode, and image diffs on every phase.
* **Lazy caches added later.** The engine uses `mutable` caches freely (posed bounds, palettes, material hashes,
  variant keys). Any new one on a type read by jobs reintroduces a race. Reviews of renderer and animation changes
  should check for `mutable` and `static` locals.
* **Order-dependent output.** Sorting ties fall back to gather order, so parallel gathers must preserve it.
  Chunk-ordered concatenation does, and the image diff catches it if not.
* **Oversubscription.** PhysX, the editor's thread pools, the asset loaders, the audio convolution thread
  (`Audio/Convolution.h`) and the GPU driver's own thread all compete. The pool size, worker sleep behaviour and the
  PhysX adapter are tuned in Phase 1 with the profiler's per-thread view.
* **GL calls hidden in CPU code.** Shader variant compiles and bone uploads are the known ones; the main-thread
  asserts find the rest.
* **Persistent mapping.** Coherent persistent maps are fast on NVIDIA and AMD but need correct fencing; an
  incorrect fence shows as flickering skinned meshes rather than a crash. Keep the old `glNamedBufferSubData` path
  behind a toggle until the fenced path has run in the smoke tests on both vendors.
* **Small scenes get slower.** Kicking jobs costs a few microseconds each. `ParallelFor` runs inline below a
  threshold (for example fewer than 256 renderables), so small editor scenes keep today's behaviour.
* **Debuggability.** Crashes inside jobs have the worker's stack, not the caller's. Inline mode reproduces most
  issues on the main thread; the crash handler should also record which job was running on each worker.

## Target hardware

Dev machine: Intel i9-7980XE (18 cores / 36 threads, Skylake-X, 24.75 MB L3), 32 GB DDR4, GTX 1080 Ti 11 GB,
Windows 11. Two things about it shape the plan:

* **Memory bandwidth, not cores, will limit scaling.** The RAM is two DIMMs (dual-channel on a quad-channel
  platform) running at 2133 MHz with XMP off. Culling and pose evaluation stream through transforms, bounds and
  bone matrices, so on this setup they will stop scaling well before 17 workers. Enabling XMP (3200 MHz) and
  populating four slots would roughly double usable bandwidth. Phase 0 baselines should note the memory config so
  later numbers are comparable.
* **The GPU may be the limit first.** A GTX 1080 Ti rendering the 4K monitor with HDR, MSAA, CSM and clustered
  lighting can be GPU-bound, in which case CPU savings show as lower main-thread time but not higher FPS. Phase 0
  records both CPU and GPU scope times (`PROFILE_GPU_SCOPE`) so this is visible. Persistent coherent mapping
  (Phase 3) is well supported on Pascal.

## Open questions

* Minimum spec: the dev machine has far more cores than a typical player PC. The default pool size, the inline
  threshold and the chunk grain should be checked on a 6-core / 12-thread machine too, once a minimum spec is set.
* Whether to move to C++20 for `std::atomic::wait`, `std::latch` and `std::span`. Not required by this plan.
* Whether the editor Scene view and Game view should keep rendering separately each frame, or share one visibility
  build when their cameras match.
