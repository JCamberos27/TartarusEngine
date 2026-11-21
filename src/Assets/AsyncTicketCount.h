#pragma once
// AssetLibrary::AsyncInFlight's ticket half, a template so the unit tests can run it on stand-in tickets (the real
// ones need the async loader, which needs GL).
#include <algorithm>
#include <memory>
#include <vector>

// Tickets whose jobs are all in but that aren't ready yet: the pump's budget left their material for the next pump.
// A frameless drain loops while loads are in flight, so these count as in flight.
template <class Ticket>
int CountWaitingTickets(const std::vector<std::weak_ptr<Ticket>>& tickets) {
    int n = 0;
    for (const auto& w : tickets) {
        const std::shared_ptr<Ticket> t = w.lock();
        if (t && !t->Ready && std::all_of(t->Jobs.begin(), t->Jobs.end(), [](const auto& j) { return j->Complete(); })) ++n;
    }
    return n;
}
