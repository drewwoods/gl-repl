# Release notes

One `<tag>.md` per release — the user-facing body of the GitHub release.
`scripts/release.py` does **not** read these; it creates the draft with a
one-line placeholder note, so attach the real body explicitly:

```sh
gh release edit v0.1 --repo drewwoods/gl-repl \
  --notes-file packaging/release/v0.1.md
```

(`gh release create ... --notes-file <file>` for a release that doesn't exist
yet.) They live here rather than in `docs/` because they ship *outward* — to
someone who has a zip and no checkout — so they describe first-launch and
runtime-dependency steps, not how the tree is built.

Keep the macOS Gatekeeper section in any new release notes. The app is ad-hoc
signed, not Developer-ID signed + notarized, so **every** download hits an
"unidentified developer" prompt and needs the right-click → Open bypass. See
`docs/RELEASE.md` for why that is deliberate, and for the signature
verification the release build enforces.
