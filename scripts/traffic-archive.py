#!/usr/bin/env python3
"""traffic-archive.py - merge a GitHub traffic snapshot into dated CSVs.

    scripts/traffic-archive.py <snapshot-dir> <output-dir>

GitHub keeps repository traffic for **14 days and then discards it** - there
is no archive, no export, and no way to ask for a window that has rolled off.
Miss two weeks and the numbers are simply gone. This turns each day's API
snapshot into append-only CSVs so the history outlives that window.

Why merge rather than append: the API reports *today* as a partial day, and
the same date will come back with a higher count on tomorrow's run. Rows are
therefore keyed by date and the newest snapshot wins, so re-running the job
(or dispatching it by hand) corrects a day instead of double-counting it.

Note what "uniques" means before reading much into it: GitHub de-duplicates
roughly per-IP per-day, so one person on a laptop and a phone counts twice and
a whole office behind one NAT counts once. It is a trend line, not a headcount.

The referrer and path tables are top-10 leaderboards rather than time series -
the API gives no history for them - so each run stamps its own rows with the
date it was taken and appends. Duplicate (date, name) pairs collapse the same
way, newest wins.

Release download counts are cumulative and never expire, so unlike the traffic
tables they cannot be lost. They are archived anyway because a daily series of
a cumulative counter is the only way to recover the per-day *rate*.
"""
import csv
import datetime
import json
import os
import sys


def load(snapshot_dir, name):
    """Read one snapshot file; missing/!JSON is an empty result, not a crash.

    A partial snapshot is normal and must not lose the rest: a brand-new repo
    has no releases, and an endpoint can fail on its own. Dropping the one
    table beats failing the run and archiving nothing."""
    path = os.path.join(snapshot_dir, name + ".json")
    try:
        with open(path, encoding="utf-8") as fh:
            return json.load(fh)
    except (OSError, ValueError) as exc:
        print("traffic-archive: skipping %s (%s)" % (name, exc), file=sys.stderr)
        return None


def merge_rows(path, key_fields, fieldnames, rows):
    """Rewrite `path` with existing rows updated by `rows`, key order stable.

    Existing rows keep their original order and new keys append, so the file
    reads chronologically and a diff shows only what actually changed."""
    existing = {}
    order = []
    if os.path.exists(path):
        with open(path, newline="", encoding="utf-8") as fh:
            for row in csv.DictReader(fh):
                key = tuple(row.get(f, "") for f in key_fields)
                if key not in existing:
                    order.append(key)
                existing[key] = row

    for row in rows:
        key = tuple(str(row.get(f, "")) for f in key_fields)
        if key not in existing:
            order.append(key)
        existing[key] = {f: str(row.get(f, "")) for f in fieldnames}

    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as fh:
        writer = csv.DictWriter(fh, fieldnames=fieldnames)
        writer.writeheader()
        for key in order:
            writer.writerow(existing[key])
    return len(order)


def day(timestamp):
    """'2026-08-31T00:00:00Z' -> '2026-08-31'."""
    return str(timestamp)[:10]


def main():
    if len(sys.argv) != 3:
        print(__doc__.strip().splitlines()[2].strip(), file=sys.stderr)
        return 2
    snapshot_dir, out_dir = sys.argv[1], sys.argv[2]
    taken = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%d")

    # Views and clones: real per-day series, keyed by their own date.
    for name, series_key in (("views", "views"), ("clones", "clones")):
        data = load(snapshot_dir, name)
        if not isinstance(data, dict):
            continue
        rows = [
            {"date": day(e.get("timestamp")),
             "count": e.get("count", 0),
             "uniques": e.get("uniques", 0)}
            for e in data.get(series_key, [])
            if e.get("timestamp")
        ]
        n = merge_rows(os.path.join(out_dir, name + ".csv"),
                       ["date"], ["date", "count", "uniques"], rows)
        print("%-10s %3d new/updated, %d total days" % (name, len(rows), n))

    # Leaderboards: no history from the API, so stamp the run date.
    for name, label in (("referrers", "referrer"), ("paths", "path")):
        data = load(snapshot_dir, name)
        if not isinstance(data, list):
            continue
        rows = [
            {"date": taken,
             label: e.get("referrer") or e.get("path", ""),
             "count": e.get("count", 0),
             "uniques": e.get("uniques", 0)}
            for e in data
        ]
        n = merge_rows(os.path.join(out_dir, name + ".csv"),
                       ["date", label], ["date", label, "count", "uniques"], rows)
        print("%-10s %3d rows, %d total" % (name, len(rows), n))

    # Release assets: cumulative counters, sampled daily for the rate.
    releases = load(snapshot_dir, "releases")
    if isinstance(releases, list):
        rows = []
        for rel in releases:
            for asset in rel.get("assets", []):
                rows.append({"date": taken,
                             "release": rel.get("tag_name", ""),
                             "asset": asset.get("name", ""),
                             "downloads": asset.get("download_count", 0)})
        n = merge_rows(os.path.join(out_dir, "downloads.csv"),
                       ["date", "release", "asset"],
                       ["date", "release", "asset", "downloads"], rows)
        print("%-10s %3d assets, %d total" % ("downloads", len(rows), n))

    return 0


if __name__ == "__main__":
    sys.exit(main())
