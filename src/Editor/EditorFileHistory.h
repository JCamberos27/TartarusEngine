#pragma once
#include "AtomicFile.h"
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <vector>
#include <stdexcept>

namespace EditorFileHistory {
struct State {
    std::filesystem::path Path;
    std::shared_ptr<const std::string> Bytes; // null means the file did not exist
};
inline State Capture(const std::filesystem::path& path) {
    std::error_code ec;
    if (!std::filesystem::exists(path,ec)) return {path,{}};
    if (ec || !std::filesystem::is_regular_file(path,ec)) throw std::runtime_error("Cannot record file: " + path.string());
    std::ifstream file(path,std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read undo pre-image: " + path.string());
    auto bytes=std::make_shared<std::string>(std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>());
    if (file.bad()) throw std::runtime_error("Cannot read undo pre-image: " + path.string());
    return {path,std::move(bytes)};
}
inline bool Equal(const State& a,const State& b) {
    return (!a.Bytes && !b.Bytes) || (a.Bytes && b.Bytes && *a.Bytes==*b.Bytes);
}
inline std::vector<State> CaptureCurrent(const std::vector<State>& states) {
    std::vector<State> result;
    for (const auto& state:states) result.push_back(Capture(state.Path));
    return result;
}
inline bool ApplyOne(const State& state) {
    if (state.Bytes) return AtomicFile::WriteBytes(state.Path,*state.Bytes,true);
    std::error_code ec;
    // Exact regular files only; never remove directories recursively.
    if (!std::filesystem::exists(state.Path,ec)) return !ec;
    return std::filesystem::is_regular_file(state.Path,ec) && std::filesystem::remove(state.Path,ec) && !ec;
}
inline bool Restore(const std::vector<State>& states) {
    auto current=CaptureCurrent(states);
    for (const auto& state:states) if (!ApplyOne(state)) {
        for (const auto& rollback:current) ApplyOne(rollback);
        return false;
    }
    return true;
}
class Journal {
    std::map<std::filesystem::path,State> before;
public:
    void Record(const std::filesystem::path& path) {
        const auto key=std::filesystem::absolute(path).lexically_normal();
        if (!before.count(key)) before.emplace(key,Capture(key));
    }
    bool Empty() const { return before.empty(); }
    void Clear() { before.clear(); }
    std::vector<State> Changes() const {
        std::vector<State> result;
        for (const auto& [path,state]:before) if (!Equal(state,Capture(path))) result.push_back(state);
        return result;
    }
};
}
