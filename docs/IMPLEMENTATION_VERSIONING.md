# Implementation Versioning

Firmware version identifiers use this format:

```text
v<plan-major>.<plan-minor>-phase<phase>-<build-format>
```

Examples:

```text
v3.0-phase0-arduino
v3.0-phase1-arduino
v3.0-phase2-arduino
v3.0-phase3-arduino
v3.0-phase4-arduino
v3.0-phase5-arduino
v3.0-phase6-arduino
```

Every firmware image must print the identifier during `setup()` using this
stable log field:

```text
implementation version=v3.0-phase6-arduino
```

Use the identifier when naming captured serial logs and test results, for
example:

```text
docs/phase2_arduino_v3.0_serial.log
docs/phase2_arduino_v3.0_results.md
```

The phase identifier changes whenever implementation behavior crosses a plan
phase boundary. A rebuild with no behavior change keeps the same identifier;
the build timestamp and compiler output belong in the result record, not in the
version string.

The current implementation is `v3.0-phase6-arduino`.
