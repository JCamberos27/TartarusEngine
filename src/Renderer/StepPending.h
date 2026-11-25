#pragma once
// Model::GpuUploadStep's rule, a template so the unit tests can run it on stand-in uploads (the real ones need GL).
// Every pending item is offered a step until one makes progress, not just the first. An item holding a staging-ring
// reservation (its copy done) has to be stepped to retire it; stepping only the first pending one stalled a model for
// good when that one waited for ring space the later one held.

enum class PendingStep { None, Waiting, Progress }; // nothing pending / all pending ones waiting / one stepped

// isPending(item): still needs its upload. step(item): true when it made progress, false when it is waiting.
template <class Range, class IsPending, class StepFn>
PendingStep StepPending(Range& items, IsPending isPending, StepFn step) {
    bool waiting = false;
    for (auto& item : items) {
        if (!isPending(item)) continue;
        if (step(item)) return PendingStep::Progress;
        waiting = true;
    }
    return waiting ? PendingStep::Waiting : PendingStep::None;
}
