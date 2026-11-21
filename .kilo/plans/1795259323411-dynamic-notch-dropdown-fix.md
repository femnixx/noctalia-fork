# Dynamic Notch Dropdown Fix Plan - v2

## Issues
1. **Dropdown box persists after notch collapse** - When dynamic notch animates to collapsed state, the dropdown popup remains visible (orphaned)
2. **Dropdown doesn't dismiss on outside click** - Clicking outside dropdown area doesn't close it; only ESC or re-clicking trigger works
3. **Dropdown items not clickable** - Selection doesn't work

## Root Cause Analysis

### Issue 1: Orphaned Dropdown on Notch Collapse
The dropdown (`SelectDropdownPopup` → `ContextMenuPopup`) is a Wayland popup surface parented to the notch's layer surface. When `animateNotchExpand(instance, false)` runs:
- The notch surface shrinks via animation
- The dropdown popup is NOT closed - it stays attached to the now-smaller parent
- Result: dropdown box floats in wrong position, detached from trigger

**Fix needed**: Close dropdown when notch collapse animation starts.

### Issue 2: No Dismiss on Outside Click
The `ContextMenuPopup` opens with `grab=true` (line 101), which should auto-dismiss on outside click via compositor. However:
- Layer shell surfaces with `Overlay` layer may not properly support `grab` popups
- The `inputSerial` is passed but may not be valid for the grab
- The `pointerParentSurface` is set to the notch's `wlSurface`, but the grab may require the layer surface

**Fix needed**: 
1. Ensure proper parent surface for grab (layer surface, not wl surface)
2. Handle dismiss manually if compositor doesn't auto-dismiss

### Issue 3: Dropdown Items Not Clickable
Could be related to:
- Grab capturing all pointer events
- Input dispatcher not properly routing to menu items
- The dropdown being parented to a surface that's being resized

## Implementation Plan

### Task 1: Close Dropdown on Notch Collapse Start
**File**: `src/shell/bar/bar.cpp` - `animateNotchExpand()`
- At start of collapse animation (`expand == false`), close any open dropdown
- Access via `instance.selectDropdownPopup` if available

```cpp
void Bar::animateNotchExpand(BarInstance& instance, bool expand) {
  if (!expand && instance.selectDropdownPopup) {
    instance.selectDropdownPopup->closeSelectDropdown();
  }
  // ... rest of function
}
```

### Task 2: Fix Parent Surface for Grab
**File**: `src/shell/bar/bar.cpp` - `initNotchScene()`
- Pass `layerSurface()` instead of `wlSurface()` for the popup parent
- The grab needs the xdg surface (layer surface) not the raw wl surface

```cpp
instance.selectDropdownPopup->setParent(
    LayerShell::popupAttach(instance.surface->layerSurface()),
    instance.surface->wlSurface(),  // keep for pointer capture translation
    instance.output
);
```
Wait - the current code already passes both. Let me check `ContextMenuPopup::open()`:
- `parent.xdgSurface` = layer surface (for grab)
- `pointerParentSurface` = wl surface (for pointer capture translation)

This looks correct. The issue may be that the layer surface is not configured yet or the grab serial is wrong.

### Task 3: Manual Dismiss on Outside Click
**File**: `lib/ui/controls/context_menu_popup.cpp` - `onPointerEvent()`
- When `Button` event on parent surface (not popup) and not captured → close popup
- Currently line 340-352 only handles this when `captured` is true

Add explicit handling:
```cpp
case PointerEvent::Type::Button:
  if (!onPopup && !captured && event.pressed) {
    // Click outside popup - dismiss
    deferClose();
    return true;
  }
  // ... existing handling
```

### Task 4: Fix Dropdown Items Clickable
The items should be clickable via `ContextMenuControl`. The `onActivate` callback is set in `SelectDropdownPopup::openSelectDropdown()` (line 70-74). This should work unless the grab interferes.

May need to ensure the popup's input dispatcher properly routes events to the menu control.

## Validation Steps
1. Build: `ninja -C build -j4`
2. Run: `./build/noctalia`
3. Open notch (hover top center)
4. Click speaker icon → dropdown opens
5. Click dropdown item → selection changes, dropdown closes
6. Click outside dropdown → dropdown closes
7. Wait for auto-collapse (or move mouse away) → notch collapses, NO orphaned dropdown
8. Repeat for mic dropdown

## Risk Mitigation
- Close dropdown in `animateNotchExpand` BEFORE animation starts
- Test both expand and collapse animations
- Verify keyboard navigation (ESC) still works
- Check other Select uses aren't affected

## Out of Scope
- Full context menu redesign
- Animating dropdown with notch (complex, low priority)