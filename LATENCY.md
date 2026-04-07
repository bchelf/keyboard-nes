# Latency Notes

## Original bug

The adapter was treating the PIO TX FIFO like an event queue:

- USB HID callbacks pushed a fresh 8-bit NES snapshot every time a report arrived.
- The main loop also pushed heartbeat snapshots every 16 ms.
- The PIO program only consumes one word per NES latch.

That mismatch lets historical snapshots pile up in the TX FIFO. When that happens,
the next NES latch can read an old state instead of the latest keyboard state.
Short press/release pairs can also disappear entirely if both transitions happen
between two NES polls.

## What changed

- The firmware now keeps one authoritative `current_nes_state`.
- Keyboard reports update that state immediately.
- PIO handoff is now "latest value only":
  - if a state word is already queued for the next latch, it is discarded
  - the newest `current_nes_state` replaces it
- The main loop keeps exactly one pending state word primed for the next latch,
  instead of continuously queueing heartbeats.
- Very short taps are stretched to a minimum visible window of 20 ms so a
  keyboard tap can survive at least one NES poll.

## Why this reduces lag

- There is no FIFO backlog of stale controller states.
- A key change replaces the pending state immediately instead of waiting behind
  older snapshots.
- The NES always consumes either the latest queued state or a freshly primed copy
  of the current state.
- Short taps that were previously recognized by USB but ended before the next
  latch now remain visible long enough for the console to sample them.

## Instrumentation

The firmware now records:

- HID report timestamps
- exposed NES state change timestamps
- PIO publish timestamps
- latch timestamps
- maximum observed TX FIFO depth
- number of overwritten stale TX words
- per-button USB press edges
- per-button press edges that were actually visible on NES latch boundaries

Set `NES_TRACE_SELECT` to `1` in `src/usb_hid.c` for a concise timestamp trace
focused on Select.
