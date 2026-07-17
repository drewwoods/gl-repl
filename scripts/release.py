#!/usr/bin/env python3
"""release.py — build, package, and upload gl-repl releases (macOS + Linux).

Each platform builds in one of three modes: skip, local (this machine), or
remote (over ssh, artifact copied back). The build plan (modes, ssh hosts/paths,
repo, music source) is edited in an arrow-key menu and persisted to
.release.ini so customizations survive between runs. Packaging always happens
locally, so the music pack (gitignored / often symlinked on the dev mac) is
staged from this checkout into a known location beside each binary.

Subcommands:
  build     Build + stage artifacts under dist/<tag>/ (no upload).
  upload    Upload already-staged dist/<tag>/ artifacts to the GitHub release.
  all       Build, then confirm and upload (default).
  config    Open the plan menu and save .release.ini (no build).

On a terminal, `build`/`all` open the plan menu first (unless --no-menu or
--yes). Config precedence: CLI/env override > .release.ini > built-in default.
Env vars (TAG, REPO, MUSIC_SRC_DIR, MACOS_MODE/HOST/PATH, LINUX_MODE/HOST/PATH,
REMOTE_HOST/REMOTE_PATH, REMOTE_BRANCH, SKIP_MACOS, SKIP_LINUX, ASSUME_YES,
ALLOW_DIRTY) are honored for one-shot overrides.
"""

import argparse
import configparser
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INI = ROOT / ".release.ini"
MODES = ["skip", "local", "remote"]

# Persisted config sections + their built-in defaults. `tag` stays blank in the
# ini (it is derived per-release); the rest persist.
DEFAULTS = {
    "release": {"repo": "drewwoods/gl-repl", "tag": "",
                "music_src_dir": "", "remote_branch": "main"},
    "macos":   {"mode": "local", "host": "", "path": ""},
    "linux":   {"mode": "remote", "host": "gracemont",
                "path": "~/code/openGL/samples/gen-ai/gl-repl"},
}

# --- ANSI helpers ------------------------------------------------------------
def _c(code, s):
    return s if not sys.stderr.isatty() else f"\033[{code}m{s}\033[0m"

def say(s):  print(_c("1;36", "release:"), s, file=sys.stderr)
def warn(s): print(_c("1;33", "release: warning:"), s, file=sys.stderr)
def die(s):  print(_c("1;31", "release: error:"), s, file=sys.stderr); sys.exit(1)


# --- config load/save --------------------------------------------------------
def load_cfg():
    cfg = {sec: dict(vals) for sec, vals in DEFAULTS.items()}
    if INI.exists():
        parser = configparser.ConfigParser()
        parser.read(INI)
        for sec in cfg:
            if parser.has_section(sec):
                for key in cfg[sec]:
                    if parser.has_option(sec, key):
                        cfg[sec][key] = parser.get(sec, key)
    _apply_env(cfg)
    return cfg


def _env(name):
    v = os.environ.get(name)
    return v if v not in (None, "") else None


def _apply_env(cfg):
    m = {
        "TAG": ("release", "tag"), "REPO": ("release", "repo"),
        "MUSIC_SRC_DIR": ("release", "music_src_dir"),
        "REMOTE_BRANCH": ("release", "remote_branch"),
        "MACOS_MODE": ("macos", "mode"), "MACOS_HOST": ("macos", "host"),
        "MACOS_PATH": ("macos", "path"),
        "LINUX_MODE": ("linux", "mode"), "LINUX_HOST": ("linux", "host"),
        "LINUX_PATH": ("linux", "path"),
        # Back-compat seeds for the Linux remote target.
        "REMOTE_HOST": ("linux", "host"), "REMOTE_PATH": ("linux", "path"),
    }
    for env_name, (sec, key) in m.items():
        val = _env(env_name)
        if val is not None:
            cfg[sec][key] = val
    if os.environ.get("SKIP_MACOS") == "1":
        cfg["macos"]["mode"] = "skip"
    if os.environ.get("SKIP_LINUX") == "1":
        cfg["linux"]["mode"] = "skip"


def save_cfg(cfg):
    parser = configparser.ConfigParser()
    for sec, vals in cfg.items():
        parser[sec] = {k: str(v) for k, v in vals.items()}
    with open(INI, "w") as f:
        f.write("# gl-repl release build plan — edit via `make release-config`.\n")
        parser.write(f)
    say(f"saved config to {INI.relative_to(ROOT)}")


# --- derived values ----------------------------------------------------------
def git(*args):
    try:
        return subprocess.check_output(["git", *args], cwd=ROOT,
                                       stderr=subprocess.DEVNULL).decode().strip()
    except Exception:
        return ""


def effective_tag(cfg):
    t = cfg["release"]["tag"].strip()
    if t:
        return t
    t = git("describe", "--tags", "--always", "--dirty")
    return t or f"dev-{git('rev-parse', '--short', 'HEAD') or 'unknown'}"


def music_dir(cfg):
    d = cfg["release"]["music_src_dir"].strip()
    if d:
        return d
    fav = ROOT / "assets" / "favorite"
    if fav.is_dir() and any(p.suffix.lower() == ".mp3" for p in fav.iterdir()):
        return "assets/favorite"
    return "assets"


# --- shell helpers -----------------------------------------------------------
def run(cmd, **kw):
    """Run a command, streaming to our stderr; raise on failure."""
    subprocess.run(cmd, cwd=kw.pop("cwd", ROOT), check=True,
                   stdout=sys.stderr, **kw)


def have_tty():
    return sys.stdin.isatty() and sys.stdout.isatty()


def stage_music(dest: Path, src_rel: str):
    src = ROOT / src_rel
    dest.mkdir(parents=True, exist_ok=True)
    n = 0
    if src.is_dir():
        for p in sorted(src.iterdir(), key=lambda p: p.name):
            if p.name.lower().endswith(".mp3"):
                shutil.copy(p, dest / p.name)  # follows symlinks
                n += 1
    rel = dest.relative_to(ROOT) if dest.is_relative_to(ROOT) else dest
    say(f"staged {n} track(s) from {src_rel} -> {rel}")
    if n == 0:
        warn(f"no .mp3 tracks in {src_rel} (run 'make fetch-music' first?)")


def warn_if_dirty(cfg):
    if os.environ.get("ALLOW_DIRTY") == "1":
        return
    if git("status", "--porcelain"):
        br = cfg["release"]["remote_branch"]
        warn("working tree is dirty — a local build reflects local changes, but")
        warn(f"a remote build compiles origin/{br} on its host.")
        warn("Commit + push for a matched release (or ALLOW_DIRTY=1 to silence).")


def remote_sync(host, path, branch):
    run(["ssh", host,
         f"cd {path} && git fetch --quiet origin {branch} && "
         f"git checkout --quiet {branch} && git pull --ff-only origin {branch}"])


# --- platform builds ---------------------------------------------------------
def build_macos(cfg, dist: Path, tag, mdir):
    m = cfg["macos"]
    if m["mode"] == "skip":
        return
    stage = dist / ".macos-stage"
    if m["mode"] == "local":
        if sys.platform != "darwin":
            warn("macOS mode is local but not on macOS — skipping macOS.")
            return
        say("building macOS app bundle locally (make app)")
        run(["make", "-C", str(ROOT), "app"])
        appdir = ROOT / "gl-repl.app"
    else:  # remote
        if not m["host"]:
            die("macOS mode is remote but no host set (edit via make release-config).")
        say(f"building macOS app bundle on {m['host']}:{m['path']}")
        remote_sync(m["host"], m["path"], cfg["release"]["remote_branch"])
        run(["ssh", m["host"], f"cd {m['path']} && make app"])
        shutil.rmtree(stage, ignore_errors=True)
        stage.mkdir(parents=True)
        say(f"copying gl-repl.app back from {m['host']}")
        run(["scp", "-q", "-r", f"{m['host']}:{m['path']}/gl-repl.app",
             str(stage / "gl-repl.app")])
        appdir = stage / "gl-repl.app"

    # Replace the single sample.mp3 make app seeds with the full pack.
    res = appdir / "Contents" / "Resources" / "assets"
    for old in res.glob("*.mp3"):
        old.unlink()
    stage_music(res, mdir)

    dist.mkdir(parents=True, exist_ok=True)
    zip_path = dist / f"gl-repl-{tag}-macos.zip"
    if zip_path.exists():
        zip_path.unlink()
    say(f"zipping -> dist/{tag}/{zip_path.name}")
    run(["zip", "-q", "-r", "-y", str(zip_path), "gl-repl.app"],
        cwd=str(appdir.parent))
    shutil.rmtree(stage, ignore_errors=True)


LINUX_README = """gl-repl — Immediate-mode OpenGL REPL ({tag}, linux-{arch})

Run from inside this directory so the bundled music is found:

    ./gl-repl

Music lives in ./assets (gl-repl scans it on startup). Drop more *.mp3
there, or into $XDG_DATA_HOME/gl-repl/music. Run with --no-audio to skip
audio entirely.

Requires system OpenGL + GLUT/freeglut runtime libraries
(e.g. libgl1 libglu1-mesa freeglut3 on Debian/Ubuntu).
"""


def build_linux(cfg, dist: Path, tag, mdir):
    m = cfg["linux"]
    if m["mode"] == "skip":
        return
    if m["mode"] == "local":
        if sys.platform.startswith("linux"):
            say("building Linux binary locally (make gl-repl)")
            run(["make", "-C", str(ROOT), "gl-repl"])
            arch = os.uname().machine
            bin_src = ("copy", ROOT / "build" / "release" / "gl-repl")
        else:
            warn("Linux mode is local but not on Linux — skipping Linux.")
            return
    else:  # remote
        if not m["host"]:
            die("Linux mode is remote but no host set (edit via make release-config).")
        say(f"building Linux binary on {m['host']}:{m['path']} "
            f"(branch {cfg['release']['remote_branch']})")
        remote_sync(m["host"], m["path"], cfg["release"]["remote_branch"])
        run(["ssh", m["host"], f"cd {m['path']} && make gl-repl"])
        arch = subprocess.check_output(
            ["ssh", m["host"], "uname -m"]).decode().strip() or "x86_64"
        bin_src = ("scp", f"{m['host']}:{m['path']}/build/release/gl-repl")

    stage_name = f"gl-repl-{tag}-linux-{arch}"
    stage = dist / stage_name
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)
    if bin_src[0] == "copy":
        shutil.copy(bin_src[1], stage / "gl-repl")
    else:
        say(f"copying binary back from {m['host']}")
        run(["scp", "-q", bin_src[1], str(stage / "gl-repl")])
    (stage / "gl-repl").chmod(0o755)

    stage_music(stage / "assets", mdir)
    (stage / "README.txt").write_text(LINUX_README.format(tag=tag, arch=arch))

    tarball = dist / f"{stage_name}.tar.gz"
    if tarball.exists():
        tarball.unlink()
    say(f"packing -> dist/{tag}/{tarball.name}")
    run(["tar", "czf", str(tarball), stage_name], cwd=str(dist))
    shutil.rmtree(stage, ignore_errors=True)


# --- build / upload orchestration --------------------------------------------
def staged_artifacts(dist: Path):
    return sorted(list(dist.glob("*.zip")) + list(dist.glob("*.tar.gz")))


def do_build(cfg, tag):
    warn_if_dirty(cfg)
    dist = ROOT / "dist" / tag
    dist.mkdir(parents=True, exist_ok=True)
    mdir = music_dir(cfg)
    build_macos(cfg, dist, tag, mdir)
    build_linux(cfg, dist, tag, mdir)
    say(f"staged artifacts in dist/{tag}/:")
    for a in staged_artifacts(dist):
        mb = a.stat().st_size / (1024 * 1024)
        print(f"  {a.name}  ({mb:.0f} MB)", file=sys.stderr)


def do_upload(cfg, tag):
    if not shutil.which("gh"):
        die("gh CLI not found — install it or upload dist/ manually.")
    dist = ROOT / "dist" / tag
    artifacts = staged_artifacts(dist)
    if not artifacts:
        die(f"no artifacts in dist/{tag}/ — run 'make release-build' first.")
    repo = cfg["release"]["repo"]
    exists = subprocess.run(["gh", "release", "view", tag, "--repo", repo],
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL).returncode == 0
    if exists:
        say(f"release {tag} exists on {repo} — uploading (clobber)")
    else:
        say(f"creating draft release {tag} on {repo}")
        run(["gh", "release", "create", tag, "--repo", repo, "--title", tag,
             "--notes",
             f"gl-repl {tag} — macOS .app and Linux binary, music pack bundled.",
             "--draft"])
    run(["gh", "release", "upload", tag, "--repo", repo, "--clobber",
         *[str(a) for a in artifacts]])
    say(f"uploaded {len(artifacts)} artifact(s) to {repo} release {tag} (draft).")
    say(f"review + publish: gh release edit {tag} --repo {repo} --draft=false")


def confirm_upload(cfg, tag):
    if os.environ.get("ASSUME_YES") == "1":
        return True
    if not have_tty():
        warn("not attached to a terminal — skipping upload.")
        say(f"artifacts staged in dist/{tag}/. Upload: make release-upload TAG={tag}")
        return False
    repo = cfg["release"]["repo"]
    ans = input(f"\033[1;35mrelease:\033[0m upload the above to GitHub release "
                f"\"{tag}\" on {repo}? [y/N] ")
    if ans.strip().lower() in ("y", "yes"):
        return True
    say(f"upload declined. Artifacts remain in dist/{tag}/.")
    say(f"Upload later with: make release-upload TAG={tag}")
    return False


# --- arrow-key plan menu (curses) --------------------------------------------
def run_menu(cfg, allow_build=True):
    """Interactive build-plan editor. Returns 'build' or 'quit'. Edits cfg in
    place and can persist to .release.ini. No-op (returns 'build') off a tty."""
    if not have_tty():
        return "build"
    try:
        import curses
    except Exception:
        return "build"

    rows = [
        {"t": "choice", "sec": "macos", "key": "mode", "label": "macOS build"},
        {"t": "text", "sec": "macos", "key": "host", "label": "macOS ssh host",
         "hint": "remote only"},
        {"t": "text", "sec": "macos", "key": "path", "label": "macOS repo path",
         "hint": "remote only"},
        {"t": "sep"},
        {"t": "choice", "sec": "linux", "key": "mode", "label": "Linux build"},
        {"t": "text", "sec": "linux", "key": "host", "label": "Linux ssh host",
         "hint": "remote only"},
        {"t": "text", "sec": "linux", "key": "path", "label": "Linux repo path",
         "hint": "remote only"},
        {"t": "sep"},
        {"t": "text", "sec": "release", "key": "tag", "label": "Release tag",
         "hint": "blank = git describe"},
        {"t": "text", "sec": "release", "key": "repo", "label": "GitHub repo"},
        {"t": "text", "sec": "release", "key": "music_src_dir",
         "label": "Music source", "hint": "blank = auto (favorite/assets)"},
        {"t": "sep"},
        {"t": "action", "key": "save", "label": "Save config to .release.ini"},
    ]
    if allow_build:
        rows.append({"t": "action", "key": "build", "label": "Start build ▶"})
    rows.append({"t": "action", "key": "quit",
                 "label": "Done" if not allow_build else "Quit (cancel)"})

    selectable = [i for i, r in enumerate(rows) if r["t"] != "sep"]

    def _menu(stdscr):
        curses.curs_set(0)
        try:
            curses.use_default_colors()
        except Exception:
            pass
        sel = 0  # index into `selectable`
        flash = ""
        while True:
            stdscr.erase()
            h, w = stdscr.getmaxyx()
            stdscr.addstr(0, 2, "gl-repl release — build plan", curses.A_BOLD)
            stdscr.addstr(1, 2, "(saved to .release.ini)", curses.A_DIM)
            for r_i, r in enumerate(rows):
                y = 3 + r_i
                if y >= h - 3:
                    break
                if r["t"] == "sep":
                    continue
                is_sel = selectable[sel] == r_i
                if r["t"] in ("choice", "text"):
                    val = cfg[r["sec"]][r["key"]]
                    if r["t"] == "choice":
                        shown = "  ".join(
                            f"[{o}]" if o == val else f" {o} " for o in MODES)
                        line = f"{r['label']:<18} {shown}"
                    else:
                        disp = val if val else "(auto)" if "blank" in r.get("hint", "") or r["key"] == "music_src_dir" else "(unset)"
                        line = f"{r['label']:<18} {disp}"
                    if r.get("hint"):
                        line = f"{line:<48} {r['hint']}"
                else:  # action
                    line = f"→ {r['label']}"
                attr = curses.A_REVERSE if is_sel else curses.A_NORMAL
                marker = "› " if is_sel else "  "
                stdscr.addstr(y, 2, (marker + line)[:w - 4], attr)
            if flash:
                stdscr.addstr(h - 2, 2, flash[:w - 4], curses.A_DIM)
            footer = ("↑/↓ move  ◄/► change  "
                      "Enter edit/select  s save  " +
                      ("b build  " if allow_build else "") + "q quit")
            stdscr.addstr(h - 1, 2, footer[:w - 4], curses.A_DIM)
            stdscr.refresh()

            ch = stdscr.getch()
            flash = ""
            r = rows[selectable[sel]]
            if ch in (curses.KEY_UP, ord("k")):
                sel = (sel - 1) % len(selectable)
            elif ch in (curses.KEY_DOWN, ord("j")):
                sel = (sel + 1) % len(selectable)
            elif ch in (curses.KEY_LEFT, curses.KEY_RIGHT) and r["t"] == "choice":
                cur = MODES.index(cfg[r["sec"]][r["key"]]) if cfg[r["sec"]][r["key"]] in MODES else 0
                step = 1 if ch == curses.KEY_RIGHT else -1
                cfg[r["sec"]][r["key"]] = MODES[(cur + step) % len(MODES)]
            elif ch in (curses.KEY_ENTER, 10, 13):
                if r["t"] == "choice":
                    cur = MODES.index(cfg[r["sec"]][r["key"]]) if cfg[r["sec"]][r["key"]] in MODES else 0
                    cfg[r["sec"]][r["key"]] = MODES[(cur + 1) % len(MODES)]
                elif r["t"] == "text":
                    new = _edit_field(stdscr, r["label"], cfg[r["sec"]][r["key"]])
                    if new is not None:
                        cfg[r["sec"]][r["key"]] = new
                elif r["t"] == "action":
                    if r["key"] == "save":
                        save_cfg(cfg)
                        flash = "saved to .release.ini"
                    else:
                        return r["key"]
            elif ch == ord("s"):
                save_cfg(cfg)
                flash = "saved to .release.ini"
            elif ch == ord("b") and allow_build:
                return "build"
            elif ch in (ord("q"), 27):
                return "quit"

    def _edit_field(stdscr, label, current):
        curses.curs_set(1)
        curses.echo()
        h, w = stdscr.getmaxyx()
        stdscr.addstr(h - 2, 2, " " * (w - 4))
        prompt = f"{label} = "
        stdscr.addstr(h - 2, 2, prompt, curses.A_BOLD)
        stdscr.addstr(h - 2, 2 + len(prompt), current)
        stdscr.move(h - 2, 2 + len(prompt) + len(current))
        try:
            raw = stdscr.getstr(h - 2, 2 + len(prompt), w - 6 - len(prompt))
            new = raw.decode("utf-8", "replace")
        except Exception:
            new = None
        finally:
            curses.noecho()
            curses.curs_set(0)
        return new

    return curses.wrapper(_menu)


# --- main --------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(description="Build/package/upload gl-repl releases.")
    ap.add_argument("command", nargs="?", default="all",
                    choices=["build", "upload", "all", "config"])
    ap.add_argument("--no-menu", action="store_true",
                    help="Skip the interactive plan menu.")
    ap.add_argument("--yes", action="store_true",
                    help="Assume yes: skip the menu and the upload prompt.")
    args = ap.parse_args()
    if args.yes:
        os.environ["ASSUME_YES"] = "1"

    cfg = load_cfg()

    if args.command == "config":
        run_menu(cfg, allow_build=False)
        save_cfg(cfg)
        return

    if args.command == "upload":
        do_upload(cfg, effective_tag(cfg))
        return

    # build / all
    if not args.no_menu and os.environ.get("ASSUME_YES") != "1":
        if run_menu(cfg, allow_build=True) == "quit":
            say("build cancelled at the plan menu.")
            return
    save_cfg(cfg)  # persist whatever plan we're about to build
    tag = effective_tag(cfg)
    do_build(cfg, tag)
    if args.command == "all" and confirm_upload(cfg, tag):
        do_upload(cfg, tag)


if __name__ == "__main__":
    main()
