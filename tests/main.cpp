// Starlifter OS test suite.
//
// The tests that matter here are the awkward ones. Anyone can show that a
// task gets assigned and completed when every packet arrives. What has to
// be demonstrated is what happens when they do not: when an offer is lost,
// an answer is lost, a node goes silent holding work, or a vehicle faults
// halfway through. Each of those has a test that makes it happen.
//
// The wire format itself -- the OTF-1 codec and the JSON parser under it --
// is tested where it lives, in telemetry-protocol.

#include "check.hpp"

int main() {
    std::printf("Starlifter OS tests\n\n");

    runNodeTests();
    runCoordinatorTests();
    runFleetTests();
    runLedgerFeedTests();

    std::printf("\n%d checks, %d failures\n", check::checks(), check::failures());
    return check::failures() == 0 ? 0 : 1;
}
