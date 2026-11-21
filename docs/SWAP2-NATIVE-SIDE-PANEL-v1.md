# Swap2-Native in-game side panel — visual proposal v1

Status: design proposal, 2026-11-16. Application code and the installed APK are unchanged.

Reference: [light and dark mockup](../assets/artwork/swap2-native/side-panel-v1/side-panel-light-dark.png). The phone was inspected through `output/swap2-sidepanel-current.png`, with Mario Kart World running and the existing panel open. The mockup is a visual reference, not a screenshot of an implemented screen.

## Problem found in the current implementation

`SidePanel.java` uses a hard-coded `0xF0202124` surface and `Color.LTGRAY` notes. Its native controls inherit `Theme.Swap2.Game` (`Theme.DeviceDefault.NoActionBar.Fullscreen`) instead of the launcher's Console v6 theme. This explains the gray translucent surface, pale system accent, oversized-looking radio groups and inconsistent styling next to `GameGraphics` rows that already use `Ui.Setting`.

The entire panel currently scrolls as one list, including its title and actions. LSFG's expanded radio groups occupy most of the phone's landscape viewport. This is a presentation issue; retain the existing preferences and renderer callbacks.

## Visual direction

Use the existing Console v6 resources, not a separate palette. Opaque neutral surfaces, high-contrast text, restrained borders, simple vector icons and a small cyan/coral brand detail. The game remains visible under a modest scrim outside the panel. No background blur or continuously animated effects.

| Token | Light | Dark |
| --- | --- | --- |
| `bn_bg` | `#F4F4F2` | `#17191D` |
| `bn_card` | `#FFFFFF` | `#24272D` |
| `bn_tint` | `#EAEAE7` | `#2D3138` |
| `bn_card_stroke` | `#D5D7D8` | `#444A54` |
| `bn_text` | `#15171B` | `#F4F4F2` |
| `bn_text_2` | `#62666D` | `#AEB4BF` |
| Primary button | near-black / white text | off-white / near-black text |

Use `s2_brand_cyan`, `s2_brand_coral`, `bn_accent`, `bn_danger` and `bn_danger_tint` from resources. The image approximates gradients and accents: implementation should use the exact flat resource colors. Replace the mockup's decorative slashes with the existing `Ui.badge()` / `s2_mark_v6`; do not introduce a new logo.

## Layout

1. Fixed header: existing brand mark, Swap2-Native, a small Quick settings label and a 48 dp close target. Show the running game's name on a separate ellipsized line.
2. Fixed segmented navigation: Graphics, Monitor, Controls. The active segment uses the primary neutral fill and inverse text. Each segment has a label and a simple existing vector icon.
3. Independently scrollable content for the selected tab. Only this region scrolls. Remember the selected tab and each tab's scroll position while this panel instance is alive.
4. Fixed bottom actions, separated by a hairline: full-width Back to game; a second row with Pause and Stop. Stop is a subdued danger action with the existing confirmation dialog.

Panel width: start with `min(440 dp, 50% of usable landscape width)`; on a narrow portrait window use the full usable width. Fit the actual text at large font scales; do not shrink all content to fit or allow horizontal scrolling. Fill the edge-to-edge background, but inset interactive content for display cutouts and transient system bars. A short viewport may scroll its content, never clip the fixed actions. Use existing 48 dp touch targets, 12 dp card gaps, 16–20 dp panel gutters, 18–20 dp card radii and the launcher's pill buttons. Use about 18–20 sp for the header, 15–16 sp for rows and 12–13 sp for secondary text.

## Tab contents and retained behavior

### Graphics

- Performance section: FPS cap row. Its value opens a themed choice dialog instead of a vertical radio list. Preserve the supported-rate bitmask, starting/unavailable states and the game's own-rate behavior. The Auto chip in the picture represents a game that controls its own rate; it must not offer an unsupported cap.
- LSFG card: master switch, real runtime status, multiplier and target as compact value rows. Keep motion-estimation resolution and the performance-mode explanation in an expandable Advanced area or another compact value row; these are omitted from the hero mockup only for clarity. Preserve `BuildConfig.NX_LSFG`, DLL requirements, `Lsfg.push`, preference scope and every existing option.
- Image section: reuse live `GameGraphics.rows` options, including upscaler, conditional FSR sharpness and mesh geometry. Maintain current live-apply behavior and conditional visibility. Do not expose restart-only options as though they apply immediately.
- Pause-on-return preference stays available here as a compact switch, with its complete existing explanation accessible.

### Monitor

- Master overlay switch.
- Compact / Detailed segmented choice.
- Scale and opacity sliders with their current percentage visible.
- Existing gesture hint and per-metric switches; all current metrics remain available. Disable dependent controls consistently when the overlay is off.
- Reuse current preferences and update behavior. Do not start new polling or duplicate the performance overlay.

### Controls

- Touch visibility and game profile selector.
- Opacity, scale, haptics, log display and reset.
- Preserve layout presets, layout editor and all rows currently supplied by `TouchControls.controlRows`.
- Keep editor callbacks: opening the editor closes the panel, respects the current paused state and restores the paused screen correctly on exit.

## Implementation guidance

- Scope the launcher's theme to this panel and its dialogs, using a themed/configured context with the saved light/dark/system selection. Do not re-theme or recreate `GameActivity`, change its full-screen window, or affect game rendering/touch artwork. Pass this context to row factories as well; the existing `GameActivity` callback currently passes the activity itself to `controlRows`.
- Prefer `Ui.Setting`, `Ui.button`, `Ui.badge`, existing vector icons and theme resources. Avoid styling every descendant recursively: values and state updates must remain explicit.
- Keep `SidePanel` visibility callbacks, native settings callbacks, touch suspension and controller input semantics. D-pad moves focus, A activates, B dismisses. Hidden tab content must not retain focus or receive navigation.
- Keep the existing single refresh loop and cancel it on close/detach. Switching tabs must not create additional loops or duplicate preference listeners.
- Slide the panel in/out over roughly 180/150 ms; respect `LauncherTheme.still`. No frame-by-frame background effects. Use launcher cues only if already appropriate to the in-game audio policy.
- Add all new labels as string resources. The image uses Portuguese; provide English defaults and Portuguese translations, with existing fallback behavior for other locales.

## Acceptance checks

- Compare light and dark palettes against the launcher, including dialogs and switches; test forced app theme opposite to the system theme.
- Check the current phone in landscape, portrait/narrow windows, display cutout sides and increased text size. Fixed actions must remain reachable and text must not overlap.
- Test every tab, scroll, close button, outside tap, Back/guide button and rapid reopen. Verify touch commands cannot pass through the open panel to the game.
- Check D-pad/A/B navigation and focus visibility, including tab changes and dialogs.
- Validate FPS support filtering, LSFG options, upscaler/sharpness, mesh switch, overlay modes/sliders, touch profiles and editor without losing or changing existing preferences.
- Verify pause, background-return preference and confirmed/cancelled Stop retain existing behavior. Unsaved progress warning is required.
- Confirm there is no extra polling after dismissal, no shader work, no renderer changes and no installation/data migration required for this design proposal.

## Deliverables

- `assets/artwork/swap2-native/side-panel-v1/side-panel-light-dark.png`: generated visual reference, built-in image generation, based on the inspected phone screen.
- `assets/artwork/swap2-native/side-panel-v1/prompt.txt`: generation prompt.
- This implementation brief. No application source has been changed for this proposal.
