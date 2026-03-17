---
description: "Use when reading, writing, or debugging the phone call state machine: hook events, DTMF collection, sequence dispatch, Goertzel mute logic, or off-hook timeout."
applyTo: "src/main.ino,src/sequence_processor.cpp,src/dtmf_goertzel.cpp,include/sequence_processor.h,include/dtmf_goertzel.h,include/phone_service.h"
---
# Bowie Phone — State Machine Rules

## Lifecycle (happy path)

```
ON_HOOK → (lift) → DIAL_TONE → (1st digit) → COLLECTING → (match/term) →
  RINGBACK → (duration expires) → PLAYBACK → (audio ends) → LOCKED → (hangup) → ON_HOOK
```

Hook callback (`main.ino`):
- **Off-hook**: `playAudioKey("dialtone")`
- **On-hook**: `audioPlayer.stop()` + `resetDTMFSequence()`

## Invariants

1. **Dialtone never resumes mid-call.** Once the first digit stops the dialtone, only hanging up and lifting again restarts it.
2. **`sequenceLocked` blocks all digit input.** Set after any sequence dispatches audio (known key *or* wrong_number). Only `resetDTMFSequence()` (on-hook) clears it.
3. **Goertzel is muted whenever non-dialtone audio plays.** The ES8388 DAC→ADC loopback would cause false detections otherwise.
4. **Goertzel stays muted while `sequenceLocked`.** Even when audio finishes, Goertzel remains muted until hangup.
5. **Non-dialtone playback gets early `return` in `loop()`.** Audio copy runs every iteration; nothing else competes for CPU during playback.

## Goertzel Mute Decision

```cpp
if (audioPlayer.isActive()) {
    mute = !playingDialtone;   // mute during any non-dialtone audio
} else {
    mute = isSequenceLocked(); // mute after sequence until hangup
}
```

Only two things unmute Goertzel: playing dialtone, or idle+unlocked (ready for digits).

## Sequence Processor State Variables

| Variable | Type | Reset by |
|---|---|---|
| `dtmfSequence[]` | char array | `resetDTMFSequence()` |
| `sequenceIndex` | int | `resetDTMFSequence()` |
| `sequenceReady` | bool | `readDTMFSequence()` processing, `resetDTMFSequence()` |
| `sequenceLocked` | bool | `resetDTMFSequence()` only |
| `lastDigitTime` | unsigned long | `addDigitToSequence()` |

## Dispatch Rules

- `isSpecialCommand()` → `processSpecialCommand()` — does NOT set `sequenceLocked` (allows further dialling after command runs).
- Registry match → ringback (2–5 rings, configurable) → `queueAudioKey(sequence)` → click (auto via `onStreamEnd()`). Sets `sequenceLocked = true`.
- Unknown sequence → `processUnknownSequence()` plays `"wrong_number"` — sets `sequenceLocked = true`.

### Ringback chain

`playAudioKey("ringback", rings × 6000ms)` → `queueAudioKey(sequence)` → `onStreamEnd()` auto-plays `"click"`.
Ring count: `random(RINGBACK_MIN_RINGS, RINGBACK_MAX_RINGS + 1)` (default 2–5). Set `RINGBACK_MIN_RINGS 0` to disable.

## Off-Hook Timeout

After `OFF_HOOK_TIMEOUT_MS` (30 s) of inactivity, `"off_hook"` warning tone plays. Repeats every timeout period until a digit is pressed or phone hangs up. Inactivity = `max(lastDigitTime, audioPlayer.getLastActive())`.

## Suffix Matching

`addDigitToSequence()` checks every suffix of the buffer against `AudioKeyRegistry` after each digit. Example: buffer `"9911"` checks `"9911"`, `"911"`, `"11"`, `"1"`. First match wins; buffer is rewritten to the matched substring.

## Notifications

- `DTMFDetected` — green LED pulse per digit.
- `ReadingSequence` — red LED solid while digits are buffered; cleared on sequence processing or hangup.

## Threading

- Goertzel task: core 0. Writes `volatile char goertzelPendingKey`.
- Main loop: core 1. Reads via `getGoertzelKey()` (atomic read-and-clear).
- `goertzelMuted`: `volatile bool`, written core 1, read core 0.
- No mutex needed — single-char and single-bool operations are atomic on ESP32.

## Detailed Documentation

See [docs/system/PHONE_STATE_MACHINE.md](../../docs/system/PHONE_STATE_MACHINE.md) for the full
state diagram, subsystem responsibilities, Goertzel mute truth table, and
sequence dispatch flow.

## Common Pitfalls

- Do not play dialtone except in the hook callback. The state machine assumes dialtone only runs at off-hook start.
- Always pair audio playback in `processNumberSequence()` with `audioStarted = true` so the lock engages.
- `processSpecialCommand()` intentionally leaves the lock open; special commands are metadata operations, not calls.
- `availableForWrite()` on AudioBoardStream (I2S ESP32 V1 driver) returns a **constant** (`I2S_BUFFER_COUNT * I2S_BUFFER_SIZE = 3072`). Do not use it as a dynamic buffer health metric.
