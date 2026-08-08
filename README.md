# Starlifter OS

**Overby Industries — distributed operating system for fleets.**

Coordinates a fleet of otherwise-independent vehicles and pods (Starlifters,
miner/refinery pods, satellites) that each run their own local control
software. This is the fleet-level layer, not the per-vehicle one.

## Status: early design

Nothing is built yet. This repo exists to hold the design work as it
happens, instead of starting from a blank page once implementation begins.

## Relationship to Other Repos

Starlifter OS sits *above* per-vehicle control, not in place of it:

- **[`power-and-propulsion`](https://github.com/Overby-Industries/power-and-propulsion)**
  already has a working flight-phase finite-state machine (`ShuttleFSM`)
  for a single vehicle. Starlifter OS doesn't replace that -- it's the
  layer that coordinates *many* vehicles each running something like it,
  handling task assignment, fleet-wide state, and comms between them.
- **[`telemetry-protocol`](https://github.com/Overby-Industries/telemetry-protocol)**
  (OTF-1) is the wire format fleet nodes would use to report status
  upward. Starlifter OS is a natural producer/consumer of OTF-1 packets,
  not a competing format.
- **[`miner-automation`](https://github.com/Overby-Industries/miner-automation)**
  and **[`satnav-swarm`](https://github.com/Overby-Industries/satnav-swarm)**
  are examples of the kind of node this OS would need to coordinate.

## Draft Architecture

See [`docs/architecture.md`](docs/architecture.md) for the initial sketch:
a distributed system of independently-operating nodes (each running local
closed-loop control, like `ShuttleFSM`) reporting to and taking assignments
from a fleet coordinator, with OTF-1 as the wire format between them.

## Open Questions

- Centralized coordinator vs. fully decentralized/swarm consensus -- which
  fits a fleet with communication delays (Earth-Moon, Earth-belt) better?
- What's the failure mode when a node loses contact with the coordinator
  for an extended period -- does it need enough local autonomy to keep
  operating (almost certainly yes, given light-delay), and if so, how much
  fleet-level logic needs to be duplicated locally vs. centralized?
- Real-time constraints: which fleet functions need hard deadlines
  (collision avoidance) vs. soft ones (task scheduling)?

These need real design work before implementation starts, not just being
decided here.

## Contributing

See [`CONTRIBUTING.md`](CONTRIBUTING.md) and [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md).
Distributed-systems and real-time/embedded folks are especially useful for
the open questions above.

## License

[Apache 2.0](LICENSE).
