# Dynamic Notch Select Dropdown Fix Plan

## Problem
The Select dropdowns (output/input device pickers) show "no parent surface set" warnings and don't open. Root cause: In `Bar::createInstance()`, the initial `buildScene()` call for notch mode (line 2637) executes **before** `surface->initialize()` (line 2640). The layer surface is created inside `initialize()`, so `instance.surface->layerSurface()` returns `nullptr` when `initNotchScene` creates the `SelectDropdownPopup`.

## Evidence
- Log shows repeated warnings at user interaction time: `21:06:04.713 [WRN] [select-dropdown-popup] no parent surface set`
- `LayerSurface::initialize()` (layer_surface.cpp:107) creates the layer surface via `zwlr_layer_surface_v1_get_layer_surface()`
- `Bar::createInstance()` calls `buildScene()` at line 2637, THEN `initialize()` at line 2640

## Fix Plan

### Task 1: Remove Early buildScene for Notch Mode
**File**: `src/shell/bar/bar.cpp`, lines 2633-2638
- Remove the `if (isNotchMode) { buildScene(...); }` block
- The configure callback (set at line 2612) will fire after `initialize()` and build the scene properly
- The `initialize()` method already commits the surface (layer_surface.cpp:133), so the surface will be mapped and receive configure events

### Task 2: Add Defensive Check in initNotchScene
**File**: `src/shell/bar/bar.cpp`, `initNotchScene` function
- Before creating `SelectDropdownPopup`, verify `instance.surface->layerSurface() != nullptr`
- If null, log a debug message and return early (scene will be built on next configure callback)
- This handles any edge case where the surface isn't ready

### Task 3: Dropdown UX Improvement - Icon-Only Trigger, Labels in Dropdown
**File**: `src/shell/bar/bar.cpp`, `buildNotchBentoLayout` function
- Current: Select shows selected device name as placeholder + dropdown has labels
- Desired: Select trigger shows only an icon (speaker/mic), dropdown shows device labels with checkmarks
- Change Select props:
  - `.placeholder = ""` (empty - no text in trigger)
  - `.glyph = "volume-high"` / `"microphone"` (icon in trigger)
  - `.showSelectedLabel = false` (if supported) or use empty placeholder
  - Dropdown entries already have labels via `DropdownRequest.options`

### Task 4: Ensure Popup Context Propagates Correctly
- Verify the popup context is set on `sceneRoot` BEFORE `buildNotchBentoLayout` is called
- Current order looks correct (lines 4579-4593 before 4775)
- Add assertion/log to confirm `instance.selectDropdownPopup` is valid when Select components are created

## Validation Steps
1. Build and run: `ninja -C build -j4 && ./build/noctalia`
2. Open Dynamic Notch (hover top center)
3. Click speaker icon → dropdown should open with device list
4. Click mic icon → dropdown should open with device list
5. Select different device → default device should change
6. Check logs: No "no parent surface set" warnings
7. Verify slider, mute buttons still work

## Risk Mitigation
- Removing early `buildScene`: Configure callback guarantees scene build after surface ready
- Defensive null check: Prevents crash if surface not ready
- No behavior change for non-notch mode (only notch mode affected)

## Out of Scope
- Full notch scene rebuild optimization
- EasyEffects integration
- Port/profile selection per device