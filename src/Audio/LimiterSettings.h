#pragma once

// Master limiter / trim / glue-compressor settings (see MasterLimiter.h for what each does).
struct LimiterSettings {
    bool Enabled = true;
    float CeilingDb = -1.0f;     // dBFS
    float LookaheadMs = 1.5f;    // 0.1 .. 10
    float ReleaseMs = 80.0f;     // time constant of the gain's recovery
    // Before the limiter: the master trim, then the glue compressor (feed-forward, linked stereo, peak detector, soft knee, the
    // gain reduction smoothed with separate attack / release). Off = bypassed bit for bit.
    float TrimDb = 0.0f;
    bool GlueEnabled = false;
    float GlueThresholdDb = -12.0f;
    float GlueRatio = 2.0f;
    float GlueKneeDb = 6.0f;
    float GlueAttackMs = 15.0f;
    float GlueReleaseMs = 200.0f;
};
