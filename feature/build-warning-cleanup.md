Build Survey

  I ran a fresh warning pass with:

  make clean
  make test-stubs TEST_JOBS=1
  make sample USE_GL_STUBS=1

  Result: the full stubs test build passed, 14/14 binaries and 1917/1917 tests. The main sample.c target added no warnings. I counted roughly 31 unique compiler warning sites, all in two families:
  possible string truncation and misleading indentation.

  Ranked Warnings

  1. Highest: generated REPL source may silently truncate

  These warnings are most important because they affect command text that the REPL stores, displays, exports, or re-imports. If input approaches MAX_LINE_LEN == 256, the program can produce a
  syntactically partial command instead of rejecting it cleanly.

  Key sites:

  - repl_editor.c:799: variable assignment source, %s%s = %s;%s
  - repl_editor.c:967: for(...) source with raw args
  - repl_core.c:2006: funcN(raw_args);
  - ui_panels.c:1691: color picker writes gluColor / glColor* / glClearColor
  - repl_export.c:1834: import conversion for glPointParameterfv(...)

  Suggested fix: centralize command-source formatting behind a helper that checks snprintf return values. If output does not fit, reject the edit/import with a status message instead of storing
  truncated source.

  2. High: identifier copies may silently truncate names

  These matter because truncated identifiers can collide or refer to the wrong variable. Some are probably safe because the structs were zeroed or a terminator is written afterward, but the intent
  is fragile and GCC is right to complain.

  Key sites:

  - repl_editor.c:615: declaration names into cmd.var_names
  - repl_export.c:1510: imported declaration names into cmd.var_names
  - repl_core.c:2549: function parameter names into local vars
  - repl_core.c:3982: loop var name into visible var frame
  - repl_core.c:3995: function parameter names into visible var frame

  Suggested fix: add a small bounded copy helper for 16-byte identifiers, and make identifier validation reject names that cannot fit before they reach these copies.

  3. Medium: workspace/export metadata truncation

  Lower impact than live command source, but it can affect persistence or diagnostics.

  - repl_export.c:60: workspace header line // @var %s = %s can exceed WORKSPACE_HEADER_LINE_LEN == 96
  - test_repl_core_examples.c:211: failed command detail string can truncate long compiler commands

  Suggested fix: for workspace headers, check snprintf and either enlarge the header line buffer or skip/reject impossible entries. The test warning is only diagnostic quality.

  4. Medium-low: formatting helper warnings with explicit terminators

  These are mostly warning noise, but they hide more serious string warnings in the build output.

  - repl_core.c:876: strncat in normalization
  - repl_core.c:1633: copying raw function text into func
  - repl_export.c:727: fallback copy in format_cmd_source_as_c
  - repl_export.c:111: deferred var name copy

  Suggested fix: replace strncpy/strncat patterns with explicit length-limited memcpy helpers or snprintf(dst, size, "%.*s", ...).

  5. Lowest: misleading indentation in color picker clamps

  These are unlikely to be behavioral bugs: each line intentionally has two independent one-line clamps. But they are cheap to clean up and currently make the build look worse than it is.

  - ui_panels.c:1893
  - ui_panels.c:1929

  Suggested fix: split each clamp onto separate lines or use a small clampf helper.

  Bottom Line

  The build is functionally passing, but the highest-value cleanup is the command-source truncation path. I would fix that first because it can create broken persisted REPL commands at boundary
  lengths. After that, clean up identifier copying, then the color picker indentation warnings.
