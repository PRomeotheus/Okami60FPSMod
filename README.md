# Okami60FPSMod

A frame interpolation mod for **Okami HD** (Steam, PC) that makes the game look like it runs at **60, 90 or 120 FPS**.

Okami HD is locked to 30 FPS because its game logic, physics and animations are tied to the frame rate. Unlocking it makes the game run too fast. This mod leaves the game running at 30 FPS internally and generates the in-between frames on your GPU using motion estimation, so the game logic stays correct while the picture is smoother.

## Features

- 60, 90 or 120 FPS output (30 = mod off)
- **Frame Rate** option added to the game's **Options → Display Settings** menu
- Menus and the HUD are left untouched
- Camera cuts are detected so they don't get blended
- The setting is saved between sessions

> 90 and 120 FPS need a monitor running at that refresh rate or higher when V-Sync is on. If your display can't show it, the mod automatically uses the highest rate it can.

## Installation

Download `Okami60FPSMod.zip` from the [latest release](../../releases/latest).

### Windows

1. In Steam, right-click **Okami HD** → **Manage** → **Browse local files**.
2. Copy `dinput8.dll` and `okami60.ini` from the zip into that folder (next to `okami.exe`).
3. Start the game.

### SteamOS / Steam Deck / Linux (Proton)

1. Switch to **Desktop Mode**.
2. In Steam, right-click **Okami HD** → **Manage** → **Browse local files**.
3. Copy `dinput8.dll` and `okami60.ini` from the zip into that folder (next to `okami.exe`).
4. Right-click **Okami HD** → **Properties** → **General**, and set **Launch Options** to:
   ```
   WINEDLLOVERRIDES="dinput8=n,b" %command%
   ```
5. Start the game (you can go back to Game Mode).

## Usage

Go to **Options → Display Settings → Frame Rate** and pick 30, 60, 90 or 120 FPS with left/right.

Keyboard shortcuts:

| Key | Action |
|-----|--------|
| F8  | Cycle frame rate (30 → 60 → 90 → 120) |
| F9  | Turn interpolation on/off (for comparison) |

Other settings are in `okami60.ini`. Set `Enabled=0` to turn the mod off without removing it.

## Uninstall

1. Open the game folder (**Manage** → **Browse local files**).
2. Delete `dinput8.dll`, `okami60.ini` and `okami60.log`.
3. On SteamOS / Linux, also clear the **Launch Options** you added.

## Building from source

Requires MinGW-w64 and Python 3 (Linux, or MSYS2 MINGW64 on Windows):

```
./build.sh
```

The DLL is written to `dist/dinput8.dll`.
