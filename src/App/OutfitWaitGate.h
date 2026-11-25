#pragma once
// The harness wait for an outfit change's loads (--npc-test, --perf-bench --smoke-shots): keep waiting while the loads
// are still in flight, give up only at a hard cap, so a slow machine or a long queue of textures never leaves a picture
// of a half-loaded outfit. A header of its own so the unit tests can drive it with a fake clock.

class OutfitWaitGate {
public:
    enum class State { Done, Waiting, CapHit };
    static constexpr double kCapSeconds = 60.0;

    // `loading`: outfit changes still pending and loads in flight. `nowSeconds`: any steady clock.
    State Step(bool loading, double nowSeconds) {
        if (!loading) return State::Done;
        if (m_Start < 0.0) m_Start = nowSeconds;
        return nowSeconds - m_Start > kCapSeconds ? State::CapHit : State::Waiting;
    }

private:
    double m_Start = -1.0;
};
