# Frozen benchmark corpus

This directory (`bench/bench-data`) is a snapshot of the built-in example
catalog used by `bench_repl`. The benchmark deliberately reads these sources through the
generated embedded data, not from `examples/`, so changing or renaming a live
example cannot change benchmark workload or identifiers.

Refresh this corpus only as an intentional benchmark-baseline change by
copying the catalog and scene sources from `examples/`, then rerun `make bench`.
