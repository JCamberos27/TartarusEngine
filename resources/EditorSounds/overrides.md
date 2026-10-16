# Editor UI sounds: premium overrides (paid sets, Drive only)

The `.wav` files in this folder are CC BY-SA and safe to commit (see `LICENSE.md`). The two paid sets below are licensed to the
project owner only. **Never copy, convert or commit them into this public repository.** An override is a local swap: the editor
loads the Drive file in place of the committed one when it is present on the machine.

Where the paid sets live: `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\`

- `SoundShelf Studio SFX\` (20 mp3 files, 44.1/48 kHz, 256 kbps)
- `UX Desktop SFX\Menu Sounds\` (The Sample Stop, 44.1 kHz 16-bit wav). Only the `Menu Sounds` folder counts. The other
  `UX Desktop SFX` folders are mp3s of freedesktop event sounds and are not part of this list.

How the picks were made: no mp3 decoder is installed here (numpy and `wave` only), so the SoundShelf picks go by file name
and frame-counted file length (every file is 1.2-2.4 s including its tail; trim each one to the event's length). The Sample Stop
files were measured (spectral centroid, ringing time, pitch). Most of them are low (65-280 Hz), so they suit only the low events.
Audition every pick before you rely on it. Where no paid file fits, the row says to keep the CC file.

To use an override: convert to 48 kHz / 16-bit / mono, cut so the transient lands at sample 0 (clicks and ticks under 120 ms),
then match the level of the CC file it replaces (ticks at -20 dBFS peak, cues at -27 LUFS on the 100 ms window, notifications
at -28 LUFS-M max).

| Event | Premium pick (exact path) | Why / confidence |
|---|---|---|
| Click | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\01 - Interaction\UI - Click - Soft.mp3` | Soft click; high |
| ClickConfirm | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\01 - Interaction\UI - Swipe - Confirm.mp3` | Named confirm. A swipe may run past 120 ms, so trim it. Medium |
| ClickCancel | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\01 - Interaction\UI - Swipe - Cancel.mp3` | Named cancel, pairs with Confirm; medium |
| ClickDestructive | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\01 - Interaction\UI - Click - Deep.mp3` | Lower, duller click; medium |
| ToggleOn | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\01 - Interaction\UI - Tap - Light.mp3` | Light (higher) tap; medium. Pitch it ±2-4 % for variants |
| ToggleOff | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\01 - Interaction\UI - Tap - Heavy.mp3` | Heavier (lower) tap, pairs with ToggleOn; medium |
| TabSelect | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\02 - Navigation and Panels\UI - Swipe - Navigation Soft.mp3` | Soft navigation; medium |
| PlayEnter | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\02 - Navigation and Panels\UI - Menu - Futuristic Open.mp3` | A mode-change "open"; medium |
| PlayExit | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\02 - Navigation and Panels\UI - Menu - Slide Out.mp3` | Exit counterpart. No futuristic close exists; low |
| Pause | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\UX Desktop SFX\Menu Sounds\094_Menu Sound 2 - The Sample Stop.wav` | Measured: soft low 172 Hz tone, rings 225 ms, centroid 169 Hz (lower than Resume); medium |
| Resume | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\UX Desktop SFX\Menu Sounds\092_Menu Sound 1  - The Sample Stop.wav` | Measured: 280/194 Hz, rings 243 ms, centroid 246 Hz (above Pause). The name has two spaces before the dash. Medium |
| Step | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\01 - Interaction\UI - Tap - Light.mp3` | Same file as ToggleOn; no better short tick exists. Low. Keeping the CC `Step.wav` is fine |
| NotifyInfo | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\03 - Notifications and Alerts\UI - Notification - App.mp3` | Neutral app notification; medium |
| NotifySuccess | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\03 - Notifications and Alerts\UI - Glass Ping - Soft.mp3` | Glassy and soft, fits Black Glass best; medium. Check that it reads as "success" next to Info |
| NotifyWarning | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\03 - Notifications and Alerts\UI - Warning Buzz.mp3` | The only paid warning. Low: the name says "buzz", which the style brief rules out. Keep the CC `NotifyWarning.wav` unless it turns out soft |
| NotifyError | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\UX Desktop SFX\Menu Sounds\093_Menu Sound 10 - The Sample Stop.wav` | Measured: dull and low (151/237 Hz), rings 463 ms. No paid set has a real error sound. Low |
| Save | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\UX Desktop SFX\Menu Sounds\100_Menu Sound 8 - The Sample Stop.wav` | Measured: the only tonal, glassy Sample Stop file (527 Hz with partials at 775 / 1055 / 1572 Hz, centroid 722 Hz, rings ~470 ms); medium |
| Undo | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\02 - Navigation and Panels\UI - Pop-Up - Close.mp3` | A "close" for the backward step; low |
| Redo | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\02 - Navigation and Panels\UI - Pop-Up - Open.mp3` | Pairs with Undo; low |
| Hover | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\01 - Interaction\UI - Hover - Sci-Fi.mp3` | The only hover file. Medium. Play it very quietly (CC Hover is -28 dBFS peak), and check that "sci-fi" isn't too bright |
| SliderTick | *(keep CC `SliderTick_*.wav`)* | Nothing short enough. Every Sample Stop file rings for 225 ms or more |
| SliderGrab | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\01 - Interaction\UI - Click - Dark.mp3` | Dark (low) click to grab; low |
| SliderRelease | *(keep CC `SliderRelease.wav`)* | No paid file pairs well with Click - Dark |
| MenuOpen | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\02 - Navigation and Panels\UI - Menu - Open.mp3` | Named exactly; high |
| MenuClose | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\02 - Navigation and Panels\UI - Modal - Close Soft Tap.mp3` | Soft close; medium |
| DragStart | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\02 - Navigation and Panels\UI - Menu - Slide In.mp3` | A lift / slide; low |
| DropAccept | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\SoundShelf Studio SFX\03 - Notifications and Alerts\UI - Notification - PC.mp3` | A short positive ping; low |
| DropReject | `G:\My Drive\Tartarus Assets\Raw\Audio\SFX\UX Desktop SFX\Menu Sounds\095_Menu Sound 3 - The Sample Stop.wav` | Measured: dull 65 Hz thump, 238 ms, the "lower and duller" shape; low. Small speakers may barely play it |

Not recommended for any event: `096_Menu Sound 4` and `099_Menu Sound 7` (loud boomy 65-108 Hz, near 0 dBFS, ~1 s),
`098_Menu Sound 6` (a low, grainy texture), `101_Menu Sound 9` (L/R anti-phase, so it cancels when folded to mono) and
`070_Atmosphere 8 ..._edited.wav` (a 150 s ambience).
