#pragma once

// The seam between the fleet layer and a vehicle's own control software.
//
// Starlifter OS sits above per-vehicle control, not in place of it. This
// interface is where "above" is drawn: everything on the far side of it --
// closed-loop control, fault detection, the decision to safe the vehicle --
// belongs to the vehicle and keeps working with no coordinator in contact
// and no fleet software running at all. power-and-propulsion's ShuttleFSM is
// the kind of thing that lives there.
//
// So the interface is deliberately one-directional about authority. The
// fleet agent may *ask* local control to begin a task, and local control may
// refuse. The fleet agent may *read* mode, phase and progress. It has no way
// to command a mode change, clear a fault, or interrupt a fault response,
// because nothing on a vehicle's safety path may depend on the fleet layer.

#include <optional>
#include <string>
#include <vector>

#include "starlifter/wire.hpp"

namespace starlifter {

enum class WorkState {
    Idle,       // on the local default task (station-keeping)
    Active,     // working on the task most recently begun
    Completed,  // that task finished; outcome() describes what it produced
    Failed      // that task was abandoned or could not be finished
};

// What a completed task produced, in the terms an OTF-1 event reports it.
// The certification fields are the vehicle's own claim about its own work.
struct WorkOutcome {
    double processedKg = 0.0;
    OutcomeStatus status = OutcomeStatus::Stored;
    std::optional<RepurposedAs> repurposedAs;
    bool smessc = false;
    std::optional<double> odciContributionKg;
};

class LocalControl {
public:
    virtual ~LocalControl() = default;

    // Advance local control by one step. Called once per fleet-agent tick,
    // before the agent reads anything else.
    virtual void tick(TimeMs now) = 0;

    // Health as the vehicle itself judges it. Entering Mode::Safe must
    // abandon any active work; the agent treats it that way regardless.
    virtual Mode mode() const = 0;

    // Name of the current local control state, reported upward verbatim and
    // never interpreted by the fleet layer.
    virtual std::string phase() const = 0;

    virtual Location location() const = 0;

    // The task actions this vehicle can perform.
    virtual std::vector<TaskAction> capabilities() const = 0;

    // Ask local control to start a task. Returning false refuses it; the
    // vehicle always has the final say. Only called while work() is not
    // Active.
    virtual bool begin(const Task& task, TimeMs now) = 0;

    virtual WorkState work() const = 0;

    // 0..1 while work() is Active.
    virtual double progress() const = 0;

    // Valid while work() is Completed.
    virtual WorkOutcome outcome() const = 0;
};

}  // namespace starlifter
