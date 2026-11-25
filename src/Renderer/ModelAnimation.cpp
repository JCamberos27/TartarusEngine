// Model animation: clips, playback, pose evaluation, node transforms and root motion.

#include "Model.h"
#include "AnimRetarget.h"
#include "SkinHideBuffer.h"
#include "Log.h"
#include "Core/DiagW1.h"
#include <chrono>

#include <glm/gtx/matrix_decompose.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <algorithm>
#include <filesystem>
#include <mutex>
#include <map>
#include <tuple>
#include <unordered_map>

namespace {
// A hidden node's scale: its subtree folds into its pivot (tiny, not zero, so nothing divides by it).
const glm::mat4 kCollapse(glm::vec4(1e-4f, 0, 0, 0), glm::vec4(0, 1e-4f, 0, 0), glm::vec4(0, 0, 1e-4f, 0), glm::vec4(0, 0, 0, 1));
} // namespace
int Model::FindAnimation(const std::string& name) const {
    for (int i = 0; i < (int)m_D->Animations.size(); ++i)
        if (m_D->Animations[i].Name == name) return i;
    return FindClipByRef(name);
}

const std::string& Model::AnimationName(int index) const {
    static const std::string kNone;
    if (index >= 0 && index < (int)m_D->Animations.size()) return m_D->Animations[index].Name;
    const int e = index - (int)m_D->Animations.size();
    return e >= 0 && e < (int)m_ExternalClips.size() ? m_ExternalClips[e].DisplayName : kNone;
}

const AnimationClip* Model::ClipAt(int i, const std::vector<int>** nodeChannel) const {
    if (i >= 0 && i < (int)m_D->Animations.size()) {
        *nodeChannel = &m_D->Animations[i].NodeChannel;
        return &m_D->Animations[i];
    }
    const int e = i - (int)m_D->Animations.size();
    if (e < 0 || e >= (int)m_ExternalClips.size()) return nullptr;
    const ExternalClip& x = m_ExternalClips[e];
    if (x.NodeChannel.size() != m_D->Nodes.size()) return nullptr; // this model was reimported since
    if (!x.Source || x.SourceIndex >= (int)x.Source->Animations.size()) return nullptr;
    const AnimationClip& clip = x.Source->Animations[x.SourceIndex];
    if (clip.Channels.size() != x.SourceChannels) return nullptr;    // the source was reimported since
    *nodeChannel = &x.NodeChannel;
    return x.Baked ? x.Baked.get() : &clip;
}

LocalTRS Model::SampleClipNode(int clipIndex, const AnimationClip& clip, int channel, int node, float ticks) const {
    LocalTRS t = clip.Channels[channel].Sample(ticks, m_D->Nodes[node].BindTRS);
    const int e = clipIndex - (int)m_D->Animations.size();
    if (e >= 0 && e < (int)m_ExternalClips.size()) {
        const auto& corr = m_ExternalClips[e].Correction;
        if (node < (int)corr.size() && corr[node] != glm::quat(1.0f, 0.0f, 0.0f, 0.0f)) {
            t.T = corr[node] * t.T;
            t.R = glm::normalize(corr[node] * t.R);
        }
    }
    return t;
}

float Model::AnimationLength(int index) const {
    const std::vector<int>* nc = nullptr;
    const AnimationClip* c = ClipAt(index, &nc);
    return c ? c->LengthSeconds() : 0.0f;
}

int Model::FindClipByRef(const std::string& ref) const {
    for (int e = 0; e < (int)m_ExternalClips.size(); ++e)
        if (m_ExternalClips[e].Ref == ref) return (int)m_D->Animations.size() + e;
    return -1;
}

int Model::AttachClip(const Model& source, int sourceIndex, const std::string& ref, const std::string& displayName) {
    if (int existing = FindClipByRef(ref); existing >= 0) return existing;
    if (AnimationCount() == 0) BumpAnimationEpoch(); // its first clip: the animation tick stops skipping it
    if (sourceIndex < 0 || sourceIndex >= (int)source.m_D->Animations.size()) return -1;
    const std::pair<std::string, const SharedData*> key(ref, source.m_D.get());
    if (std::find(m_UnmatchedClips.begin(), m_UnmatchedClips.end(), key) != m_UnmatchedClips.end()) return -1;
    const AnimationClip& clip = source.m_D->Animations[sourceIndex];
    ExternalClip x;
    x.Source = source.m_D;
    x.SourceIndex = sourceIndex;
    x.SourceChannels = clip.Channels.size();
    x.Ref = ref;
    x.DisplayName = displayName;
    // The node / channel match and the rest corrections depend only on the two models' shared import data: every
    // instance of a model (a squad's rifles, each soldier's body) attaching the same clip reuses the first's.
    struct MatchKey {
        const SharedData* Target; const SharedData* Source; int Index; size_t Channels;
        bool operator<(const MatchKey& o) const {
            return std::tie(Target, Source, Index, Channels) < std::tie(o.Target, o.Source, o.Index, o.Channels);
        }
    };
    struct Match {
        std::weak_ptr<SharedData> Target, Source; // a freed model's address can be reused: these say it's still the same
        std::vector<int> NodeChannel;
        std::vector<glm::quat> Correction;
        std::shared_ptr<const AnimationClip> Baked;
        int Matched = 0;
    };
    static std::mutex s_MatchMutex;
    static std::map<MatchKey, Match> s_Matches;
    const MatchKey mkey{m_D.get(), source.m_D.get(), sourceIndex, clip.Channels.size()};
    {
        std::lock_guard<std::mutex> lock(s_MatchMutex);
        const auto it = s_Matches.find(mkey);
        if (it != s_Matches.end() && it->second.Target.lock() == m_D && it->second.Source.lock() == source.m_D) {
            if (it->second.Matched == 0) {
                m_UnmatchedClips.push_back(key);
                return -1;
            }
            x.NodeChannel = it->second.NodeChannel;
            x.Correction = it->second.Correction;
            x.Baked = it->second.Baked;
            m_ExternalClips.push_back(std::move(x));
            return (int)m_D->Animations.size() + (int)m_ExternalClips.size() - 1;
        }
    }
    int matched = 0;
    auto remember = [&]() {
        std::lock_guard<std::mutex> lock(s_MatchMutex);
        if (s_Matches.size() > 4096) s_Matches.clear();
        Match& mt = s_Matches[mkey];
        mt.Target = m_D;
        mt.Source = source.m_D;
        mt.NodeChannel = x.NodeChannel;
        mt.Correction = x.Correction;
        mt.Baked = x.Baked;
        mt.Matched = matched;
    };
    // A UE4 mannequin pack on a UE5 rig: different spine counts, rest poses and proportions - baked onto this
    // skeleton (AnimRetarget) rather than copying locals by name.
    if (RetargetIsUe4ToUe5(source.m_D->Nodes, m_D->Nodes)) {
        auto baked = std::make_shared<AnimationClip>();
        if (RetargetBakeClip(source.m_D->Nodes, clip, m_D->Nodes, *baked, x.NodeChannel)) {
            matched = (int)baked->Channels.size();
            x.Baked = std::move(baked);
            Log::Info("Animation: '" + ref + "' retargeted from a UE4 mannequin onto '" +
                      std::filesystem::u8path(m_Path).filename().u8string() + "' (" + std::to_string(matched) + " bones baked).",
                      LogContext::Asset(m_Path));
            remember();
            m_ExternalClips.push_back(std::move(x));
            return (int)m_D->Animations.size() + (int)m_ExternalClips.size() - 1;
        }
    }
    x.NodeChannel.assign(m_D->Nodes.size(), -1);
    {
        std::unordered_map<std::string, int> byName;
        byName.reserve(m_D->Nodes.size());
        for (int n = (int)m_D->Nodes.size() - 1; n >= 0; --n) byName[m_D->Nodes[n].Name] = n; // the first of a name wins
        for (int c = 0; c < (int)clip.Channels.size(); ++c)
            if (const auto it = byName.find(clip.Channels[c].BoneName); it != byName.end()) {
                x.NodeChannel[it->second] = c;
                ++matched;
            }
    }
    if (matched == 0) {
        remember();
        // A bare "-1" at the call site made mixed FBX exports nearly impossible to diagnose.
        // Give the author a small, actionable sample from each side without flooding the log
        // with an entire production skeleton.
        auto sample = [](auto count, auto nameAt) {
            std::string out;
            const int n = std::min<int>((int)count, 4);
            for (int i = 0; i < n; ++i) {
                if (i) out += ", ";
                out += nameAt(i);
            }
            return out.empty() ? std::string("(none)") : out;
        };
        Log::Warn("Animation: '" + ref + "' has no skeleton-node matches on '" + m_Path +
                  "' (clip samples: " + sample(clip.Channels.size(), [&](int i) { return clip.Channels[i].BoneName; }) +
                  "; target samples: " + sample(m_D->Nodes.size(), [&](int i) { return m_D->Nodes[i].Name; }) + ")",
                  LogContext::Asset(m_Path));
        m_UnmatchedClips.push_back(key);
        return -1;
    }
    // Rest-orientation differences at the top of the animated hierarchy (a bone whose parent the
    // clip doesn't drive): the clip's rotations and translations there are in its own file's
    // frame. The usual case is an axis convention - a Z-up pack on a Y-up rig, whose root holds
    // the conversion as an FBX pre-rotation that the clip's root keys don't include - which
    // would otherwise lay the character on its back. Same-export rigs get no correction.
    for (int n = 0; n < (int)m_D->Nodes.size(); ++n) {
        if (x.NodeChannel[n] < 0) continue;
        const int parent = m_D->Nodes[n].Parent;
        if (parent >= 0 && x.NodeChannel[parent] >= 0) continue;
        const AnimNode* src = nullptr;
        for (const AnimNode& s : source.m_D->Nodes)
            if (s.Name == m_D->Nodes[n].Name) { src = &s; break; }
        if (!src) continue;
        const glm::quat corr = glm::normalize(m_D->Nodes[n].BindTRS.R * glm::inverse(src->BindTRS.R));
        if (std::abs(corr.w) > 0.99996f) continue; // under ~1 degree: the same rest pose
        if (x.Correction.empty()) x.Correction.assign(m_D->Nodes.size(), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
        x.Correction[n] = corr;
        Log::Info("Animation: '" + ref + "' is authored in a different rest frame at '" + m_D->Nodes[n].Name +
                  "' (" + std::to_string((int)std::lround(glm::degrees(glm::angle(corr)))) + " deg, e.g. Z-up vs Y-up); "
                  "retargeted onto '" + std::filesystem::u8path(m_Path).filename().u8string() + "'.",
                  LogContext::Asset(m_Path));
    }
    if (matched * 2 < (int)clip.Channels.size())
        Log::Warn("Animation: '" + ref + "' matches only " + std::to_string(matched) + " of its " +
                  std::to_string(clip.Channels.size()) + " animated bones on '" + m_Path +
                  "' - is it for a different skeleton?", LogContext::Asset(m_Path));
    remember();
    m_ExternalClips.push_back(std::move(x));
    return (int)m_D->Animations.size() + (int)m_ExternalClips.size() - 1;
}

void Model::PlayAnimation(int index, float fadeSeconds, AnimationWrapMode wrap, float speed) {
    if (NeverAnimates()) BumpAnimationEpoch(); // its pending pose change must reach the animation tick
    if (index >= AnimationCount()) index = -1;
    // Taking playback back from ApplyLocalPose: the built-in path owns the pose again. There is no
    // live clip to fade from in that case (the animator's pose isn't one), so this starts clean.
    if (m_ExternalPose) { m_ExternalPose = false; m_Anim = {}; m_AnimFrom = {}; m_FadeDuration = m_FadeElapsed = 0.0f; }
    // Crossfade from the current pose (a stopped model fades from its bind pose).
    if (fadeSeconds > 0.0f && (m_Anim.Clip >= 0 || index >= 0)) {
        m_AnimFrom = m_Anim;
        m_FadeElapsed = 0.0f;
        m_FadeDuration = fadeSeconds;
    } else {
        m_AnimFrom = {};
        m_FadeDuration = m_FadeElapsed = 0.0f;
    }
    m_Anim = {};
    m_Anim.Clip = index;
    m_Anim.Speed = speed;
    m_Anim.Wrap = wrap;
    m_PosePending = true;
}

void Model::UpdateAnimation(float dt) {
    if (m_ExternalPose) return; // the Animator Controller posed this model (ApplyLocalPose)
    const int clipCount = AnimationCount();
    if (m_Anim.Clip >= clipCount) m_Anim.Clip = -1;         // #96 — reimported with fewer clips
    if (m_AnimFrom.Clip >= clipCount) m_AnimFrom.Clip = -1;
    const bool fading = m_FadeDuration > 0.0f;
    if (m_Anim.Clip < 0 && !fading && !m_PosePending) return;

    // Root motion (SetRootMotion): each playing clip's motion over this step, mixed by the fade.
    RootMotionMix motion;
    const float fadeW = fading ? std::clamp(m_FadeElapsed / m_FadeDuration, 0.0f, 1.0f) : 1.0f;
    auto advance = [&](PlaybackState& s) {
        if (s.Clip < 0) return;
        const float before = s.Time;
        s.Time += dt * s.Speed;
        if (m_RootMotionNode >= 0) {
            const float w = &s == &m_Anim ? fadeW : 1.0f - fadeW;
            motion.Add(ClipRootMotion(s.Clip, before, s.Time, s.Wrap, m_RootMotionNode, m_RootMotionSettings), w);
        }
        // Once: stop at the end (back to the bind pose, like Unity's legacy Animation).
        const float len = AnimationLength(s.Clip);
        if (s.Wrap == AnimationWrapMode::Once && (s.Time >= len || s.Time < 0.0f)) s.Clip = -1;
    };
    advance(m_Anim);
    if (fading) {
        advance(m_AnimFrom);
        m_FadeElapsed += std::fabs(dt);
        if (m_FadeElapsed >= m_FadeDuration) { m_FadeDuration = 0.0f; m_AnimFrom = {}; }
    }
    if (m_RootMotionNode >= 0) m_RootMotionPending = m_RootMotionPending.Then(motion.Result());
    EvaluatePose();
    m_PosePending = false;
}

bool Model::NodeTransform(const std::string& name, glm::mat4& out) const {
    const int found = NodeIndex(name);
    return found >= 0 && NodeTransformAt(found, out);
}

bool Model::NodeTransformAt(int found, glm::mat4& out) const {
    if (found < 0 || found >= (int)m_D->Nodes.size()) return false;
    if (m_PoseDirty) ResolveAppliedPose();
    const auto& nodes = m_D->Nodes;
    const size_t idx = (size_t)found;

    // Same "is a pose live?" test UploadBoneMatrices uses, plus m_NodeGlobals being populated:
    // it is scratch filled by EvaluatePose, so before the first evaluation (or after a
    // reimport) it is simply absent and the bind walk below is the truthful answer.
    const bool posed = (m_ExternalPose || m_Anim.Clip >= 0 || m_FadeDuration > 0.0f) && m_NodeGlobals.size() == nodes.size();
    if (posed) {
        if (idx < m_GlobalValid.size() && !m_GlobalValid[idx]) ResolveNodeGlobal(found);
        out = m_NodeGlobals[idx];
        return true;
    }

    // Bind pose: accumulate each ancestor's bind-local transform, parents first.
    glm::mat4 global(1.0f);
    for (int i = (int)idx; i >= 0; i = nodes[i].Parent) global = nodes[i].BindLocal * global;
    out = global;
    return true;
}

const std::vector<unsigned char>& Model::SkinPath() const {
    auto& path = m_D->SkinPath;
    const auto& nodes = m_D->Nodes;
    if (path.size() == nodes.size()) return path;
    path.assign(nodes.size(), 0);
    for (int i = (int)nodes.size() - 1; i >= 0; --i) // children after parents: walk back up
        if (nodes[i].BoneId >= 0 || path[i]) {
            path[i] = 1;
            if (nodes[i].Parent >= 0) path[nodes[i].Parent] = 1;
        }
    return path;
}

void Model::ResolveNodeGlobal(int node) const {
    const auto& nodes = m_D->Nodes;
    thread_local std::vector<int> chain;
    chain.clear();
    for (int n = node; n >= 0 && !m_GlobalValid[n]; n = nodes[n].Parent) chain.push_back(n);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const int i = *it, p = nodes[i].Parent;
        const glm::mat4 local = m_AppliedPose[i].ToMatrix();
        m_NodeGlobals[i] = p >= 0 ? AffineMul(m_NodeGlobals[p], local) : local;
        if ((size_t)i < m_HiddenNodes.size() && m_HiddenNodes[i]) m_NodeGlobals[i] = AffineMul(m_NodeGlobals[i], kCollapse);
        m_GlobalValid[i] = 1;
    }
}

void Model::EvaluatePose() {
    m_PoseDirty = m_PaletteDirty = false;
    const auto& nodes = m_D->Nodes;
    m_GlobalValid.clear(); // every global below
    m_NodeGlobals.resize(nodes.size());
    if (m_FinalBoneMatrices.size() < (size_t)MAX_BONES) m_FinalBoneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
    ++m_PoseVersion;

    const std::vector<int>* curMap = nullptr;
    const std::vector<int>* fromMap = nullptr;
    const AnimationClip* cur = m_Anim.Clip >= 0 ? ClipAt(m_Anim.Clip, &curMap) : nullptr;
    const AnimationClip* from = (m_FadeDuration > 0.0f && m_AnimFrom.Clip >= 0) ? ClipAt(m_AnimFrom.Clip, &fromMap) : nullptr;
    const bool blending = m_FadeDuration > 0.0f;
    const float w = blending ? std::clamp(m_FadeElapsed / m_FadeDuration, 0.0f, 1.0f) : 1.0f;
    const float curTicks = cur ? WrappedClipTicks(*cur, m_Anim.Time, m_Anim.Wrap) : 0.0f;
    const float fromTicks = from ? WrappedClipTicks(*from, m_AnimFrom.Time, m_AnimFrom.Wrap) : 0.0f;

    // Root motion: the root node of each clip is put back in place before the two are blended.
    const int rootNode = m_RootMotionNode < (int)nodes.size() ? m_RootMotionNode : -1;
    auto inPlace = [&](const LocalTRS& sampled, const PlaybackState& s, int node) {
        // Ping-pong has no root motion to hand over (ClipRootMotion), so it plays as authored.
        if (s.Clip < 0 || s.Wrap == AnimationWrapMode::PingPong) return sampled;
        const glm::mat4 parent = nodes[node].Parent >= 0 ? m_NodeGlobals[nodes[node].Parent] : glm::mat4(1.0f);
        const glm::mat4 toModel = m_D->GlobalInverseTransform * parent;
        const glm::mat4 x = toModel * sampled.ToMatrix();
        const glm::mat4 x0 = InPlaceStart(s.Clip, node);
        const glm::mat4 local = glm::inverse(toModel) * RootMotionInPlace(x, x0, m_RootMotionSettings);
        LocalTRS out = sampled;
        glm::vec3 skew, scale; glm::vec4 persp;
        if (glm::decompose(local, scale, out.R, out.T, skew, persp)) out.R = glm::normalize(out.R);
        return out;
    };

    for (size_t i = 0; i < nodes.size(); ++i) {
        const AnimNode& n = nodes[i];
        const int cc = cur ? (*curMap)[i] : -1;
        const bool strip = (int)i == rootNode;
        glm::mat4 local;
        if (!blending) {
            if (cc < 0) local = n.BindLocal;
            else {
                const LocalTRS b = SampleClipNode(m_Anim.Clip, *cur, cc, (int)i, curTicks);
                local = (strip ? inPlace(b, m_Anim, (int)i) : b).ToMatrix();
            }
        } else {
            const int fc = from ? (*fromMap)[i] : -1;
            if (cc < 0 && fc < 0) {
                local = n.BindLocal;
            } else {
                LocalTRS a = fc >= 0 ? SampleClipNode(m_AnimFrom.Clip, *from, fc, (int)i, fromTicks) : n.BindTRS;
                LocalTRS b = cc >= 0 ? SampleClipNode(m_Anim.Clip, *cur, cc, (int)i, curTicks) : n.BindTRS;
                if (strip && fc >= 0) a = inPlace(a, m_AnimFrom, (int)i);
                if (strip && cc >= 0) b = inPlace(b, m_Anim, (int)i);
                local = LocalTRS::Blend(a, b, w).ToMatrix();
            }
        }
        m_NodeGlobals[i] = n.Parent >= 0 ? AffineMul(m_NodeGlobals[n.Parent], local) : local;
        if (i < m_HiddenNodes.size() && m_HiddenNodes[i]) m_NodeGlobals[i] = AffineMul(m_NodeGlobals[i], kCollapse);
        if (n.BoneId >= 0) m_FinalBoneMatrices[n.BoneId] = AffineMul(AffineMul(m_D->GlobalInverseTransform, m_NodeGlobals[i]), n.BoneOffset);
    }
}

int Model::NodeIndex(const std::string& name) const {
    const auto it = m_D->NodeLookup.find(name);
    return it != m_D->NodeLookup.end() ? it->second : -1;
}

void Model::BindLocalPose(std::vector<LocalTRS>& out) const {
    const auto& nodes = m_D->Nodes;
    out.resize(nodes.size());
    for (size_t i = 0; i < nodes.size(); ++i) out[i] = nodes[i].BindTRS;
}

bool Model::SampleLocalPose(int clip, float seconds, AnimationWrapMode wrap, std::vector<LocalTRS>& out,
                            std::vector<unsigned char>* driven) const {
    const auto& nodes = m_D->Nodes;
    BindLocalPose(out);
    if (driven) driven->assign(nodes.size(), 0);
    const std::vector<int>* map = nullptr;
    const AnimationClip* c = clip >= 0 ? ClipAt(clip, &map) : nullptr;
    if (!c) return false;
    // Same sampling EvaluatePose does for the playing clip, so an animator-driven model and a
    // PlayAnimation-driven one land on identical poses for the same clip and time.
    const float ticks = WrappedClipTicks(*c, seconds, wrap);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const int ch = (*map)[i];
        if (ch < 0) continue;
        out[i] = SampleClipNode(clip, *c, ch, (int)i, ticks);
        if (driven) (*driven)[i] = 1;
    }
    return true;
}

void Model::ApplyLocalPose(const std::vector<LocalTRS>& pose) {
    const auto& nodes = m_D->Nodes;
    if (pose.size() != nodes.size()) return;
    if (DiagW1::InFpIk) ++DiagW1::Applies;
    m_AppliedPose = pose;
    ++m_PoseVersion;
    m_ExternalPose = true;
    m_PosePending = false;
    m_PoseDirty = m_PaletteDirty = true; // the globals and palette follow when read (ResolveAppliedPose / ResolvePalette)
}

void Model::ResolveAppliedPose() const {
    const auto diagT0 = std::chrono::steady_clock::now();
    struct DiagEnd { std::chrono::steady_clock::time_point t0; size_t n; ~DiagEnd() { if (DiagW1::InFpIk) { ++DiagW1::Resolves; DiagW1::ResolveNodes += n; DiagW1::ResolveMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); } } } diagEnd{diagT0, m_D->Nodes.size()};
    m_PoseDirty = false;
    const auto& nodes = m_D->Nodes;
    const std::vector<LocalTRS>& pose = m_AppliedPose;
    m_NodeGlobals.resize(nodes.size());
    const std::vector<unsigned char>& path = SkinPath();
    m_GlobalValid = path; // the rest on demand (ResolveNodeGlobal)
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!path[i]) continue;
        const AnimNode& n = nodes[i];
        const glm::mat4 local = i < pose.size() ? pose[i].ToMatrix() : n.BindLocal; // a pose from before a reimport
        m_NodeGlobals[i] = n.Parent >= 0 ? AffineMul(m_NodeGlobals[n.Parent], local) : local;
        if (i < m_HiddenNodes.size() && m_HiddenNodes[i]) m_NodeGlobals[i] = AffineMul(m_NodeGlobals[i], kCollapse);
    }
}

void Model::ResolvePalette() const {
    if (m_PoseDirty) ResolveAppliedPose();
    m_PaletteDirty = false;
    if (m_FinalBoneMatrices.size() < (size_t)MAX_BONES) m_FinalBoneMatrices.assign(MAX_BONES, glm::mat4(1.0f));
    const auto& nodes = m_D->Nodes;
    for (size_t i = 0; i < nodes.size(); ++i) {
        const AnimNode& n = nodes[i];
        if (n.BoneId >= 0) m_FinalBoneMatrices[n.BoneId] = AffineMul(AffineMul(m_D->GlobalInverseTransform, m_NodeGlobals[i]), n.BoneOffset);
    }
}

void Model::PosedBounds(glm::vec3& outMin, glm::vec3& outMax) const {
    outMin = m_D->BoundsMin;
    outMax = m_D->BoundsMax;
    const bool posed = m_ExternalPose || (m_Anim.Clip >= 0 && m_Anim.Clip < AnimationCount()) || m_FadeDuration > 0.0f;
    const int bones = std::min(m_D->BoneCounter, (int)m_D->BoneBoxMin.size());
    if (!posed || bones <= 0) return; // the bind pose: the bind bounds are exact
    if (m_PaletteDirty) ResolvePalette();
    if (m_FinalBoneMatrices.size() < (size_t)bones) return;
    if (m_PosedBoundsVersion != m_PoseVersion) {
        glm::vec3 lo = m_D->UnweightedMin, hi = m_D->UnweightedMax;
        for (int b = 0; b < bones; ++b) {
            const glm::vec3 bmin = m_D->BoneBoxMin[(size_t)b], bmax = m_D->BoneBoxMax[(size_t)b];
            if (bmin.x > bmax.x) continue; // no vertex on this bone
            const glm::mat4& m = m_FinalBoneMatrices[(size_t)b];
            const glm::vec3 c = glm::vec3(m * glm::vec4((bmin + bmax) * 0.5f, 1.0f));
            const glm::vec3 h = (bmax - bmin) * 0.5f;
            const glm::vec3 e = glm::abs(glm::vec3(m[0])) * h.x + glm::abs(glm::vec3(m[1])) * h.y + glm::abs(glm::vec3(m[2])) * h.z;
            lo = glm::min(lo, c - e);
            hi = glm::max(hi, c + e);
        }
        if (lo.x > hi.x) { lo = m_D->BoundsMin; hi = m_D->BoundsMax; }
        const glm::vec3 pad(0.02f); // weights that don't quite sum to one
        m_PosedMin = lo - pad;
        m_PosedMax = hi + pad;
        m_PosedBoundsVersion = m_PoseVersion;
    }
    outMin = m_PosedMin;
    outMax = m_PosedMax;
}

void Model::SetHiddenNodes(const std::vector<int>& nodes) {
    std::vector<unsigned char> hidden;
    for (int n : nodes) {
        if (n < 0 || n >= NodeCount()) continue;
        if (hidden.empty()) hidden.assign(NodeCount(), 0);
        hidden[n] = 1;
    }
    if (hidden == m_HiddenNodes) return;
    m_HiddenNodes.swap(hidden);
    // An applied pose is re-resolved with the new set when next read (an animated one picks it up on its next tick).
    m_PoseDirty = m_PaletteDirty = true;
    ++m_PoseVersion;
}

int Model::FindRootMotionNode(const std::string& name) const {
    const auto& nodes = m_D->Nodes;
    if (!name.empty()) return NodeIndex(name);
    // The auto pick is asked for every frame: remember it per import.
    if (m_AutoRootMotionFor == m_D.get()) return m_AutoRootMotionNode;
    m_AutoRootMotionFor = m_D.get();
    m_AutoRootMotionNode = -1;
    auto leaf = [](const std::string& s) { // "mixamorig:Hips" -> "hips"
        const size_t colon = s.find_last_of(':');
        std::string l = colon == std::string::npos ? s : s.substr(colon + 1);
        for (char& ch : l) ch = (char)std::tolower((unsigned char)ch);
        return l;
    };
    for (int i = 0; i < (int)nodes.size(); ++i)
        if (nodes[i].Parent >= 0 && leaf(nodes[i].Name) == "root") return m_AutoRootMotionNode = i;
    for (int i = 0; i < (int)nodes.size(); ++i) {
        const std::string l = leaf(nodes[i].Name);
        if (l == "hips" || l == "pelvis") return m_AutoRootMotionNode = i;
    }
    return -1;
}

glm::mat4 Model::SampleNodeModelSpace(int clip, float seconds, AnimationWrapMode wrap, int node) const {
    const auto& nodes = m_D->Nodes;
    if (node < 0 || node >= (int)nodes.size()) return glm::mat4(1.0f);
    const std::vector<int>* map = nullptr;
    const AnimationClip* c = clip >= 0 ? ClipAt(clip, &map) : nullptr;
    const float ticks = c ? WrappedClipTicks(*c, seconds, wrap) : 0.0f;
    glm::mat4 g(1.0f);
    for (int i = node; i >= 0; i = nodes[i].Parent) {
        const int ch = c ? (*map)[i] : -1;
        g = (ch >= 0 ? SampleClipNode(clip, *c, ch, i, ticks).ToMatrix() : nodes[i].BindLocal) * g;
    }
    return m_D->GlobalInverseTransform * g;
}

glm::mat4 Model::PoseNodeModelSpace(const std::vector<LocalTRS>& pose, int node) const {
    const auto& nodes = m_D->Nodes;
    if (node < 0 || node >= (int)nodes.size() || pose.size() != nodes.size()) return glm::mat4(1.0f);
    glm::mat4 g(1.0f);
    for (int i = node; i >= 0; i = nodes[i].Parent) g = pose[i].ToMatrix() * g;
    return m_D->GlobalInverseTransform * g;
}

void Model::SetPoseNodeModelSpace(std::vector<LocalTRS>& pose, int node, const glm::mat4& modelSpace) const {
    const auto& nodes = m_D->Nodes;
    if (node < 0 || node >= (int)nodes.size() || pose.size() != nodes.size()) return;
    const glm::mat4 parent = nodes[node].Parent >= 0 ? PoseNodeModelSpace(pose, nodes[node].Parent)
                                                     : m_D->GlobalInverseTransform;
    const glm::mat4 local = glm::inverse(parent) * modelSpace;
    glm::vec3 scale, skew, t; glm::vec4 persp;
    glm::quat r;
    if (!glm::decompose(local, scale, r, t, skew, persp)) return;
    pose[node].T = t;
    pose[node].R = glm::normalize(r);
}

void Model::StripRootMotion(std::vector<LocalTRS>& pose, int clip, int node, const RootMotionSettings& s, float loopTime) const {
    if (node < 0 || node >= NodeCount() || pose.size() != (size_t)NodeCount()) return;
    const glm::mat4 x0 = InPlaceStart(clip, node);
    glm::mat4 inPlace = RootMotionInPlace(PoseNodeModelSpace(pose, node), x0, s);
    const AnimationClip* c = nullptr;
    if (loopTime >= 0.0f && clip >= 0) {
        const std::vector<int>* map = nullptr;
        c = ClipAt(clip, &map);
    }
    const float len = c ? c->LengthSeconds() : 0.0f;
    if (c && c->StartTicks > 0.0f && len > 1e-3f) {
        // The hips: the root's child that the body hangs from.
        const auto& nodes = m_D->Nodes;
        int hips = -1;
        for (int i = 0; i < (int)nodes.size() && hips < 0; ++i)
            if (nodes[i].Parent == node) hips = i;
        if (hips >= 0) {
            // Where the in-place hips are at the loop's two ends, over the ground (the root's own motion taken out).
            auto hipsAt = [&](float t) {
                const glm::mat4 r = SampleNodeModelSpace(clip, t, AnimationWrapMode::ClampForever, node);
                const glm::mat4 h = SampleNodeModelSpace(clip, t, AnimationWrapMode::ClampForever, hips);
                return glm::vec3(RootMotionInPlace(r, x0, s) * glm::inverse(r) * h[3]);
            };
            glm::vec3 drift = hipsAt(len) - hipsAt(0.0f);
            drift.y = 0.0f;
            const float p = std::clamp(std::fmod(loopTime, len) / len, 0.0f, 1.0f);
            inPlace = glm::translate(glm::mat4(1.0f), -drift * p) * inPlace;
        }
    }
    SetPoseNodeModelSpace(pose, node, inPlace);
}

glm::mat4 Model::InPlaceStart(int clip, int node) const {
    glm::mat4 x0 = SampleNodeModelSpace(clip, 0.0f, AnimationWrapMode::ClampForever, node);
    const std::vector<int>* map = nullptr;
    const AnimationClip* c = clip >= 0 ? ClipAt(clip, &map) : nullptr;
    if (c && c->StartTicks > 0.0f) x0[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return x0;
}

RootMotionDelta Model::ClipRootMotion(int clip, float t0, float t1, AnimationWrapMode wrap, int node,
                                      const RootMotionSettings& s) const {
    const float len = AnimationLength(clip);
    if (node < 0 || !(len > 0.0f) || wrap == AnimationWrapMode::PingPong) return {};
    const bool loop = wrap == AnimationWrapMode::Loop;
    return RootMotionBetween(
        [&](float t) { return SampleNodeModelSpace(clip, t, AnimationWrapMode::ClampForever, node); },
        t0, t1, len, loop, s);
}

void Model::SetRootMotion(int node, const RootMotionSettings& s) {
    if (node >= NodeCount()) node = -1;
    if (node != m_RootMotionNode) m_RootMotionPending = {};
    m_RootMotionNode = node;
    m_RootMotionSettings = s;
}

RootMotionDelta Model::ConsumeRootMotion() {
    const RootMotionDelta d = m_RootMotionPending;
    m_RootMotionPending = {};
    return d;
}
