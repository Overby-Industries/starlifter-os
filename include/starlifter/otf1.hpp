#pragma once

// OTF-1 packets as typed values, and the only two functions that turn them
// into bytes and back.
//
// architecture.md's principle 2 is "OTF-1 as the only wire format". This
// header is how that is enforced rather than merely intended: a Node and a
// Coordinator never hand each other an object, only the std::string that
// encode() produced, and neither will act on a string decode() rejects.
//
// The canonical definition is the JSON Schema in the telemetry-protocol
// repo (a pinned copy lives in protocol/). decode() applies the same rules
// in C++ -- required fields, closed enums, no unknown fields -- and the test
// suite holds the two to the same corpus of valid and invalid packets.

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "starlifter/time.hpp"

namespace starlifter {

enum class Role { Node, Coordinator };
enum class NodeType { Reclaimer, Miner, Refinery, Shuttle };
enum class Zone { LEO, MEO, GEO, Cislunar, NEA, Belt, Surface };
enum class ObjectType { Fragment, SpentStage, DerelictSatellite, Regolith, Volatile, Ore };

// What a node can be asked to do, and what it reports having done. The two
// enums are the same six verbs in two tenses; eventActionFor() is the map.
enum class TaskAction { Capture, Process, Refine, Store, Deorbit, Deploy };
enum class EventAction { Captured, Processed, Refined, Stored, Deorbited, Deployed };
EventAction eventActionFor(TaskAction action);

enum class RepurposedAs { ShieldBlock, Aggregate, Ingot, Propellant, Polymer, None };
enum class OutcomeStatus { Stored, InTransit, Delivered, Failed };

// Health as the node itself judges it.
//   Nominal   available for work
//   Degraded  finishing what it holds, taking nothing new
//   Safe      local fault response has taken over; any held task is abandoned
enum class Mode { Nominal, Degraded, Safe };

// The node's own view of coordinator contact.
enum class LinkView { Connected, Autonomous };

enum class TaskState { Active, Completed, Failed };
enum class AckResult { Accepted, Rejected };
enum class RejectReason { Busy, Degraded, SafeMode, Expired, Unsupported, Refused };

const char* toString(Role value);
const char* toString(NodeType value);
const char* toString(Zone value);
const char* toString(ObjectType value);
const char* toString(TaskAction value);
const char* toString(EventAction value);
const char* toString(RepurposedAs value);
const char* toString(OutcomeStatus value);
const char* toString(Mode value);
const char* toString(LinkView value);
const char* toString(TaskState value);
const char* toString(AckResult value);
const char* toString(RejectReason value);

struct Endpoint {
    std::string id;
    Role role = Role::Node;
};

struct Location {
    Zone zone = Zone::LEO;
    std::optional<double> altitudeKm;
    std::optional<double> inclinationDeg;
};

struct Target {
    ObjectType objectType = ObjectType::Fragment;
    std::optional<std::string> catalogId;
    std::optional<double> sizeCm;
    std::optional<double> massKg;
};

// One unit of work a coordinator can offer to a node.
struct Task {
    std::string id;
    TaskAction action = TaskAction::Capture;
    Location location;
    std::optional<Target> target;
};

// A node's report on the task it holds, or the last one it finished.
struct TaskStatus {
    std::string id;
    TaskState state = TaskState::Active;
    std::optional<double> progress;  // 0..1
};

// kind: "event" -- something that happened to material. The original OTF-1
// packet, and the one the public record is built from.
struct EventPacket {
    TimeMs timestamp = 0;
    std::string missionId;
    std::string podId;
    NodeType missionType = NodeType::Reclaimer;
    Location location;
    std::optional<Target> target;
    EventAction action = EventAction::Captured;
    std::optional<double> processedKg;
    std::optional<RepurposedAs> repurposedAs;
    OutcomeStatus outcomeStatus = OutcomeStatus::Stored;
    bool smessc = false;
    std::optional<double> odciContributionKg;
    std::optional<std::string> proofOfCleanupToken;
    std::optional<std::string> taskId;  // the assignment this event completes
};

// kind: "status" -- a node's complete snapshot of itself.
struct StatusPacket {
    TimeMs timestamp = 0;
    Endpoint source;  // role is always Node
    std::uint64_t seq = 0;
    NodeType type = NodeType::Reclaimer;
    Mode mode = Mode::Nominal;
    std::optional<std::string> phase;  // local control state name, informative only
    LinkView link = LinkView::Autonomous;
    std::vector<TaskAction> capabilities;
    Location location;
    std::optional<TaskStatus> task;
};

// kind: "heartbeat" -- liveness, from either side.
struct HeartbeatPacket {
    TimeMs timestamp = 0;
    Endpoint source;
    std::uint64_t seq = 0;
};

// kind: "assignment" -- a coordinator offering one task to one node.
struct AssignmentPacket {
    TimeMs timestamp = 0;
    Endpoint source;  // role is always Coordinator
    std::string destination;
    std::uint64_t seq = 0;
    Task task;
    TimeMs acceptBy = 0;  // the node must refuse the offer after this instant
};

// kind: "ack" -- a node's answer to an assignment.
struct AckPacket {
    TimeMs timestamp = 0;
    Endpoint source;  // role is always Node
    std::string destination;
    std::uint64_t seq = 0;
    std::string taskId;
    AckResult result = AckResult::Accepted;
    std::optional<RejectReason> reason;  // present exactly when rejected
};

using Packet =
    std::variant<EventPacket, StatusPacket, HeartbeatPacket, AssignmentPacket, AckPacket>;

// Serialise a packet as one line of compact JSON.
std::string encode(const EventPacket& packet);
std::string encode(const StatusPacket& packet);
std::string encode(const HeartbeatPacket& packet);
std::string encode(const AssignmentPacket& packet);
std::string encode(const AckPacket& packet);
std::string encode(const Packet& packet);

// Parse and validate one packet. Returns false -- and says why in `error` --
// for anything that is not a well-formed OTF-1 packet: malformed JSON, a
// missing or mistyped field, a value outside a closed enum, an unknown
// field, or a source whose role does not fit the packet's kind.
bool decode(const std::string& text, Packet& out, std::string* error = nullptr);

}  // namespace starlifter
