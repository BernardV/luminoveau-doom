# Luminoveau engine patches

The Luminoveau engine is **not vendored** in this repo — CMake fetches it from
upstream at a pinned commit (`FetchContent`, see the top of `../CMakeLists.txt`)
and applies these patches on top at configure time.

- **Remote:** `git@github.com:bXi/luminoveau.git` (`LUMINOVEAU_GIT_URL`)
- **Pinned commit:** `LUMINOVEAU_GIT_TAG` in `../CMakeLists.txt`
- No SSH keys? Configure with
  `-DLUMINOVEAU_GIT_URL=https://github.com/bXi/luminoveau.git`.

## Patches

| File | Touches | What |
|------|---------|------|
| `luminoveau-virtualcontrols-touch.patch` | `src/platform/input/virtualcontrols.{h,cpp}` | Touch controls: physical-coords, look region + tap, labels, viewport-relative sizing, mouse-emulation toggle |
| `luminoveau-input-no-gamepad.patch` | `src/platform/input/input.cpp` | `LUMI_NO_GAMEPAD=1` escape hatch (gamepad init can hang on macOS) |
| `luminoveau-shader-transpile.patch` | `cmake/ShaderTranspile.cmake` | Web GLSL→WGSL transpile fixes (Tint/Dawn/glslang) |

All are additive / opt-in and are being sent upstream. `apply.sh` applies them
idempotently (skips any already applied) so re-configuring never errors.

## Updating the engine

1. Pick the new upstream commit; set `LUMINOVEAU_GIT_TAG` in `../CMakeLists.txt`.
2. Re-apply the patches against it and fix any that no longer apply:
   ```sh
   git clone git@github.com:bXi/luminoveau.git /tmp/lumi && cd /tmp/lumi
   git checkout <new-commit>
   for p in <repo>/doomgame/patches/luminoveau-*.patch; do
       git apply "$p" || echo "REBASE NEEDED: $p"   # fix the rejects, then regenerate
   done
   # after fixing, regenerate a changed patch (engine-relative paths):
   git diff -- <changed files> > <repo>/doomgame/patches/<name>.patch
   ```
3. Once a patch is merged upstream, drop it here and bump the pin past the merge.
4. `rm -rf build-*/_deps/luminoveau-*` and reconfigure to pull the new pin.
