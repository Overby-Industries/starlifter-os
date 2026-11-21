#pragma once

// The wire vocabulary of the fleet layer.
//
// architecture.md's principle 2 is "OTF-1 as the only wire format". The
// format, and the C++ that reads and writes it, live in the
// telemetry-protocol repository as libotf1; this header brings the names
// the fleet layer uses into its own namespace. Nothing is redefined here.
// A Node and a Coordinator never hand each other an object, only the
// std::string that otf1::encode() produced, and neither acts on a string
// otf1::decode() rejects.

#include <otf1/otf1.hpp>

namespace starlifter {

// Time: milliseconds since the Unix epoch, UTC. Nothing in the fleet layer
// reads the machine's clock; every function that needs "now" is handed it.
using otf1::TimeMs;
using otf1::formatTimestamp;
using otf1::parseTimestamp;

// Vocabulary.
using otf1::AckResult;
using otf1::EventAction;
using otf1::LinkView;
using otf1::Mode;
using otf1::NodeType;
using otf1::ObjectType;
using otf1::OutcomeStatus;
using otf1::RejectReason;
using otf1::RepurposedAs;
using otf1::Role;
using otf1::TaskAction;
using otf1::TaskState;
using otf1::Zone;
using otf1::eventActionFor;
using otf1::toString;

// Parts of packets.
using otf1::Endpoint;
using otf1::Location;
using otf1::Target;
using otf1::Task;
using otf1::TaskStatus;

// Packets, and the only two functions that turn them into bytes and back.
using otf1::AckPacket;
using otf1::AssignmentPacket;
using otf1::EventPacket;
using otf1::HeartbeatPacket;
using otf1::Packet;
using otf1::StatusPacket;
using otf1::decode;
using otf1::encode;

}  // namespace starlifter
