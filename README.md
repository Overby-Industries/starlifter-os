# Starlifter OS

[![Version](https://img.shields.io/badge/Version-0.1.0--Alpha-blue?style=for-the-badge&logo=github)](https://github.com/Overby-Industries/starlifter-os/releases)
[![Build & Test](https://img.shields.io/github/actions/workflow/status/Overby-Industries/starlifter-os/build.yml?branch=main&style=for-the-badge&label=Build%20%26%20Test&logo=githubactions&logoColor=white)](https://github.com/Overby-Industries/starlifter-os/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/License-Apache_2.0-lightgrey?style=for-the-badge&logo=apache)](LICENSE)
[![Wire Format](https://img.shields.io/badge/Wire_Format-OTF--1-purple?style=for-the-badge)](https://github.com/Overby-Industries/telemetry-protocol)
[![Language](https://img.shields.io/badge/C%2B%2B-17-00599C?style=for-the-badge&logo=cplusplus&logoColor=white)](CMakeLists.txt)
[![Status](https://img.shields.io/badge/Status-First_Slice-orange?style=for-the-badge)](#status)

> *"A task leaves a node only on evidence. Silence is never evidence."*

**Overby Industries — distributed operating system for fleets.**

Coordinates a fleet of otherwise-independent vehicles and pods (Starlifters,
miner/refinery pods, satellites) that each run their own local control
software. This is the fleet-level layer, not the per-vehicle one.

## Status

**First working slice.** A fleet coordinator and an on-vehicle fleet agent
exist, talk to each other only in OTF-1 packets, and are exercised by a
deterministic simulation against the conditions that shape the design:
light-delay, link outages, a vehicle faulting mid-task, and a node that never
comes back.

What is here:

| Piece | Where | What it does |
|---|---|---|
| OTF-1 codec | `include/starlifter/otf1.hpp` | Typed packets, strict encode and decode, no dependencies |
| Fleet agent | `include/starlifter/node.hpp` | Runs on a vehicle: reports status, answers offers, keeps working out of contact |
| Local control seam | `include/starlifter/local_control.hpp` | The line between fleet software and a vehicle's own control |
| Coordinator | `include/starlifter/coordinator.hpp` | Fleet-wide state, contact supervision, task scheduler |
| Simulator | `include/starlifter/sim.hpp`, `sim/main.cpp` | Coordinator, nodes and delayed, interruptible links on a simulated clock |
| Tests | `tests/` | 501 checks, no framework, run under `ctest` |

What it is not: flight software. There is no radio, no real transport, no
authentication, and the coordinator keeps its state in memory. See
[Not Built Yet](#not-built-yet).

## Quick Start

C++17 and CMake. No other dependencies.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure

./build/starlifter_fleet_sim
```

To check that everything the simulation put on the wire is valid OTF-1,
using the published JSON Schema rather than this repo's own decoder:

```bash
pip install jsonschema
./build/starlifter_fleet_sim --log fleet.otf1.jsonl --feed feed.otf1.jsonl
python tools/check_wire_log.py fleet.otf1.jsonl feed.otf1.jsonl
```

## What the Simulation Shows

`starlifter_fleet_sim` runs one coordinator and five nodes through twenty
simulated minutes in which four things go wrong:

| What goes wrong | What happens |
|---|---|
| Two pods sit at a near-Earth asteroid, 40 light-seconds away | Offers are given a deadline that allows for the trip. Nothing arrives already expired. |
| Reclaimer `A2` faults and safes itself mid-task | It reports the task failed. The task returns to the queue and `A1` completes it. `A2` is offered nothing further. |
| Refinery `R7` drops out of contact for five minutes mid-task | It finishes alone and holds the record on board. When the link returns, its own report settles the task. Nothing is duplicated. |
| Miner `M3` goes silent for good just as it is offered work | The task is marked in doubt and stays with `M3`. It is not handed to another node. |

The end of the run:

```
Tasks
  T-0001  capture  LEO  completed A1  failures 0
  T-0002  capture  LEO  completed A2  failures 0
  T-0003  capture  LEO  completed A1  failures 0
  T-0004  deorbit  LEO  completed A1  failures 1
  T-0005  deploy   LEO  completed S1  failures 0
  T-0006  process  NEA  completed M3  failures 0
  T-0007  refine   NEA  completed R7  failures 0
  T-0008  process  NEA  in-doubt  M3  failures 0
  T-0009  capture  LEO  completed A1  failures 0

Wire
  1479 packets sent, 459 lost to outages
  ack 10  assignment 10  event 8  heartbeat 240  status 1211

Summary
  tasks      8 completed, 1 in doubt, 0 active, 0 offered, 0 queued, 0 failed
  nodes      4 in contact, 1 lost
  record     8 events, 3730.8 kg processed, 1730.8 kg toward ODCI
  conflicts  0
```

The run is deterministic. The same scenario produces the same packets in the
same order on every machine and every compiler.

## How It Works

Three rules carry most of the design. The full account, including the task
lifecycle and every case the coordinator distinguishes, is in
[`docs/fleet-coordination.md`](docs/fleet-coordination.md).

**The coordinator offers; the vehicle decides.** An assignment is an offer
with an expiry, not a command. The fleet agent checks what it can, then asks
the vehicle's own control software, which may refuse. The fleet layer has no
way to command a mode change, clear a fault, or interrupt a fault response.

**Losing contact changes nothing about the work in hand.** A node that stops
hearing the coordinator keeps its current task to completion, then holds its
local default. It keeps the record of finished work on board until it is
heard again.

**A task leaves a node only on evidence.** When a node holding a task goes
quiet, the coordinator cannot tell a dead node from a working node behind a
dead link, and across light-minutes the second is the ordinary case.
Reassigning on a timeout would routinely send two vehicles after the same
piece of debris. So no timeout reassigns work. A task returns to the queue
only when the node itself says so, and a task held by a silent node is
reported as in doubt and left where it is.

## Repo Structure

```
starlifter-os/
├── include/starlifter/
│   ├── otf1.hpp            # OTF-1 packets, encode(), decode()
│   ├── node.hpp            # fleet agent (runs on the vehicle)
│   ├── local_control.hpp   # the seam to per-vehicle control
│   ├── coordinator.hpp     # fleet state and scheduler
│   ├── sim.hpp             # simulated pods, links and clock
│   ├── json.hpp            # minimal strict JSON, used only by otf1
│   └── time.hpp            # millisecond UTC time and ISO-8601
├── src/                    # implementations of the above
├── sim/main.cpp            # starlifter_fleet_sim: the demo scenario
├── tests/                  # starlifter_tests, plain checks, no framework
├── protocol/               # pinned copy of the OTF-1 schema and fixtures
├── tools/check_wire_log.py # validates a wire log against the schema
├── docs/
│   ├── architecture.md             # the system sketch and its principles
│   ├── fleet-coordination.md       # how the built slice behaves, rule by rule
│   └── ai-advisory-layer-draft.md  # Tier 0 / Tier 1 decision layer (not built)
└── CMakeLists.txt
```

## Relationship to Other Repos

Starlifter OS sits *above* per-vehicle control, not in place of it:

- **[`power-and-propulsion`](https://github.com/Overby-Industries/power-and-propulsion)**
  already has a working flight-phase finite-state machine (`ShuttleFSM`)
  for a single vehicle. Starlifter OS doesn't replace that -- it's the
  layer that coordinates *many* vehicles each running something like it.
  `LocalControl` is the interface a `ShuttleFSM` adapter would implement.
  The simulation uses a stand-in pod instead; the adapter is not written.
- **[`telemetry-protocol`](https://github.com/Overby-Industries/telemetry-protocol)**
  (OTF-1) is the wire format. This slice needed OTF-1 to carry more than
  mission events, so the format gained four fleet packet kinds: `status`,
  `heartbeat`, `assignment` and `ack`. They are defined there, not here.
- **[`overby-transparency-protocol`](https://github.com/Overby-Industries/overby-transparency-protocol)**
  is the tamper-evident ledger an OTF-1 event can be appended to. The
  coordinator forwards every completed-work event verbatim on a feed for
  exactly that kind of consumer; nothing publishes the feed yet.
- **[`miner-automation`](https://github.com/Overby-Industries/miner-automation)**
  and **[`satnav-swarm`](https://github.com/Overby-Industries/satnav-swarm)**
  are examples of the kind of node this OS would need to coordinate.

## Draft Architecture

See [`docs/architecture.md`](docs/architecture.md) for the system sketch:
independently-operating nodes, each running local closed-loop control,
reporting to and taking assignments from a fleet coordinator, with OTF-1 as
the wire format between them. It now records which parts of that sketch the
first slice implements.

See [`docs/ai-advisory-layer-draft.md`](docs/ai-advisory-layer-draft.md)
for the AI/human decision layer above the coordinator, split into two
tiers: **Tier 0** is pre-approved, deterministic policy that fires
instantly on time-critical physical threats (incoming debris, etc.) with
no AI or human in that loop -- deliberation would itself be the hazard.
**Tier 1** is slower multi-agent deliberation with human approval, for
non-urgent decisions where taking a few minutes costs nothing. Neither
tier ever sits in a pod's own safety-critical fault path. Neither is built.

## Not Built Yet

- **A real transport.** The simulator moves packets in-process. No radio, no
  sockets, no delay-tolerant networking.
- **Authentication.** A node acts on any packet that names its configured
  coordinator. OTF-1 has no signature field.
- **Coordinator persistence and failover.** State is in memory. A restarted
  coordinator relearns the fleet from the next round of status packets but
  has forgotten every task it issued.
- **Releasing an in-doubt task.** Deciding that a silent node's task should
  go to another node anyway is a judgement about risk. That belongs to the
  advisory layer and a human, and there is no cancel message to back it.
- **Tier 0 and Tier 1.** The decision layer above the coordinator.
- **Recovering a safed vehicle.** Safe mode is sticky, as in `ShuttleFSM`.
  Recovery is ground-commanded and has no message here.
- **Guaranteed delivery of events.** A node holds its events through an
  outage it can detect. An event sent into a link that has failed without
  the node noticing is lost from the feed; the task's outcome still reaches
  the coordinator through the node's status.
- **A `ShuttleFSM` adapter** behind `LocalControl`.
- **Scheduling beyond first-come.** No priorities, deadlines, per-node
  queues, or accounting for propellant and power.

## Open Questions

The first slice answers none of these. It runs one in-memory coordinator
because that is the least it could run, not because that is the decision.

- Centralized coordinator vs. fully decentralized/swarm consensus -- which
  fits a fleet with communication delays (Earth-Moon, Earth-belt) better?
- What's the failure mode when a node loses contact with the coordinator
  for an extended period? The slice gives a node enough autonomy to finish
  what it holds and wait. How much fleet-level logic needs to be duplicated
  locally beyond that is still open.
- Real-time constraints: which fleet functions need hard deadlines
  (collision avoidance) vs. soft ones (task scheduling)?
- How closely must clocks agree? The coordinator's conclusion that an offer
  was never taken rests on comparing its clock with a node's. The slice
  allows one second of disagreement and does not measure it.

These need real design work, not just being decided here.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md).
Distributed-systems and real-time/embedded folks are especially useful for
the open questions above.

## License

[Apache 2.0](LICENSE).
