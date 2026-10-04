#pragma once
#include <string>
#include <vector>

#include "../Assets/KnifeFxImport.h"

// The Knife packs' texture libraries on the GPU (docs/BLOOD_FX.md, "v2"): one GL_TEXTURE_2D_ARRAY of
// BC3 colour and one of BC5 normals per library (KnifeFxImport::Library), read from the git-ignored
// project/assets/Effects/Knife/*.kfx that `--import-knife-fx` writes. Entries are looked up by name
// once (an id), then read per spawn.
class KnifeFxLibrary {
public:
    static KnifeFxLibrary& Get(); // deliberately leaked (GL objects outlive static destruction otherwise)

    struct Entry {
        std::string Name;
        KnifeFxImport::Library Lib = KnifeFxImport::Library::Sprite;
        int ColorLayer = 0, NormalLayer = -1;
        int Cols = 1, Rows = 1, Frames = 1;
        unsigned Flags = 0;
        float Smoothness = 0.5f, Aspect = 1.0f;
    };

    // Reads the libraries once (needs the GL context). False when none is there (the import hasn't been
    // run): every Find then returns -1 and the effects that need them stay off, past one log line.
    bool Load();
    bool Loaded() const { return m_Loaded; }
    int Find(const std::string& name) const; // -1 if unknown
    const Entry* At(int id) const { return id >= 0 && id < (int)m_Entries.size() ? &m_Entries[id] : nullptr; }
    // The library's arrays (0 when that library is missing).
    unsigned ColorArray(KnifeFxImport::Library lib) const { return m_Color[(int)lib]; }
    unsigned NormalArray(KnifeFxImport::Library lib) const { return m_Normal[(int)lib]; }

private:
    KnifeFxLibrary() = default;
    bool m_Loaded = false, m_LoadTried = false;
    std::vector<Entry> m_Entries;
    unsigned m_Color[(int)KnifeFxImport::Library::Count] = {};
    unsigned m_Normal[(int)KnifeFxImport::Library::Count] = {};
};
