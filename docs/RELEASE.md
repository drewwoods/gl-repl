# Release Packaging & macOS App Bundle

Reference for `make release` (orchestrated by [`scripts/release.py`](../scripts/release.py))
and `make app`. Moved here from `CLAUDE.md` to keep the agent brief compact.

## `make release`

`make release` builds both platform artifacts and, **after an explicit
confirmation prompt**, uploads them to a GitHub release. Orchestrated by
[`scripts/release.py`](../scripts/release.py) (Python — `curses` menu +
`configparser` persistence):

- **Build plan (skip / local / remote per platform).** Each platform builds in
  one of three modes: `skip`, `local` (this machine — macOS needs Darwin, Linux
  needs Linux), or `remote` (over ssh on a host, artifact copied back). Defaults
  are macOS `local` + Linux `remote`, but any combination works: Linux-only on a
  local Linux box, both on remote hosts, etc. When run on a terminal, `make
  release` opens an **arrow-key plan menu**: ↑/↓ move between fields, ◄/► cycle a
  platform through skip→local→remote, Enter edits a text field (ssh host, path,
  tag, repo, music source) or activates an action, `s` saves, `b` builds, `q`
  quits. `make release-config` opens the same menu to just edit + save without
  building. Non-interactively (no tty, `--no-menu`, or `--yes`/`ASSUME_YES=1`)
  it uses the persisted/env plan and skips the menu.
- **Persistence.** The plan (modes, ssh hosts/paths, repo, remote branch, music
  source, pin ref) is saved to `.release.ini` (repo root, gitignored) on
  save/build, so customizations survive between runs. Config precedence is
  **CLI/env override > `.release.ini` > built-in default**; `tag` is not
  persisted (derived per-release from `git describe` of the pinned commit).
- **SHA pinning + verification (every host builds the identical commit).** The
  whole release is pinned to one commit SHA, resolved once up front from the
  `pin` ref (default: local `HEAD` when any platform builds locally, else
  `origin/<branch>`). The menu surfaces the resolved target
  (`target: <sha> <describe> (ref …)`) so you see what all hosts will build
  before proceeding — press `r` to `git fetch` and refresh it. Each **remote**
  host fetches the branch, `git checkout --detach`es to that exact SHA, and is
  re-`rev-parse`d to confirm it landed there (aborting if the commit isn't
  pushed). Each **local** platform is verified with `HEAD == target` and a clean
  tree (else it aborts — a dirty/ahead local build wouldn't match the pin;
  `ALLOW_DIRTY=1` downgrades the clean check to a warning). So a two-host build
  can't silently mix SHAs, and the artifact tag names that one commit. Override
  the pin with the menu's *Pin ref* field or `PIN=<ref>`.
- **macOS** (`MACOS_MODE`) is built via `make app` (locally, or on
  `MACOS_HOST:MACOS_PATH` for remote — the `.app` is copied back). The release
  step then swaps the full music pack into
  `gl-repl.app/Contents/Resources/assets/` (replacing the single `sample.mp3`
  `make app` seeds) and zips the bundle.
- **Linux** (`LINUX_MODE`, host `LINUX_HOST`/`LINUX_PATH`, defaulting to
  `gracemont` / `~/code/openGL/samples/gen-ai/gl-repl`; `REMOTE_HOST`/`REMOTE_PATH`
  seed those): a remote build fast-forwards that checkout to `origin/main` and
  runs `make gl-repl`; a local build just runs `make gl-repl` here. Either way
  the binary is tarred with the music pack in `./assets` plus a `README.txt`.
- **Music source** is `music_src_dir` (env `MUSIC_SRC_DIR`) — `assets/favorite`
  when it holds any `*.mp3`, else flat `assets/` (same favorite/fallback idiom
  the web build's `MUSIC_SRC_DIR` uses). All packaging happens locally, so the
  gitignored/symlinked tracks are staged from this checkout for both platforms.
  The "known location" in each package matches the binary's playlist search:
  `Contents/Resources/assets` (mac) and `./assets` (linux).
- **Confirmation gate.** `make release` stages everything under `dist/<tag>/`,
  prints a manifest, then prompts on the tty before uploading. Declining (or no
  tty) leaves the artifacts staged and prints the `make release-upload` command.
  `make release-build` stops after staging; `make release-upload` pushes the
  staged artifacts. The GitHub release is created as a **draft**; publish with
  `gh release edit <tag> --repo <repo> --draft=false`.
- **Tag pins to the build commit.** When the release tag does not already exist,
  `gh release create` is passed `--target <pinned-sha>`, so publishing the draft
  creates the git tag on the exact commit the artifacts were built from (not the
  default branch's then-current HEAD). If the tag already exists, GitHub uses its
  commit and `--target` is moot. Creating a release fails cleanly if that commit
  isn't pushed to GitHub yet (it can only tag a commit it has).
- **Knobs.** Set them in the menu (persisted), or override per-run via env /
  make-vars — the Makefile forwards `TAG`, `REPO`, `PIN`, `REMOTE_BRANCH`, the
  plan knobs `MACOS_MODE`/`MACOS_HOST`/`MACOS_PATH` and
  `LINUX_MODE`/`LINUX_HOST`/`LINUX_PATH`, plus `ASSUME_YES` / `ALLOW_DIRTY` when
  set (e.g. `make release LINUX_MODE=local TAG=v1.0.0`). `MUSIC_SRC_DIR`,
  `REMOTE_HOST`/`REMOTE_PATH`, and the back-compat `SKIP_MACOS=1` / `SKIP_LINUX=1`
  (= mode `skip`) are read from the environment.

`make fetch-music` is the inverse: it downloads the music pack from the GitHub
release (via [`scripts/fetch-music.sh`](../scripts/fetch-music.sh)) into the
**local** assets folder (`MUSIC_DEST`, default `assets/`). `MUSIC_TAG=<tag>`
selects a release other than the default `assets-v1` asset release.

## macOS app bundle (`make app`)

`make app` packages the binary into `gl-repl.app` so the Dock/Finder show
a real icon instead of the launching terminal's, and so a Finder launch
has music. It assembles `Contents/{MacOS/gl-repl, Resources/, Info.plist}`:

- **Icon** — `APP_ICON_SVG` (default `packaging/macos/gl-repl-soft-cube.svg`)
  is rasterized via `rsvg-convert` (needs `brew install librsvg`) into an
  `.iconset` and packed with `iconutil` into `gl-repl.icns`. Source SVGs +
  their generators live in `packaging/macos/` (`gen_soft_cube.py` →
  `make icon-cube` / `icon-cube-strong`; `gen_retro_a.py` → `make
  icon-regen`). Swap icons by repointing `APP_ICON_SVG`.
- **Music** — copies `assets/sample.mp3` into `Contents/Resources/assets/`.
  The binary finds it via the bundled-assets playlist source
  (`<exe>/../Resources/assets`), so the bundle ships with a track even though
  the Finder cwd is `/`. Only `sample.mp3` is bundled (small download; avoids
  shipping the full playlist); the per-user music folder is for adding more.
- **Ad-hoc signature** — the last `make app` step is
  `codesign --force --deep --sign - gl-repl.app` (ad-hoc, no Apple account).
  Without it, a *downloaded* copy is quarantined + fully unsigned, which macOS
  (esp. Apple Silicon) reports as **"gl-repl.app is damaged"** — a hard block.
  A valid ad-hoc signature downgrades that to the ordinary "unidentified
  developer" prompt (right-click → Open / Privacy & Security → Open Anyway).
  Editing `Resources/` breaks the seal ("a sealed resource is missing or
  invalid"), so the **release** flow (`scripts/release.py` `build_macos`)
  re-signs as its final step, *after* swapping in the full music pack and
  before zipping — the make app signature alone would be invalid in the
  shipped bundle. Re-signing needs a mac (codesign); packaging a remote-mac
  build on a non-mac host warns and leaves it unsigned. Proper Developer-ID
  signing + notarization (removes the prompt entirely) is intentionally not
  done — it needs a paid account.
- **Signing fails loudly, and is verified on the shipped bytes.** On macOS both
  `make app` and the release re-sign hard-fail instead of warning: a silently
  unsigned bundle is exactly what ships as "damaged", so it must not survive to
  an upload. The mac zip is built with **`ditto -c -k --sequesterRsrc
  --keepParent`** (the Apple-supported bundle archiver) rather than `zip`, then
  `verify_macos_zip()` unpacks that archive, applies a
  `com.apple.quarantine` xattr exactly as a browser download would, and re-runs
  `codesign --verify --deep --strict`. An invalid signature on a quarantined app
  *is* the "damaged" block, so this is the check that keeps it out of a release;
  it aborts the build rather than staging a broken artifact.

Build products (`gl-repl.app/`, `gl-repl.icns`, `gl-repl.iconset/`) are
gitignored; the committed `.svg`s are the source of truth. Pure packaging
— no source changes — so the `-std=c99` / Linux build is untouched.
