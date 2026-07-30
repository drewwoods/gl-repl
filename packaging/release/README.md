# Release notes

One `<tag>.md` per release — the user-facing body of the GitHub release. Name
the file after the tag and `make release` picks it up with no further wiring:
the upload step defaults to `(auto)`, which resolves `packaging/release/<tag>.md`
and falls back to gh's one-line placeholder when there is no such file.

Pick a different one from the `make release` / `make release-config` menu
(**Release notes** row, ◄/► to cycle), or per-run:

```sh
make release TAG=v0.1 NOTES=v0.1.md   # bare name = this directory
make release TAG=v0.1 NOTES='(none)'  # force the placeholder
```

Applying a set of notes to an already-uploaded release, without a rebuild:

```sh
gh release edit v0.1 --repo drewwoods/gl-repl \
  --notes-file packaging/release/v0.1.md
```

`README.md` is skipped by the menu listing, so this file never shows up as a
choice. Notes live here rather than in `docs/` because they ship *outward* — to
someone who has a zip and no checkout — so they describe first-launch and
runtime-dependency steps, not how the tree is built.

Keep the macOS Gatekeeper section in any new release notes. The app is ad-hoc
signed, not Developer-ID signed + notarized, so **every** download hits an
"unidentified developer" prompt and needs the right-click → Open bypass. See
`docs/RELEASE.md` for why that is deliberate, and for the signature
verification the release build enforces.
