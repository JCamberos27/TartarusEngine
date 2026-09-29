#include "Wardrobe.h"

#include <json.hpp>

#include <algorithm>
#include <random>
#include <cctype>
#include <set>
#include <sstream>

using json = nlohmann::json;

namespace Wardrobe {
namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool IEquals(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }

bool EndsWithI(const std::string& s, const std::string& suffix) {
    return suffix.size() <= s.size() && IEquals(s.substr(s.size() - suffix.size()), suffix);
}

bool StartsWithI(const std::string& s, const std::string& prefix) {
    return prefix.size() <= s.size() && IEquals(s.substr(0, prefix.size()), prefix);
}

std::string Normalize(std::string p) {
    std::replace(p.begin(), p.end(), '\\', '/');
    while (p.size() > 2 && p[0] == '.' && p[1] == '/') p.erase(0, 2);
    return p;
}

std::string Join(const std::string& root, const std::string& rel) {
    if (rel.empty()) return rel;
    const std::string r = Normalize(rel);
    if (root.empty() || StartsWithI(r, "assets/")) return r;
    return Normalize(root) + "/" + r;
}

std::vector<std::string> Strings(const json& j, const char* key) {
    std::vector<std::string> out;
    if (j.contains(key) && j[key].is_array())
        for (const auto& v : j[key]) if (v.is_string()) out.push_back(v.get<std::string>());
    return out;
}

std::string String(const json& j, const char* key, const std::string& fallback = std::string()) {
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : fallback;
}

Match ReadMatch(const json& j) {
    Match m;
    m.Slot = String(j, "slot");
    m.NameHasAny = Strings(j, "nameHasAny");
    m.NameLacksAll = Strings(j, "nameLacksAll");
    m.MaterialHasAny = Strings(j, "materialHasAny");
    m.Tags = Strings(j, "tags");
    m.LacksTags = Strings(j, "lacksTags");
    return m;
}

bool AnyIn(const std::string& name, const std::vector<std::string>& tokens) {
    for (const auto& t : tokens) if (Contains(name, t)) return true;
    return false;
}

std::vector<std::string> Folders(const std::string& path) {
    std::vector<std::string> out;
    std::stringstream ss(Normalize(path));
    for (std::string part; std::getline(ss, part, '/');) out.push_back(part);
    if (!out.empty()) out.pop_back(); // the file
    return out;
}

// A hat's name as a haircut suffix: "SKM_F_Hat_Warm" -> "Hat_Warm".
std::string HatToken(const std::string& stem) {
    for (const char* p : {"SKM_F_", "SKM_", "SM_"})
        if (StartsWithI(stem, p)) return stem.substr(std::string(p).size());
    return stem;
}

} // namespace

const char* GenderName(Gender g) { return g == Gender::Female ? "Female" : "Male"; }

const SlotDef* Wardrobe::Slot(const std::string& id) const {
    for (const auto& s : Slots) if (s.Id == id) return &s;
    return nullptr;
}

bool Contains(const std::string& name, const std::string& token) {
    return !token.empty() && Lower(name).find(Lower(token)) != std::string::npos;
}

std::string Stem(const std::string& path) {
    std::string p = Normalize(path);
    const size_t slash = p.find_last_of('/');
    if (slash != std::string::npos) p = p.substr(slash + 1);
    const size_t dot = p.find_last_of('.');
    return dot == std::string::npos ? p : p.substr(0, dot);
}

std::string PrettyName(const std::string& stem) {
    std::string s = stem;
    for (const char* p : {"SKM_F_", "SKM_", "SM_", "Quantum_"})
        if (StartsWithI(s, p)) { s = s.substr(std::string(p).size()); break; }
    std::string out;
    std::stringstream ss(s);
    for (std::string word; std::getline(ss, word, '_');) {
        if (word.empty()) continue;
        if (IEquals(word, "Tshirt")) word = "T-Shirt";
        if (!out.empty()) out += ' ';
        out += word;
    }
    return out.empty() ? stem : out;
}

std::string SkinBase(const Wardrobe& w, const std::string& matPath) {
    const std::string stem = Stem(matPath);
    for (const auto& base : w.SkinMaterials) {
        if (IEquals(stem, base)) return base;
        for (const auto& body : w.Bodies)
            for (const auto& race : body.Races)
                if (!race.SkinSuffix.empty() && IEquals(stem, base + race.SkinSuffix)) return base;
    }
    return std::string();
}

bool Parse(const std::string& jsonText, Wardrobe& out, std::string* error) {
    auto fail = [&](const std::string& message) {
        if (error) *error = message;
        return false;
    };
    json j;
    try { j = json::parse(jsonText); } catch (const std::exception& e) { return fail(std::string("not valid JSON: ") + e.what()); }
    if (!j.is_object()) return fail("not a JSON object");

    Wardrobe w;
    w.Name = String(j, "name", "Wardrobe");
    w.Root = Normalize(String(j, "root"));
    w.ItemFolders = Strings(j, "itemFolders");
    w.ExcludeFolders = Strings(j, "excludeFolders");
    w.SkinMaterials = Strings(j, "skinMaterials");
    w.HatSlot = String(j, "hatSlot", w.HatSlot);
    w.HairSlot = String(j, "hairSlot", w.HairSlot);

    if (j.contains("slots") && j["slots"].is_array())
        for (const auto& s : j["slots"]) {
            SlotDef d;
            d.Id = String(s, "id");
            if (d.Id.empty()) continue;
            d.Label = String(s, "label", d.Id);
            d.Icon = String(s, "icon");
            d.Folders = Strings(s, "folders");
            d.NameSuffix = String(s, "nameSuffix");
            d.HeadAttached = s.value("headAttached", false);
            d.Layer = s.contains("layer") && s["layer"].is_number() ? s["layer"].get<int>() : DefaultLayer(d.Id);
            d.Hides = s.contains("hides") && s["hides"].is_boolean() ? s["hides"].get<bool>() : DefaultHides(d.Id);
            w.Slots.push_back(std::move(d));
        }

    if (!j.contains("bodies") || !j["bodies"].is_object()) return fail("no \"bodies\"");
    for (int g = 0; g < 2; ++g) {
        const char* name = GenderName((Gender)g);
        if (!j["bodies"].contains(name)) continue;
        const json& b = j["bodies"][name];
        BodyDef& body = w.Bodies[g];
        if (b.contains("parts") && b["parts"].is_array())
            for (const auto& p : b["parts"])
                if (p.is_object() && !String(p, "part").empty())
                    body.Parts.emplace_back(String(p, "part"), Join(w.Root, String(p, "model")));
        if (b.contains("alternates") && b["alternates"].is_object())
            for (const auto& [k, v] : b["alternates"].items())
                if (v.is_string()) body.Alternates[k] = Join(w.Root, v.get<std::string>());
        if (b.contains("races") && b["races"].is_array())
            for (const auto& r : b["races"]) {
                RaceDef race;
                race.Name = String(r, "name");
                if (race.Name.empty()) continue;
                race.Head = Join(w.Root, String(r, "head"));
                race.SkinSuffix = String(r, "skin");
                if (r.contains("tone") && r["tone"].is_array() && r["tone"].size() == 3)
                    race.Tone = glm::vec3(r["tone"][0].get<float>(), r["tone"][1].get<float>(), r["tone"][2].get<float>());
                if (r.contains("parts") && r["parts"].is_object())
                    for (const auto& [k, v] : r["parts"].items())
                        if (v.is_string()) race.Parts[k] = Join(w.Root, v.get<std::string>());
                body.Races.push_back(std::move(race));
            }
    }
    if (w.Bodies[0].Parts.empty() && w.Bodies[1].Parts.empty()) return fail("no body has parts");

    if (j.contains("covers") && j["covers"].is_array())
        for (const auto& c : j["covers"]) {
            CoverRule r;
            r.Slot = String(c, "slot");
            r.NameHasAny = Strings(c, "nameHasAny");
            r.NameLacksAll = Strings(c, "nameLacksAll");
            r.Skin = c.contains("skin") && c["skin"].is_boolean() ? (c["skin"].get<bool>() ? 1 : 0) : -1;
            r.Hide = Strings(c, "hide");
            r.Replace = String(c, "replace");
            r.With = String(c, "with");
            if (!r.Slot.empty()) w.Covers.push_back(std::move(r));
        }
    if (j.contains("clears") && j["clears"].is_array())
        for (const auto& c : j["clears"]) {
            ClearRule r;
            r.Slot = String(c, "slot");
            r.NameHasAny = Strings(c, "nameHasAny");
            r.PathHasAny = Strings(c, "pathHasAny");
            r.Tags = Strings(c, "tags");
            r.Clear = String(c, "clear");
            if (!r.Slot.empty() && !r.Clear.empty()) w.Clears.push_back(std::move(r));
        }
    if (j.contains("pairs") && j["pairs"].is_array())
        for (const auto& c : j["pairs"]) {
            PairRule r{String(c, "whenSlot"), String(c, "whenNameHas"), String(c, "slot"), String(c, "suffix"),
                       String(c, "whenNameLacks")};
            if (!r.WhenSlot.empty() && !r.Slot.empty() && !r.Suffix.empty()) w.Pairs.push_back(std::move(r));
        }
    if (j.contains("layers") && j["layers"].is_array())
        for (const auto& c : j["layers"]) {
            LayerRule r;
            r.Slot = String(c, "slot");
            r.NameHasAny = Strings(c, "nameHasAny");
            r.NameLacksAll = Strings(c, "nameLacksAll");
            r.Layer = c.contains("layer") && c["layer"].is_number() ? c["layer"].get<int>() : -1;
            r.Over = Strings(c, "over");
            if (!r.Slot.empty()) w.Layers.push_back(std::move(r));
        }
    if (j.contains("tags") && j["tags"].is_array())
        for (const auto& c : j["tags"]) {
            TagRule r{ReadMatch(c), Strings(c, "add")};
            if (!r.Tags.empty()) w.TagRules.push_back(std::move(r));
        }
    if (j.contains("excludes") && j["excludes"].is_array())
        for (const auto& c : j["excludes"]) {
            if (!c.contains("a") || !c.contains("b")) continue;
            ExcludeRule r;
            r.A = ReadMatch(c["a"]);
            r.B = ReadMatch(c["b"]);
            r.Soft = c.value("soft", false);
            r.Why = String(c, "why");
            w.Excludes.push_back(std::move(r));
        }
    if (j.contains("styles") && j["styles"].is_array())
        for (const auto& c : j["styles"]) {
            Style st;
            st.Name = String(c, "name");
            if (st.Name.empty()) continue;
            st.Weight = c.value("weight", 1.0f);
            const std::string g = String(c, "gender");
            st.Gender = IEquals(g, "Female") ? 1 : IEquals(g, "Male") ? 0 : -1;
            st.DefaultFill = c.value("defaultFill", 0.0f);
            if (c.contains("fill") && c["fill"].is_object())
                for (const auto& [slot, v] : c["fill"].items())
                    if (v.is_number()) st.Fill[slot] = v.get<float>();
            w.Styles.push_back(std::move(st));
        }
    w.RandomOrder = Strings(j, "randomOrder");
    if (j.contains("items") && j["items"].is_array())
        for (const auto& c : j["items"]) {
            ItemOverride o;
            o.Path = Join(w.Root, String(c, "path"));
            o.Hidden = c.value("hidden", false);
            o.Slot = String(c, "slot");
            o.Name = String(c, "name");
            const std::string g = String(c, "gender");
            o.Gender = IEquals(g, "Female") ? 1 : IEquals(g, "Male") ? 0 : -1;
            if (!o.Path.empty()) w.Items.push_back(std::move(o));
        }
    out = std::move(w);
    return true;
}

bool Classify(const Wardrobe& w, const std::string& path, const std::vector<std::string>& materials, Item& out) {
    Item item;
    item.Path = Normalize(path);
    item.Stem = Stem(item.Path);
    item.Materials = materials;
    const ItemOverride* ov = nullptr;
    for (const auto& o : w.Items) if (IEquals(o.Path, item.Path)) { ov = &o; break; }
    if (ov && ov->Hidden) return false;

    const std::vector<std::string> folders = Folders(item.Path);
    for (const auto& f : folders)
        for (const auto& x : w.ExcludeFolders)
            if (IEquals(f, x)) return false;

    if (ov && !ov->Slot.empty()) item.Slot = ov->Slot;
    for (auto it = folders.rbegin(); it != folders.rend() && item.Slot.empty(); ++it)
        for (const auto& slot : w.Slots) {
            const bool inFolder = std::any_of(slot.Folders.begin(), slot.Folders.end(),
                                              [&](const std::string& f) { return IEquals(f, *it); });
            if (inFolder && (slot.NameSuffix.empty() || EndsWithI(item.Stem, slot.NameSuffix))) { item.Slot = slot.Id; break; }
        }
    if (item.Slot.empty() || !w.Slot(item.Slot)) return false;

    const bool femaleFolder = std::any_of(folders.begin(), folders.end(), [](const std::string& f) { return IEquals(f, "Female"); });
    item.Sex = (femaleFolder || StartsWithI(item.Stem, "SKM_F_")) ? Gender::Female : Gender::Male;
    if (ov && ov->Gender >= 0) item.Sex = (Gender)ov->Gender;
    item.Name = ov && !ov->Name.empty() ? ov->Name : PrettyName(item.Stem);
    item.Skin = std::any_of(materials.begin(), materials.end(), [&](const std::string& m) { return !SkinBase(w, m).empty(); });
    for (const auto& rule : w.TagRules) // in order: a rule may match the tags an earlier one gave
        if (Matches(rule.When, item))
            for (const auto& t : rule.Tags)
                if (!HasTag(item, t)) item.Tags.push_back(t);
    out = std::move(item);
    return true;
}

void MarkVariants(const Wardrobe& w, std::vector<Item>& items) {
    auto has = [&](const Item& like, const std::string& stem) {
        return std::any_of(items.begin(), items.end(), [&](const Item& i) {
            return i.Sex == like.Sex && i.Slot == like.Slot && IEquals(i.Stem, stem);
        });
    };
    for (auto& it : items) {
        it.Variant = false;
        if (it.Slot == w.HairSlot)
            for (const auto& hat : items)
                if (hat.Sex == it.Sex && hat.Slot == w.HatSlot) {
                    const std::string t = "_" + HatToken(hat.Stem);
                    if (EndsWithI(it.Stem, t) && has(it, it.Stem.substr(0, it.Stem.size() - t.size()))) { it.Variant = true; break; }
                }
        for (const auto& rule : w.Pairs)
            if (it.Slot == rule.Slot && EndsWithI(it.Stem, rule.Suffix) &&
                has(it, it.Stem.substr(0, it.Stem.size() - rule.Suffix.size())))
                it.Variant = true;
    }
}

const RaceDef* FindRace(const Wardrobe& w, Gender g, const std::string& name) {
    const auto& races = w.Body(g).Races;
    for (const auto& r : races) if (IEquals(r.Name, name)) return &r;
    return races.empty() ? nullptr : &races.front();
}

Resolved Resolve(const Wardrobe& w, const std::vector<Item>& catalog, const Request& request) {
    Resolved result;
    const BodyDef& body = w.Body(request.Sex);
    const RaceDef* race = FindRace(w, request.Sex, request.Race);

    auto find = [&](const std::string& slot, const std::function<bool(const Item&)>& pred) -> const Item* {
        for (const auto& it : catalog)
            if (it.Sex == request.Sex && it.Slot == slot && pred(it)) return &it;
        return nullptr;
    };

    // The items asked for, where they fit this gender and slot.
    std::map<std::string, const Item*> chosen;
    for (const auto& [slot, path] : request.Items) {
        if (path.empty()) continue;
        const Item* it = find(slot, [&](const Item& i) { return IEquals(i.Path, Normalize(path)); });
        if (it) chosen[slot] = it;
    }

    // Pairs: boots -> the pants' _Inboots cut, and back.
    for (const auto& rule : w.Pairs) {
        auto target = chosen.find(rule.Slot);
        if (target == chosen.end()) continue;
        auto when = chosen.find(rule.WhenSlot);
        const bool want = when != chosen.end() && Contains(when->second->Stem, rule.WhenNameHas) &&
                          !Contains(when->second->Stem, rule.WhenNameLacks);
        const std::string stem = target->second->Stem;
        const std::string base = EndsWithI(stem, rule.Suffix) ? stem.substr(0, stem.size() - rule.Suffix.size()) : stem;
        const std::string desired = want ? base + rule.Suffix : base;
        if (IEquals(desired, stem)) continue;
        if (const Item* alt = find(rule.Slot, [&](const Item& i) { return IEquals(i.Stem, desired); })) {
            result.Notes.push_back(target->second->Name + " -> " + alt->Name);
            target->second = alt;
        }
    }

    // A hat takes the haircut cut for it (Bobcut + Cap -> Bobcut_Cap); without it, the plain cut.
    if (auto hair = chosen.find(w.HairSlot); hair != chosen.end()) {
        std::vector<std::string> tokens;
        for (const auto& it : catalog)
            if (it.Sex == request.Sex && it.Slot == w.HatSlot) tokens.push_back(HatToken(it.Stem));
        std::sort(tokens.begin(), tokens.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        std::string base = hair->second->Stem;
        for (const auto& t : tokens)
            if (EndsWithI(base, "_" + t)) { base = base.substr(0, base.size() - t.size() - 1); break; }
        std::string desired = base;
        if (auto hat = chosen.find(w.HatSlot); hat != chosen.end()) {
            const std::string fitted = base + "_" + HatToken(hat->second->Stem);
            if (find(w.HairSlot, [&](const Item& i) { return IEquals(i.Stem, fitted); })) desired = fitted;
        }
        if (!IEquals(desired, hair->second->Stem))
            if (const Item* alt = find(w.HairSlot, [&](const Item& i) { return IEquals(i.Stem, desired); })) {
                result.Notes.push_back(hair->second->Name + " -> " + alt->Name);
                hair->second = alt;
            }
        // A fitted cut that carries the hat itself (the pack's Bobcut_Cap has the cap in it): the hat goes.
        auto builtIn = [&](const Item& cut, const Item& hat) {
            return std::any_of(w.Excludes.begin(), w.Excludes.end(), [&](const ExcludeRule& e) {
                return !e.Soft && Matches(e.A, cut) && Matches(e.B, hat);
            });
        };
        if (auto hat = chosen.find(w.HatSlot); hat != chosen.end() && builtIn(*hair->second, *hat->second)) {
            result.Notes.push_back(hair->second->Name + " has the " + hat->second->Name + " built in");
            chosen.erase(hat);
        }
    }

    // Clears: a hat with its own hair, a jacket with its own shirt.
    for (const auto& rule : w.Clears) {
        auto it = chosen.find(rule.Slot);
        if (it == chosen.end() || !chosen.count(rule.Clear)) continue;
        const bool byName = AnyIn(it->second->Stem, rule.NameHasAny);
        const bool byPath = AnyIn(it->second->Path, rule.PathHasAny);
        const bool byTag = std::any_of(rule.Tags.begin(), rule.Tags.end(), [&](const std::string& t) { return HasTag(*it->second, t); });
        if (!byName && !byPath && !byTag) continue;
        result.Notes.push_back(it->second->Name + " replaces " + chosen[rule.Clear]->Name);
        result.Dropped.push_back(rule.Clear);
        chosen.erase(rule.Clear);
    }

    // Excludes: the second of two items that don't go together comes off (a soft one only warns).
    for (const auto& rule : w.Excludes)
        for (auto a = chosen.begin(); a != chosen.end(); ++a) {
            if (!Matches(rule.A, *a->second)) continue;
            for (auto b = chosen.begin(); b != chosen.end(); ++b) {
                if (a == b || !Matches(rule.B, *b->second)) continue;
                const std::string why = rule.Why.empty() ? std::string() : " (" + rule.Why + ")";
                if (rule.Soft) { result.Clashes.push_back(a->second->Name + " with " + b->second->Name + why); continue; }
                result.Notes.push_back(b->second->Name + " doesn't go with " + a->second->Name + why);
                result.Dropped.push_back(b->first);
                chosen.erase(b);
                break;
            }
        }

    // The body: the gender's parts, the race's own, its head.
    std::vector<std::pair<std::string, std::string>> parts = body.Parts;
    if (race) {
        for (auto& [part, model] : parts)
            if (auto o = race->Parts.find(part); o != race->Parts.end()) model = o->second;
        if (!race->Head.empty()) parts.emplace_back("Head", race->Head);
    }

    // Covers: parts under an item aren't built, or are swapped for their alternate.
    for (const auto& rule : w.Covers) {
        auto it = chosen.find(rule.Slot);
        if (it == chosen.end()) continue;
        const Item& item = *it->second;
        if (!rule.NameHasAny.empty() && !AnyIn(item.Stem, rule.NameHasAny)) continue;
        if (AnyIn(item.Stem, rule.NameLacksAll)) continue;
        if (rule.Skin >= 0 && (rule.Skin == 1) != item.Skin) continue;
        for (const auto& hide : rule.Hide) {
            const size_t before = parts.size();
            parts.erase(std::remove_if(parts.begin(), parts.end(), [&](const auto& p) { return p.first == hide; }), parts.end());
            if (parts.size() != before) result.Notes.push_back(item.Name + " hides " + hide);
        }
        if (!rule.Replace.empty())
            if (auto alt = body.Alternates.find(rule.With); alt != body.Alternates.end())
                for (auto& [part, model] : parts)
                    if (part == rule.Replace && !IEquals(model, alt->second)) {
                        model = alt->second;
                        result.Notes.push_back(rule.Replace + " -> " + PrettyName(Stem(alt->second)));
                    }
    }

    for (const auto& [part, model] : parts) {
        if (model.empty()) continue;
        result.Pieces.push_back({part, model, true, part == "Head"});
    }
    for (const auto& slot : w.Slots)
        if (auto it = chosen.find(slot.Id); it != chosen.end())
            result.Pieces.push_back({slot.Id, it->second->Path, false, slot.HeadAttached});
    return result;
}

bool HasTag(const Item& item, const std::string& tag) {
    return std::any_of(item.Tags.begin(), item.Tags.end(), [&](const std::string& t) { return IEquals(t, tag); });
}

bool Matches(const Match& m, const Item& item) {
    if (!m.Slot.empty() && !IEquals(m.Slot, item.Slot)) return false;
    if (!m.NameHasAny.empty() && !AnyIn(item.Stem, m.NameHasAny)) return false;
    if (AnyIn(item.Stem, m.NameLacksAll)) return false;
    if (!m.MaterialHasAny.empty() &&
        std::none_of(item.Materials.begin(), item.Materials.end(), [&](const std::string& mat) { return AnyIn(Stem(mat), m.MaterialHasAny); }))
        return false;
    if (!m.Tags.empty() && std::none_of(m.Tags.begin(), m.Tags.end(), [&](const std::string& t) { return HasTag(item, t); })) return false;
    return std::none_of(m.LacksTags.begin(), m.LacksTags.end(), [&](const std::string& t) { return HasTag(item, t); });
}

namespace {
bool Clears(const ClearRule& rule, const Item& item, const Item& other) {
    if (!IEquals(rule.Slot, item.Slot) || !IEquals(rule.Clear, other.Slot)) return false;
    return AnyIn(item.Stem, rule.NameHasAny) || AnyIn(item.Path, rule.PathHasAny) ||
           std::any_of(rule.Tags.begin(), rule.Tags.end(), [&](const std::string& t) { return HasTag(item, t); });
}
} // namespace

bool Conflicts(const Wardrobe& w, const Item& a, const Item& b, bool soft) {
    for (const auto& rule : w.Excludes) {
        if (rule.Soft && !soft) continue;
        if ((Matches(rule.A, a) && Matches(rule.B, b)) || (Matches(rule.A, b) && Matches(rule.B, a))) return true;
    }
    for (const auto& rule : w.Clears)
        if (Clears(rule, a, b) || Clears(rule, b, a)) return true;
    return false;
}

bool InStyle(const Wardrobe& w, const Item& item, const Style& style) {
    if (HasTag(item, style.Name)) return true;
    return std::none_of(w.Styles.begin(), w.Styles.end(), [&](const Style& s) { return HasTag(item, s.Name); });
}

const Style* FindStyle(const Wardrobe& w, const std::string& name) {
    for (const auto& s : w.Styles) if (IEquals(s.Name, name)) return &s;
    return nullptr;
}

Request Randomize(const Wardrobe& w, const std::vector<Item>& catalog, const Request& base, unsigned seed,
                  const std::vector<std::string>& keep, const std::string& style, std::string* styleOut) {
    std::mt19937 rng(seed);
    auto chance = [&](float p) { return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng) < p; };
    auto kept = [&](const std::string& s) { return std::any_of(keep.begin(), keep.end(), [&](const std::string& k) { return IEquals(k, s); }); };
    Request req = base;

    const auto& races = w.Body(req.Sex).Races;
    if (!kept("Race") && !races.empty()) req.Race = races[rng() % races.size()].Name;

    // The style: the one asked for, else one for this gender by weight.
    const Style* st = style.empty() ? nullptr : FindStyle(w, style);
    if (!st) {
        std::vector<const Style*> pool;
        float total = 0.0f;
        for (const auto& s : w.Styles)
            if (s.Gender < 0 || s.Gender == (int)req.Sex) { pool.push_back(&s); total += std::max(s.Weight, 0.0f); }
        float pick = std::uniform_real_distribution<float>(0.0f, total)(rng);
        for (const Style* s : pool) {
            st = s;
            if ((pick -= std::max(s->Weight, 0.0f)) <= 0.0f) break;
        }
    }
    if (styleOut) *styleOut = st ? st->Name : std::string();

    std::vector<std::string> order = w.RandomOrder;
    for (const auto& s : w.Slots)
        if (std::none_of(order.begin(), order.end(), [&](const std::string& o) { return IEquals(o, s.Id); })) order.push_back(s.Id);

    // What's on so far (kept slots first), to check each new pick against.
    std::vector<const Item*> worn;
    auto item = [&](const std::string& path) -> const Item* {
        for (const auto& it : catalog) if (it.Sex == req.Sex && IEquals(it.Path, Normalize(path))) return &it;
        return nullptr;
    };
    for (const auto& [slot, path] : req.Items)
        if (kept(slot))
            if (const Item* it = item(path)) worn.push_back(it);

    for (const auto& slot : order) {
        if (!w.Slot(slot) || kept(slot)) continue;
        req.Items.erase(slot);
        float p = 0.3f;
        if (st) {
            const auto f = st->Fill.find(slot);
            p = f != st->Fill.end() ? f->second : st->DefaultFill;
        }
        if (!chance(p)) continue;
        std::vector<const Item*> options;
        for (const auto& it : catalog) {
            if (it.Sex != req.Sex || it.Slot != slot || it.Variant || (st && !InStyle(w, it, *st))) continue;
            if (std::any_of(worn.begin(), worn.end(), [&](const Item* o) { return Conflicts(w, it, *o); })) continue;
            options.push_back(&it);
        }
        if (options.empty()) continue;
        const Item* pick = options[rng() % options.size()];
        req.Items[slot] = pick->Path;
        worn.push_back(pick);
    }
    return req;
}

int DefaultLayer(const std::string& slot) {
    static const std::pair<const char*, int> layers[] = {
        {"Shoes", 2}, {"Pants", 3}, {"Top", 4}, {"Outerwear", 6}, {"Collar", 7}, {"Bag", 8}, {"Wrist L", 8},
        {"Wrist R", 8}, {"Hair", 9}, {"Beard", 9}, {"Glasses", 9}, {"Hat", 10},
    };
    for (const auto& [name, layer] : layers)
        if (IEquals(slot, name)) return layer;
    return 5;
}

bool DefaultHides(const std::string& slot) {
    return !IEquals(slot, "Hair") && !IEquals(slot, "Beard") && !IEquals(slot, "Glasses") && !IEquals(slot, "Wrist L") &&
           !IEquals(slot, "Wrist R");
}

Layering LayerOf(const Wardrobe& w, const std::string& slot, const std::string& itemPath, bool bodyPart) {
    Layering l;
    if (bodyPart) return l;
    const SlotDef* def = w.Slot(slot);
    l.Layer = def ? def->Layer : DefaultLayer(slot);
    l.Hides = def ? def->Hides : DefaultHides(slot);
    const std::string stem = Stem(itemPath);
    for (const auto& rule : w.Layers) {
        if (!IEquals(rule.Slot, slot) || (!rule.NameHasAny.empty() && !AnyIn(stem, rule.NameHasAny)) ||
            AnyIn(stem, rule.NameLacksAll))
            continue;
        if (rule.Layer >= 0) l.Layer = rule.Layer;
        l.Over.insert(l.Over.end(), rule.Over.begin(), rule.Over.end());
    }
    return l;
}

bool Hides(const Layering& over, const std::string& underSlot, const Layering& under, const std::string& overSlot) {
    if (!over.Hides) return false;
    // A piece moved over this one (a hood that's up, over the balaclava) isn't under it as well.
    for (const auto& s : under.Over)
        if (!overSlot.empty() && IEquals(s, overSlot)) return false;
    for (const auto& s : over.Over)
        if (IEquals(s, underSlot)) return true;
    return over.Layer > under.Layer;
}

std::string SkinMaterialFor(const Wardrobe& w, const RaceDef& race, const std::string& matPath,
                            const std::function<bool(const std::string&)>& exists) {
    const std::string base = SkinBase(w, matPath);
    if (base.empty()) return matPath;
    const std::string p = Normalize(matPath);
    const size_t slash = p.find_last_of('/');
    const std::string dir = slash == std::string::npos ? std::string() : p.substr(0, slash + 1);
    if (!race.SkinSuffix.empty()) {
        const std::string variant = dir + base + race.SkinSuffix + ".mat";
        if (!exists || exists(variant)) return variant;
    }
    return dir + base + ".mat";
}

std::vector<std::string> Colourways(const std::string& matPath, const std::vector<std::string>& siblings) {
    std::vector<std::string> out;
    for (const auto& s : siblings)
        if (EndsWithI(s, ".mat")) out.push_back(Normalize(s));
    const std::string self = Normalize(matPath);
    if (!self.empty() && std::none_of(out.begin(), out.end(), [&](const std::string& s) { return IEquals(s, self); }))
        out.push_back(self);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return Lower(Stem(a)) < Lower(Stem(b)); });
    return out;
}

Diff MakeDiff(const std::map<std::string, std::string>& current, const std::vector<Piece>& desired) {
    Diff d;
    std::set<std::string> wanted;
    for (const auto& p : desired) {
        wanted.insert(p.Slot);
        auto it = current.find(p.Slot);
        if (it == current.end()) d.Create.push_back(p);
        else if (!IEquals(Normalize(it->second), Normalize(p.Path))) d.Remodel.push_back(p);
    }
    for (const auto& [slot, path] : current)
        if (!wanted.count(slot)) d.Destroy.push_back(slot);
    return d;
}

} // namespace Wardrobe
