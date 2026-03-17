# Phone State Machine

The Bowie Phone firmware models a traditional telephone call flow as a
cooperative state machine spread across three subsystems: the **PhoneService**
(hook detection), the **Goertzel DTMF decoder** (digit recognition), and the
**Sequence Processor** (digit accumulation → dispatch).

---

## State Diagram

```
                     ┌──────────────────────┐
                     │       ON_HOOK         │
                     │  (idle / ringing)     │
                     └──────────┬───────────┘
                            lift│handset
                     ┌──────────▼───────────┐
                     │      DIAL_TONE        │
                     │  dialtone generator   │
                     │  Goertzel UNMUTED     │
                     └──────────┬───────────┘
                          first │digit
                     ┌──────────▼───────────┐
                     │   COLLECTING_DIGITS   │
                     │  dialtone stopped     │◄──────────────┐
                     │  suffix matching      │               │
                     │  Goertzel UNMUTED     │   no match &  │
                     └──┬───────┬───────┬───┘   buffer not   │
              match     │   */#│term  │full    full          │
          ┌─────────────┘       │       └────────────────────┘
          │         ┌───────────┘
          ▼         ▼
   ┌──────────────────────┐           ┌──────────────────────┐
   │  PROCESSING_SEQUENCE │──special──▶│   SPECIAL_COMMAND    │
   │  readDTMFSequence()  │  command   │  (no audio lock)     │
   └──────────┬───────────┘           └──────────────────────┘
              │
      ┌───────┴────────┐
      │known key       │unknown
      ▼                ▼
   ┌────────────┐  ┌──────────────┐
   │  PLAYBACK  │  │ WRONG_NUMBER │
   │  locked    │  │  locked      │
   │  Goertzel  │  │  Goertzel    │
   │  MUTED     │  │  MUTED       │
   └─────┬──────┘  └──────┬───────┘
         │audio           │audio
         │ends            │ends
         ▼                ▼
   ┌────────────────────────────┐
   │         LOCKED             │
   │  awaiting hangup           │
   │  Goertzel MUTED            │
   │  off_hook every 30 s       │
   └─────────────┬──────────────┘
         │hang up
         ▼
   ┌──────────────────────┐
   │       ON_HOOK         │
   │  resetDTMFSequence()  │
   │  audioPlayer.stop()   │
   └───────────────────────┘
```

## Off-Hook Timeout

If the handset stays off-hook with no digit presses and no audio playback for
`OFF_HOOK_TIMEOUT_MS` (30 s), an `"off_hook"` warning tone plays. The warning
repeats every ~30 s until the user dials a digit or hangs up.

```
  DIAL_TONE / IDLE_OFFHOOK
       │
       │ 30 s inactivity
       ▼
  WARNING_TONE ──▶ (finishes) ──▶ wait 30 s ──▶ WARNING_TONE ...
       │
       │ digit pressed
       ▼
  COLLECTING_DIGITS (timeout resets)
```

Inactivity is measured as `max(lastDigitTime, audioPlayer.getLastActive())`.

---

## Subsystem Responsibilities

### PhoneService (`phone_service.h`)

| Method | Role |
|---|---|
| `Phone.begin()` | Initialise GPIO for hook switch (SHK pin) |
| `Phone.loop()` | Debounced hook-state polling, fires callback on transitions |
| `Phone.isOffHook()` | Current hook state |
| `Phone.setHookCallback(fn)` | Register `void(bool)` callback for hook transitions |

The hook callback in `main.ino`:
- **Off-hook**: plays `"dialtone"` generator via `playAudioKey("dialtone")`.
- **On-hook**: calls `audioPlayer.stop()` + `resetDTMFSequence()`.

### Goertzel DTMF Decoder (`dtmf_goertzel.h`)

Runs on **FreeRTOS core 0** as a dedicated task, leaving core 1 for audio
playback. Reads from the I2S RX (ADC/mic) DMA channel independently of the TX
(DAC/speaker) channel.

| Function | Role |
|---|---|
| `initGoertzelDecoder()` | Register 8 DTMF frequencies, set thresholds |
| `startGoertzelTask()` | Launch task on core 0: `copy()` → `evaluateBlock()` |
| `getGoertzelKey()` | Consume pending detected digit (single `volatile char`) |
| `setGoertzelMuted(bool)` | Suppress detection during non-dialtone playback |
| `isGoertzelMuted()` | Query mute state |
| `resetGoertzelState()` | Clear accumulators and pending key |

**Detection pipeline**: GoertzelStream fires `onGoertzelFrequency()` per
frequency above threshold → magnitudes accumulated in `blockRowMags[4]` /
`blockColMags[4]` → `evaluateBlock()` finds strongest row+col → magnitude
floor check → twist ratio check → consecutive-block debounce →
`goertzelPendingKey` set.

**ES8388 DAC→ADC loopback**: The codec has an internal loopback that feeds
speaker output back into the mic path. Two mitigations:
1. **Goertzel mute** — set when non-dialtone audio is playing.
2. **Magnitude floor** (`minDetectionMagnitude = 40.0`) — real DTMF presses
   produce magnitudes in the hundreds; loopback artifacts are typically 12–24.

### Sequence Processor (`sequence_processor.h`)

| Function | Role |
|---|---|
| `addDtmfDigit(digit)` | Validate digit, check locks, feed to `addDigitToSequence()` |
| `readDTMFSequence()` | If `sequenceReady`, dispatch via `processNumberSequence()` |
| `resetDTMFSequence()` | Clear all state (called on hang-up) |
| `isSequenceLocked()` | True after a matched sequence plays (blocks input until hangup) |
| `isReadingSequence()` | True if ≥ 1 digit in buffer |
| `isSequenceReady()` | True if a complete match or terminator was detected |
| `getLastDigitTime()` | Timestamp of last digit (for off-hook timeout) |

**Suffix matching**: Each new digit triggers a scan of all suffixes of the
current buffer against `AudioKeyRegistry`. Example: after dialling `"9911"`,
the suffixes `"9911"`, `"911"`, `"11"`, `"1"` are checked. A match on `"911"`
causes the buffer to be rewritten to `"911"` and marked ready.

**Terminators**: `*` and `#` immediately complete the current buffer (they are
not appended to the key string).

**Sequence lock**: After `processNumberSequence()` starts audio for a known
key, `sequenceLocked = true`. While locked:
- `addDtmfDigit()` silently drops all incoming digits.
- The main loop mutes Goertzel.
- Only `resetDTMFSequence()` (called on hang-up) clears the lock.

---

## Main Loop State Logic (`loop()`)

```cpp
if (Phone.isOffHook()) {
    // 1. Off-hook timeout check
    // 2. Audio copy + Goertzel mute control
    if (audioPlayer.isActive()) {
        audioPlayer.copy();
        if (NOT playing dialtone) {
            setGoertzelMuted(true);
            return;  // audio has full CPU — skip everything else
        }
    } else {
        setGoertzelMuted(isSequenceLocked());
    }
    // 3. Consume Goertzel key → addDtmfDigit()
    // 4. If sequenceReady → readDTMFSequence()
} else {
    handlePhoneHomeLoop();  // periodic check-in
}

// Rate-limited maintenance (telnet, WiFi, downloads, debug input)
```

Key design points:
- Audio copy runs **every loop iteration** for glitch-free playback.
- Non-dialtone playback triggers an **early return** so nothing else competes
  for CPU.
- Maintenance tasks are rate-limited to every 10 ms during digit collection or
  100 ms otherwise.

---

## Goertzel Mute Truth Table

| audioPlayer active? | Playing dialtone? | sequenceLocked? | Goertzel muted? |
|---|---|---|---|
| Yes | Yes | — | **No** (listening for digits) |
| Yes | No  | — | **Yes** (suppress loopback) |
| No  | —   | Yes | **Yes** (waiting for hangup) |
| No  | —   | No  | **No** (ready for digits) |

---

## LED Notifications

| Event | LED | Behaviour |
|---|---|---|
| `DTMFDetected` | Green | Brief pulse per digit |
| `ReadingSequence` | Red | On while digits are in buffer; off after processing or hangup |
| `WiFiConnected` | — | Managed by wifi_manager |
| `TailscaleConnected` | — | Managed by tailscale_manager |

---

## Thread Safety

The Goertzel task (core 0) and the main loop (core 1) communicate through a
single `volatile char goertzelPendingKey`. The main loop reads it with
`getGoertzelKey()` (read-and-clear). No mutex is needed because:

- Only the Goertzel task writes non-zero values.
- Only the main loop reads and clears.
- A single `char` write/read is atomic on ESP32 (32-bit aligned access).

The `goertzelMuted` flag is a `volatile bool` written from core 1 and read
from core 0 — also atomic.

---

## Sequence Processing Dispatch

```
processNumberSequence(sequence)
  ├─ isSpecialCommand()? → processSpecialCommand()  [audioStarted = false]
  ├─ registry.hasKey()?
  │    ├─ ENABLE_PLAYLIST_FEATURES? → playPlaylist() → fallback playAudioKey()
  │    └─ else → playAudioKey()                      [audioStarted = true]
  └─ else → processUnknownSequence()                 [audioStarted = true]
         └─ plays "wrong_number" audio
```

Both registry-matched keys and unknown sequences set `sequenceLocked = true`.
The dialtone never resumes — only hanging up and picking back up restarts the
call flow. Special commands leave the lock clear, allowing further digit input.
