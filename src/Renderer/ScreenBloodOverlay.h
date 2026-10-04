#pragma once
#include <memory>

class Shader;
class ScreenBlood;

// Draws ScreenBlood's splats over the finished frame (docs/BLOOD_FX.md, v2): Real Blood's splash flipbook from
// the Knife sprite library, wet and glossy (its normal map catches a light from above), alpha blended, under
// the HUD. Nothing to draw (or no Knife library): no GL work at all.
class ScreenBloodOverlay {
public:
    ScreenBloodOverlay();
    ~ScreenBloodOverlay();
    ScreenBloodOverlay(const ScreenBloodOverlay&) = delete;
    ScreenBloodOverlay& operator=(const ScreenBloodOverlay&) = delete;

    void Draw(unsigned int dstFbo, int width, int height, const ScreenBlood& blood);

private:
    std::unique_ptr<Shader> m_Shader;
    unsigned int m_VAO = 0;
    bool m_Failed = false;
    int m_Entry = -2; // KnifeFxLibrary id of the splash sheet (-2: not looked up yet)
};
