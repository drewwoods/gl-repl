# Third-Party Licenses

This project bundles the following third-party components. Their licenses are
reproduced (or pointed to) below; all are permissive.

---

## freeglut

- **Upstream:** <https://github.com/freeglut/freeglut> (currently vendored from
  a fork that adds the headless OSMesa backend and env/signal-gated frame
  capture on the windowed backends; see `VENDORED.txt`).
- **Vendored at:** `third_party/freeglut/` (built as a static library on macOS —
  Cocoa backend by default, or the headless OSMesa backend under
  `make ... FREEGLUT_OSMESA=1`). See `third_party/freeglut/VENDORED.txt` for the
  exact pinned source + commit; at time of writing it is
  `3523ba740cdf713a268040f27e89d0a60350b2f7`.
- **License:** X-Consortium / MIT-style (the freeglut license).

The full contributor list lives in `third_party/freeglut/AUTHORS` (current
maintainers: John F. Fay, Diederick C. Niehorster, John Tsiombikas). The
license text, reproduced verbatim from `third_party/freeglut/COPYING`:

```
  Freeglut Copyright
  ------------------

  Freeglut code without an explicit copyright is covered by the following
  copyright:

  Copyright (c) 1999-2000 Pawel W. Olszta. All Rights Reserved.
  Permission is hereby granted, free of charge,  to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction,  including without limitation the rights
  to use, copy,  modify, merge,  publish, distribute,  sublicense,  and/or sell
  copies or substantial portions of the Software.

  The above  copyright notice  and this permission notice  shall be included in
  all copies or substantial portions of the Software.

  THE SOFTWARE  IS PROVIDED "AS IS",  WITHOUT WARRANTY OF ANY KIND,  EXPRESS OR
  IMPLIED,  INCLUDING  BUT  NOT LIMITED  TO THE WARRANTIES  OF MERCHANTABILITY,
  FITNESS  FOR  A PARTICULAR PURPOSE  AND NONINFRINGEMENT.  IN  NO EVENT  SHALL
  PAWEL W. OLSZTA BE LIABLE FOR ANY CLAIM,  DAMAGES OR OTHER LIABILITY, WHETHER
  IN  AN ACTION  OF CONTRACT,  TORT OR OTHERWISE,  ARISING FROM,  OUT OF  OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

  Except as contained in this notice,  the name of Pawel W. Olszta shall not be
  used  in advertising  or otherwise to promote the sale, use or other dealings
  in this Software without prior written authorization from Pawel W. Olszta.
```

---

## miniaudio

- **Upstream:** <https://github.com/mackron/miniaudio>
- **Vendored at:** [`include/miniaudio.h`](include/miniaudio.h) (single-header library).
- **Author:** David Reid (mackron@gmail.com).
- **License:** dual-licensed — your choice of **public domain (Unlicense)** or
  **MIT No Attribution (MIT-0)**. Neither option legally requires attribution;
  it is acknowledged here as a courtesy. The full text of both license options
  is at the end of [`include/miniaudio.h`](include/miniaudio.h).
