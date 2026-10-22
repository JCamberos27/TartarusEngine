// The Decal Library panel and the state the decal tools share (docs/DECALS.md, "Editor tools"):
// the index of every .decal under assets/Decals (read a slice a frame, so opening the panel never
// stalls), its category tree, search and favourites, the hover card that previews a decal on a wall
// swatch, the palette picker, the brush and auto-grime popovers, and the radial menu's decal mode.
// The tools themselves (Place Decal, Decal Brush, auto-grime, the ghost) are in
// EditorLayer_DecalTools.cpp.
//
// Motion is all Glass::Motion() in the Viewport category (monochrome; Reduce Motion or the
// category off draws everything settled): tiles cascade in from the first, lift on hover, a
// favourite's heart pops, the hover card eases in. Nothing here holds an animation at rest.
#include "EditorLayer.h"
#include "EditorLayerInternal.h"
#include "EditorIcons.h"
#include "EditorPanels.h"
#include "EditorSettings.h"
#include "EditorSound.h"
#include "EditorTheme.h"
#include "EditorUIHelpers.h"
#include "EditorUIPrimitives.h"
#include "DecalEditorState.h"
#include "AssetThumbnailLoader.h"
#include "Glass/GlassDragMotion.h"
#include "Glass/GlassMotion.h"
#include "DecalAsset.h"
#include "GLStateCache.h"
#include "Log.h"
#include "ProjectPaths.h"
#include "World.h"
#include "gl.h"

#include <imgui.h>
#include <imgui_internal.h> // ImHashStr, ImLerp
#include <IconsFontAwesome6.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;
using namespace EditorInternal;

namespace {

constexpr Glass::MotionCategory kViewport = Glass::MotionCategory::Viewport;
constexpr int kScanPerFrame = 48;     // .decal files read a frame while indexing
constexpr int kMaxRecent = 12;
constexpr int kRadialPalettes = 3, kRadialCategories = 6, kRadialMax = 12;
const char* kDecalRoot = "assets/Decals"; // the library indexes this folder

ImU32 White(float a) { return IM_COL32(255, 255, 255, (int)(ImClamp(a, 0.0f, 1.0f) * 255.0f + 0.5f)); }
ImU32 Black(float a) { return IM_COL32(0, 0, 0, (int)(ImClamp(a, 0.0f, 1.0f) * 255.0f + 0.5f)); }

std::string LowerCopy(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// "a2" < "a10": digit runs compare as numbers, everything else without case.
bool NaturalLess(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit((unsigned char)a[i]) && std::isdigit((unsigned char)b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && std::isdigit((unsigned char)a[i2])) ++i2;
            while (j2 < b.size() && std::isdigit((unsigned char)b[j2])) ++j2;
            const std::string na = a.substr(i, i2 - i), nb = b.substr(j, j2 - j);
            const size_t za = na.find_first_not_of('0'), zb = nb.find_first_not_of('0');
            const std::string ta = za == std::string::npos ? "" : na.substr(za), tb = zb == std::string::npos ? "" : nb.substr(zb);
            if (ta.size() != tb.size()) return ta.size() < tb.size();
            if (ta != tb) return ta < tb;
            i = i2;
            j = j2;
            continue;
        }
        const int ca = std::tolower((unsigned char)a[i]), cb = std::tolower((unsigned char)b[j]);
        if (ca != cb) return ca < cb;
        ++i;
        ++j;
    }
    return a.size() - i < b.size() - j;
}

std::string GenericPath(const fs::path& p) {
    std::error_code ec;
    const fs::path c = fs::weakly_canonical(p, ec);
    return (ec ? p : c).generic_u8string();
}

// assets/Decals/... : the project-relative spelling settings and palettes use.
std::string RelativePath(const std::string& abs) {
    std::string rel = ProjectPaths::Relativize(abs);
    std::replace(rel.begin(), rel.end(), '\\', '/');
    return rel;
}

uint32_t HashText(const char* s) {
    uint32_t h = 2166136261u;
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

// A tileable plaster texture: two octaves of value noise and a fine grain, in grey so the wall tone
// (dark, mid, light) tints it. 128 px, repeating.
uint32_t MakeWallTexture() {
    constexpr int N = 128;
    auto hash = [](int x, int y, int seed) {
        uint32_t h = (uint32_t)(x * 374761393 + y * 668265263 + seed * 2246822519u);
        h = (h ^ (h >> 13)) * 1274126177u;
        return (float)((h ^ (h >> 16)) & 0xFFFF) / 65535.0f;
    };
    auto value = [&](float u, float v, int period, int seed) {
        const float fx = u * period, fy = v * period;
        const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
        const float tx = fx - x0, ty = fy - y0;
        const float sx = tx * tx * (3.0f - 2.0f * tx), sy = ty * ty * (3.0f - 2.0f * ty);
        auto at = [&](int x, int y) { return hash(((x % period) + period) % period, ((y % period) + period) % period, seed); };
        return ImLerp(ImLerp(at(x0, y0), at(x0 + 1, y0), sx), ImLerp(at(x0, y0 + 1), at(x0 + 1, y0 + 1), sx), sy);
    };
    std::vector<unsigned char> px((size_t)N * N * 4);
    for (int y = 0; y < N; ++y)
        for (int x = 0; x < N; ++x) {
            const float u = (float)x / N, v = (float)y / N;
            float g = 0.78f + 0.22f * (value(u, v, 4, 1) * 0.6f + value(u, v, 16, 2) * 0.4f);
            g *= 0.93f + 0.07f * hash(x, y, 7);
            const unsigned char c = (unsigned char)std::lround(ImClamp(g, 0.0f, 1.0f) * 255.0f);
            unsigned char* d = &px[((size_t)y * N + x) * 4];
            d[0] = d[1] = d[2] = c;
            d[3] = 255;
        }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, N, N, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    GLStateCache::Invalidate(); // raw glBindTexture above
    return tex;
}

// The wall swatch's tint for each tone: dark, mid, light plaster.
ImU32 WallTint(int tone, float alpha) {
    static const ImU32 kTones[3] = {IM_COL32(58, 56, 54, 255), IM_COL32(118, 114, 108, 255), IM_COL32(214, 208, 198, 255)};
    ImU32 c = kTones[ImClamp(tone, 0, 2)];
    const int a = (int)(((c >> IM_COL32_A_SHIFT) & 0xFF) * ImClamp(alpha, 0.0f, 1.0f));
    return (c & ~IM_COL32_A_MASK) | ((ImU32)a << IM_COL32_A_SHIFT);
}

void DrawWall(ImDrawList* dl, uint32_t tex, ImVec2 mn, ImVec2 mx, int tone, float rounding, float alpha, float texelsPerPx) {
    if (!tex) {
        dl->AddRectFilled(mn, mx, WallTint(tone, alpha), rounding);
        return;
    }
    const float w = (mx.x - mn.x) * texelsPerPx / 128.0f, h = (mx.y - mn.y) * texelsPerPx / 128.0f;
    dl->AddImageRounded((ImTextureID)(intptr_t)tex, mn, mx, ImVec2(0, 0), ImVec2(w, h), WallTint(tone, alpha), rounding);
}

} // namespace

// --- State ---------------------------------------------------------------------------------------

DecalEditorState& EditorLayer::DecalEd() {
    if (m_DecalEd) return *m_DecalEd;
    m_DecalEd = std::make_shared<DecalEditorState>();
    DecalEditorState& ed = *m_DecalEd;
    const EditorSettings& s = EditorSettings::Get();
    ed.Brush.Radius = s.DecalBrushRadius;
    ed.Brush.Spacing = s.DecalBrushSpacing;
    ed.Brush.Density = s.DecalBrushDensity;
    ed.Brush.Size = s.DecalBrushSize;
    ed.Brush.SizeJitter = s.DecalBrushSizeJitter;
    ed.Brush.RotationJitter = s.DecalBrushRotationJitter;
    ed.Brush.Opacity = s.DecalBrushOpacity;
    ed.Brush.AlignToSurface = s.DecalBrushAlign;
    ed.Brush.FollowStroke = s.DecalBrushFollowStroke;
    ed.Grime.Spacing = s.DecalGrimeSpacing;
    ed.Grime.Density = s.DecalGrimeDensity;
    ed.Grime.MaxSites = s.DecalGrimeMax;
    if (!s.DecalRecent.empty()) ed.PlacePath = ProjectPaths::Resolve(s.DecalRecent.front());
    return ed;
}

namespace DecalEdit {

// Writes the brush and grime values back to the preferences (flushed once a frame by the editor loop).
void SaveToolSettings(const DecalEditorState& ed) {
    EditorSettings& s = EditorSettings::Get();
    s.DecalBrushRadius = ed.Brush.Radius;
    s.DecalBrushSpacing = ed.Brush.Spacing;
    s.DecalBrushDensity = ed.Brush.Density;
    s.DecalBrushSize = ed.Brush.Size;
    s.DecalBrushSizeJitter = ed.Brush.SizeJitter;
    s.DecalBrushRotationJitter = ed.Brush.RotationJitter;
    s.DecalBrushOpacity = ed.Brush.Opacity;
    s.DecalBrushAlign = ed.Brush.AlignToSurface;
    s.DecalBrushFollowStroke = ed.Brush.FollowStroke;
    s.DecalGrimeSpacing = ed.Grime.Spacing;
    s.DecalGrimeDensity = ed.Grime.Density;
    s.DecalGrimeMax = ed.Grime.MaxSites;
    EditorSettings::Save();
}

// --- Indexing ------------------------------------------------------------------------------------

void BeginScan(DecalEditorState& ed) {
    if (ed.ScanStarted) return;
    ed.ScanStarted = true;
    std::error_code ec;
    const fs::path root = fs::u8path(ProjectPaths::Resolve(kDecalRoot));
    for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = LowerCopy(it->path().extension().u8string());
        if (ext == ".decal") {
            ed.ScanQueue.push_back(it->path());
        } else if (ext == ".decalpalette") {
            auto p = DecalPalette::Load(GenericPath(it->path()));
            if (!p->Missing) ed.Palettes.push_back(std::move(p));
        }
    }
    std::sort(ed.ScanQueue.begin(), ed.ScanQueue.end());
    std::sort(ed.Palettes.begin(), ed.Palettes.end(),
              [](const auto& a, const auto& b) { return NaturalLess(a->Name, b->Name); });
    // The active palette from the preferences.
    const std::string want = EditorSettings::Get().DecalPalette;
    ed.ActivePalette = -1;
    for (int i = 0; i < (int)ed.Palettes.size(); ++i)
        if (!want.empty() && RelativePath(ed.Palettes[i]->Path) == want) ed.ActivePalette = i;
    ed.Entries.reserve(ed.ScanQueue.size());
}

int CategoryFor(DecalEditorState& ed, const std::string& path, std::unordered_map<std::string, int>& byPath) {
    if (auto it = byPath.find(path); it != byPath.end()) return it->second;
    DecalEditorState::Category c;
    const size_t slash = path.find_last_of('/');
    c.Path = path;
    c.Name = slash == std::string::npos ? path : path.substr(slash + 1);
    if (slash != std::string::npos) c.Parent = CategoryFor(ed, path.substr(0, slash), byPath);
    ed.Cats.push_back(c);
    const int idx = (int)ed.Cats.size() - 1;
    byPath[path] = idx;
    return idx;
}

// After the last file: depth-first sorted categories, entries in (category, name) order, counts.
void FinishScan(DecalEditorState& ed, std::unordered_map<std::string, int>& byPath) {
    const int n = (int)ed.Cats.size();
    std::vector<std::vector<int>> kids(n + 1); // kids[n] = roots
    for (int i = 0; i < n; ++i) kids[ed.Cats[i].Parent < 0 ? n : ed.Cats[i].Parent].push_back(i);
    for (auto& k : kids)
        std::sort(k.begin(), k.end(), [&](int a, int b) { return NaturalLess(ed.Cats[a].Name, ed.Cats[b].Name); });
    std::vector<int> order, remap(n, 0);
    std::vector<std::pair<int, int>> stack;
    for (int r = (int)kids[n].size() - 1; r >= 0; --r) stack.push_back({kids[n][r], 0});
    while (!stack.empty()) {
        const int c = stack.back().first;
        stack.pop_back();
        remap[c] = (int)order.size();
        order.push_back(c);
        for (int k = (int)kids[c].size() - 1; k >= 0; --k) stack.push_back({kids[c][k], 0});
    }
    std::vector<DecalEditorState::Category> sorted;
    sorted.reserve(n);
    for (int c : order) {
        DecalEditorState::Category cat = ed.Cats[c];
        cat.Parent = cat.Parent < 0 ? -1 : remap[cat.Parent];
        sorted.push_back(cat);
    }
    for (int i = 0; i < n; ++i) {
        sorted[i].Depth = sorted[i].Parent < 0 ? 0 : sorted[sorted[i].Parent].Depth + 1;
        sorted[i].End = i + 1;
    }
    for (int i = n - 1; i >= 0; --i)
        if (sorted[i].Parent >= 0) sorted[sorted[i].Parent].End = std::max(sorted[sorted[i].Parent].End, sorted[i].End);
    ed.Cats.swap(sorted);
    for (auto& e : ed.Entries) e.Category = remap[e.Category];
    std::stable_sort(ed.Entries.begin(), ed.Entries.end(), [](const auto& a, const auto& b) {
        return a.Category != b.Category ? a.Category < b.Category : NaturalLess(a.Name, b.Name);
    });
    for (const auto& e : ed.Entries)
        for (int c = e.Category; c >= 0; c = ed.Cats[c].Parent) ++ed.Cats[c].Count;
    // The first category opens, so the tree isn't a wall of folders at first sight.
    for (auto& c : ed.Cats) c.Open = c.Depth == 0 && false;
    (void)byPath;
    // Hearts from the preferences.
    std::unordered_set<std::string> fav(EditorSettings::Get().DecalFavorites.begin(), EditorSettings::Get().DecalFavorites.end());
    for (auto& e : ed.Entries) e.Favorite = fav.count(e.Rel) != 0;
    ed.ScanDone = true;
    ed.Dirty = true;
}

void ScanStep(DecalEditorState& ed) {
    static std::unordered_map<std::string, int> byPath;
    if (!ed.ScanStarted || ed.ScanDone) return;
    if (ed.ScanAt == 0) byPath.clear();
    int budget = kScanPerFrame;
    while (ed.ScanAt < ed.ScanQueue.size() && budget-- > 0) {
        const fs::path& file = ed.ScanQueue[ed.ScanAt++];
        const std::string abs = GenericPath(file);
        const auto asset = DecalAsset::Load(abs);
        if (!asset || asset->Missing) continue;
        DecalEditorState::Entry e;
        e.Path = abs;
        e.Rel = RelativePath(abs);
        e.Name = file.stem().u8string();
        if (e.Name.rfind("DA_", 0) == 0 && e.Name.size() > 3) e.Name = e.Name.substr(3);
        std::string category = asset->Category;
        if (category.empty()) { // the folder under assets/Decals
            category = fs::relative(file.parent_path(), fs::u8path(ProjectPaths::Resolve(kDecalRoot))).generic_u8string();
            if (category == ".") category = "Other";
        }
        e.Category = CategoryFor(ed, category, byPath);
        e.Tags = asset->Tags;
        e.Search = LowerCopy(e.Name + " " + category + " " + asset->Tags);
        if (!asset->AlbedoPath.empty()) e.Albedo = ProjectPaths::Resolve(asset->AlbedoPath);
        e.Mask = asset->LumaMask;
        e.Invert = asset->InvertMask;
        e.Base = asset->BaseColor;
        e.Size = asset->DefaultSize;
        e.Tier = asset->Tier;
        e.Channels = (uint8_t)((asset->AlbedoOp > 0) | (asset->NormalOp > 0) << 1 | (asset->RoughnessOp > 0) << 2 |
                               (asset->MetalOp > 0) << 3 | (asset->AOOp > 0) << 4 | (asset->EmissiveOp > 0) << 5);
        ed.Entries.push_back(std::move(e));
        ed.Dirty = true;
    }
    if (ed.ScanAt >= ed.ScanQueue.size()) {
        FinishScan(ed, byPath);
        byPath.clear();
        ed.ScanQueue.clear();
        ed.ScanQueue.shrink_to_fit();
    }
}

bool InCategory(const DecalEditorState& ed, int entryCat, int selected) {
    return selected < 0 || (entryCat >= selected && entryCat < ed.Cats[selected].End);
}

void RebuildVisible(DecalEditorState& ed) {
    ed.Dirty = false;
    ed.Visible.clear();
    ed.Visible.reserve(ed.Entries.size());
    // Search words: every one must appear in the entry's name, category or tags.
    const std::string query = LowerCopy(ed.Search);
    std::vector<std::string> words;
    for (size_t i = 0; i < query.size();) {
        while (i < query.size() && query[i] == ' ') ++i;
        const size_t j = query.find(' ', i);
        if (i < query.size()) words.push_back(query.substr(i, (j == std::string::npos ? query.size() : j) - i));
        i = j == std::string::npos ? query.size() : j;
    }
    auto matches = [&](const DecalEditorState::Entry& e) {
        for (const std::string& w : words)
            if (e.Search.find(w) == std::string::npos) return false;
        return true;
    };
    if (ed.SelectedCategory == DecalEditorState::kRecent) {
        std::unordered_map<std::string, int> byRel;
        for (int i = 0; i < (int)ed.Entries.size(); ++i) byRel[ed.Entries[i].Rel] = i;
        for (const std::string& rel : EditorSettings::Get().DecalRecent)
            if (auto it = byRel.find(rel); it != byRel.end() && matches(ed.Entries[it->second])) ed.Visible.push_back(it->second);
        return;
    }
    for (int i = 0; i < (int)ed.Entries.size(); ++i) {
        const auto& e = ed.Entries[i];
        if (ed.SelectedCategory == DecalEditorState::kFavorites) {
            if (!e.Favorite) continue;
        } else if (ed.SelectedCategory >= 0 && !InCategory(ed, e.Category, ed.SelectedCategory)) {
            continue;
        }
        if (matches(e)) ed.Visible.push_back(i);
    }
}

int FindEntry(const DecalEditorState& ed, const std::string& absPath) {
    for (int i = 0; i < (int)ed.Entries.size(); ++i)
        if (ed.Entries[i].Path == absPath) return i;
    return -1;
}

// The entries the brush draws from when its source is a category (rebuilt when the category or the index changes).
void RefreshPool(DecalEditorState& ed) {
    const std::string& cat = EditorSettings::Get().DecalSourceCategory;
    if (ed.PoolFor == cat && ed.PoolEntries == (int)ed.Entries.size()) return;
    ed.PoolFor = cat;
    ed.PoolEntries = (int)ed.Entries.size();
    ed.CategoryPool.clear();
    int c = -1;
    for (int i = 0; i < (int)ed.Cats.size(); ++i)
        if (ed.Cats[i].Path == cat) { c = i; break; }
    if (c < 0) return;
    for (int i = 0; i < (int)ed.Entries.size(); ++i)
        if (InCategory(ed, ed.Entries[i].Category, c)) ed.CategoryPool.push_back(i);
}

// The source the brush paints from: 0 a single decal, 1 a category, 2 a palette.
int SourceKind(const DecalEditorState& ed) {
    if (ed.ActivePalette >= 0 && ed.ActivePalette < (int)ed.Palettes.size()) return 2;
    return EditorSettings::Get().DecalSourceCategory.empty() ? 0 : 1;
}

void NoteRecent(DecalEditorState& ed, const std::string& absPath) {
    EditorSettings& s = EditorSettings::Get();
    const std::string rel = RelativePath(absPath);
    s.DecalRecent.erase(std::remove(s.DecalRecent.begin(), s.DecalRecent.end(), rel), s.DecalRecent.end());
    s.DecalRecent.insert(s.DecalRecent.begin(), rel);
    if ((int)s.DecalRecent.size() > kMaxRecent) s.DecalRecent.resize(kMaxRecent);
    EditorSettings::Save();
    if (ed.SelectedCategory == DecalEditorState::kRecent) ed.Dirty = true;
}

void SetPalette(DecalEditorState& ed, int index) {
    EditorSettings& s = EditorSettings::Get();
    ed.ActivePalette = index;
    s.DecalPalette = index >= 0 ? RelativePath(ed.Palettes[index]->Path) : std::string();
    if (index >= 0) s.DecalSourceCategory.clear();
    EditorSettings::Save();
}

void ToggleFavorite(DecalEditorState& ed, int entry) {
    auto& e = ed.Entries[entry];
    EditorSettings& s = EditorSettings::Get();
    e.Favorite = !e.Favorite;
    if (e.Favorite) s.DecalFavorites.push_back(e.Rel);
    else s.DecalFavorites.erase(std::remove(s.DecalFavorites.begin(), s.DecalFavorites.end(), e.Rel), s.DecalFavorites.end());
    EditorSettings::Save();
    if (ed.SelectedCategory == DecalEditorState::kFavorites) ed.Dirty = true;
    ed.HeartPops[ed.NextHeart] = entry;
    static const ImGuiID kHeartId = ImHashStr("##decalHeart");
    if (Glass::Motion().CategoryOn(kViewport))
        Glass::Motion().Pulse(kHeartId + (ImGuiID)ed.NextHeart, Glass::DecalHeartPop.durationMs * 0.001f);
    ed.NextHeart = (ed.NextHeart + 1) % 4;
    EditorSound::Play(EditorSound::Event::LibraryFavourite, -1.0f, e.Favorite ? 1.0f : 0.84f);
}

// --- Drawing -------------------------------------------------------------------------------------

AssetThumbnailLoader::Thumb ThumbFor(AssetThumbnailLoader& thumbs, const DecalEditorState::Entry& e) {
    if (e.Albedo.empty()) return {};
    return thumbs.Get(e.Albedo, e.Mask ? (e.Invert ? AssetThumbnailLoader::Source::MaskInverted : AssetThumbnailLoader::Source::Mask)
                                       : AssetThumbnailLoader::Source::Image);
}

// A decal over a wall swatch: the swatch, then the thumbnail tinted by the decal's base colour, aspect kept.
void DrawDecalOnWall(ImDrawList* dl, AssetThumbnailLoader& thumbs, DecalEditorState& ed, const DecalEditorState::Entry& e,
                     ImVec2 mn, ImVec2 mx, float rounding, float alpha, float inset, float texelsPerPx) {
    DrawWall(dl, ed.WallTex, mn, mx, ed.WallTone, rounding, alpha, texelsPerPx);
    const AssetThumbnailLoader::Thumb t = ThumbFor(thumbs, e);
    if (!t.Tex) return;
    const ImVec2 uv0(0.0f, t.FlipV ? 1.0f : 0.0f), uv1(1.0f, t.FlipV ? 0.0f : 1.0f);
    const glm::vec3 c = glm::clamp(e.Base, glm::vec3(0.0f), glm::vec3(1.0f));
    const ImU32 tint = IM_COL32((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255), (int)(255 * ImClamp(alpha, 0.0f, 1.0f)));
    dl->AddImage((ImTextureID)(intptr_t)t.Tex, ImVec2(mn.x + inset, mn.y + inset), ImVec2(mx.x - inset, mx.y - inset), uv0, uv1, tint);
}

const char* ChannelNames[6] = {"Albedo", "Normal", "Rough", "Metal", "AO", "Emissive"};

void DrawHoverCard(DecalEditorState& ed, AssetThumbnailLoader& thumbs, const DecalEditorState::Entry& e, float alpha, float uiScale) {
    const float side = 196.0f * uiScale;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImClamp(alpha, 0.0f, 1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * uiScale, 8.0f * uiScale));
    if (ImGui::BeginTooltip()) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        DrawDecalOnWall(dl, thumbs, ed, e, p, ImVec2(p.x + side, p.y + side), 6.0f * uiScale, 1.0f, 6.0f * uiScale, 1.0f);
        dl->AddRect(p, ImVec2(p.x + side, p.y + side), EditorTheme::U32(EditorTheme::Hairline), 6.0f * uiScale);
        ImGui::Dummy(ImVec2(side, side));
        ImGui::Spacing();
        EditorTheme::PushBody();
        ImGui::TextUnformatted(e.Name.c_str());
        EditorTheme::PopFont();
        ImGui::TextColored(EditorTheme::Dim, "%s", ed.Cats[e.Category].Path.c_str());
        ImGui::TextColored(EditorTheme::Secondary, "%.2f x %.2f m   %d px   ", e.Size.x, e.Size.y, e.Tier);
        ImGui::SameLine(0.0f, 0.0f);
        bool first = true;
        for (int c = 0; c < 6; ++c) {
            if (!(e.Channels & (1 << c))) continue;
            ImGui::TextColored(EditorTheme::Dim, "%s%s", first ? "" : " . ", ChannelNames[c]);
            ImGui::SameLine(0.0f, 0.0f);
            first = false;
        }
        ImGui::NewLine();
        if (!e.Tags.empty()) ImGui::TextColored(EditorTheme::Dim, "%s", e.Tags.c_str());
        ImGui::EndTooltip();
    }
    ImGui::PopStyleVar(2);
}

// A heart from the classic parametric curve, `r` px from its centre to its point.
void DrawHeart(ImDrawList* dl, ImVec2 c, float r, ImU32 fill, ImU32 line, bool filled) {
    constexpr int N = 28;
    ImVec2 pts[N];
    const float k = r / 15.0f;
    for (int i = 0; i < N; ++i) {
        const float t = 2.0f * IM_PI * (float)i / (float)N;
        const float x = 16.0f * std::pow(std::sin(t), 3.0f);
        const float y = 13.0f * std::cos(t) - 5.0f * std::cos(2.0f * t) - 2.0f * std::cos(3.0f * t) - std::cos(4.0f * t);
        pts[i] = ImVec2(c.x + x * k, c.y - (y - 2.0f) * k);
    }
    if (filled) dl->AddConcavePolyFilled(pts, N, fill);
    else dl->AddPolyline(pts, N, line, ImDrawFlags_Closed, 1.4f);
}

} // namespace DecalEdit
using namespace DecalEdit;

// --- Tool state --------------------------------------------------------------------------------

void EditorLayer::SetDecalTool(DecalToolMode mode) {
    if (mode == m_DecalTool) mode = DecalToolMode::None;
    if (mode != DecalToolMode::None) {
        m_HandTool = false;
        m_MeasureTool = false;
        m_MeasurePoints.clear();
        DecalEditorState& ed = DecalEd();
        BeginScan(ed);
        // Nothing to place or paint with yet: show the library, where it is picked.
        if (mode == DecalToolMode::Place && ed.PlacePath.empty()) m_ShowDecalLibrary = true;
        if (mode == DecalToolMode::Brush && SourceKind(ed) == 0 && ed.PlacePath.empty()) m_ShowDecalLibrary = true;
    }
    m_DecalTool = mode;
}

bool EditorLayer::DecalToolOwnsWheel() const {
    if (m_DecalTool == DecalToolMode::None || m_Radial.Open) return false;
    const ImGuiIO& io = ImGui::GetIO();
    return IsMouseOverSceneViewport() && !io.KeyCtrl && !io.KeyAlt && !ImGui::IsMouseDown(ImGuiMouseButton_Right);
}

bool EditorLayer::DecalGhostPreview(const DecalComponent*& comp, glm::mat4& world) const {
    if (!m_DecalEd || !m_DecalEd->GhostView.Active || m_DecalTool == DecalToolMode::None) return false;
    comp = &m_DecalEd->GhostView.Comp;
    world = m_DecalEd->GhostView.World;
    return true;
}

// --- The radial menu's decal mode -----------------------------------------------------------------

int EditorLayer::DecalRadialBuild() {
    DecalEditorState& ed = DecalEd();
    BeginScan(ed);
    ed.Radial.clear();
    const EditorSettings& s = EditorSettings::Get();
    // A wedge has room for about six characters, so each slot gets a one-word label; the hub shows
    // the hovered slot's full name.
    auto shortLabel = [](std::string name) {
        if (name.rfind("DP_", 0) == 0) name = name.substr(3);
        static const std::pair<const char*, const char*> kShort[] = {
            {"Water Damage", "Water"}, {"Gang Turf", "Turf"}, {"WarPaint", "War"}, {"Graffiti", "Graffiti"}};
        for (const auto& [full, label] : kShort)
            if (name == full) return std::string(label);
        // A trailing number names a decal best ("Stain_591" -> "#591").
        size_t digits = name.size();
        while (digits > 0 && std::isdigit((unsigned char)name[digits - 1])) --digits;
        if (digits < name.size() && name.size() - digits <= 4 && digits > 0) return "#" + name.substr(digits);
        // Else the first word, at most seven letters.
        size_t cut = name.find_first_of(" _-");
        for (size_t k = 1; k < name.size() && k < cut; ++k)
            if (std::isupper((unsigned char)name[k]) && std::islower((unsigned char)name[k - 1])) { cut = k; break; }
        name = name.substr(0, std::min<size_t>(cut, 7));
        return name;
    };
    // A distinct glyph per palette / top-level category, by name; a generic one for anything new.
    auto iconFor = [](const std::string& name, const char* fallback) -> const char* {
        static const std::pair<const char*, const char*> kIcons[] = {
            {"Squat", ICON_FA_HOUSE_CRACK}, {"Water Damage", ICON_FA_FAUCET_DRIP}, {"Gang Turf", ICON_FA_CROWN},
            {"Graffiti", ICON_FA_SPRAY_CAN}, {"Grime", ICON_FA_BIOHAZARD}, {"Signs", ICON_FA_SIGN_HANGING},
            {"Stains", ICON_FA_DROPLET}, {"Stamps", ICON_FA_STAMP}, {"WarPaint", ICON_FA_SKULL}};
        for (const auto& [n, icon] : kIcons)
            if (name == n) return icon;
        return fallback;
    };
    for (int i = 0; i < (int)ed.Palettes.size() && i < kRadialPalettes; ++i) {
        DecalEditorState::RadialSlot slot;
        slot.What = DecalEditorState::RadialSlot::Palette;
        slot.Index = i;
        slot.Label = shortLabel(ed.Palettes[i]->Name);
        slot.FullName = ed.Palettes[i]->Name;
        slot.Icon = iconFor(ed.Palettes[i]->Name, ICON_FA_PALETTE);
        slot.On = ed.ActivePalette == i;
        ed.Radial.push_back(std::move(slot));
    }
    int cats = 0;
    for (int i = 0; i < (int)ed.Cats.size() && cats < kRadialCategories; ++i) {
        if (ed.Cats[i].Parent >= 0) continue;
        DecalEditorState::RadialSlot slot;
        slot.What = DecalEditorState::RadialSlot::Category;
        slot.Index = i;
        slot.Label = shortLabel(ed.Cats[i].Name);
        slot.FullName = ed.Cats[i].Name;
        slot.Icon = iconFor(ed.Cats[i].Name, ICON_FA_FOLDER_TREE);
        slot.On = SourceKind(ed) == 1 && s.DecalSourceCategory.rfind(ed.Cats[i].Path, 0) == 0;
        ed.Radial.push_back(std::move(slot));
        ++cats;
    }
    std::unordered_map<std::string, int> byRel;
    for (int i = 0; i < (int)ed.Entries.size(); ++i) byRel[ed.Entries[i].Rel] = i;
    for (const std::string& rel : s.DecalRecent) {
        if ((int)ed.Radial.size() >= kRadialMax) break;
        const auto it = byRel.find(rel);
        if (it == byRel.end()) continue;
        DecalEditorState::RadialSlot slot;
        slot.What = DecalEditorState::RadialSlot::Recent;
        slot.Index = it->second;
        slot.Label = shortLabel(ed.Entries[it->second].Name);
        slot.FullName = ed.Entries[it->second].Name;
        slot.Icon = ICON_FA_CLOCK_ROTATE_LEFT;
        slot.On = SourceKind(ed) == 0 && ed.PlacePath == ed.Entries[it->second].Path;
        ed.Radial.push_back(std::move(slot));
    }
    return (int)ed.Radial.size();
}

bool EditorLayer::DecalRadialItem(int i, const char*& icon, const char*& label) const {
    if (!m_DecalEd || i < 0 || i >= (int)m_DecalEd->Radial.size()) return false;
    const auto& slot = m_DecalEd->Radial[i];
    icon = slot.Icon;
    label = slot.Label.c_str();
    return slot.On;
}

const char* EditorLayer::DecalRadialFullName(int i) const {
    if (!m_DecalEd || i < 0 || i >= (int)m_DecalEd->Radial.size()) return "";
    return m_DecalEd->Radial[i].FullName.c_str();
}

void EditorLayer::RunDecalRadialItem(int i) {
    DecalEditorState& ed = DecalEd();
    if (i < 0 || i >= (int)ed.Radial.size()) return;
    const auto slot = ed.Radial[i];
    EditorSettings& s = EditorSettings::Get();
    switch (slot.What) {
        case DecalEditorState::RadialSlot::Palette:
            SetPalette(ed, slot.Index);
            break;
        case DecalEditorState::RadialSlot::Category:
            SetPalette(ed, -1);
            s.DecalSourceCategory = ed.Cats[slot.Index].Path;
            EditorSettings::Save();
            break;
        case DecalEditorState::RadialSlot::Recent:
            SetPalette(ed, -1);
            s.DecalSourceCategory.clear();
            ed.PlacePath = ed.Entries[slot.Index].Path;
            EditorSettings::Save();
            break;
    }
}

// --- The panel -------------------------------------------------------------------------------------

namespace {

// A small round pill button with a glyph, for the strip.
bool StripToggle(const char* id, const char* icon, const char* label, bool on, const char* tip) {
    const bool clicked = EditorUIPrimitives::Pill(id, icon, label, on);
    if (ImGui::IsItemHovered()) EditorUI::SetTooltip("%s", tip);
    return clicked;
}

} // namespace

void EditorLayer::DrawDecalLibraryPanel(World& world) {
    if (!m_ShowDecalLibrary) {
        if (m_DecalEd) m_DecalEd->HoverEntry = -1;
        return;
    }
    DecalEditorState& ed = DecalEd();
    BeginScan(ed);
    ScanStep(ed);
    EditorSettings& settings = EditorSettings::Get();
    Glass::ImAnimContext& motion = Glass::Motion();
    const bool animate = motion.CategoryOn(kViewport);
    static const ImGuiID kCascadeId = ImHashStr("##decalCascade");
    static const ImGuiID kHeartId = ImHashStr("##decalHeart");
    static const ImGuiID kCardId = ImHashStr("##decalCard");

    ImGui::SetNextWindowSize(ImVec2(760.0f * m_UIScale, 560.0f * m_UIScale), ImGuiCond_FirstUseEver);
    PushTabChromeText();
    const bool open = ImGui::Begin(EditorPanels::DecalLibrary, &m_ShowDecalLibrary);
    PopTabChromeText();
    if (!open) {
        ed.HoverEntry = -1;
        ImGui::End();
        return;
    }
    if (!ed.WallTex && ImGui::GetCurrentContext()) ed.WallTex = MakeWallTexture();

    // --- Strip: tools, source, popovers ---------------------------------------------------------
    EditorSoundEvents::ScopedClickSilence quiet; // pills and tile clicks make their own sounds
    {
        const DecalToolMode tool = m_DecalTool;
        if (StripToggle("##place", ICON_FA_STAMP, "Place", tool == DecalToolMode::Place,
                        "Place Decal (P): click a surface to stamp the selected decal. Scroll turns it, Shift+Scroll sizes it."))
            SetDecalTool(DecalToolMode::Place);
        ImGui::SameLine();
        if (StripToggle("##brush", ICON_FA_SPRAY_CAN, "Brush", tool == DecalToolMode::Brush,
                        "Decal Brush (B): paint along a stroke. Ctrl erases. Hold Space to pick what to paint."))
            SetDecalTool(DecalToolMode::Brush);
        ImGui::SameLine();
        ImGui::TextColored(EditorTheme::Hairline, "|");
        ImGui::SameLine();

        int src = SourceKind(ed);
        static const char* const kSrc[3] = {"Decal", "Category", "Palette"};
        ImGui::BeginDisabled(false);
        const int before = src;
        if (EditorInternal::Segmented("##src", &src, kSrc, 3)) {
            EditorSound::Play(EditorSound::Event::PaletteSwap);
            if (src == 0) {
                SetPalette(ed, -1);
                settings.DecalSourceCategory.clear();
            } else if (src == 1) {
                SetPalette(ed, -1);
                settings.DecalSourceCategory = ed.SelectedCategory >= 0 ? ed.Cats[ed.SelectedCategory].Path
                                             : ed.Cats.empty() ? std::string() : ed.Cats.front().Path;
            } else if (!ed.Palettes.empty()) {
                SetPalette(ed, std::max(ed.ActivePalette, 0));
            }
            EditorSettings::Save();
            (void)before;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            EditorUI::SetTooltip("What the Decal Brush paints from: the selected decal, a category, or a palette.");
        ImGui::SameLine();

        src = SourceKind(ed);
        if (src == 2) {
            ImGui::SetNextItemWidth(ImMin(ImGui::GetContentRegionAvail().x * 0.3f, 190.0f * m_UIScale));
            const char* current = ed.ActivePalette >= 0 ? ed.Palettes[ed.ActivePalette]->Name.c_str() : "No palette";
            if (ImGui::BeginCombo("##palette", current)) {
                for (int i = 0; i < (int)ed.Palettes.size(); ++i) {
                    ImGui::PushID(i);
                    if (ImGui::Selectable(ed.Palettes[i]->Name.c_str(), i == ed.ActivePalette)) {
                        SetPalette(ed, i);
                        EditorSound::Play(EditorSound::Event::PaletteSwap);
                    }
                    if (ImGui::IsItemHovered() && !ed.Palettes[i]->Description.empty())
                        EditorUI::SetTooltip("%s", ed.Palettes[i]->Description.c_str());
                    ImGui::PopID();
                }
                ImGui::Separator();
                if (ImGui::Selectable(ICON_FA_HEART "  New from favourites...")) ImGui::OpenPopup("##newPalette");
                ImGui::EndCombo();
            }
        } else if (src == 1) {
            ImGui::TextColored(EditorTheme::Secondary, "%s", settings.DecalSourceCategory.c_str());
        } else {
            const int cur = FindEntry(ed, ed.PlacePath);
            ImGui::TextColored(EditorTheme::Secondary, "%s", cur >= 0 ? ed.Entries[cur].Name.c_str() : "Pick a decal below");
        }

        if (ImGui::BeginPopup("##newPalette")) {
            ImGui::TextColored(EditorTheme::Secondary, "Save the %d favourites as a palette",
                               (int)settings.DecalFavorites.size());
            ImGui::SetNextItemWidth(220.0f * m_UIScale);
            ImGui::InputTextWithHint("##pname", "Name", ed.PaletteName, sizeof(ed.PaletteName));
            ImGui::BeginDisabled(!ed.PaletteName[0] || settings.DecalFavorites.empty());
            if (EditorInternal::PrimaryButton("Save")) {
                DecalPalette p;
                std::string name = ed.PaletteName;
                p.Name = name;
                p.Description = "From the Decal Library favourites.";
                for (const std::string& rel : settings.DecalFavorites) {
                    DecalPaletteEntry e;
                    e.DecalPath = rel;
                    p.Entries.push_back(e);
                }
                std::string file = name;
                for (char& c : file) if (!std::isalnum((unsigned char)c) && c != '_' && c != '-') c = '_';
                std::error_code ec;
                const fs::path dir = fs::u8path(ProjectPaths::Resolve("assets/Decals/Palettes"));
                fs::create_directories(dir, ec);
                p.Path = GenericPath(dir / ("DP_" + file + ".decalpalette"));
                if (p.Save()) {
                    ed.Palettes.push_back(std::make_shared<DecalPalette>(p));
                    SetPalette(ed, (int)ed.Palettes.size() - 1);
                    EditorSound::Play(EditorSound::Event::PaletteSwap);
                    ed.PaletteName[0] = '\0';
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndDisabled();
            ImGui::EndPopup();
        }

        // Right end: popovers.
        const float btnW = ImGui::CalcTextSize("Auto-grime").x + 56.0f * m_UIScale;
        ImGui::SameLine(ImMax(ImGui::GetCursorPosX() + 8.0f * m_UIScale, ImGui::GetWindowContentRegionMax().x - btnW * 2.0f - 8.0f * m_UIScale));
        if (StripToggle("##brushOpts", ICON_FA_SLIDERS, "Brush", false, "Brush settings: radius, spacing, density, size and rotation jitter."))
            ImGui::OpenPopup("##brushPopup");
        ImGui::SameLine();
        if (StripToggle("##grimeOpts", ICON_FA_WAND_MAGIC_SPARKLES, "Auto-grime", false,
                        "Probe the selected room and dress its wall bases, corners, windows, doors and pipes from the palette."))
            ImGui::OpenPopup("##grimePopup");
    }

    // Brush popover.
    if (ImGui::BeginPopup("##brushPopup")) {
        DecalTools::BrushSettings& b = ed.Brush;
        bool changed = false;
        ImGui::PushItemWidth(190.0f * m_UIScale);
        SectionHeader("Decal Brush");
        SettingsLabel("Radius");
        changed |= EditorUI::SliderFloat("##radius", &b.Radius, 0.02f, 3.0f, "%.2f m");
        SettingsLabel("Spacing");
        changed |= EditorUI::SliderFloat("##spacing", &b.Spacing, 0.05f, 3.0f, "%.2f m");
        SettingsLabel("Density");
        changed |= EditorUI::SliderFloat("##density", &b.Density, 0.0f, 6.0f, "%.1f / dab");
        SettingsLabel("Size");
        changed |= EditorUI::SliderFloat("##size", &b.Size, 0.1f, 4.0f, "%.2fx");
        SettingsLabel("Size jitter");
        changed |= EditorUI::SliderFloat("##sizej", &b.SizeJitter, 0.0f, 0.9f, "%.0f%%", 0);
        SettingsLabel("Rotation jitter");
        changed |= EditorUI::SliderFloat("##rotj", &b.RotationJitter, 0.0f, 180.0f, "%.0f deg");
        SettingsLabel("Opacity");
        changed |= EditorUI::SliderFloat("##opacity", &b.Opacity, 0.05f, 1.0f, "%.2f");
        ImGui::PopItemWidth();
        changed |= SettingsCheckbox("Align to surface", &b.AlignToSurface);
        changed |= SettingsCheckbox("Follow stroke", &b.FollowStroke);
        HintText("Ctrl erases. Scroll sizes the radius, Shift+Scroll the decals.");
        if (changed) SaveToolSettings(ed);
        ImGui::EndPopup();
    }

    // Auto-grime popover.
    if (ImGui::BeginPopup("##grimePopup")) {
        ImGui::PushItemWidth(190.0f * m_UIScale);
        SectionHeader("Auto-grime");
        HintText("Select a room (its walls and floor, or the object that holds them), then run. Decals go under one "
                 "'Grime (auto)' object.");
        bool changed = false;
        SettingsLabel("Palette");
        ImGui::TextColored(EditorTheme::Secondary, "%s",
                           ed.ActivePalette >= 0 ? ed.Palettes[ed.ActivePalette]->Name.c_str()
                           : ed.Palettes.empty() ? "none found" : ed.Palettes.front()->Name.c_str());
        SettingsLabel("Seed");
        ImGui::SetNextItemWidth(110.0f * m_UIScale);
        ImGui::InputInt("##seed", &ed.GrimeSeed, 0, 0);
        ImGui::SameLine();
        if (EditorInternal::ActionButton(ICON_FA_DICE, "A new seed")) {
            ed.GrimeSeed = (int)(DecalTools::Mix((uint32_t)ed.GrimeSeed, (uint32_t)ImGui::GetFrameCount()) & 0xFFFFu);
            EditorSound::Play(EditorSound::Event::Randomize);
        }
        SettingsLabel("Density");
        changed |= EditorUI::SliderFloat("##gd", &ed.Grime.Density, 0.0f, 2.0f, "%.2f");
        SettingsLabel("Probe spacing");
        changed |= EditorUI::SliderFloat("##gs", &ed.Grime.Spacing, 0.3f, 2.0f, "%.2f m");
        SettingsLabel("Max decals");
        changed |= EditorUI::SliderInt("##gm", &ed.Grime.MaxSites, 10, 400);
        ImGui::PopItemWidth();
        SettingsLabel("Where");
        {
            static const uint32_t kBits[8] = {DecalSites::Wall, DecalSites::Base, DecalSites::Corner, DecalSites::Window,
                                              DecalSites::Door, DecalSites::Pipe, DecalSites::Ceiling, DecalSites::Floor};
            static const char* const kNames[8] = {"Walls", "Bases", "Corners", "Windows", "Doors", "Pipes", "Ceiling", "Floor"};
            const float wrap = ImGui::GetCursorPosX();
            const float limit = wrap + 300.0f * m_UIScale;
            for (int i = 0; i < 8; ++i) {
                ImGui::PushID(i);
                const bool on = (ed.GrimeKinds & kBits[i]) != 0;
                const float w = ImGui::CalcTextSize(kNames[i]).x + 26.0f * m_UIScale;
                if (i > 0 && ImGui::GetCursorPosX() + w < limit) ImGui::SameLine();
                if (EditorUIPrimitives::Pill("##k", nullptr, kNames[i], on)) ed.GrimeKinds ^= kBits[i];
                ImGui::PopID();
            }
        }
        SettingsCheckbox("Replace the last pass", &ed.GrimeReplace);
        const bool canRun = HasAnySelection();
        ImGui::BeginDisabled(!canRun);
        if (EditorInternal::PrimaryButton(ICON_FA_WAND_MAGIC_SPARKLES "  Run on selection")) {
            RunAutoGrime(world);
            changed = true;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (EditorInternal::SecondaryButton("Clear")) ClearAutoGrime(world);
        if (!canRun) HintText("Nothing selected.");
        if (!ed.GrimeStatus.empty()) HintText(ed.GrimeStatus.c_str());
        if (changed) SaveToolSettings(ed);
        ImGui::EndPopup();
    }

    // The palette's entries as a strip of previews.
    if (SourceKind(ed) == 2 && ed.ActivePalette >= 0) {
        const DecalPalette& pal = *ed.Palettes[ed.ActivePalette];
        std::unordered_map<std::string, int> byPath;
        const float cell = 26.0f * m_UIScale;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float limit = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
        float total = 0.0f;
        for (const auto& e : pal.Entries) total += e.Weight;
        int shown = 0;
        for (const DecalPaletteEntry& pe : pal.Entries) {
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (p.x + cell > limit || shown >= 40) break;
            const int idx = FindEntry(ed, ProjectPaths::Resolve(pe.DecalPath));
            ImGui::PushID(shown);
            ImGui::InvisibleButton("##pe", ImVec2(cell, cell));
            if (idx >= 0) DrawDecalOnWall(dl, m_AssetThumbs, ed, ed.Entries[idx], p, ImVec2(p.x + cell, p.y + cell), 4.0f, 1.0f, 2.0f, 2.0f);
            else dl->AddRectFilled(p, ImVec2(p.x + cell, p.y + cell), EditorTheme::U32(EditorTheme::Raised), 4.0f);
            if (ImGui::IsItemHovered())
                EditorUI::SetTooltip("%s\n%.0f%% of picks", idx >= 0 ? ed.Entries[idx].Name.c_str() : pe.DecalPath.c_str(),
                                     total > 0.0f ? 100.0f * pe.Weight / total : 0.0f);
            ImGui::PopID();
            ImGui::SameLine(0.0f, 3.0f * m_UIScale);
            ++shown;
        }
        ImGui::NewLine();
    }
    ImGui::Dummy(ImVec2(0.0f, 2.0f * m_UIScale));

    // --- Search row ------------------------------------------------------------------------------
    {
        const float controls = 140.0f * m_UIScale;
        if (SearchField("decalSearch", ed.Search, sizeof(ed.Search), "Search decals, categories, tags", -controls)) ed.Dirty = true;
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70.0f * m_UIScale);
        float tile = settings.DecalLibraryTile;
        if (EditorUI::SliderFloat("##tile", &tile, 48.0f, 160.0f, "")) {
            settings.DecalLibraryTile = tile;
            EditorSettings::Save();
        }
        if (ImGui::IsItemHovered()) EditorUI::SetTooltip("Tile size");
        ImGui::SameLine();
        static const char* const kTones[3] = {"Dark wall", "Mid wall", "Light wall"};
        if (EditorInternal::ActionButton(ICON_FA_CIRCLE_HALF_STROKE, kTones[ImClamp(ed.WallTone, 0, 2)])) {
            ed.WallTone = (ed.WallTone + 1) % 3;
            EditorSound::Play(EditorSound::Event::SwatchPick);
        }
    }
    const uint32_t hash = HashText(ed.Search);
    if (hash != ed.SearchHash) {
        ed.SearchHash = hash;
        ed.Dirty = true;
    }

    // --- Body ------------------------------------------------------------------------------------
    if (ed.Dirty) {
        RebuildVisible(ed);
        ed.CascadeKey++;
    }
    static int lastCascade = -1;
    static bool wasShown = false;
    if (ed.CascadeKey != lastCascade || !wasShown) {
        lastCascade = ed.CascadeKey;
        if (animate && !ed.Visible.empty()) {
            const float total = Glass::StaggerDelay(Glass::DecalTileStaggerCap, Glass::DecalTileStaggerMs, Glass::DecalTileStaggerCap) +
                                Glass::DecalTileIn.durationMs * 0.001f;
            motion.Pulse(kCascadeId, total);
        }
    }
    wasShown = true;

    const float treeW = ImClamp(ImGui::GetContentRegionAvail().x * 0.24f, 130.0f * m_UIScale, 220.0f * m_UIScale);
    ImGui::BeginChild("##decalTree", ImVec2(treeW, 0.0f), ImGuiChildFlags_None);
    {
        auto row = [&](const char* icon, const char* label, int count, int id, int depth, bool hasKids, bool* openFlag) -> bool {
            const bool sel = ed.SelectedCategory == id;
            const float h = ImGui::GetFrameHeight();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x;
            ImGui::PushID(id + 10);
            ImGui::PushID(label);
            const bool clicked = ImGui::InvisibleButton("##row", ImVec2(w, h));
            const bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            ImGui::PopID();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            EditorUIPrimitives::DrawRowState(dl, p, ImVec2(p.x + w, p.y + h), sel, hov);
            const float indent = 6.0f * m_UIScale + depth * 12.0f * m_UIScale;
            bool chevronHit = false;
            if (hasKids && openFlag) {
                const ImVec2 c(p.x + indent + 5.0f * m_UIScale, p.y + h * 0.5f);
                const ImVec2 mouse = ImGui::GetIO().MousePos;
                chevronHit = clicked && std::abs(mouse.x - c.x) < 9.0f * m_UIScale;
                EditorUIPrimitives::DrawChevron(dl, c, 4.0f * m_UIScale, *openFlag ? IM_PI * 0.5f : 0.0f,
                                                EditorTheme::U32(hov || sel ? EditorTheme::Text : EditorTheme::Dim));
            }
            float x = p.x + indent + 14.0f * m_UIScale;
            if (icon) {
                dl->AddText(ImVec2(x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), EditorTheme::U32(sel ? EditorTheme::AccentBright : EditorTheme::Secondary), icon);
                x += ImGui::CalcTextSize(icon).x + 6.0f * m_UIScale;
            }
            char cnt[16];
            std::snprintf(cnt, sizeof(cnt), "%d", count);
            const float cw = ImGui::CalcTextSize(cnt).x;
            EditorUIPrimitives::TextEllipsis(dl, ImVec2(x, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), p.x + w - cw - 10.0f * m_UIScale,
                                             EditorTheme::U32(sel ? EditorTheme::AccentBright : EditorTheme::Text), label);
            dl->AddText(ImVec2(p.x + w - cw - 6.0f * m_UIScale, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f), EditorTheme::U32(EditorTheme::Dim), cnt);
            if (chevronHit) {
                *openFlag = !*openFlag;
                EditorSound::Play(*openFlag ? EditorSound::Event::TreeExpand : EditorSound::Event::TreeCollapse);
                return false;
            }
            if (clicked) {
                ed.SelectedCategory = id;
                if (openFlag && hasKids) *openFlag = true;
                ed.Dirty = true;
                EditorSound::Play(EditorSound::Event::TabSelect);
            }
            return clicked;
        };
        int favCount = 0;
        for (const auto& e : ed.Entries) favCount += e.Favorite;
        row(ICON_FA_LAYER_GROUP, "All Decals", (int)ed.Entries.size(), DecalEditorState::kAllCategories, 0, false, nullptr);
        row(ICON_FA_HEART, "Favourites", favCount, DecalEditorState::kFavorites, 0, false, nullptr);
        row(ICON_FA_CLOCK_ROTATE_LEFT, "Recent", (int)settings.DecalRecent.size(), DecalEditorState::kRecent, 0, false, nullptr);
        ImGui::Dummy(ImVec2(0.0f, 4.0f * m_UIScale));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 2.0f * m_UIScale));
        const int n = (int)ed.Cats.size();
        for (int i = 0; i < n; ++i) {
            const bool hasKids = ed.Cats[i].End > i + 1;
            bool& catOpen = ed.Cats[i].Open;
            row(hasKids ? nullptr : nullptr, ed.Cats[i].Name.c_str(), ed.Cats[i].Count, i, ed.Cats[i].Depth, hasKids, &catOpen);
            if (hasKids && !catOpen) i = ed.Cats[i].End - 1; // skip the collapsed subtree
        }
        if (!ed.ScanDone) {
            ImGui::Dummy(ImVec2(0.0f, 4.0f * m_UIScale));
            ImGui::TextColored(EditorTheme::Dim, "Indexing %d / %d", (int)ed.Entries.size(), (int)(ed.ScanQueue.size()));
        }
    }
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##decalTiles", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None);
    {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float tile = ImClamp(settings.DecalLibraryTile, 48.0f, 200.0f) * m_UIScale;
        const float gap = 8.0f * m_UIScale;
        const float availW = ImGui::GetContentRegionAvail().x;
        const int cols = std::max(1, (int)std::floor((availW + gap) / (tile + gap)));
        const int rows = ((int)ed.Visible.size() + cols - 1) / cols;
        const double now = ImGui::GetTime();
        const float cascade = animate ? motion.PulseElapsed(kCascadeId, 1.0e3f) : 1.0e3f;
        int hoverNow = -1;

        if (ed.Visible.empty()) {
            if (!ed.ScanDone) EditorUIPrimitives::EmptyState(ICON_FA_STAMP, "Indexing the decal library...");
            else if (ed.Entries.empty())
                EditorUIPrimitives::EmptyState(ICON_FA_STAMP, "No decals yet", "Decals live under assets/Decals (tools/assets/cook_decal_library.py).");
            else if (ed.SelectedCategory == DecalEditorState::kFavorites)
                EditorUIPrimitives::EmptyState(ICON_FA_HEART, "No favourites yet", "Hover a decal and click its heart.");
            else
                EditorUIPrimitives::EmptyState(ICON_FA_MAGNIFYING_GLASS, "Nothing matches", "Try another word or category.");
        } else {
            ImGuiListClipper clipper;
            clipper.Begin(rows, tile + gap);
            while (clipper.Step()) {
                for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; ++r) {
                    for (int c = 0; c < cols; ++c) {
                        const int vi = r * cols + c;
                        if (vi >= (int)ed.Visible.size()) break;
                        const int ei = ed.Visible[vi];
                        const DecalEditorState::Entry& e = ed.Entries[ei];
                        const ImVec2 cell(ImGui::GetWindowContentRegionMin().x + c * (tile + gap),
                                          ImGui::GetCursorStartPos().y + r * (tile + gap));
                        ImGui::SetCursorPos(cell);
                        const ImVec2 p0 = ImGui::GetCursorScreenPos();

                        // The cascade: each tile arrives a step after the one before, left to right, top to bottom.
                        float arrive = 1.0f;
                        if (cascade < 1.0e2f) {
                            const float delay = Glass::ImAnimContext::GridRippleDelay(c, r, cols, rows, 0, 0, Glass::DecalTileStaggerMs, Glass::DecalTileStaggerCap);
                            arrive = Glass::EaseProgress(Glass::DecalTileIn, (cascade - delay) / (Glass::DecalTileIn.durationMs * 0.001f));
                        }
                        const float a = ImClamp(arrive, 0.0f, 1.0f);

                        ImGui::PushID(ei);
                        // The heart's corner is tested first so a click there favourites instead of selecting.
                        const float hr = 8.0f * m_UIScale;
                        const ImVec2 hc(p0.x + tile - hr - 5.0f * m_UIScale, p0.y + hr + 5.0f * m_UIScale);
                        const bool overHeart = ImGui::IsMouseHoveringRect(ImVec2(hc.x - hr - 3.0f, hc.y - hr - 3.0f), ImVec2(hc.x + hr + 3.0f, hc.y + hr + 3.0f));
                        ImGui::InvisibleButton("##tile", ImVec2(tile, tile));
                        const bool hovered = ImGui::IsItemHovered() && a >= 1.0f;
                        const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
                        if (clicked && overHeart) {
                            ToggleFavorite(ed, ei);
                        } else if (clicked) {
                            ed.PlacePath = e.Path;
                            EditorSound::Play(EditorSound::Event::Select);
                            if (SourceKind(ed) != 0) { /* the brush keeps its palette / category; Place uses this decal */ }
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_None) && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !overHeart) {
                            ed.PlacePath = e.Path;
                            if (m_DecalTool != DecalToolMode::Brush) SetDecalTool(DecalToolMode::Place);
                        }
                        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
                            ImGui::SetDragDropPayload("ASSET_DECAL_PATH", e.Path.c_str(), e.Path.size() + 1);
                            Glass::DragGhostLabel(ICON_FA_STAMP, e.Name.c_str());
                            ImGui::EndDragDropSource();
                            hoverNow = -1;
                        }
                        if (ImGui::BeginPopupContextItem("##tileMenu")) {
                            if (ImGui::MenuItem(e.Favorite ? ICON_FA_HEART "  Remove favourite" : ICON_FA_HEART "  Favourite")) ToggleFavorite(ed, ei);
                            if (ImGui::MenuItem(ICON_FA_STAMP "  Place this decal")) { ed.PlacePath = e.Path; SetDecalTool(DecalToolMode::Place); }
                            if (ImGui::MenuItem(ICON_FA_SPRAY_CAN "  Paint with this decal")) {
                                ed.PlacePath = e.Path;
                                SetPalette(ed, -1);
                                settings.DecalSourceCategory.clear();
                                EditorSettings::Save();
                                if (m_DecalTool != DecalToolMode::Brush) SetDecalTool(DecalToolMode::Brush);
                            }
                            if (ImGui::MenuItem(ICON_FA_FOLDER_TREE "  Paint with this category")) {
                                SetPalette(ed, -1);
                                settings.DecalSourceCategory = ed.Cats[e.Category].Path;
                                EditorSettings::Save();
                                if (m_DecalTool != DecalToolMode::Brush) SetDecalTool(DecalToolMode::Brush);
                            }
                            ImGui::Separator();
                            if (ImGui::MenuItem(ICON_FA_COPY "  Copy path")) ImGui::SetClipboardText(e.Rel.c_str());
                            ImGui::EndPopup();
                        }
                        ImGui::PopID();
                        if (hovered && ImGui::IsWindowHovered()) hoverNow = ei;

                        // Hover: the tile lifts and brightens (HoverGlow: nothing is held at rest).
                        const ImGuiID tid = ImGui::GetID(&e);
                        const float glow = animate ? motion.HoverGlow(tid, hovered) : (hovered ? 1.0f : 0.0f);
                        const float lift = Glass::TileHoverLiftPx * m_UIScale * glow;
                        const float grow = (Glass::TileHoverScale - 1.0f) * tile * 0.5f * glow;
                        const float rise = (1.0f - a) * Glass::DecalTileRisePx * m_UIScale;
                        const ImVec2 mn(p0.x - grow, p0.y - grow - lift + rise), mx(p0.x + tile + grow, p0.y + tile + grow - lift + rise);
                        const float rounding = 6.0f * m_UIScale;
                        if (glow > 0.0f)
                            EditorUIPrimitives::DrawSoftShadow(dl, mn, mx, rounding, 8.0f * m_UIScale, Glass::TileShadowAlpha * glow);
                        DrawDecalOnWall(dl, m_AssetThumbs, ed, e, mn, mx, rounding, a, 4.0f * m_UIScale, 1.5f);
                        const bool inUse = e.Path == ed.PlacePath;
                        dl->AddRect(mn, mx, inUse ? White(0.9f * a) : EditorTheme::U32(EditorTheme::WithAlpha(EditorTheme::Hairline, a)),
                                    rounding, 0, inUse ? 2.0f : 1.0f);
                        // The name, on a fade at the tile's foot.
                        const float nh = ImGui::GetTextLineHeight() + 4.0f * m_UIScale;
                        dl->AddRectFilledMultiColor(ImVec2(mn.x, mx.y - nh * 1.6f), ImVec2(mx.x, mx.y), Black(0.0f), Black(0.0f), Black(0.62f * a), Black(0.62f * a));
                        EditorUIPrimitives::TextEllipsis(dl, ImVec2(mn.x + 5.0f * m_UIScale, mx.y - nh), mx.x - 4.0f * m_UIScale, White(0.85f * a), e.Name.c_str());

                        // The heart: filled when favourited, an outline on hover; it pops (a spring) when toggled.
                        float heartScale = 1.0f;
                        for (int h = 0; h < 4; ++h)
                            if (ed.HeartPops[h] == ei && motion.PulseActive(kHeartId + (ImGuiID)h))
                                heartScale = ImLerp(Glass::DecalHeartFromScale, 1.0f, Glass::EaseProgress(Glass::DecalHeartPop, motion.PulseProgress(kHeartId + (ImGuiID)h)));
                        if (e.Favorite || hovered)
                            DrawHeart(dl, ImVec2(hc.x - grow, hc.y - grow - lift + rise), hr * heartScale * (overHeart ? 1.12f : 1.0f),
                                      White(a), White((e.Favorite ? 1.0f : overHeart ? 0.95f : 0.7f) * a), e.Favorite);
                    }
                }
            }
            clipper.End();
        }

        // The hover card: after a beat on one tile, a larger look at the decal on a wall.
        if (hoverNow != ed.HoverEntry) {
            ed.HoverEntry = hoverNow;
            ed.HoverSince = now;
        }
        const bool wantCard = ed.HoverEntry >= 0 && (now - ed.HoverSince) * 1000.0 >= (double)Glass::DecalPreviewDelayMs && !ImGui::IsMouseDown(ImGuiMouseButton_Left);
        if (wantCard) ed.PreviewEntry = ed.HoverEntry;
        const float cardA = animate ? motion.Animate(kCardId, wantCard ? 1.0f : 0.0f, 0.0f, Glass::DecalPreviewIn) : (wantCard ? 1.0f : 0.0f);
        if (cardA > 0.001f && ed.PreviewEntry >= 0 && ed.PreviewEntry < (int)ed.Entries.size())
            DrawHoverCard(ed, m_AssetThumbs, ed.Entries[ed.PreviewEntry], cardA, m_UIScale);
    }
    ImGui::EndChild();

    // Dropping a decal onto the tile area does nothing; the panel only drags out.
    ImGui::End();
}
