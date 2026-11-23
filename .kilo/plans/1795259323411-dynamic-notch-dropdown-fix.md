# Fix Segmentation Fault on Speaker Toggle Click

## Issue
Segmentation fault occurs when clicking the speaker toggle (mute/unmute) icon in the Dynamic Notch expanded view.

## Root Cause Analysis

The crash occurs in the speaker mute button click handler when calling `setSinkMuted()`. Possible causes:

1. **Null `m_wpMixer` in PipeWireService**: The `m_wpMixer` (WirePlumber mixer) proxy might be null or invalid when `setNodeMuted` tries to call `m_wpMixer->setMuted()`.

2. **Invalid surface operations**: The click handler calls `requestLayout()`, `requestRedraw()`, `requestFrameTick()` on `instance.surface` which might be in an invalid state.

3. **Use-after-free in lambda**: The click handler captures `instance` by reference, which could become dangling if the `BarInstance` is destroyed.

4. **Race condition in PipeWire**: The `setNodeMuted` call triggers `rebuildState()` which fires the change callback, potentially causing reentrancy issues.

## Implementation Plan

### Task 1: Add Null Checks in PipeWireService::setNodeMuted
**File**: `src/pipewire/pipewire_service.cpp`
- Add defensive null checks for `m_wpMixer` before calling `setMuted`
- Add null check for `nd.proxy` before using it
- Add try-catch or error handling for WirePlumber calls

### Task 2: Add Defensive Checks in Click Handler
**File**: `src/shell/bar/bar.cpp` (lines ~4028-4045)
- Add null check for `instance.surface` before calling surface methods
- Use `instance.surface.get()` instead of raw pointer access
- Add try-catch around surface operations

### Task 3: Fix Potential Reentrancy in PipeWire Change Callback
**File**: `src/pipewire/pipewire_service.cpp` and `src/shell/bar/bar.cpp`
- Defer `rebuildState()` and `emitChanged()` to avoid reentrancy
- Use a flag to prevent recursive callbacks

### Task 4: Add Debug Logging for Debugging
**Files**: Both files
- Add debug logs before/after critical operations to help diagnose future crashes

## Validation Plan
1. Build and run: `ninja -C build -j4 && ./build/noctalia`
2. Test speaker mute toggle multiple times
3. Test mic mute toggle
4. Verify no segfault on repeated clicks
5. Run all 206 tests: `ninja -C build -j4 test`

## Risk Mitigation
- Add comprehensive null checks without changing behavior
- Use existing logging infrastructure for debugging
- Keep changes minimal and focused on crash prevention