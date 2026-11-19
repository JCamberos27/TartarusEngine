#pragma once
// Text matching shared by the editor host and the TartarusEditor module DLL (the Asset Browser,
// Hierarchy and Console filters, the Asset Browser's t:/l: search). Standard library only, so the
// module can include it: it used to keep copies of these.
#include <algorithm>
#include <cctype>
#include <set>
#include <string>
#include <vector>

namespace EditorSearch {

// Case-insensitive substring match; an empty filter matches everything. No allocation.
inline bool MatchesFilter(const std::string& filter, const std::string& text) {
    if (filter.empty()) return true;
    const auto eq = [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); };
    return std::search(text.begin(), text.end(), filter.begin(), filter.end(), eq) != text.end();
}

// Virtual folder paths are '/'-joined segments (e.g. "Props/Guns"): the parent and the last segment.
inline std::string ParentFolderOf(const std::string& folderPath) {
    const size_t slash = folderPath.find_last_of('/');
    return slash == std::string::npos ? std::string() : folderPath.substr(0, slash);
}
inline std::string LeafNameOf(const std::string& folderPath) {
    const size_t slash = folderPath.find_last_of('/');
    return slash == std::string::npos ? folderPath : folderPath.substr(slash + 1);
}

// Whether the space/tab separated `filter` holds `token` exactly.
inline bool SearchHasToken(const std::string& filter, const std::string& token) {
    std::string t;
    for (size_t i = 0; i <= filter.size(); ++i) {
        if (i == filter.size() || filter[i] == ' ' || filter[i] == '\t') {
            if (t == token) return true;
            t.clear();
        } else {
            t += filter[i];
        }
    }
    return false;
}

// Removes `token` from `filter` if it's there, else appends it; the tokens are re-joined by single spaces.
inline void ToggleSearchToken(std::string& filter, const std::string& token) {
    std::vector<std::string> tokens;
    std::string t;
    bool removed = false;
    for (size_t i = 0; i <= filter.size(); ++i) {
        if (i == filter.size() || filter[i] == ' ' || filter[i] == '\t') {
            if (!t.empty()) {
                if (t == token) removed = true;
                else tokens.push_back(t);
            }
            t.clear();
        } else {
            t += filter[i];
        }
    }
    if (!removed) tokens.push_back(token);
    filter.clear();
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i) filter += " ";
        filter += tokens[i];
    }
}

// Unity's Project-window search syntax: plain words match the asset name and are ANDed together
// ("coastal scene" needs both words); "t:Model" restricts by asset type, and several t: terms are
// ORed ("t:Model t:Texture" means either kind); "l:label" restricts by label, and several l: terms
// are ANDed (must carry every listed label). Typed directly or built by the Type/Label filter
// dropdown buttons - both just edit this same text.
struct ParsedAssetSearch {
    std::vector<std::string> nameTerms;
    std::vector<std::string> typeTerms;  // lower-cased kind names: "model", "texture", ...
    std::vector<std::string> labelTerms; // lower-cased
};

inline ParsedAssetSearch ParseAssetSearch(const std::string& filter) {
    ParsedAssetSearch result;
    const auto lower = [](std::string s) {
        for (char& c : s) c = (char)std::tolower((unsigned char)c);
        return s;
    };
    std::string token;
    for (size_t i = 0; i <= filter.size(); ++i) {
        const bool end = i == filter.size() || std::isspace((unsigned char)filter[i]);
        if (!end) { token += filter[i]; continue; }
        if (token.empty()) continue;
        if (token.size() > 2 && (token[0] == 't' || token[0] == 'T') && token[1] == ':') result.typeTerms.push_back(lower(token.substr(2)));
        else if (token.size() > 2 && (token[0] == 'l' || token[0] == 'L') && token[1] == ':') result.labelTerms.push_back(lower(token.substr(2)));
        else result.nameTerms.push_back(token);
        token.clear();
    }
    return result;
}

// `kind` is the asset's type name (lower-cased: "model", "texture", ...), `labels` its label set.
// Name terms AND, type terms OR (any one is enough), label terms AND (every one must be present,
// matched whole and case-insensitively) - the semantics Unity documents for t: / l:.
inline bool MatchesAssetSearch(const ParsedAssetSearch& parsed, const std::string& name, const std::string& kind,
                               const std::set<std::string>& labels) {
    for (const auto& term : parsed.nameTerms)
        if (!MatchesFilter(term, name)) return false;
    if (!parsed.typeTerms.empty() &&
        std::find(parsed.typeTerms.begin(), parsed.typeTerms.end(), kind) == parsed.typeTerms.end())
        return false;
    for (const auto& term : parsed.labelTerms) {
        bool found = false;
        for (const auto& lbl : labels)
            if (term.size() == lbl.size() && MatchesFilter(term, lbl)) { found = true; break; }
        if (!found) return false;
    }
    return true;
}

} // namespace EditorSearch
