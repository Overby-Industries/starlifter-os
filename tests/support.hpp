#pragma once

// Shared fixtures for the node, coordinator and fleet tests.

#include <string>
#include <vector>

#include "starlifter/coordinator.hpp"
#include "starlifter/local_control.hpp"
#include "starlifter/otf1.hpp"

namespace support {

using namespace starlifter;

// 2025-02-18T14:05:00Z. Every test clock starts here.
constexpr TimeMs kT0 = 1739887500000;
constexpr TimeMs kSecond = 1000;
constexpr TimeMs kMinute = 60 * kSecond;

inline const char* kCoordinator = "fleet-coordinator";

inline Task makeTask(const std::string& id, TaskAction action = TaskAction::Capture,
                     Zone zone = Zone::LEO, double massKg = 5.2) {
    Task task;
    task.id = id;
    task.action = action;
    task.location.zone = zone;
    Target target;
    target.objectType = action == TaskAction::Capture || action == TaskAction::Deorbit
                            ? ObjectType::Fragment
                            : ObjectType::Regolith;
    target.massKg = massKg;
    task.target = target;
    return task;
}

// Decode a batch of packets, keeping only those of one kind.
template <typename T>
std::vector<T> only(const std::vector<std::string>& packets) {
    std::vector<T> found;
    for (const std::string& text : packets) {
        Packet packet;
        if (decode(text, packet)) {
            if (const T* typed = std::get_if<T>(&packet)) found.push_back(*typed);
        }
    }
    return found;
}

inline std::vector<std::string> packetsOf(const std::vector<Outgoing>& outgoing) {
    std::vector<std::string> packets;
    for (const Outgoing& item : outgoing) packets.push_back(item.packet);
    return packets;
}

// A local controller a test can steer by hand: every reading the fleet
// agent takes is a plain field.
class FakeControl : public LocalControl {
public:
    Mode currentMode = Mode::Nominal;
    WorkState currentWork = WorkState::Idle;
    Location where;
    std::vector<TaskAction> can = {TaskAction::Capture, TaskAction::Deorbit};
    bool refuse = false;
    double fraction = 0.0;
    WorkOutcome result;
    int beginCalls = 0;
    Task lastBegun;

    void tick(TimeMs) override {}
    Mode mode() const override { return currentMode; }
    std::string phase() const override {
        return currentWork == WorkState::Active ? "Working" : "StationKeeping";
    }
    Location location() const override { return where; }
    std::vector<TaskAction> capabilities() const override { return can; }
    bool begin(const Task& task, TimeMs) override {
        ++beginCalls;
        if (refuse) return false;
        lastBegun = task;
        currentWork = WorkState::Active;
        fraction = 0.0;
        return true;
    }
    WorkState work() const override { return currentWork; }
    double progress() const override { return fraction; }
    WorkOutcome outcome() const override { return result; }
};

}  // namespace support
