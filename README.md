# NFSW.HUDScale

**HUD and menu scaling for Need for Speed: World at 1440p, 4K and other high resolutions.**

Current version: **1.0.0**. See the [release notes](RELEASE_NOTES.md).

At high resolutions, NFS World draws its HUD, menus and windows at their original pixel size, so at 1440p or 4K they become tiny. The usual workaround is to run the game at 1080p and upscale the whole picture, which makes the 3D world blurry.

NFSW.HUDScale is an ASI plugin that scales the game's UI instead. The game renders at your native resolution while the HUD, menus and windows are drawn at a readable size, and everything stays clickable, draggable and in the right place.

## Features

- **Scales the whole UI:** HUD (speedometer, top bar, power-up wheel, race widgets), full-screen menus (safehouse, aftermarket, options, …), windows (driver, social, chat, achievements, …) and pop-ups.
- **Automatic scale:** based on screen height, or set your own factor.
- **Anchored to their edge:** HUD pieces stay against the screen edge they belong to; centred elements stay centred.
- **Correct mouse input:** clicks, hovering and dragging match the enlarged windows, and the in-event HUD buttons (Quit Event, Car/Camera, Snapshot) work too.
- **Window dragging:** works 1:1, and positions can be remembered across restarts.
- **Tooltips and pop-ups:** they appear next to what you hover, including the aftermarket, performance-part and skill-mod info pop-ups that flip to the side of the cursor.
- **Separate tooltip size:** set relative to the UI scale.
- **Per-element control by file name:** exclude, pin, or scale only the content.
- **Optional:** scaled car name tags, top bar and power-up wheel toggles.
- **Optional cursor auto-hide:** hides the mouse cursor while driving after a few seconds without mouse activity. It never hides in menus, shops or windows.
- **Safe on other builds:** if the game version doesn't match, the plugin disables itself and logs why. It never patches blindly.

## Requirements

- Need for Speed: World on a **Soapbox Race World** server. The supported build is `nfsw.exe` SHA-256 `b2c616c641d3512b4d1edec9aab750f3308281752628abc845142d1c434ec819`.
- An **ASI loader**. Soapbox installs normally include one as `dinput8.dll` in the game folder. If yours doesn't, use [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader) (x86) renamed to `dinput8.dll`.
- Windows. The game can run fullscreen or borderless.

## Installation

1. Download the latest release.
2. Copy `NFSW.HUDScale.asi` and `NFSW.HUDScale.ini` into your game folder, next to `nfsw.exe` (e.g. `...\NFSWorld\Game Files\`).
3. Set the game to your monitor's native resolution and start it through your launcher as usual. If you used an upscaler such as Lossless Scaling for the UI, turn it off.

**Uninstall:** delete `NFSW.HUDScale.asi`, `NFSW.HUDScale.ini` and, if present, `NFSW.HUDScale.positions.ini`.

## Configuration

All settings are in `NFSW.HUDScale.ini`. Restart the game after changing them.

### Size

| Setting | Default | Description |
|---|---|---|
| `UIScale` | `0` | Fixed UI scale factor (e.g. `1.5`). `0` = automatic: screen height ÷ `BaseHeight`. |
| `BaseHeight` | `768` | Screen height the UI is laid out for in automatic mode. **Lower = bigger UI.** |
| `MinScale` | `1.0` | The UI is never scaled below this. |
| `TooltipScale` | `0.7` | Tooltip size relative to the UI scale (`0.75` = 75% of it). |

How `BaseHeight` affects the automatic scale:

| Resolution | `BaseHeight = 1080` | `BaseHeight = 768` | `BaseHeight = 720` |
|---|---|---|---|
| 2560×1440 | 1.33× | 1.88× | 2.0× |
| 3840×2160 | 2.0× | 2.81× | 3.0× |

At 1440p, `BaseHeight = 768` gives the same UI size as running the game at 768p and upscaling; lower values make the UI larger still.

### What gets scaled

| Setting | Default | Description |
|---|---|---|
| `ScaleFullscreenMovies` | `1` | Full-screen menus. |
| `ScaleWindowMovies` | `1` | HUD pieces and windows. |
| `ScaleTopBar` | `1` | Top button bar (`CommonHUD`). |
| `ScalePowerupWheel` | `1` | Power-up wheel (`powerup`). |
| `ScaleNameBadges` | `1` | Car name tags above other players. |
| `FixMenuTooltips` | `1` | Correct position of menu tooltips and item info pop-ups. |
| `RememberWindowPositions` | `1` | Remember where you drag windows (`NFSW.HUDScale.positions.ini`; delete it to reset). |

### Cursor

| Setting | Default | Description |
|---|---|---|
| `HideCursorInGame` | `1` | `1` = hide the cursor while only HUD is on screen, after `HideCursorDelay` seconds without mouse movement or clicks. Any mouse activity or menu brings it back. |
| `HideCursorDelay` | `5` | Seconds of inactivity before the cursor is hidden. |
| `HudMovies` | *(HUD list)* | Elements that count as HUD. Anything else that's visible counts as a menu and keeps the cursor. With `Log = 1`, the log names whatever keeps the cursor (`cursor: menu open (Name)`). |

### Per-element lists

Elements are matched by their Flash file name, without `.gfx`. Set `Log = 1` to see every name in the log as `movie loaded: ...`.

| Setting | Default | Behaviour |
|---|---|---|
| `ExcludeMovies` | `WorldMap` | Never scaled. The world map window is resizable in-game and works best untouched. |
| `TooltipMovies` | `Tooltip,Tooltip_powerupManagement` | Enlarged by `TooltipScale` at the spot the game chose. |
| `TopLeftMovies` | `ContextMenu,DropdownMenu` | Enlarged, keeping the top-left corner where the game opened them. |
| `InPlaceMovies` | `PlayerNameBadge` | Scaled around their own centre (only with `ScaleNameBadges = 1`). |
| `ContentOnlyMovies` | *(empty)* | The window stays as the game sets it; only its content is enlarged. |

### Troubleshooting

| Setting | Default | Description |
|---|---|---|
| `Log` | `0` | `1` = write `NFSW.HUDScale.log` next to the plugin. |
| `ExcludeSizes` | *(empty)* | Leave viewports of an exact size alone (`WxH`, as shown in the log). |
| `ScreenRectCallers` | *(empty)* | Advanced: extra game code addresses that need the on-screen window rectangle. |
| `DebugMouseMovies` | *(empty)* | Advanced: log which elements receive each mouse click (names, `*` = all). Needs `Log = 1`. |
| `DebugMouseStage` | *(empty)* | Advanced: log mouse routing for one element (stage size `WxH`). |

## Known limitations

- **World map:** excluded by default. Its window uses its own input handling, so scaling its rectangle breaks moving and resizing. Resize it in-game instead.
- **Supported build only:** written for the build above. Other builds are detected, and the plugin stays inactive.
- **UI scale only:** the 3D field of view, aspect ratio and in-world 3D HUD elements aren't changed.
- **Testing:** done at 2560×1440. Other resolutions, including 4K, use the same code paths but haven't been tested as extensively.
- **Cursor auto-hide and HUD list:** the list of HUD elements was built from free roam, races and events. If some screen keeps the cursor visible while driving, the log names it so you can add it to `HudMovies`.

## Building from source

Requires Visual Studio 2022 or later with the C++ desktop workload (MSVC x86 toolset).

```
src\build.cmd
```

The output goes to `build\NFSW.HUDScale.asi` plus the default `build\NFSW.HUDScale.ini`.

## How it works

NFS World's UI is built from Scaleform GFx (Flash) movies. Each HUD piece, menu and window is a separate movie that the game places in screen pixels. The plugin:
- hooks GFx's viewport functions to enlarge each movie, either by scaling its rectangle or its content;
- re-anchors each movie to the screen edge it belongs to;
- keeps the game's own layout, mouse, drag and docking code consistent by translating between the game's coordinates and the on-screen ones;
- patches a few game routines that pass screen coordinates straight into Flash or read Flash sizes as pixels: map resize, menu tooltips, item pop-ups, and the event HUD's click area;
- optionally hides the cursor through the game's own `SetCursor` calls, while only HUD elements are on screen.

The full reverse-engineering notes are in [docs/TECHNICAL.md](docs/TECHNICAL.md).

This is unrelated to the NFS: Most Wanted widescreen fix. That game uses EA's older FEng UI engine, so its fix can't be ported.

## Credits

- Reverse engineering and implementation: the NFSW.HUDScale authors.
- Thanks to the community mod **NFSWUIScale** for feature ideas: identifying elements by file name, anchoring context menus at their top-left corner, scaling name tags in place, and remembering window positions.
- Need for Speed: World is © Electronic Arts. This project isn't affiliated with EA or the Soapbox Race World team.
