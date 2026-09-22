# Local assimp patches

assimp is pulled straight from `github.com/assimp/assimp` at a pinned `GIT_TAG` (see the top of
`CMakeLists.txt`) rather than forked. Anything we need fixed upstream but can't wait for is
carried here as a `.patch` file and applied by `tools/apply_assimp_patches.cmake` during
configure - after `FetchContent_MakeAvailable(assimp)` has written the sources, before anything
compiles them. No manual step, and a fresh clone behaves identically.

Add a patch: drop `NNNN-short-description.patch` in this folder. CMake re-configures when a
patch here changes, and the applier probes each patch with `git apply --check` /
`--check --reverse`, so patches apply once and stay applied.

To (re)generate one after editing `build/_deps/assimp-src`:

```
git -C build/_deps/assimp-src diff --output="<repo>\tools\assimp_patches\NNNN-desc.patch"
```

## 0001-join-vertices-keep-skin-bindings-apart.patch

`aiProcess_JoinIdenticalVertices` compared only a vertex's geometric attributes - position,
normal, tangent, UVs, colour - and never looked at what it was skinned to. Two vertices sitting
in the same place but driven by different bones therefore looked "identical", one was folded
into the other, and the binding it carried was destroyed.

Two separate defects, both needed:

1. **Identity ignored the skin binding.** The map key now carries each vertex's
   `(bone index, weight)` pairs, so differently-skinned coincident vertices stay separate.
   Hash/equality live in `JoinVerticesProcess.cpp` next to the existing `std::hash<Vertex>`
   specializations, so the public `assimp/Vertex.h` other processes depend on is untouched.
2. **A bone losing every weight kept its old count.** The rewrite was guarded by
   `if (newWeights.size() > 0)`, so when *all* of a bone's weights were filtered out, the bone
   kept `mNumWeights` and the original vertex ids - indices into a buffer that had already been
   shrunk. Downstream loaders that skip out-of-range ids then dropped the whole bone in silence.

**Why it mattered here:** the AKS-74U view model carries its spare magazine as a second mesh
island modelled exactly on top of the installed one and driven by its own `mag2` bone - they
occupy the same space at rest and only the bone animation separates them. Joining merged the
pair, kept `magazine`, and left `mag2` pointing at vertices that no longer existed, so the
engine dropped all 17594 of its weights and the spare magazine never moved. See
`FPS_ANIMATION_INVESTIGATION.md`.
