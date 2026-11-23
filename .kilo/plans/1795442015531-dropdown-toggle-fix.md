# Plan: Fix Dynamic Notch Animation and Visual Issues

## Issues Reported
1. **Visual corruption after expand/shrink** - After one expand/collapse cycle, the notch shows expanded content (Bento grid) clipped to collapsed pill size
2. **Animations don't work** - The expand/collapse animations don't run smoothly or at all
3. **Notch disappears after one cycle** - After expand/collapse, the notch can't be expanded again (no pointer events received)

## Root Cause Analysis

### Issue 1: Visual Corruption
- The `contentClip` is created at expanded size (`maxW`, `maxH`) but resized to current pill size each frame
- `notchBentoRoot` (expanded content) is a child of `contentClip` with `clipChildren = true`
- When collapsed, `notchBentoRoot` should be hidden via `setVisible(false)` and `setOpacity(0)`, but the visibility/opacity might not be properly enforced
- The `notchCollapsedContainer` (collapsed content) should be visible when collapsed

### Issue 2: Animations Don't Work
- Frame ticks are only requested during animation (`notchAnimationId != 0`) or when visualizer is playing (line 5562)
- When collapsed and idle, no frame ticks are requested
- The animation system relies on frame callbacks to progress
- Initial frame tick is requested in `initNotchScene` but may not be sustained

### Issue 3: Notch Disappears After One Cycle
- When collapsed, `exclusiveZone` is set to 0 (line 6518)
- A surface with `exclusiveZone = 0` on Overlay layer may not reliably receive pointer events
- The `contentClip` Enter handler triggers expand, but if the surface doesn't receive pointer events, Enter never fires
- The `notchPointerInside` tracking might not reset properly between cycles
- The auto-collapse timer logic might conflict with manual hover expand

## Implementation Plan

### Task 1: Fix Frame Tick Management
**File**: `src/shell/bar/bar.cpp` - `prepareNotchFrame`
- Always request frame ticks when in notch mode (not just during animation)
- The surface needs continuous frame callbacks to receive pointer events and drive animations
- Remove the condition that only requests frame ticks during animation

### Task 2: Fix Exclusive Zone for Collapsed Notch
**File**: `src/shell/bar/bar.cpp` - `animateNotchExpand` completion callback
- Keep `exclusiveZone = barConfig.thickness` even when collapsed (like a regular bar)
- This ensures the surface reserves space and receives pointer events
- The input region (set per-frame to pill bounds) already handles click-through outside the pill

### Task 3: Fix Content Visibility During Animation
**File**: `src/shell/bar/bar.cpp` - `layoutNotchWidgets`
- Ensure `notchBentoRoot` is properly hidden when collapsed (progress < 0.35)
- Ensure `notchCollapsedContainer` is properly shown when collapsed
- Add explicit visibility/opacity enforcement for both states
- The cross-fade thresholds (0.35 and 0.5) should be consistent

### Task 4: Fix Pointer Event Handling for Re-expand
**File**: `src/shell/bar/bar.cpp` - `onPointerEvent` Enter handler
- The Enter handler checks if pointer is over collapsed pill area (line 6123)
- But if `exclusiveZone = 0`, the surface might not get Enter events
- Ensure the surface always gets pointer events by keeping exclusiveZone > 0
- Verify `notchPointerInside` is properly managed

### Task 5: Fix Initial State and Surface Creation
**File**: `src/shell/bar/bar.cpp` - `createInstance` and `initNotchScene`
- The surface is created at expanded size but starts collapsed
- Ensure initial frame tick is properly requested
- Verify the contentClip initial size matches collapsed state for input region

### Task 6: Clean Up Auto-Collapse Timer Logic
**File**: `src/shell/bar/bar.cpp` - `animateNotchExpand` completion
- The auto-collapse timer should only start when fully expanded AND not hovered
- Currently it checks `!instance.notchPointerInside` but this might be stale
- Add proper reset of `notchPointerInside` on collapse completion

## Code Changes

### In `prepareNotchFrame` (line ~5561):
```cpp
// OLD: Only request frame tick during animation or visualizer
if (instance.surface != nullptr && (instance.notchAnimationId != 0 || instance.notchMediaPlaying)) {
    instance.surface->requestFrameTick();
}

// NEW: Always request frame tick in notch mode to keep surface alive and receiving events
if (instance.surface != nullptr) {
    instance.surface->requestFrameTick();
}
```

### In `animateNotchExpand` completion (line ~6518):
```cpp
// OLD: Set exclusiveZone to 0 when collapsed
instance.surface->setExclusiveZone(expand ? static_cast<std::uint32_t>(instance.barConfig.thickness) : 0);

// NEW: Always keep exclusiveZone = thickness for notch mode (like a regular bar)
instance.surface->setExclusiveZone(static_cast<std::uint32_t>(instance.barConfig.thickness));
```

### In `layoutNotchWidgets` (line ~5238-5278):
```cpp
// Add explicit enforcement for collapsed state
if (!collapsed) {
    // ... existing expanded logic ...
    if (instance.notchCollapsedContainer != nullptr) {
        instance.notchCollapsedContainer->setVisible(false);
        instance.notchCollapsedContainer->setOpacity(0.0F);
    }
} else {
    // Collapsed: hide Bento root, show collapsed container
    if (instance.notchBentoRoot != nullptr) {
        instance.notchBentoRoot->setVisible(false);
        instance.notchBentoRoot->setOpacity(0.0F);
    }
    if (instance.notchCollapsedContainer != nullptr) {
        instance.notchCollapsedContainer->setVisible(true);
        instance.notchCollapsedContainer->setOpacity(1.0F);
    }
}
```

### In `animateNotchExpand` completion - reset pointer state (after line ~6504):
```cpp
instance.notchAnimationId = 0;
instance.notchExpanded = expand;
// Reset pointer inside state on collapse completion
if (!expand) {
    instance.notchPointerInside = false;
}
```

### In `createInstance` - ensure initial frame tick:
```cpp
// After surface initialization, request frame tick for notch mode
if (isNotchMode) {
    instance->surface->requestFrameTick();
}
```

## Validation Steps
1. Build passes: `ninja -C build -j4`
2. Run noctalia with Dynamic Notch enabled
3. Hover over notch area - should expand smoothly
4. Move mouse away - should collapse after delay
5. Hover again - should re-expand (not disappear)
6. Repeat expand/collapse multiple times - should work consistently
7. Check visual appearance: collapsed shows album art/visualizer, expanded shows Bento grid
8. No visual corruption (expanded content clipped to collapsed size)
9. Memory usage stays within 60-100MB range
10. CPU/GPU usage near zero when idle (no unnecessary frame ticks when not needed - but frame ticks are needed for pointer events)

## Risk Mitigation
- Frame ticks every frame when notch is visible may increase CPU/GPU slightly
- But this is necessary for Wayland surface to receive pointer events
- The visualizer already keeps frame ticks alive when playing
- Alternative: Use a timer to poll for hover, but Wayland frame callbacks are the proper way
- Test on different compositors (Niri, sway, Hyprland) if possible

## Open Questions
1. Should we optimize frame tick requests to only when surface is mapped/visible?
2. Does the exclusiveZone change affect other bar behaviors (auto-hide, etc.)?
3. Should the auto-collapse timeout be configurable per-user?

## Performance Target
- Binary size: < 100MB (current noctalia target)
- Memory: < 100MB RSS
- CPU: Near 0% when idle, < 5% during animation
- Frame rate: 60-144 FPS smooth animation