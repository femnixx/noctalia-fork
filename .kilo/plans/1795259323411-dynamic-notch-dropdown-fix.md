# Dynamic Notch Dropdown Fixes - Plan

## Issues to Fix

### 1. Trigger Shows Text Instead of Icon Only
**Current behavior**: Select trigger displays selected device name (e.g., "Built-in Audio") even when `triggerGlyph` is set.
**Desired behavior**: Trigger shows ONLY the icon (speaker/mic glyph), dropdown shows device names with checkmarks.

**Root cause**: `Select::syncTriggerText()` (line 410-416) always sets trigger label text to `selectedText()` or `placeholder`. It doesn't respect `m_triggerGlyph` for display-only mode.

### 2. Dropdown Doesn't Close on Outside Click
**Current behavior**: Dropdown only closes via Escape key or clicking the dropdown button again. Clicking outside does nothing.
**Desired behavior**: Clicking anywhere outside the dropdown area should dismiss it.

**Root cause**: Wayland `xdg_popup` with `grab=true` should auto-dismiss on outside click, but this may not be working due to:
- Parent surface configuration issues
- The `pointerParentSurface` not being set correctly for the grab
- The popup not receiving dismiss events from compositor

### 3. Dropdown Selection Might Not Work Properly
Need to verify the selection callback properly updates the default device and UI state.

## Implementation Plan

### Task 1: Fix Trigger Display - Icon Only Mode
**File**: `lib/ui/controls/select.cpp`
- Modify `syncTriggerText()` to show empty text when `m_triggerGlyph` is set (or add a flag for "icon-only" mode)
- Alternative: Add a `setShowSelectedText(bool)` method to control this behavior

### Task 2: Fix Dropdown Dismiss on Outside Click
**Files**: 
- `lib/ui/controls/context_menu_popup.cpp` - Verify grab configuration
- `lib/ui/controls/select_dropdown_popup.cpp` - Ensure proper parent surface setup

The key is ensuring the Wayland compositor properly manages the popup grab. The `ContextMenuPopup::open()` sets:
- `grab = true` (line 101) - should auto-dismiss on outside click
- `pointerParentSurface = request.pointerParentSurface` (line 217) - for pointer capture translation

Check if the parent surface is correctly set in `SelectDropdownPopup::openSelectDropdown()` (line 102-108 in select_dropdown_popup.cpp).

### Task 3: Verify Selection Works
**File**: `src/shell/bar/bar.cpp` - Verify the `onSelectionChanged` callback properly:
1. Updates PipeWire default sink/source
2. Triggers UI refresh via `requestLayout()`/`requestRedraw()`
3. Updates the Select's selected index visually

## Validation Steps

1. **Build and run**: `ninja -C build -j4 && ./build/noctalia`
2. **Test trigger display**: Open notch → verify speaker/mic icons show WITHOUT text
3. **Test dropdown open**: Click speaker icon → dropdown opens with device list
4. **Test selection**: Click different device → default changes, checkmark moves
5. **Test dismiss**: Click outside dropdown → dropdown closes
6. **Test Escape**: Press Escape → dropdown closes
7. **Test mic dropdown**: Same tests for microphone
8. **Check logs**: No "no parent surface set" warnings

## Risk Mitigation

- **Minimal changes**: Only modify `syncTriggerText()` and verify popup parent setup
- **Preserve existing behavior**: Other Select uses (if any) should not be affected
- **Test both dropdowns**: Volume (output) and Mic (input) device pickers

## Out of Scope

- Full context menu redesign
- Keyboard navigation improvements
- Search/filter in dropdown