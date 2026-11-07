#include "SurfaceLookup.h"

#include "Components.h"
#include "World.h"

#include <cctype>

namespace {
std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
} // namespace

void SurfaceLookup::Parse(const std::string& table) const {
    m_Table = table;
    m_Parsed = true;
    m_Entries.clear();
    size_t pos = 0;
    while (pos < table.size()) {
        size_t end = table.find(';', pos);
        if (end == std::string::npos) end = table.size();
        const std::string entry = table.substr(pos, end - pos);
        pos = end + 1;
        const size_t eq = entry.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        Entry e;
        e.Surface = entry.substr(0, eq);
        size_t w = eq + 1;
        while (w <= entry.size()) {
            size_t we = entry.find(',', w);
            if (we == std::string::npos) we = entry.size();
            std::string word = Lower(entry.substr(w, we - w));
            while (!word.empty() && word.front() == ' ') word.erase(word.begin());
            while (!word.empty() && word.back() == ' ') word.pop_back();
            if (!word.empty()) e.Words.push_back(std::move(word));
            w = we + 1;
        }
        m_Entries.push_back(std::move(e));
    }
}

std::string SurfaceLookup::FromName(const std::string& table, const std::string& name, const std::string& fallback) const {
    if (!m_Parsed || table != m_Table) Parse(table);
    const std::string hay = Lower(name);
    for (const Entry& e : m_Entries)
        for (const std::string& word : e.Words)
            if (hay.find(word) != std::string::npos) return e.Surface;
    return fallback;
}

std::string SurfaceLookup::FromEntity(const World& world, std::uint32_t entity, const std::string& table, const std::string& fallback) const {
    const auto e = static_cast<entt::entity>(entity);
    if (entity == 0xFFFFFFFFu || !world.Registry.valid(e)) return fallback;
    std::string name;
    if (const auto* c = world.Registry.try_get<ColliderComponent>(e)) name += c->Material + " ";
    if (const auto* t = world.Registry.try_get<TagComponent>(e)) name += t->Tag + " ";
    if (const auto* n = world.Registry.try_get<NameComponent>(e)) name += n->Name;
    return FromName(table, name, fallback);
}
