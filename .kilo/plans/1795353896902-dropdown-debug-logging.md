# Plan: Fix Dynamic Notch Dropdown Overlay Issues

## Issue
The dropdown selectors (speaker/mic device pickers) in Dynamic Notch expanded view have these problems:
1. **Appears behind other components** - dropdown renders in the flex layout order, behind the slider
2. **Should be on top of every component** - needs high z-index to be selectable
3. **Should update icons in real-time** - when a device is selected, the mute button glyph should update (volume-high vs volume-off, microphone vs microphone-off)

## Root Cause
The dropdown is a child of the card's vertical flex (`notchVolumeCard`/`notchMicCard`). In flex layout:
- Children render in order added (dropdown added before slider → renders behind slider)
- No z-index stacking across flex children by default
- Dropdown participates in layout, pushing slider down when visible

## Current Structure
```
notchVolumeCard (vertical flex)
  ├── volumeHeader (horizontal flex)
  │     ├── muteButton
  │     ├── volumeLabel
  │     ├── deviceNameLabel
  │     └── chevronButton (trigger)
  ├── notchVolumeDropdown (vertical flex) ← participates in layout, z-index 0
  └── volumeSlider (flexGrow=1) ← renders on top
```

## Solution: Make Dropdown an Overlay
Transform the dropdown from a flex child to an absolutely positioned overlay:

1. **Remove from flex layout**: `setParticipatesInLayout(false)`
2. **High z-index**: `setZIndex(100)` to render above all card content
3. **Absolute positioning**: Position relative to trigger button each frame in `updateNotchGeometry`/`prepareNotchFrame`
4. **Real-time icon updates**: Update mute button glyph when default device changes

## Implementation Plan

### Task 1: Fix Dropdown as Overlay (Volume)
**File**: `src/shell/bar/bar.cpp`
- Set `participatesInLayout(false)` on `notchVolumeDropdown`
- Set `setZIndex(100)` on `notchVolumeDropdown`
- In `updateNotchGeometry` or `prepareNotchFrame`, position dropdown absolutely below trigger button
- Calculate position from trigger button's absolute position

### Task 2: Fix Dropdown as Overlay (Mic)
**File**: `src/shell/bar/bar.cpp`
- Same changes for `notchMicDropdown`

### Task 3: Real-time Icon Updates
**File**: `src/shell/bar/bar.cpp`
- When device selection changes (in dropdown item onClick), update the mute button glyph
- Volume mute button: "volume-high" → "volume-off" based on mute state
- Mic mute button: "microphone" → "microphone-off" based on mute state
- Also update when PipeWire change callback fires (external device changes)

### Task 4: Click-outside to Close
**File**: `src/shell/bar/bar.cpp`
- Add click handler on card/scene to close dropdown when clicking outside
- Or use existing input area handling

## Technical Details

### Positioning the Dropdown
```cpp
// In updateNotchGeometry or prepareNotchFrame:
if (instance.notchVolumeDropdown && instance.notchVolumeDropdownOpen) {
    // Get trigger button position in card coordinates
    float triggerX = instance.notchVolumeDeviceSelector->x();
    float triggerY = instance.notchVolumeDeviceSelector->y();
    float triggerW = instance.notchVolumeDeviceSelector->width();
    float triggerH = instance.notchVolumeDeviceSelector->height();
    
    // Position dropdown below trigger, aligned to right edge
    instance.notchVolumeDropdown->setPosition(triggerX + triggerW - dropdownWidth, triggerY + triggerH + gap);
}
```

### Icon Update Logic
```cpp
// In dropdown item onClick:
instance.notchVolumeMuteButton->setGlyph(sink->muted ? "volume-off" : "volume-high");

// In PipeWire change callback:
updateMuteButtonGlyphs(instance);
```

## Validation
1. Build: `ninja -C build -j4`
2. Run: `./build/noctalia`
3. Open Dynamic Notch expanded view
4. Click speaker chevron - dropdown should appear ON TOP of slider, not behind
5. Select a device - dropdown closes, mute button icon updates
6. Click mic chevron - same behavior
7. Change default device externally (pavucontrol) - icons update in real-time
8. Click outside dropdown - closes dropdown

## Risk Mitigation
- Keep dropdown as child of card for coordinate system simplicity
- Use existing `updateNotchGeometry` which runs every frame for positioning
- Minimal changes to existing logic
- Preserve all logging added previously