## What and why

<!-- One paragraph: the problem, the fix, and anything a reviewer should look at first. -->

## How it was verified

- [ ] Builds in Release (and Debug if you touched anything config-sensitive)
- [ ] `--unit-tests` exits 0 (run through `Start-Process -Wait`; Release is a GUI-subsystem exe)
- [ ] `--smoke-test` passes (CI runs `tests/smoke-scenes/`; also run `--smoke-test project\scenes` if you touched a project scene)
- [ ] Any new `*Component` / `*Tag` in `Components.h` is registered in `ComponentRegistry.cpp` or listed in `tools/component_registration_allowlist.txt`
- [ ] New input actions are in `InputMap::Defaults()` **and** `project/settings.json`

## Housekeeping

- [ ] No unrelated scene/asset re-saves (the editor rewrites `.json` scenes and `.meta` files when it opens them, so revert any you didn't mean to change)
- [ ] Scratch output (`work/*.exe`, logs, commit-message files) not committed
- [ ] Docs updated where behaviour changed

<!--
First-person weapons / animation: also complete the checklist at the bottom of
FPS_WEAPON_INTEGRATION.md, and update FPS_ANIMATION_SYSTEM.md if a rule, key or constant changed.
-->
