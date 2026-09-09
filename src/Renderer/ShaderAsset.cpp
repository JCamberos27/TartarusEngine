#ifdef _MSC_VER
#pragma warning(disable: 4996) // sscanf: use sscanf_s
#endif
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

ShaderPropType ParseType(const std::string& t) {
    if (t == "Float")     return ShaderPropType::Float;
    if (t == "Color")     return ShaderPropType::Color;
    if (t == "Texture2D") return ShaderPropType::Texture2D;
    if (t == "Bool")      return ShaderPropType::Bool;
    if (t == "Vec2")      return ShaderPropType::Vec2;
    if (t == "Vec3")      return ShaderPropType::Vec3;
    if (t == "Vec4")      return ShaderPropType::Vec4;
    if (t == "Int")       return ShaderPropType::Int;
    return ShaderPropType::Float;
}

// Parse one property line (with optional leading [Hidden]):
//   [Hidden] _Name ("Display", Type) = default
bool ParsePropLine(const std::string& rawLine, ShaderProperty& out) {
    std::string line = Trim(rawLine);
    if (line.empty()) return false;

    out.Hidden = false;
    if (line.rfind("[Hidden]", 0) == 0) {
        out.Hidden = true;
        line = Trim(line.substr(8));
    }

    if (line.empty() || line[0] != '_') return false;

    // Property name: up to '(' or whitespace
    size_t i = 0;
    while (i < line.size() && !std::isspace((unsigned char)line[i]) && line[i] != '(') ++i;
    out.Name = line.substr(0, i);

    // ("Display", Type)
    size_t lp = line.find('(', i);
    size_t rp = (lp != std::string::npos) ? line.find(')', lp) : std::string::npos;
    if (lp == std::string::npos || rp == std::string::npos) return false;
    std::string inner = line.substr(lp + 1, rp - lp - 1);
    size_t comma = inner.find(',');
    if (comma == std::string::npos) return false;
    std::string disp = Trim(inner.substr(0, comma));
    if (disp.size() >= 2 && disp.front() == '"' && disp.back() == '"')
        disp = disp.substr(1, disp.size() - 2);
    out.DisplayName = disp;
    out.Type = ParseType(Trim(inner.substr(comma + 1)));

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
    return true;
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

    std::string token;
    while (f >> token) {
        if (token == "Properties") {
            std::string block = ReadBlock(f);
            std::istringstream ss(block);
            std::string line;
            while (std::getline(ss, line)) {
                line = Trim(line);
                // Strip C++ style line comments
                size_t cmt = line.find("//");
                if (cmt != std::string::npos) line = Trim(line.substr(0, cmt));
                if (line.empty()) continue;
                ShaderProperty prop;
                if (ParsePropLine(line, prop)) {
                    prop.PropIndex = (int)sa->m_Props.size();
                    sa->m_Props.push_back(prop);
                }
            }
        } else if (token == "Keywords") {
            std::string block = ReadBlock(f);
            std::istringstream ss(block);
            std::string kw;
            while (ss >> kw) {
                if (!kw.empty() && kw[0] != '/')
                    sa->m_Keywords.push_back(kw);
            }
        } else if (token == "Vertex") {
            sa->m_VertFile = Trim(ReadBlock(f));
        } else if (token == "Fragment") {
            sa->m_FragFile = Trim(ReadBlock(f));
        }
        // "Queue", "//"-comments and other unknown tokens are skipped
    }

    sa->BuildBindings();
    return sa;
}

void ShaderAsset::BuildBindings() {
    m_Bindings.clear();
    int unit = 1; // material texture units start at 1 (engine IBL at 11-13)
    for (int i = 0; i < (int)m_Props.size(); ++i) {
        PropertyBinding b;
        b.Type      = m_Props[i].Type;
        b.PropIndex = i;
        b.TextureUnit = -1;
        if (m_Props[i].Type == ShaderPropType::Texture2D) {
            assert(unit < 11 && "material texture unit collision with engine IBL range");
            b.TextureUnit = unit++;
        }
        m_Bindings.push_back(b);
    }
}

void ShaderAsset::CompileVariant(ShaderVariantKey key) {
    std::string vertSrc = ShaderLibrary::ReadFile(m_VertFile);
    std::string fragSrc = m_FragFile.empty() ? "" : ShaderLibrary::ReadFile(m_FragFile);

    if (key != 0) {
        auto inject = [&](std::string& src) {
            std::string defines;
            for (int i = 0; i < (int)m_Keywords.size(); ++i)
                if (key & (1u << i))
                    defines += "#define " + m_Keywords[i] + "\n";
            size_t nl = src.find('\n');
            if (nl != std::string::npos) src.insert(nl + 1, defines);
            else src = defines + src;
        };
        inject(vertSrc);
        if (!fragSrc.empty()) inject(fragSrc);
    }

    if (fragSrc.empty())
        m_Variants[key] = std::make_unique<Shader>(vertSrc);      // compute
    else
        m_Variants[key] = std::make_unique<Shader>(vertSrc, fragSrc);
}

Shader* ShaderAsset::Variant(ShaderVariantKey key) {
    auto it = m_Variants.find(key);
    if (it == m_Variants.end()) {
        CompileVariant(key);
        it = m_Variants.find(key);
    }
    return it->second.get();
}
