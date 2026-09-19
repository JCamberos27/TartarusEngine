#pragma once

#include <string>

class World;
class Model;
class AssetLibrary;

// Advances every AnimatorComponent by dt: spin, orbit, bob, and light hue cycling. Call once
// per frame while the scene is playing (never in edit mode — the authored pose must stay put
// so a save never bakes a mid-animation frame). Play mode snapshots and restores the scene
// around itself, so the transforms/colors this mutates are reset on Stop for free.
void UpdateAnimators(World& world, float dt);

// #175 / #113 — applies every SkeletalAnimationComponent to its entity's model while playing:
// starts PlayAutomatically clips on the first frame, crossfades when Clip changes, follows live
// Speed / WrapMode edits, stops when IsPlaying goes false, and clears IsPlaying when a Once clip
// finishes. The model itself advances in the render loop (Model::TickAnimationOnce).
void UpdateSkeletalAnimations(World& world, AssetLibrary& assets);

// #175 — a SkeletalAnimationComponent::Clip reference -> a clip index on `model`, or -1:
//   ""                     the model's first clip
//   "Run"                  the model's own clip named Run
//   "assets/walking.fbx"   the (first) clip of another model file, retargeted onto this model
//   "assets/pack.fbx#Run"  a named clip of another model file
// Another file's clip is loaded through `assets` and attached to this instance on first use.
int ResolveAnimationClip(Model& model, const std::string& clipRef, AssetLibrary& assets);
// The reference AND picker label for clip `sourceClip` of the model file at `absPath`.
std::string AnimationClipRef(const Model& source, int sourceClip);
std::string AnimationClipLabel(const Model& source, int sourceClip);
