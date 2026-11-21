# Dynamic Notch Audio Controls Fix Plan

## Problem Analysis

The volume and microphone controls in the Dynamic Notch expanded view have multiple issues:

1. **Dropdowns don't open** - Select components for device selection don't show dropdown
2. **Sliders don't move/drag** - Volume/mic sliders appear stuck at 50%
3. **Icons don't update** - Mute buttons show wrong state
4. **No rebuild on audio changes** - UI doesn't refresh when PipeWire devices change

## Root Causes Identified

### 1. Initial Audio State Empty at Init Time
- `initNotchScene` runs when surface is first configured (early startup)
- PipeWireService hasn't connected and enumerated sinks/sources yet
- `m_audio->state().sinks` and `.sources` are empty vectors
- Select dropdowns created with empty options
- Sliders initialized to hardcoded 50% instead of actual volume

### 2. Wrong Sink/Source Index Used
- Code uses `state.sinks[0]` and `state.sources[0]` (first device)
- Should use `state.defaultSinkId` / `state.defaultSourceId` to find actual default device
- Volume, mute state, and device picker selection all affected

### 3. SelectDropdownPopup Context Setup
- Added in previous fix but timing may be wrong
- Need to verify popup context propagates before Select children are created

### 4. No Change Callback for Audio State
- PipeWireService has `setChangeCallback` but Bar doesn't use it
- Notch scene built once, never rebuilt when devices appear/disappear/default changes

### 5. Input Region Covers Only Pill Bounds
- Input region tracks animated pill correctly
- But Select dropdown opens separate popup surface - shouldn't be affected

## Implementation Plan

### Task 1: Fix Sink/Source Lookup by Default ID
**File**: `src/shell/bar/bar.cpp`
- Create helper lambdas in `prepareNotchFrame` and `buildNotchBentoLayout`:
  ```cpp
  auto findDefaultSink = [&](const AudioState& state) -> const AudioNode* {
    for (const auto& s : state.sinks) if (s.id == state.defaultSinkId) return &s;
    return state.sinks.empty() ? nullptr : &state.sinks[0];
  };
  auto findDefaultSource = [&](const AudioState& state) -> const AudioNode* { ... };
  ```
- Replace all `state.sinks[0]` and `state.sources[0]` with these helpers
- Apply to: mute buttons, volume labels, sliders, device picker selection

### Task 2: Defer Device Picker Population Until Audio Ready
**File**: `src/shell/bar/bar.cpp`
- In `buildNotchBentoLayout`: Only populate Select options if `!state.sinks.empty()`
- If empty, set placeholder text indicating "No devices found" or "Connecting..."
- Add `rebuildNotchDevicePickers()` function called from `prepareNotchFrame` when audio state changes

### Task 3: Add PipeWire Change Callback to Rebuild Notch
**File**: `src/shell/bar/bar.cpp`
- In `initNotchScene` (after `instance.m_audio = m_audio;`):
  ```cpp
  if (instance.m_audio) {
    instance.m_audio->setChangeCallback([this, &instance]() {
      if (instance.isNotchMode && instance.surface) {
        // Rebuild only the device pickers, not entire scene
        instance.surface->requestLayout();
        instance.surface->requestRedraw();
      }
    });
  }
  ```
- Create `updateNotchDevicePickers(BarInstance&)` function to refresh Select options and selection
- Call from `prepareNotchFrame` when `m_changeSerial` changes

### Task 4: Initialize Sliders with Actual Volume
**File**: `src/shell/bar/bar.cpp`
- In `buildNotchBentoLayout`: Set slider `.value` from actual audio state if available
- If no sinks/sources yet, default to 50 but mark for update
- In `prepareNotchFrame`: Always sync slider value from current default sink/source

### Task 5: Verify SelectDropdownPopup Setup
**File**: `src/shell/bar/bar.cpp`
- In `initNotchScene`: Ensure popup context is set BEFORE `buildNotchBentoLayout` is called
- Current order looks correct (sceneRoot created → popup context set → buildNotchBentoLayout)
- Add debug logging to verify `popupContext()` returns non-null in Select

### Task 6: Fix Mic Mute Button Logic
**File**: `src/shell/bar/bar.cpp` line 4156-4161
- Current: `state.sources.empty() || state.sources[0].muted`
- Should use `findDefaultSource()` helper

## Validation Steps

1. **Build and run**: `ninja -C build -j4 && ./build/noctalia`
2. **Test volume slider**: Drag slider → system volume should change, label updates
3. **Test mic slider**: Drag slider → mic volume should change, label updates
4. **Test mute buttons**: Click → should toggle mute, icon should change
4. **Test device dropdowns**: Click output/input dropdown → should show device list, selection should change default device
5. **Test hot-plug**: Connect/disconnect Bluetooth headphones → dropdown should update
6. **Check logs**: Look for "Notch:" debug logs and any "SelectDropdownPopup" warnings

## Risk Mitigation

- **Minimal rebuild**: Only rebuild device pickers, not entire Bento layout
- **Fallback handling**: If no default sink found, use first available or disable controls
- **Thread safety**: PipeWire callbacks run on main thread via fd polling, safe to request layout/redraw

## Dependencies

- Requires PipeWireService to be connected and emitting change callbacks
- SelectDropdownPopup must be properly parented to LayerShell surface
- Audio state must have valid `defaultSinkId`/`defaultSourceId` (0 = no default)

## Out of Scope

- Full notch scene rebuild (would cause flicker)
- Audio device routing/sub-device selection (port/profile selection)
- EasyEffects integration