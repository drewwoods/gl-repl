#!/usr/bin/env python3
"""Assert every assets/music.json path resolves inside a built web site dir.

Run by the Pages workflow after assembling _site: a manifest entry pointing at
a file that did not get copied would ship a track that 404s on click, and the
browser only finds out when the user presses play.
"""
import json
import os
import sys


def main(site_dir):
    manifest = os.path.join(site_dir, "assets", "music.json")
    with open(manifest, encoding="utf-8") as fh:
        tracks = json.load(fh)
    missing = [t["path"] for t in tracks
               if not os.path.isfile(os.path.join(site_dir, t["path"]))]
    if missing:
        print("manifest references missing files:", *missing, sep="\n  ",
              file=sys.stderr)
        return 1
    print("manifest OK: %d tracks resolve" % len(tracks))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "_site"))
