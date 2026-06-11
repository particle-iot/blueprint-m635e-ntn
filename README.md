# M635E NTN Blueprint Demo

An always-on satellite (NTN) demonstration blueprint for the Particle M635e platform. This application demonstrates LTE-M ↔ NTN fallback behaviour for permanently powered devices, and doubles as a rich NTN diagnostic tool for evaluating satellite connectivity, antenna placement, and network transitions.

> **Note:** This is intentionally an *always-on* demo. Power management, deep sleep, battery optimisation, and low-power modem operation are excluded from this release to reduce complexity and maximise visibility into system behaviour.

## Features

- **LTE-M operation** with periodic publishing
- **NTN (satellite) operation** with location-assisted acquisition
- **LTE → NTN fallback** when LTE publishes haved failed beyond a configured timeout
- **NTN → LTE recovery** via periodic LTE retry
- **eSIM profile switching** between LTE and NTN profiles
- **Single publish abstraction** that routes to LTE or NTN automatically
- **Verbose UART diagnostics** — signal quality, satellite acquisition, state transitions, publish results, and timing estimates
- **Simulated LTE loss mode** for testing fallback without leaving coverage
- **Hardware watchdog** protection against stuck states
- **Prebuilt NTN-first binary** for immediate experimentation

## Requirements

### Hardware

- Particle **M635e** (M-SoM with NTN support)
- LTE-M antenna
- NTN-capable antenna (see Particle docs for more info)
- USB connection for UART/debug output
- Permanent power supply (this is an always-on application)

### Software

- **Device OS 6.4.2** or later
- eSIM profiles for both LTE and NTN installed on the device (profiles may or may not be activated — the application handles both cases). These are installed when going through setup.particle.io
- Does **not** depend on the Location API

## Quick Start

1. Connect LTE and NTN antennas to the M635e.
2. Go to [blueprints.particle.io/](https://blueprints.particle.io/m635e-ntn/) and select "Use this blueprint" and then "Deploy to my device"
3. Go through the process at setup.particle.io to configure your device, installed the eSIM profiles and finally, this application
4. Open a serial terminal to view UART debug output ("particle serial monitor" from the CLI). As soon as programming is completed, it will begin to search for NTN satellites and connect to the Particle cloud. The default configuration is **NTN-first / NTN-only publishing**, so satellite behaviour is demonstrated immediately.
5. Make sure you have a clear view of the sky (outdoors or by a window) for the device to connect

## Customizing the Demo

1. Download this blueprint from github and open in Particle Workbench
2. Select M-SoM as the device type and then compile and upload the program by plugging in the device to your computer over USB
3. Modify the config.h parameters to update if you are publishing from cellular, satellite or both!

On every boot the application resets to LTE-first mode internally (if enabled), resets all timers and the state machine, enables the watchdog, and reinitialises the modem.

## Antenna Guidance

NTN connectivity is highly sensitive to antenna selection and placement:

- Use an antenna rated for the NTN band with clear sky visibility.
- Place the antenna with an unobstructed view of the sky — away from metal enclosures, windows with coatings, and overhangs.
- Use the UART satellite acquisition and signal-quality output to compare placements empirically — this application is designed as a placement-evaluation tool.

## Configuration

All runtime behaviour is defined in a single top-level configuration file:

| Option | Description | Default |
|---|---|---|
| LTE enable | Enable/disable LTE-M connectivity | varies by example config |
| NTN enable | Enable/disable NTN connectivity | enabled |
| LTE publish interval | How often to publish over LTE | — |
| NTN publish interval | How often to publish over NTN | — |
| LTE failure timeout | No successful LTE publish before switching to NTN | 15 minutes |
| NTN retry duration | NTN time before retrying LTE | — |
| NTN max message size | Payload size limit | 256 bytes |
| NTN rate limit | Minimum publish spacing | 1 message / 30 s |
| Location source | Fixed coordinates, or GPS with fixed fallback | — |
| Vitals on connect | Publish vitals on each new connection | — |
| Vitals rate limit | Vitals publish timing | — |
| Fake LTE loss | Simulate LTE loss after LTE connects (with timeout) | disabled |

All options are documented inline in the config file.

### Example Configurations

**NTN-only (default shipped config)** — demonstrates satellite connectivity immediately:
- LTE disabled, NTN enabled, NTN-first publishing.

**LTE-only** — baseline cellular operation:
- LTE enabled, NTN disabled.

**Hybrid (LTE primary, NTN fallback)** — real-world deployment pattern:
- Both enabled. LTE is used until no successful publish occurs within the failure timeout (default 15 min), then the device switches eSIM profile and attaches to NTN. LTE is retried periodically; on restoration the device returns to LTE.

Use the **fake LTE loss** option to exercise the hybrid fallback lifecycle without leaving LTE coverage.

## How It Works

### Connectivity Lifecycle

1. LTE operation and publish success/failure monitoring
2. LTE loss detection (no successful publish within timeout)
3. NTN activation (eSIM profile switch, location-assisted attach)
4. NTN publishing (size- and rate-limited)
5. Periodic LTE restoration testing
6. Return to LTE

**LTE failure definition:** LTE is considered unavailable when no successful LTE publish occurs within the configured timeout window — even if registration, attach, or partial connectivity succeeds.

### Publish Abstraction

The application exposes a single publish call that routes to the LTE or NTN stack based on the current operating mode:

```cpp
SOMETHING.publish(payload);
```

This layer centralises rate limiting (1 message / 30 s on NTN), payload sizing (256-byte NTN limit), retries, logging, metrics, and publish accounting. Message types include vitals payloads, publishes, and subscribes (all rate-limited on NTN).

### Location Handling

NTN attach requires location. Sources, in order of preference:

1. Hard-coded coordinates (environment variable captured during setup)
2. GPS integrated into the application

Acquired location is cached and reused on each NTN attach attempt, with background refresh where possible.

### Watchdog & Recovery

The application watchdog protects against stuck connection state machines, blocked attaches, blocked location acquisition, profile-switching hangs, and publish loop stalls. Repeated attach failures escalate to a modem reset.

### LED Behaviour

The system LED is overridden to clearly signal NTN acquisition status.

## Debug Output Interpretation

The UART output is designed as a continuous NTN diagnostic environment. Expect to see:

- **Startup banner** — full runtime configuration at boot
- **Operating mode** — current mode (NTN vs LTE) reported continuously
- **Signal metrics** — LTE RSSI/RSRP/SINR and NTN signal quality where available
- **Satellite activity** — acquisition progress and visible satellites
- **State transitions** — connection state machine changes, eSIM profile switches, modem transitions
- **Publish results** — attempts, successes, failure causes, and success/failure counters
- **Timing estimates** — retry windows and time remaining before fallback/recovery transitions
- **Location status** — fixed vs acquired, acquisition progress

The goal is that you can understand what the modem is doing even when an NTN connection is unsuccessful.

## NTN Operational Expectations

- Satellite acquisition can take significantly longer than LTE attach; the timing-estimate log lines show where the device is in the process.
- NTN payloads are limited to **256 bytes** and **1 message per 30 seconds** — the publish layer enforces both.
- A valid location is required before NTN attach.
- Clear sky view is essential; use the diagnostic output to validate placement.

## Skylo Conformance

This application is expected to serve as the demo app for Skylo conformance testing. The primary design goal is correct customer-facing behaviour; a conformance-specific branch may diverge as needed. The shipped configuration undergoes a Skylo compliance review.

## Troubleshooting

| Symptom | Things to check |
|---|---|
| No NTN attach | Sky visibility, antenna selection/placement, valid location available, NTN eSIM profile installed |
| Stuck during attach | Watchdog and modem-reset escalation will recover automatically; check state transition logs for the failure cause |
| Publishes failing on LTE | Expected to trigger NTN fallback after the timeout — check publish failure log lines for cause |
| Device never falls back to NTN | Verify NTN is enabled in config and the LTE failure timeout; use fake LTE loss mode to force the transition |
| No location | Set a fixed location in config, or ensure GPS has sky view |

## License / Support

Refer to Particle documentation and support channels for the M635e and NTN platform.
