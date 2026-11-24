# Matching tail latency by event and book depth

Three runs at each depth submitted ten million synthetic events with seed
20261120. The benchmark records a seeded 1-in-128 sample of
`MatchingEngine::submit` using the host's monotonic clock, plus the first event.
It reports nearest-rank percentiles. The table gives the median of the three
run-level measurements, in nanoseconds.

| Configured base levels per side | Mean live orders | p50 | p95 | p99 | p99.9 | p99.9 range |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 8 | 48 | 42 | 125 | 250 | 417 | 334–750 |
| 64 | 388 | 83 | 167 | 333 | 500 | 458–875 |
| 512 | 3,100 | 166 | 292 | 500 | 916 | 500–1,375 |

At 512 base levels per side, median market-order p99.9 was 1,708 ns, versus
750 ns for new orders, 542 ns for cancels, and 792 ns for modifies. A given
run contained 6,349 market samples, so that percentile depended on a handful
of samples. The largest market p99.9 across the three trials was 6,750 ns.

The separate six-second macOS stack sample on the 512-level workload included
483 leaf samples in `OrderBook::level_qty`, 471 in `MatchingEngine::execute`,
and 414 in `OrderBook::consume`. Price-level lookup, fill execution and queue
updates are reasonable places to inspect before changing the data structure.
The profile samples the whole process, including workload generation and
periodic depth snapshots. Inlining can move samples into a caller.

## Reproduce

```sh
cmake -S cpp -B cpp/build/tail -DCMAKE_BUILD_TYPE=Release -DFAIRCROSS_ENABLE_ASAN=OFF
cmake --build cpp/build/tail --target faircross_lob_bench -j2
for trial in 1 2 3; do
  for depth in 8 64 512; do
    cpp/build/tail/faircross_lob_bench 10000000 20261120 "$depth"
  done
done

# macOS stack sample. Run separately from latency measurements.
cpp/build/tail/faircross_lob_bench 130000000 20261120 512 > profile-workload.json &
target=$!
sample "$target" 6 1 -file profile.txt
wait "$target"
```

The host was an Apple M5 Pro with 48 GiB, running macOS 27.2.1 and AppleClang
21.0.0. An MLX benchmark was active during the measurement window. The matcher
timings include timer overhead and can capture preemption. The workload
starts with an empty book, and price modifies can add a level outside the
configured base ladder. Results describe this synthetic stream and host.
[Run records and stack counts](m5-tail-latency.json)
