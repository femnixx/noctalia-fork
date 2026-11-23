# Plan: Replace Custom Dropdown with Built-in Select Control for Notch Device Selectors

## Issue
The current custom dropdown implementation for notch device selectors (speaker/mic) has issues:
1. Dropdown not visible (clipped by contentClip)
2. Complex positioning logic with absolute coordinates
3. Click-outside handling with overlays
4. Real-time icon updates needed

The codebase already has a complete `Select` control with built-in dropdown popup (`SelectDropdownPopup`) that handles all of this automatically.

## Solution: Use Built-in Select Control
Replace the custom dropdown Flex + overlay implementation with the standard `ui::select()` control for both volume and mic device selectors.

## Current Custom Implementation (to be removed)
- `notchVolumeDropdown` / `notchMicDropdown` (Flex with buttons)
- `notchVolumeDropdownOverlay` / `notchMicDropdownOverlay` (InputArea for click-outside)
- Complex positioning in `prepareNotchFrame` using `Node::absolutePosition`
- Manual visibility toggling
- Manual glyph updates on selection

## New Implementation Using Select Control
```cpp
// In the header row where chevron trigger currently is:
instance.notchVolumeSelect = static_cast<Select*>(volumeHeader->addChild(
    ui::select({
        .options = deviceLabels,  // Vector of sink labels
        .selectedIndex = defaultSinkIndex,
        .placeholder = "Select output device",
        .triggerGlyph = "chevron-down",
        .onSelectionChanged = [&](size_t index, string_view label) {
            // Set default sink
            instance.m_audio->setDefaultSink(deviceIds[index]);
            // Update mute button glyph
            updateVolumeMuteButtonGlyph(instance);
        },
        .configure = [](Select& s) { s.setFixedWidth(120 * scale); }
    })
));
```

## Implementation Plan

### Task 1: Add Select Pointers to BarInstance
**File**: `src/shell/bar/bar_instance.h`
- Add `Select* notchVolumeSelect = nullptr;`
- Add `Select* notchMicSelect = nullptr;`

### Task 2: Create Select Controls in initNotchScene
**File**: `src/shell/bar/bar.cpp` (in `initNotchScene` where header row is built)
- Replace chevron trigger button with `ui::select()` for volume
- Replace chevron trigger button with `ui::select()` for mic
- Set options from current audio state
- Set selectedIndex to current default device
- Add onSelectionChanged callback to set default device and update glyph

### Task 3: Add Helper to Populate Select Options
**File**: `src/shell/bar/bar.cpp`
- Create `updateNotchDeviceSelects(BarInstance&)` function
- Called from:
  - Initial population after `buildNotchBentoLayout`
  - PipeWire change callback (when devices change)
  - After device selection (to refresh list if needed)

### Task 4: Update Mute Button Glyph on Selection
**File**: `src/shell/bar/bar.cpp`
- In onSelectionChanged callback, update mute button glyph for new default device
- Also called from `updateNotchMuteButtonGlyphs()` for PipeWire changes

### Task 5: Remove Custom Dropdown Code
**File**: `src/shell/bar/bar.cpp`
- Remove `notchVolumeDropdown`, `notchMicDropdown` creation
- Remove `notchVolumeDropdownOverlay`, `notchMicDropdownOverlay` creation
- Remove positioning logic in `prepareNotchFrame`
- Remove overlay sync logic in `prepareNotchFrame`
- Remove from BarInstance struct

### Task 6: Clean Up BarInstance
**File**: `src/shell/bar/bar_instance.h`
- Remove: `notchVolumeDropdown`, `notchMicDropdown`
- Remove: `notchVolumeDropdownOverlay`, `notchMicDropdownOverlay`
- Remove: `notchVolumeDropdownOpen`, `notchMicDropdownOpen`
- Add: `notchVolumeSelect`, `notchMicSelect`

### Task 7: Fix Notch Collapse When Select Dropdown Open
**File**: `src/shell/bar/bar.cpp`
- In `contentClip` onLeave handler (already has check at line ~4874) - verify it works
- In `Bar::onPointerEvent` global Leave handler (line ~6090-6118) - ADD check for Select dropdown open
- Use `notchVolumeSelect->open()` and `notchMicSelect->open()` to prevent collapse
- Also add `onDropdownStateChange` callback to track dropdown state if needed

### Task 8: Prevent Notch Disappearing After Selection
**File**: `src/shell/bar/bar.cpp`
- After selection in onSelectionChanged, the dropdown closes
- Ensure notch doesn't collapse immediately after selection
- The existing check for `selectOpen` should handle this if the dropdown state is tracked correctly

## Benefits of Using Select Control
1. **Built-in dropdown popup** - Uses `SelectDropdownPopup` (Wayland popup, properly layered)
2. **Proper clipping** - Popup is a separate Wayland surface, not clipped by contentClip
3. **Keyboard navigation** - Arrow keys, Enter, Escape work automatically
4. **Click-outside** - Handled by popup internally
5. **Consistent styling** - Matches other selects in settings/control center
6. **Less code** - ~200 lines removed, ~50 lines added

## Validation
1. Build: `ninja -C build -j4`
2. Run: `./build/noctalia`
3. Open Dynamic Notch expanded view
4. Click speaker select - dropdown should open and be visible
5. Hover over dropdown options - notch should STAY EXPANDED (not collapse)
6. Select different sink - should update default sink and mute button glyph
7. Notch should NOT disappear after selection
8. Click mic select - same behavior
9. Change default device in pavucontrol - select should update
10. Keyboard navigation - arrow keys, Enter, Escape should work

## Risk Mitigation
- Keep all debug logging during transition
- Test with various device counts (0, 1, many)
- Ensure SelectDropdownPopup parent is set correctly (already done for settings window)
- The Select control creates its own popup surface via SelectDropdownPopup

## Open Questions
1. **Select width**: Should it be fixed width or fill available space? The trigger button currently is 28px. The Select trigger will show the selected device name.
2. **Placeholder vs current selection**: Should show current device name when selected, placeholder when none.
3. **Trigger glyph**: Currently chevron-down. Select supports custom trigger glyph.
4. **Max dropdown height**: SelectDropdownPopup may need max height constraint for many devices.