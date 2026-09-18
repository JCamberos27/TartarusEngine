#ifdef _MSC_VER
#pragma warning(disable: 4996) // sscanf: use sscanf_s
#endif
#include "gl.h"
#include "ShaderAsset.h"
#include "Shader.h"
#include "ShaderLibrary.h"
#include "Log.h"

#include <fstream>
#include <sstream>
#include <cctype>
#include <cassert>
#include <cstdio>
#include <algorithm>
#include <filesystem>

namespace {

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Reads from `in` until "{...}" is found, returns the content between braces.
std::string ReadBlock(std::istream& in) {
    char c;
    while (in.get(c) && c != '{') {}
    std::string body;
    int depth = 1;
    while (in.get(c)) {
        if (c == '{') ++depth;
        else if (c == '}') { if (--depth == 0) break; }
        body += c;
    }
    return body;
}

// #104 — false for an unknown type name; it used to become Float silently.
bool ParseType(const std::string& t, ShaderPropType& out) {
    if (t == "Float")     { out = ShaderPropType::Float;     return true; }
    if (t == "Color")     { out = ShaderPropType::Color;     return true; }
    if (t == "Texture2D" || t == "2D") { out = ShaderPropType::Texture2D; return true; } // "2D": Unity's spelling
    if (t == "Bool")      { out = ShaderPropType::Bool;      return true; }
    if (t == "Vec2")      { out = ShaderPropType::Vec2;      return true; }
    if (t == "Vec3")      { out = ShaderPropType::Vec3;      return true; }
    if (t == "Vec4" || t == "Vector") { out = ShaderPropType::Vec4; return true; }
    if (t == "Int" || t == "Integer") { out = ShaderPropType::Int;  return true; }
    return false;
}

// #104 — removes // and /* */ comments (outside "..." strings) before anything is tokenised, so a
// comment mentioning "Vertex", "Properties" or a brace can no longer start or end a block.
// Newlines are kept, so line structure (one property per line) survives.
std::string StripComments(const std::string& src) {
    std::string out;
    out.reserve(src.size());
    bool inString = false, inLine = false, inBlock = false;
    for (size_t i = 0; i < src.size(); ++i) {
        const char c = src[i], n = i + 1 < src.size() ? src[i + 1] : '\0';
        if (inLine) { if (c == '\n') { inLine = false; out += c; } continue; }
        if (inBlock) {
            if (c == '*' && n == '/') { inBlock = false; ++i; }
            else if (c == '\n') out += c;
            continue;
        }
        if (inString) { if (c == '"') inString = false; if (c == '\n') inString = false; out += c; continue; }
        if (c == '"') { inString = true; out += c; continue; }
        // "engine://..." / "project://..." references: a // straight after ':' is a URI, not a comment.
        if (c == '/' && n == '/' && (i == 0 || src[i - 1] != ':')) { inLine = true; ++i; continue; }
        if (c == '/' && n == '*') { inBlock = true; ++i; continue; }
        out += c;
    }
    return out;
}

// #104 render-state keyword values. False (with the caller logging) for anything unrecognised.
bool ParseBlendFactor(const std::string& t, unsigned& out) {
    static const std::pair<const char*, unsigned> kFactors[] = {
        {"One", GL_ONE}, {"Zero", GL_ZERO}, {"SrcColor", GL_SRC_COLOR}, {"SrcAlpha", GL_SRC_ALPHA},
        {"DstColor", GL_DST_COLOR}, {"DstAlpha", GL_DST_ALPHA},
        {"OneMinusSrcColor", GL_ONE_MINUS_SRC_COLOR}, {"OneMinusSrcAlpha", GL_ONE_MINUS_SRC_ALPHA},
        {"OneMinusDstColor", GL_ONE_MINUS_DST_COLOR}, {"OneMinusDstAlpha", GL_ONE_MINUS_DST_ALPHA},
    };
    for (const auto& [name, v] : kFactors) if (t == name) { out = v; return true; }
    return false;
}

bool ParseDepthFunc(const std::string& t, unsigned& out) {
    static const std::pair<const char*, unsigned> kFuncs[] = {
        {"Less", GL_LESS}, {"LEqual", GL_LEQUAL}, {"Equal", GL_EQUAL}, {"GEqual", GL_GEQUAL},
        {"Greater", GL_GREATER}, {"NotEqual", GL_NOTEQUAL}, {"Always", GL_ALWAYS}, {"Never", GL_NEVER},
    };
    for (const auto& [name, v] : kFuncs) if (t == name) { out = v; return true; }
    return false;
}

// Unity queue names map onto the engine's three queues; the number (or the name's base plus an
// offset) is the in-queue sort index.
bool ParseQueue(const std::string& t, int& queue, int& index) {
    size_t split = t.find_first_of("+-");
    const std::string name = t.substr(0, split);
    int offset = 0;
    if (split != std::string::npos) {
        try { offset = std::stoi(t.substr(split)); } catch (...) { return false; }
    }
    if (!name.empty() && std::isdigit((unsigned char)name[0])) {
        int n = 0;
        try { n = std::stoi(name); } catch (...) { return false; }
        n += offset;
        queue = n >= 2750 ? 2 : n >= 2450 ? 1 : 0;
        index = n;
        return true;
    }
    if (name == "Background" || name == "Geometry") { queue = 0; index = (name == "Background" ? 1000 : 2000) + offset; return true; }
    if (name == "AlphaTest")                        { queue = 1; index = 2450 + offset; return true; }
    if (name == "Transparent" || name == "Overlay") { queue = 2; index = (name == "Overlay" ? 4000 : 3000) + offset; return true; }
    return false;
}

enum class PropParse { NotAProperty, Ok, Error };

// Parse one property line, with optional leading attributes:
//   [Header(Surface)] [HDR] _Name ("Display", Type) = default
PropParse ParsePropLine(const std::string& rawLine, ShaderProperty& out, std::string& error) {
    std::string line = Trim(rawLine);
    if (line.empty()) return PropParse::NotAProperty;

    out.Hidden = false;
    while (!line.empty() && line[0] == '[') {
        const size_t close = line.find(']');
        if (close == std::string::npos) { error = "unterminated [attribute]"; return PropParse::Error; }
        const std::string attr = Trim(line.substr(1, close - 1));
        line = Trim(line.substr(close + 1));
        // Header(...) / Tooltip(...) take free text; quotes around it are optional.
        auto argOf = [&](const char* name) -> std::string {
            const size_t lp = attr.find('('), rp = attr.rfind(')');
            if (attr.rfind(name, 0) != 0 || lp == std::string::npos || rp == std::string::npos || rp < lp) return {};
            std::string a = Trim(attr.substr(lp + 1, rp - lp - 1));
            if (a.size() >= 2 && a.front() == '"' && a.back() == '"') a = a.substr(1, a.size() - 2);
            return a;
        };
        if (attr == "Hidden" || attr == "HideInInspector") out.Hidden = true;
        else if (attr == "HDR") out.HDR = true;
        else if (attr == "Toggle" || attr.rfind("Toggle(", 0) == 0) out.Toggle = true;
        else if (attr == "Normal") out.NormalMap = true;
        else if (attr == "NoScaleOffset") out.NoScaleOffset = true;
        else if (attr.rfind("Header", 0) == 0) out.Header = argOf("Header");
        else if (attr.rfind("Tooltip", 0) == 0) out.Tooltip = argOf("Tooltip");
        else Log::Warn("ShaderAsset: unknown property attribute [" + attr + "] ignored.");
    }

    if (line.empty() || line[0] != '_') return PropParse::NotAProperty;

    // Property name: up to '(' or whitespace
    size_t i = 0;
    while (i < line.size() && !std::isspace((unsigned char)line[i]) && line[i] != '(') ++i;
    out.Name = line.substr(0, i);

    // ("Display", Type)
    size_t lp = line.find('(', i);
    // Matching ')' (depth-counted), so a nested "Range(0, 1)" type doesn't end the group early.
    size_t rp = std::string::npos;
    if (lp != std::string::npos) {
        int depth = 0;
        for (size_t k = lp; k < line.size(); ++k) {
            if (line[k] == '(') ++depth;
            else if (line[k] == ')' && --depth == 0) { rp = k; break; }
        }
    }
    if (lp == std::string::npos || rp == std::string::npos) { error = "expected (\"Display Name\", Type)"; return PropParse::Error; }
    std::string inner = line.substr(lp + 1, rp - lp - 1);
    size_t comma = inner.find(',');
    if (comma == std::string::npos) { error = "expected (\"Display Name\", Type)"; return PropParse::Error; }
    std::string disp = Trim(inner.substr(0, comma));
    if (disp.size() >= 2 && disp.front() == '"' && disp.back() == '"')
        disp = disp.substr(1, disp.size() - 2);
    out.DisplayName = disp;
    const std::string typeStr = Trim(inner.substr(comma + 1));
    // #106 — Unity-style Range(min, max): a Float with slider limits for the material editor.
    if (typeStr.rfind("Range", 0) == 0) {
        out.Type = ShaderPropType::Float;
        float lo = 0.0f, hi = 1.0f;
        if (std::sscanf(typeStr.c_str(), "Range ( %f , %f )", &lo, &hi) == 2 ||
            std::sscanf(typeStr.c_str(), "Range(%f,%f)", &lo, &hi) == 2) {
            out.HasRange = true;
            out.RangeMin = lo < hi ? lo : hi;
            out.RangeMax = lo < hi ? hi : lo;
        }
    } else if (!ParseType(typeStr, out.Type)) {
        error = "unknown property type '" + typeStr + "' (Float, Range(min, max), Color, Texture2D, Bool, Int, Vec2, Vec3, Vec4)";
        return PropParse::Error;
    }
    if (out.Toggle && out.Type != ShaderPropType::Float && out.Type != ShaderPropType::Int && out.Type != ShaderPropType::Bool)
        out.Toggle = false; // [Toggle] only means something on a number
    if (out.HDR && out.Type != ShaderPropType::Color) out.HDR = false;
    if (out.NormalMap && out.Type != ShaderPropType::Texture2D) out.NormalMap = false;

    // = default
    size_t eq = line.find('=', rp);
    if (eq != std::string::npos) {
        std::string def = Trim(line.substr(eq + 1));
        switch (out.Type) {
        case ShaderPropType::Texture2D:
            if (def.size() >= 2 && def.front() == '"' && def.back() == '"')
                def = def.substr(1, def.size() - 2);
            out.DefaultTex = def;
            break;
        case ShaderPropType::Bool:
            out.DefaultBool = (def == "1" || def == "true");
            break;
        case ShaderPropType::Color:
        case ShaderPropType::Vec4: {
            float x=0,y=0,z=0,w=1;
            int n = (int)std::count(def.begin(), def.end(), ',');
            if (n >= 3) std::sscanf(def.c_str(), "(%f, %f, %f, %f)", &x, &y, &z, &w);
            else        std::sscanf(def.c_str(), "(%f, %f, %f)", &x, &y, &z);
            out.DefaultVec = {x, y, z, w};
            break;
        }
        case ShaderPropType::Vec2: {
            float x=0,y=0;
            std::sscanf(def.c_str(), "(%f, %f)", &x, &y);
            out.DefaultVec = {x, y, 0, 1};
            break;
        }
        case ShaderPropType::Vec3: {
            float x=0,y=0,z=0;
            std::sscanf(def.c_str(), "(%f, %f, %f)", &x, &y, &z);
            out.DefaultVec = {x, y, z, 1};
            break;
        }
        default:
            try { out.DefaultFloat = std::stof(def.empty() ? "0" : def); } catch (...) {}
            break;
        }
    }
    if (out.NormalMap && out.DefaultTex.empty()) out.DefaultTex = "normal";
    return PropParse::Ok;
}

} // namespace

std::shared_ptr<ShaderAsset> ShaderAsset::ParseFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) {
        Log::Error("ShaderAsset: cannot open '" + path + "'");
        return nullptr;
    }

    auto sa = std::make_shared<ShaderAsset>();
    sa->m_Path = path;

    std::ostringstream raw;
    raw << f.rdbuf();
    std::istringstream text(StripComments(raw.str())); // #104
    std::string token;
    while (text >> token) {
        if (token == "Properties") {
            std::string block = ReadBlock(text);
            std::istringstream ss(block);
            std::string line;
            int lineNo = 0;
            while (std::getline(ss, line)) {
                ++lineNo;
                line = Trim(line);
                if (line.empty()) continue;
                ShaderProperty prop;
                std::string error;
                const PropParse r = ParsePropLine(line, prop, error);
                if (r == PropParse::Ok) {
                    prop.PropIndex = (int)sa->m_Props.size();
                    sa->m_Props.push_back(prop);
                } else if (r == PropParse::Error) {
                    Log::Error("ShaderAsset: '" + path + "' Properties line " + std::to_string(lineNo) + ": " +
                               error + " - property skipped.", LogContext::Asset(path));
                }
            }
        } else if (token == "Keywords") {
            std::string block = ReadBlock(text);
            std::istringstream ss(block);
            std::string kw;
            while (ss >> kw) sa->m_Keywords.push_back(kw);
            // The variant key is a 32-bit mask (ShaderVariantKey), so bit 32+ can never be
            // selected. Enforced in Debug and Release (#354).
            if (sa->m_Keywords.size() > 32) {
                Log::Error("ShaderAsset: '" + path + "' declares " +
                           std::to_string(sa->m_Keywords.size()) +
                           " keywords; only the first 32 are usable — the rest are dropped.");
                sa->m_Keywords.resize(32);
            }
        } else if (token == "Vertex") {
            sa->m_VertFile = Trim(ReadBlock(text));
        } else if (token == "Fragment") {
            sa->m_FragFile = Trim(ReadBlock(text));
        } else if (token == "Cull" || token == "ZWrite" || token == "ZTest" || token == "Blend" || token == "Queue") {
            // #104 — render state. A bad value is reported and ignored (the pass default stays).
            std::string v;
            text >> v;
            ShaderRenderState& st = sa->m_State;
            bool ok = true;
            if (token == "Cull") {
                if (v == "Back") st.Cull = ShaderRenderState::CullMode::Back;
                else if (v == "Front") st.Cull = ShaderRenderState::CullMode::Front;
                else if (v == "Off") st.Cull = ShaderRenderState::CullMode::Off;
                else ok = false;
            } else if (token == "ZWrite") {
                if (v == "On") st.ZWrite = 1; else if (v == "Off") st.ZWrite = 0; else ok = false;
            } else if (token == "ZTest") {
                ok = ParseDepthFunc(v, st.ZTest);
            } else if (token == "Blend") {
                if (v == "Off") st.Blend = 0;
                else {
                    std::string dst;
                    text >> dst;
                    ok = ParseBlendFactor(v, st.BlendSrc) && ParseBlendFactor(dst, st.BlendDst);
                    if (ok) st.Blend = 1;
                    v += " " + dst;
                }
            } else {
                ok = ParseQueue(v, st.Queue, st.QueueIndex);
            }
            if (!ok) Log::Error("ShaderAsset: '" + path + "': unrecognised " + token + " value '" + v + "' - ignored.",
                                LogContext::Asset(path));
        } else {
            Log::Warn("ShaderAsset: '" + path + "': unknown token '" + token + "' ignored.", LogContext::Asset(path));
        }
    }
    if (sa->m_VertFile.empty()) {
        Log::Error("ShaderAsset: '" + path + "' has no Vertex { file } block.", LogContext::Asset(path));
        return nullptr;
    }

    sa->BuildBindings();
    sa->m_Deps.push_back(ShaderLibrary::DependencyKey(path)); // #158
    return sa;
}

bool ShaderAsset::DependsOnAny(const std::vector<std::string>& changed) const {
    for (const std::string& d : m_Deps)
        if (std::find(changed.begin(), changed.end(), d) != changed.end()) return true;
    return false;
}

bool ShaderAsset::ReloadFromDisk() {
    std::shared_ptr<ShaderAsset> fresh = ParseFile(m_Path);
    if (!fresh) {
        Log::Error("Shader hot reload: '" + m_Path + "' failed to parse - kept the previous version.", LogContext::Asset(m_Path));
        return false;
    }
    m_Props = std::move(fresh->m_Props);
    m_Bindings = std::move(fresh->m_Bindings);
    m_Keywords = std::move(fresh->m_Keywords);
    m_State = fresh->m_State;
    m_VertFile = std::move(fresh->m_VertFile);
    m_FragFile = std::move(fresh->m_FragFile);
    m_Deps = std::move(fresh->m_Deps);
    m_Variants.clear(); // recompiled lazily from the new sources
    m_LastCompileError.clear();
    Log::Info("Shader hot reload: reloaded '" + m_Path + "'.", LogContext::Asset(m_Path));
    return true;
}

void ShaderAsset::BuildBindings() {
    m_Bindings.clear();
    // Material texture units: 1..7, then 16 and up. 0 is the depth-prepass albedo, 8/9/10 the
    // shadow maps, 11-13 IBL, 14 the opaque-colour copy, 15 SSAO (see
    // SceneRenderer::ApplyFrameState). #207 — the budget used to stop at 7, which left the
    // Standard shader's clear-coat / thickness maps (#206) with nowhere to go; units from 16 up
    // to the driver's GL_MAX_TEXTURE_IMAGE_UNITS (capped at 32) are now used too. Anything past
    // that still gets no unit and a warning (#354).
    static int s_MaxUnits = 0;
    if (s_MaxUnits == 0) {
        GLint n = 16;
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &n);
        s_MaxUnits = std::clamp((int)n, 16, 32);
    }
    int unit = 1;
    for (int i = 0; i < (int)m_Props.size(); ++i) {
        PropertyBinding b;
        b.Type      = m_Props[i].Type;
        b.PropIndex = i;
        b.TextureUnit = -1;
        if (m_Props[i].Type == ShaderPropType::Texture2D) {
            if (unit == 8) unit = 16; // skip the engine's reserved 8..15
            if (unit >= s_MaxUnits) {
                Log::Warn("ShaderAsset: '" + m_Path + "' texture property '" + m_Props[i].Name +
                          "' exceeds the material texture-unit budget (" + std::to_string(7 + s_MaxUnits - 16) +
                          " on this GPU); left unbound.");
            } else {
                b.TextureUnit = unit++;
            }
        }
        m_Bindings.push_back(b);
    }
}

bool ShaderAsset::IsBuiltinKeyword(const std::string& k) {
    return k == "_CLEARCOAT" || k == "_ANISO" || k == "_SHEEN" || k == "_SUBSURFACE" ||
           k == "_TRANSMISSION" || k == "_REFLECTION_PROBES";
}

void ShaderAsset::CompileVariant(ShaderVariantKey key) {
    // #208 — stages resolve against this descriptor's folder (engine:// / project:// explicit).
    const std::string baseDir = std::filesystem::path(m_Path).parent_path().string();
    const std::string vertPath = ShaderLibrary::ResolveRef(m_VertFile, baseDir);
    const std::string fragPath = m_FragFile.empty() ? std::string() : ShaderLibrary::ResolveRef(m_FragFile, baseDir);
    std::vector<std::string> deps;
    std::string vertSrc = ShaderLibrary::ReadFileAt(vertPath, m_Path, &deps);
    std::string fragSrc = m_FragFile.empty() ? "" : ShaderLibrary::ReadFileAt(fragPath, m_Path, &deps);
    for (std::string& d : deps) // #158: remember what this variant was built from
        if (std::find(m_Deps.begin(), m_Deps.end(), d) == m_Deps.end()) m_Deps.push_back(std::move(d));
    // A missing stage is an asset error, never a reason to compile the vertex file alone as a
    // compute shader (which is what an empty fragment source used to mean below).
    if (vertSrc.empty() || (!m_FragFile.empty() && fragSrc.empty())) {
        m_LastCompileError = "Stage source not found: " + (vertSrc.empty() ? vertPath : fragPath) +
                             " (referenced by " + m_Path + ")";
        Log::Error("ShaderAsset: " + m_LastCompileError + " - objects using it fall back to the default shader.",
                   LogContext::Asset(m_Path));
        m_Variants[key] = nullptr;
        return;
    }

    if (key != 0) {
        auto inject = [&](std::string& src) {
            std::string defines;
            for (int i = 0; i < (int)m_Keywords.size(); ++i)
                if (key & (1u << i))
                    defines += "#define " + m_Keywords[i] + "\n";
            // #104 — after the #version line (GLSL requires it first, and a file may open with
            // comments), not blindly after line 1. No #version: prepend.
            size_t at = std::string::npos;
            for (size_t pos = 0; pos < src.size();) {
                const size_t eol = src.find('\n', pos);
                const size_t first = src.find_first_not_of(" \t", pos);
                if (first != std::string::npos && src.compare(first, 8, "#version") == 0 && (eol == std::string::npos || first < eol)) {
                    at = eol == std::string::npos ? src.size() : eol + 1;
                    break;
                }
                if (eol == std::string::npos) break;
                pos = eol + 1;
            }
            if (at == std::string::npos) src = defines + src;
            else {
                if (at == src.size() && (src.empty() || src.back() != '\n')) src += '\n', at = src.size();
                src.insert(at, defines);
            }
        };
        inject(vertSrc);
        if (!fragSrc.empty()) inject(fragSrc);
    }

    // #100 — Shader's constructors throw on a GLSL compile/link error. This runs lazily from
    // SceneRenderer mid-draw, so an uncaught throw here (a typo in a user .shader/.glsl) used to
    // unwind to main() and close the editor. Catch it, report it once, and cache the failure
    // (nullptr) so it isn't recompiled every frame; SceneRenderer already falls back to the
    // built-in program when Variant() returns null. ForgetFailedVariants() (the Inspector's
    // shader preview calls it) retries after the file is fixed.
    try {
        if (fragSrc.empty())
            m_Variants[key] = std::make_unique<Shader>(vertSrc);      // compute
        else
            m_Variants[key] = std::make_unique<Shader>(vertSrc, fragSrc);
    } catch (const std::exception& e) {
        std::string keywords;
        for (int i = 0; i < (int)m_Keywords.size(); ++i)
            if (key & (1u << i)) keywords += (keywords.empty() ? "" : " ") + m_Keywords[i];
        Log::Error("ShaderAsset: '" + m_VertFile + (m_FragFile.empty() ? "" : "' / '" + m_FragFile) +
                   "' failed to compile" + (keywords.empty() ? "" : " (variant: " + keywords + ")") +
                   " - objects using it fall back to the default shader. " + e.what());
        m_LastCompileError = e.what();
        m_Variants[key] = nullptr;
    }
}

void ShaderAsset::ForgetFailedVariants() {
    for (auto it = m_Variants.begin(); it != m_Variants.end();)
        it = it->second ? std::next(it) : m_Variants.erase(it);
    m_LastCompileError.clear();
}

Shader* ShaderAsset::Variant(ShaderVariantKey key) {
    auto it = m_Variants.find(key);
    if (it == m_Variants.end()) {
        CompileVariant(key);
        it = m_Variants.find(key);
    }
    return it->second.get();
}
