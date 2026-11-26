# Music Tab with Lyrics Sync - Implementation Plan

## Current State
- Home tab positioning fix implemented in `layoutNotchWidgets` (lines 4768-4782) but **build failing** due to lyrics service integration errors
- Lyrics service integration partially implemented in `prepareNotchFrame` but has compilation errors
- LyricsService class exists in `src/services/lyrics_service.h/.cpp` with LRCLIB fetching, caching, and parsing

## Issues to Fix

### 1. Build Errors - Lyrics Service Integration (BLOCKER)
**Errors:**
- `invalid use of incomplete type 'class noctalia::services::LyricsService'` - forward declaration visible but class definition not parsed
- `'LyricsResult' is not a member of 'noctalia::services'` - header not being included properly
- Template argument errors in lambda callback

**Root Cause:** Include path or namespace issue with `services/lyrics_service.h` in bar.cpp

### 2. Home Tab "Out of Reach" - Positioning Bug (Phase 1)
**Fix already in source** - tab content panels now positioned dynamically in `layoutNotchWidgets` based on current pill dimensions. Needs build to succeed to verify.

### 3. Media Tab - Lyrics Not Hooked Up (Phase 2)
- Track change detection and lyrics fetch via `m_lyricsService` ✓ (in source)
- Populate `notchLyricsPanel` with Label per line ✓ (in source)
- Highlight active line based on `positionUs` ✓ (in source)
- Auto-scroll to keep active line visible ✓ (in source)
- Handle plain lyrics (no timestamps) with marquee ✓ (in source)

### 4. Polish & Edge Cases (Phase 3)
- Loading state while fetching lyrics ✓ (in source)
- "No lyrics found" state ✓ (in source)
- Instrumental track detection (hide lyrics panel) ✓ (in source)
- Cache works (second play = instant) - needs verification

## Files to Modify
- `src/shell/bar/bar.cpp` - Fix lyrics service include/usage, verify tab positioning fix
- `src/shell/bar/bar_instance.h` - Already has lyrics data members
- `src/services/lyrics_service.h/.cpp` - Already implemented
- `src/app/application_services.cpp` - Already sets cache directory

## Implementation Steps

### Step 1: Fix Build Errors (CRITICAL)
1. Verify `services/lyrics_service.h` include path in build system
2. Check for circular includes between bar.h, bar_instance.h, and lyrics_service.h
3. Fix lambda callback signature in `prepareNotchFrame` (line ~5195)
4. Ensure `noctalia::services::LyricsResult` is visible

### Step 2: Verify Home Tab Positioning Fix
1. Build successfully
2. Test that tab content panels reposition correctly during expand/collapse animation
3. Verify Home tab content visible and reachable when notch expanded
4. Verify all 5 tabs switch content correctly

### Step 3: Complete Lyrics Integration
1. Test lyrics fetch on track change
2. Verify synced lyrics highlight current line
3. Verify auto-scroll keeps active line visible
4. Verify plain lyrics display with marquee
5. Test cache behavior (second play = instant)

### Step 4: Edge Cases & Polish
1. Loading state display
2. No lyrics found handling
3. Instrumental track detection
4. Memory/performance validation

## Validation Checklist
- [ ] Build completes without errors
- [ ] Home tab content visible and reachable when notch expanded
- [ ] All 5 tabs switch content correctly
- [ ] Media tab shows player + lyrics panel
- [ ] Lyrics fetch from LRCLIB works on track change
- [ ] Synced lyrics highlight current line based on playback position
- [ ] Auto-scroll keeps current line visible
- [ ] Cache works (second play = instant)
- [ ] Fallback to plain lyrics works
- [ ] No lyrics state handled gracefully (panel hidden)
- [ ] Instrumental track detection (hide lyrics panel)

## Technical Notes

### Tab Positioning Fix (Already in Source)
In `layoutNotchWidgets` (lines 4768-4782):
```cpp
const float headerH = 36.0F * contentScale;
const float headerY = 12.0F * contentScale;
const float gridY = headerY + headerH + 8.0F * contentScale;  // 56px
const float innerPad = 12.0F * contentScale;
const float tabContentY = gridY;
const float tabContentW = pillW - 2.0F * innerPad;
const float tabContentH = pillH - tabContentY - innerPad;
for (auto* tabContent : instance.notchTabContents) {
  if (tabContent != nullptr) {
    tabContent->setPosition(innerPad, tabContentY);
    tabContent->setSize(tabContentW, std::max(0.0F, tabContentH));
  }
}
```

This runs every frame via `updateNotchGeometry` → `layoutNotchWidgets`, so tab content tracks animated pill dimensions.

### Lyrics Fetch Callback Fix Needed
Current code at line ~5195 has template deduction issues:
```cpp
[instancePtr = &instance, cacheKey = currentLyricsCacheKey, this](std::optional<noctalia::services::LyricsResult> result) {
```

Need to ensure `LyricsResult` is fully defined at this point (include order).

## Dependencies
- LyricsService requires curl/jsoncpp (already in project)
- Cache directory: `$XDG_CACHE_HOME/noctalia/lyrics` or `~/.cache/noctalia/lyrics`
- LRCLIB API: `https://lrclib.net/api/get?artist=...&title=...&duration=...`

## Rollout
1. Fix build → test on development machine
2. Verify all tab content positioning
3. Test lyrics with various track types (synced, plain, instrumental, not found)
4. No config changes required - uses existing notch config