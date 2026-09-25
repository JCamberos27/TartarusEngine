# The launch screen

`run-editor.cmd` (the desktop "Tartarus Engine" shortcut) shows a launch screen while it runs
the Release build, then starts the editor. The build runs behind the screen the whole time, so
the screen adds no launch time. The screen closes once the build finishes.

The composition, in a 3:2 frame: the art on the left, and on the right the Tartarus Engine
lockup with the status, a progress bar and the version line (`v0.1.0 · branch @ commit`).

## The sequence

1. **Power on.** A bright line draws across the tube and opens into the picture.
2. The art fades up, lit by the density of each glyph. The lockup wipes in behind a gold edge.
3. **Building.** `B U I L D I N G` pulses and the bar fills. The lockup flickers faintly, like
   candlelight, and the taskbar button shows the same progress.
4. **Ready.** `R E A D Y` shows with the build time. The tube powers off (the picture collapses
   to a line, then to a point) and the editor starts.
5. **If the build fails**, the art makes way for the errors: paths made relative, duplicates
   merged, at most 14 lines. Then it asks: **Y** (or click) launches the previous build, **N**
   or Esc closes.

Any key or a click skips the intro. **M** mutes the sound; the choice is remembered.

## Files

| File | What |
|------|------|
| `launch-crt.ps1` | The CRT screen: a borderless WPF window with the tube shader. Tried first. |
| `CrtLaunch.cs` | Its native side: the shader effect, the art's text runs, the git line (read from `.git`, no git process), the synthesized sounds and their mixer, and a few Win32 calls. |
| `crt.fx` / `crt.ps` | The tube: its source, and the compiled pixel shader (ps_3_0) that ships. |
| `launch-screen.ps1` | The console version of the same screen: the fallback. It also provides `-Reveal`, which shows the console window again. |
| `TartarusEngineAscii.txt` | The text, split into `::banner` and `::art` sections. |

## How run-editor.cmd drives it

- **The classic console.** Windows Terminal, the Windows 11 default, ignores window sizing, so
  the script reopens itself in `conhost.exe`, minimized. The shortcut starts conhost directly
  with a `classic` argument, which skips that hop.
- It runs `launch-crt.ps1 -Build`, and falls back to `launch-screen.ps1 -Build` on exit code 99.
- **Exit codes:**

  | Code | Meaning |
  |------|---------|
  | 0 | Built. The editor starts. |
  | 10 | The build failed. Launch the previous build. |
  | 11 | The build failed. Close. |
  | 99 | The CRT screen couldn't start (no WPF, no window). Nothing was built yet. |
  | Anything else | The screen itself failed. run-editor.cmd shows the console and prints the tail of `build\last-build.log`. |

## Details

- **Window.** 3:2, 80% of the height of the monitor under the mouse, centred on it, and
  DPI-aware. It has the engine's icon and taskbar progress. The console hides itself, and the
  window takes the foreground, so Y / N reach it.
- **The tube** (`crt.fx`):
  - barrel curvature;
  - convergence error that grows toward the edges;
  - phosphor glow;
  - a fixed 540-line picture and 720 grille triads, the same on any monitor (capped where a
    monitor is too small to show them);
  - a rolling refresh band, flicker, grain, a vignette and a highlight on the glass.

  Without ps_3_0 hardware, the window plays without the tube.
- **Sound.** Nothing is recorded; every sound is synthesized when the screen starts:
  - power on: a relay clunk, a degauss swell and static;
  - while lit: the high-voltage whine and mains hum, a seamless loop that rises and falls with
    the picture;
  - power off: a falling zap and a thump.

  It is mixed live into a `waveOut` stream. No audio device just means silence.
- **Progress.** MSBuild prints one `Project.vcxproj -> output` line per finished project. The bar
  counts them against the last successful build, with its duration filling in between, and
  holds at 97% until the build exits.

## Files it writes (all in `build\`, ignored by git)

| File | What |
|------|------|
| `last-build.log` | The build's output. |
| `launch-progress.txt` | The last successful build: its project count and seconds. The progress estimate uses it. |
| `launch-settings.txt` | `sound=on` / `sound=off`. |
| `launcher-cache\CrtLaunch-<hash>.dll` | `CrtLaunch.cs`, compiled once (keyed by a hash of the source and the .NET version) and loaded from here after. The first launch after editing the file takes about 0.5 s longer. |

## Editing

- **The shader:** edit `crt.fx`, then rebuild `crt.ps` with the Windows SDK's compiler, and
  commit both:

  ```
  fxc /nologo /T ps_3_0 /E main /O3 /Fo crt.ps crt.fx
  ```

- **`CrtLaunch.cs`** recompiles itself on the next launch; there is nothing to do.
- **The art or lockup:** edit `TartarusEngineAscii.txt`. Blank rows and the common indent are
  trimmed, so the layout sizes to the ink.
- **Try the screen without building:** run `launch-crt.ps1` without `-Build`. It plays the
  intro, shows READY and closes.
