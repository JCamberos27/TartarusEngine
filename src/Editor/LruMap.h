#pragma once
#include <cstddef>
#include <list>
#include <string>
#include <unordered_map>
#include <utility>

// A string-keyed map that remembers use order, for the editor's bounded caches (thumbnails,
// waveforms). Find() counts as a use; Oldest() is the least recently used entry, the one to
// evict. Not thread-safe: main thread only, like its callers' maps.
template <class V>
class LruMap {
public:
    // The entry for `key`, moved to the most-recently-used end; null when absent.
    V* Find(const std::string& key) {
        auto it = m_Index.find(key);
        if (it == m_Index.end()) return nullptr;
        m_Items.splice(m_Items.begin(), m_Items, it->second);
        return &it->second->second;
    }
    // The entry for `key` without counting it as a use; null when absent.
    V* Peek(const std::string& key) {
        auto it = m_Index.find(key);
        return it == m_Index.end() ? nullptr : &it->second->second;
    }
    // Adds (or replaces) `key` as the most recently used entry.
    V& Insert(const std::string& key, V value) {
        if (V* v = Find(key)) { *v = std::move(value); return *v; }
        m_Items.emplace_front(key, std::move(value));
        m_Index.emplace(key, m_Items.begin());
        return m_Items.front().second;
    }
    // Removes `key`, handing its value to `onRemove` first (to free a GL texture, say).
    template <class F> bool Erase(const std::string& key, F&& onRemove) {
        auto it = m_Index.find(key);
        if (it == m_Index.end()) return false;
        onRemove(it->second->second);
        m_Items.erase(it->second);
        m_Index.erase(it);
        return true;
    }
    bool Erase(const std::string& key) { return Erase(key, [](V&) {}); }
    // Removes least-recently-used entries until at most `maxSize` are left.
    template <class F> void TrimTo(size_t maxSize, F&& onRemove) {
        while (m_Items.size() > maxSize) {
            onRemove(m_Items.back().second);
            m_Index.erase(m_Items.back().first);
            m_Items.pop_back();
        }
    }
    void TrimTo(size_t maxSize) { TrimTo(maxSize, [](V&) {}); }
    // Every value, most recently used first.
    template <class F> void ForEach(F&& f) { for (auto& item : m_Items) f(item.second); }
    void Clear() { m_Items.clear(); m_Index.clear(); }
    size_t Size() const { return m_Items.size(); }

private:
    using Items = std::list<std::pair<std::string, V>>; // front = most recently used
    Items m_Items;
    std::unordered_map<std::string, typename Items::iterator> m_Index;
};
