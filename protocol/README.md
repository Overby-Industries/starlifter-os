# protocol/

A pinned copy of the OTF-1 wire format this repo is built and tested against.

| Path | What it is |
|---|---|
| `otf-1.schema.json` | The OTF-1 JSON Schema (Draft 2020-12) |
| `examples/` | One valid packet per file, covering every packet kind |
| `invalid/` | One malformed packet per file; each must be rejected |

**This is not the source of truth.** OTF-1 is defined in
[`telemetry-protocol`](https://github.com/Overby-Industries/telemetry-protocol),
and changes to the format are made there. The copy here exists so that this
repo builds and tests on its own, against a known version of the format,
without fetching another repository.

It is used three ways:

- `starlifter_tests` requires the C++ decoder to accept every packet in
  `examples/` unchanged and to reject every packet in `invalid/`.
- `tools/check_wire_log.py` validates everything the fleet simulation puts on
  the wire against `otf-1.schema.json`.
- CI runs both.

## Updating the copy

From a `telemetry-protocol` checkout next to this one:

```bash
cp ../telemetry-protocol/schema/otf-1.schema.json protocol/
cp ../telemetry-protocol/examples/*.json          protocol/examples/
cp ../telemetry-protocol/tests/invalid/*.json     protocol/invalid/
```

Then rebuild and run the tests. A format change that the C++ decoder does not
yet implement shows up as a failing test, which is the point of pinning it.
