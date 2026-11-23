# Plan: Fix Dropdown Toggle Behavior in Dynamic Notch

## Issues Reported
1. **Dropdown doesn't toggle** - Clicking the trigger button (chevron) always opens the dropdown but never closes it when clicked again
2. **Dropdown should close when notch collapses** - Already handled in `animateNotchExpand`, but need to verify

## Root Cause Analysis
The trigger buttons (`notchVolumeDeviceSelector` and `notchMicDeviceSelector`) have `onClick` handlers that always call `showDeviceDropdown()` which opens the dropdown. There's no logic to check if the dropdown is already open and close it instead (toggle behavior).

The `SelectDropdownPopup` has:
- `isSelectDropdownOpen()` - to check if dropdown is open
- `closeSelectDropdown()` - to close the dropdown

But we need to track which selector (volume or mic) currently has the dropdown open, since there's only one `SelectDropdownPopup` instance shared between both selectors.

## Implementation Plan

### Task 1: Add tracking for active dropdown selector
**File**: `src/shell/bar/bar_instance.h`
- Add enum or field to track which selector's dropdown is open: `enum class NotchDropdownTarget { None, Volume, Mic };`
- Add field: `NotchDropdownTarget notchDropdownTarget = NotchDropdownTarget::None;`

### Task 2: Update showDeviceDropdown to track active target
**File**: `src/shell/bar/bar.cpp` - `showDeviceDropdown`
- Set `instance.notchDropdownTarget = isVolume ? NotchDropdownTarget::Volume : NotchDropdownTarget::Mic;` when opening
- Reset to `None` in `onDismiss` callback

### Task 3: Modify trigger button onClick handlers to toggle
**File**: `src/shell/bar/bar.cpp` - where trigger buttons are created (lines ~4102 and ~4259)
- Check if dropdown is open AND the target matches this selector
- If yes, call `closeSelectDropdown()` instead of `showDeviceDropdown()`
- If no, call `showDeviceDropdown()`

### Task 4: Verify notch collapse closes dropdown
**File**: `src/shell/bar/bar.cpp` - `animateNotchExpand` (line ~6431)
- Already has logic to close dropdown on collapse - verify it works with new tracking

## Code Changes

### In bar_instance.h:
```cpp
enum class NotchDropdownTarget { None, Volume, Mic };

struct BarInstance {
  // ... existing fields ...
  NotchDropdownTarget notchDropdownTarget = NotchDropdownTarget::None;
  // ... existing fields ...
};
```

### In showDeviceDropdown:
```cpp
callbacks.onDismiss = [&instance]() {
    kLog.debug("NotchSelect: Dropdown dismissed");
    instance.notchDropdownOpen = false;
    instance.notchDropdownTarget = NotchDropdownTarget::None;
};
callbacks.onOpen = [&instance, isVolume]() {
    kLog.debug("NotchSelect: Dropdown opened");
    instance.notchDropdownOpen = true;
    instance.notchDropdownTarget = isVolume ? NotchDropdownTarget::Volume : NotchDropdownTarget::Mic;
};
```

### In trigger button onClick:
```cpp
.onClick = [&instance, isVolume = true]() {
    if (instance.selectDropdownPopup && instance.selectDropdownPopup->isSelectDropdownOpen() && 
        instance.notchDropdownTarget == (isVolume ? NotchDropdownTarget::Volume : NotchDropdownTarget::Mic)) {
        instance.selectDropdownPopup->closeSelectDropdown();
    } else {
        showDeviceDropdown(instance, isVolume);
    }
}
```

## Validation Steps
1. Build passes
2. Open Dynamic Notch expanded view
3. Click speaker chevron - dropdown opens
4. Click speaker chevron again - dropdown closes (toggle)
5. Click mic chevron - dropdown opens for mic
6. Click mic chevron again - dropdown closes (toggle)
7. Click speaker, then click mic - switches to mic dropdown
8. Move mouse away - notch collapses, dropdown closes
9. Hover again - notch re-expands