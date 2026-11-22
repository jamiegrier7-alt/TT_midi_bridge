# MIDI Bridge Threading Plan

## Goal

Separate timing-critical MIDI processing from slower hardware and UI operations so Raspberry Pi USB serial, I2C, SPI, GPIO, OLED, or debug activity cannot delay MIDI events.

## Priorities

1. MIDI thread
2. Sensor thread
3. GPIO thread
4. MacroPad thread

## Thread Responsibilities

### MIDI Thread

- Process incoming ALSA sequencer events.
- Send outgoing queued MIDI events to ALSA.
- Perform no I2C, SPI, GPIO, serial, OLED, or blocking display operations.
- Use the shortest possible queue operations and no file or debug I/O.

### Sensor Thread

- Poll the Sweet Sixteen faders over I2C.
- Poll the Terminal Tedium MCP3208 channels over SPI.
- Apply dead-zone filtering.
- Publish only changed sensor values to the MIDI thread.
- Receive a fixed polling interval and use a monotonic clock.

### Terminal Tedium GPIO Thread

- Own the libgpiod button and gate requests.
- Prefer libgpiod edge events where practical; otherwise poll at a fixed short interval.
- Detect rising and falling edges.
- Publish timestamped button/gate events instead of raw hardware state.
- Keep gate input polling because it will support future features.
- Keep gate output writes and startup LED health flashing isolated from the MIDI thread.

### MacroPad Thread

- Own USB serial connection, reads, writes, and reconnect behavior.
- Handle HID/uinput actions and OLED/FUDI updates.
- Preserve the 8 ms serial pacing needed by the RP2040.
- Forward serial line events to a small application event queue.
- Never block the MIDI thread.

## Shared State

- **Sensor snapshot:** latest I2C fader and SPI pot values, atomically or through a short mutex-protected section.
- **MIDI queue:** bounded queue of outgoing MIDI events; drop or coalesce only non-critical sensor updates, never control/gate action timing.
- **GPIO event queue:** timestamped button and gate edges.
- **MacroPad state:** mode and system-control prompt state protected from cross-thread access.
- **Shutdown state:** atomic stop flag shared by all threads.

No thread should hold a mutex while performing hardware I/O.

## Implementation Phases

### 1. Baseline and Safety

- Create a dedicated Git branch.
- Record startup behavior and normal runtime without `--debug`.
- Record MIDI response, I2C/SPI availability, GPIO edge behavior, and any ALSA XRuns.
- Preserve the current implementation as a rollback point.

### 2. Extract Hardware Operations

- Move fader and pot polling into standalone functions.
- Move Terminal Tedium button/gate polling into standalone functions.
- Move MacroPad serial operations behind a dedicated update function.
- Keep the existing main loop as a temporary supervisor.

### 3. Add the Sensor Thread

- Start a sensor thread with a monotonic polling interval.
- Publish changed fader/pot values to a latest-value snapshot.
- Continue sending MIDI from the existing main loop initially.
- Verify sensor values and timing before moving ALSA sending.

### 4. Add the GPIO Thread

- Move button and gate reads out of the main loop.
- Publish timestamped edge events.
- Route Terminal Tedium events to system-control logic and the MacroPad display.
- Preserve the 1+2 system-control behavior and 1=reboot, 2=shutdown, 3=cancel mapping.

### 5. Add the MacroPad Thread

- Move serial reads and writes to a dedicated thread.
- Keep FUDI display writes serialized to avoid RP2040 buffer races.
- Add reconnect handling without blocking the MIDI thread.
- Restore the active mode display after cancel.

### 6. Add the MIDI Thread

- Move ALSA input and output processing into a dedicated thread.
- Drain GPIO and sensor event queues through the MIDI thread or a small coordinator.
- Remove I2C, SPI, GPIO, USB, and debug I/O from the MIDI timing path.
- Add bounded queues and backpressure handling.

### 7. Real-Time Scheduling

- Add `SCHED_FIFO` or `SCHED_RR` for the MIDI thread only if Pi testing shows it is beneficial.
- Configure rtprio/capabilities safely.
- Avoid raising the priority of sensor or MacroPad threads.
- Monitor latency and scheduler overruns rather than assuming real-time priority improves performance.

## Logging and Debugging

- Normal runtime must remain silent.
- Every console, diagnostic, or debug message must execute only when `--debug` is set.
- Debug logging must never occur on the MIDI timing-critical thread.
- Queue optional diagnostics to a low-priority debug reporter or print outside the hot MIDI path.
- Keep hardware actions visible only as required; normal runtime output should remain silent.

## Validation

### Functional

- 16 fader values remain accurate.
- 6 potentiometer values remain accurate.
- 14-bit and 7-bit MIDI modes continue to work.
- Button 1, 2, and 3 state changes are detected once per edge.
- 1+2 opens the system-control prompt.
- 1 reboots, 2 shuts down, and 3 cancels/restores the mode display.
- Gate input rising edges are retained for future use.
- Both gate outputs continue to operate correctly.

### Timing

- MIDI events continue during slow MacroPad serial writes.
- Sensor reads do not block incoming or outgoing ALSA processing.
- GPIO edge handling remains responsive.
- No queue starvation or unbounded memory growth occurs.
- A clean shutdown joins every thread before releasing ALSA/I2C/SPI/libgpiod resources.

### Failure Handling

- MacroPad disconnection/reconnection does not stop MIDI.
- I2C or SPI failure does not stop ALSA processing.
- libgpiod failure is handled once and reported only in debug mode.
- Thread startup failure returns a clear process result and triggers clean rollback/shutdown.

## Exit Criteria

The threading refactor is complete when MIDI processing is independent of all other hardware threads, normal operation is silent, debug output is conditionally enabled, and all existing hardware behavior passes a live Pi test.
