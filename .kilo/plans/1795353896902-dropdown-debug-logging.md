# Plan: Fix Dynamic Notch Dropdown Issues

## Issues Reported
1. **Dropdown won't dismiss** - Clicking outside or pressing Escape doesn't close the dropdown; only selecting an item closes it
2. **Notch disappears after selection** - After selecting a device, the notch collapses and can't be accessed again
3. **Device labels don't update in real-time** - The speaker/mic labels between dropdown and icon don't update immediately when selection changes
4. **Notch should persist** - After expand/collapse cycles, the notch should remain accessible

## Root Cause Analysis

### Issue 1: Dropdown Won't Dismiss
The `SelectDropdownPopup` is a Wayland popup surface. When clicking outside the popup, the Wayland compositor should dismiss it and call `onDismiss`. However, the popup might not be properly handling outside clicks, or the `onDismiss` callback might not be firing.

### Issue 2: Notch Disappears After Selection
Sequence:
1. User clicks dropdown item → `onSelect` fires → sets default device → calls `updateNotchMuteButtonGlyphs`
2. Dropdown closes → `onDismiss` fires → sets `notchDropdownOpen = false`
3. Pointer is now outside the notch surface (dropdown was a separate Wayland surface)
4. `contentClip` Leave event fires → starts collapse timer
5. Collapse timer fires → `animateNotchExpand(false)` → notch collapses
6. User can't access notch again because it's collapsed

The fix in `animateNotchExpand` closes the dropdown before collapse (line 6402), but the issue is the Leave event happens BEFORE the dropdown closes.

### Issue 3: Labels Don't Update in Real-Time
The `onSelect` callback calls `updateNotchMuteButtonGlyphs()` but doesn't update `notchVolumeSinkLabel` and `notchMicSourceLabel`. These are only updated in `prepareNotchFrame` which runs per-frame, but the notch collapses before the next frame.

### Issue 4: Notch Persistence
The auto-collapse timer (line 6474) starts after expansion completes. If the notch collapses and user tries to hover again, it should re-expand. But there might be a state issue.

## Implementation Plan

### Task 1: Fix Dropdown Dismissal
**File**: `src/shell/bar/bar.cpp` - `showDeviceDropdown`
- Verify `SelectDropdownPopup` properly dismisses on outside click/Escape
- The `onDismiss` callback should fire when clicking outside

### Task 2: Prevent Notch Collapse After Selection
**File**: `src/shell/bar/bar.cpp` - `showDeviceDropdown` `onSelect` callback
- After setting default device, set `instance.notchPointerInside = true` to prevent immediate collapse
- Stop the collapse timer: `instance.notchCollapseTimer.stop()`
- Update device name labels immediately in the callback

### Task 3: Update Device Labels in Real-Time
**File**: `src/shell/bar/bar.cpp` - `showDeviceDropdown` `onSelect` callback
- Call a helper to update `notchVolumeSinkLabel` and `notchMicSourceLabel` immediately after selection
- Reuse the same logic from `prepareNotchFrame`

### Task 4: Ensure Notch Re-expands on Hover
**File**: `src/shell/bar/bar.cpp` - `Bar::onPointerEvent` Enter handler
- Verify hover re-expansion works after collapse
- Check that `notchExpanded` state is properly reset

### Task 5: Build and Test
- Build: `ninja -C build -j4`
- Test all scenarios

## Code Changes

### In `showDeviceDropdown` onSelect callback:
```cpp
callbacks.onSelect = [&instance, isVolume](std::size_t index) {
    kLog.debug("NotchSelect: Dropdown selection index={}, isVolume={}", index, isVolume);
    if (!instance.m_audio) return;
    auto& state = instance.m_audio->state();
    const auto& devices = isVolume ? state.sinks : state.sources;
    if (index < devices.size()) {
      if (isVolume) {
        instance.m_audio->setDefaultSink(devices[index].id);
        kLog.debug("NotchSelect: Set default sink to {}", devices[index].id);
      } else {
        instance.m_audio->setDefaultSource(devices[index].id);
        kLog.debug("NotchSelect: Set default source to {}", devices[index].id);
      }
      // Update mute button glyph for new default device
      updateNotchMuteButtonGlyphs(instance);
      
      // NEW: Update device name labels immediately
      updateNotchDeviceLabels(instance);
      
      // NEW: Prevent notch collapse by stopping timer and marking pointer inside
      instance.notchCollapseTimer.stop();
      instance.notchPointerInside = true;
    }
  };
```

### New helper function `updateNotchDeviceLabels`:
```cpp
void updateNotchDeviceLabels(BarInstance& instance) {
  if (!instance.m_audio) return;
  auto& state = instance.m_audio->state();
  
  if (instance.notchVolumeSinkLabel) {
    const AudioNode* sink = [&state]() -> const AudioNode* {
      for (const auto& s : state.sinks) if (s.id == state.defaultSinkId) return &s;
      return state.sinks.empty() ? nullptr : &state.sinks[0];
    }();
    if (sink) instance.notchVolumeSinkLabel->setText(audioDeviceLabel(*sink));
  }
  
  if (instance.notchMicSourceLabel) {
    const AudioNode* source = [&state]() -> const AudioNode* {
      for (const auto& s : state.sources) if (s.id == state.defaultSourceId) return &s;
      return state.sources.empty() ? nullptr : &state.sources[0];
    }();
    if (source) instance.notchMicSourceLabel->setText(audioDeviceLabel(*source));
  }
}
```

## Validation Steps
1. Build passes
2. Open Dynamic Notch expanded view
3. Click speaker/mic chevron - dropdown opens
4. Click outside dropdown - dropdown closes (doesn't require selection)
5. Select different device - labels update immediately, notch stays expanded
6. Move mouse away - notch collapses after delay
7. Hover again - notch re-expands
8. Repeat - notch remains accessible