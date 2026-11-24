# lob-sim v1.0.0

Lob-sim is a single-instrument continuous matching engine and a Python
environment for policies that use depth, FIFO queue position, and execution
feedback. The Python distribution uses version `1.0.0`.

## Install and run

The qualification matrix targets Linux Ubuntu 24.04 on x86_64 and ARM64, with
CPython 3.10 or 3.12 and NumPy 1.26 or later. Source builds require a C++20
compiler with 128-bit integer support, Python development headers, CMake 3.24
or later, and a build tool such as Ninja or Make. CI uses Clang. The build
backend installs nanobind into its isolated build environment. The wheel is
specific to the stated Linux architecture and CPython version.

From a checkout, build and install in a virtual environment and run both
normalized replay fixtures through the installed example.

```sh
python3 -m venv .venv
. .venv/bin/activate
python -m pip install .
python -m lob_sim --replay fixtures/continuous/session.csv
python -m lob_sim --replay fixtures/continuous/session.bin --format binary
```

The example quotes a buy, receives a partial fill, cancels the remainder, and
finishes the four-step episode before replaying a fixture. To build distributable
artifacts, run `python -m pip install build` followed by `python -m build`.
Install the `.whl` matching your architecture and CPython in a clean virtual
environment with `python -m pip install /path/to/lob_sim-1.0.0-wheel.whl`. The
`.tar.gz` source distribution is also installable with pip.

## C++20 interface

Include `faircross/continuous/matching_engine.hpp` and use
`faircross::continuous`. With `add_subdirectory(cpp)`, link the CMake target
`faircross_continuous`. Neither this target nor `./scripts/check.sh` requires a
Python package.

```cpp
#include <variant>
#include "faircross/continuous/matching_engine.hpp"

using namespace faircross::continuous;

MatchingEngine engine;
auto parsed = Event::create(MsgType::New, 10, 0, 20, 101, Side::Sell);
if (const auto* event = std::get_if<Event>(&parsed)) {
    auto result = engine.submit(*event);
    auto asks = engine.book().depth(Side::Sell, 5);
}
```

[ADR-019 and ADR-020](DECISIONS.md#adr-019-native-continuous-event-contract-and-level-3-order-lifecycle)
record the event, ID lifecycle, and priority decisions. Each book is one
session. Native IDs and timestamps are uint64, including zero. Prices are
positive int64 ticks, quantities are positive uint64 lots, and timestamps are
nanoseconds on the caller's session clock.

| Event | ID | Quantity | Price | Side |
| --- | --- | --- | --- | --- |
| `New` | Unused | Initial lots | Required | Required |
| `Cancel` | Live | Lots to remove, up to remaining | Omitted | Omitted |
| `Modify` | Live | Replacement **remaining** lots | Required | Inherited |
| `Market` | Unused | Requested lots | Omitted | Required |

`Event::create` returns an `Event` or typed `Status` for invalid message,
ID, timestamp, quantity, price, or side. `MatchingEngine::submit` returns
`EventResult` with `status`, ordered `fills`, `remaining` lots under the
event's ID, and `canceled` lots. `canceled` covers explicit cancellations and
unfilled market volume. For a rejected event both quantities are zero, fills
are empty, and the book stays at the previous state. A same-price replacement
with lower remaining quantity removes the difference under the modify
operation. Each fill is one trade containing both IDs, taker side, price,
quantity, and taker timestamp.

Limit and market orders consume the best eligible opposite price first and
fill the oldest order at that price, using submission order for FIFO. A
limit order rests its leftover lots at its limit. A market order retires
its ID immediately even if its entire quantity is unfilled. Cancel and
partial fill preserve priority. Same-price modifications that maintain or
decrease remaining quantity also preserve priority and the resting timestamp.
A price change or size increase rejoins at the tail with the modify timestamp
if any volume remains after crossing. Direct submission uses the order of
calls, including when timestamps tie or decrease.

`DuplicateOrderId` covers both live and retired IDs on new and market orders.
Cancel and modify distinguish `NotFound` and `AlreadyClosed`. Cancellation
past the live quantity returns `InvalidQuantity`. A resting level's aggregate
quantity exceeding uint64 returns `Overflow`. `OrderBook::apply` remains
available for direct lifecycle management without matching, including
`UnsupportedEvent` for market orders.

`engine.book()` exposes the read-only L3 state. `find(id)` returns a resting
order or no result. `depth(side, n)` returns at most `n` levels best to worst,
including price, lots, and order count. `queue(side, price)` returns the
level's FIFO IDs. `queue_position(id)` returns the live order, lots ahead, and
orders ahead at that price, or no result. `active_orders()` returns the live
order count.

The batch auction continues to use its own rules, proofs, and frozen fixtures.
ITCH feeds contain exchange-reported executions and use the separate
reconstruction path in `faircross/marketdata/replay.hpp`. The two normalized
formats below submit input orders and lifecycle messages to the continuous
matcher.

## Normalized event replay

`faircross/continuous/replay.hpp` provides `EventReader(std::istream&, ReplayFormat)`
and `replay(engine, reader, on_event)`. The optional callback gets the event
and outcome after each submission. In C++, callback references are valid for
the duration of the call. `ReplayReport` contains a submitted event count
(including engine rejections) and an optional diagnostic. The reader retains
one input record at a time, and the callback controls whether outcomes
accumulate in user memory.

Files require nondecreasing timestamps. Equal timestamps keep file order.
Malformed data, a version mismatch, I/O failure, or a decreasing timestamp
stops replay before submission of that record and preserves the committed
prefix. A complete, valid event that the engine rejects is counted and
reported to the callback. Replay then continues. Each diagnostic contains
`error`, `location`, `message`, and an applicable native `status`, if any.

The checked-in [CSV](../fixtures/continuous/session.csv) and
[binary fixture](../fixtures/continuous/session.bin) describe the same
12 events and include two business-rule rejections.

### CSV v1

Header `timestamp_ns,type,order_id,side,price_ticks,qty` on physical line 1.
Fields contain case-sensitive `New`, `Cancel`, `Modify`, `Market`, `Buy`,
`Sell`, or decimal integers as applicable. Leave absent fields empty. Quotes,
embedded commas, spaces, BOMs, and blank records are invalid. Line endings
may be LF or CRLF, and the final row can end at EOF. The record limit is 256
bytes excluding the line ending. Diagnostic locations are 1-based lines.

### Binary v1

The header is eight bytes. It starts with ASCII `FCLB`, then a little-endian
uint16 version of `1` and a little-endian uint16 reserved value of `0`. Each
record is 36 bytes. All multibyte fields are little-endian.

| Offset | Size | Encoding |
| ---: | ---: | --- |
| 0 | 1 | Type as uint8, `0` New, `1` Cancel, `2` Modify, `3` Market |
| 1 | 1 | Side as uint8, `0` omitted, `1` Buy, `2` Sell |
| 2 | 2 | Reserved, zero |
| 4 | 8 | Timestamp as uint64 |
| 12 | 8 | Order ID as uint64 |
| 20 | 8 | Price as two's-complement int64, zero for omitted |
| 28 | 8 | Quantity as uint64 |

Binary diagnostics number records from 1 and identify the header as 0. EOF
between records completes replay. A partial fixed-length record produces
`Truncated`. Future incompatible binary changes require a new header version.
CSV v1 is identified by its fixed header.

## Python API

`lob_sim.Event.create(type, order_id, timestamp_ns, qty, price_ticks=None, side=None)`
takes `MsgType` and `Side` enums and returns `Event` or `Status`. Its numeric
fields accept integer values or objects implementing `__index__`. Out-of-range
values, floating-point values, and Python booleans return the corresponding
field status. `lob_sim.MatchingEngine.submit(event)` returns `EventResult`,
with a list of `Trade` objects. `depth`, `queue`, and `queue_position` expose
the native counts and integer values directly.

Use `engine.replay(str(path), ReplayFormat.Csv, callback)` or
`ReplayFormat.Binary` for a streaming replay into an engine. A callback may
retain its event and result objects. `env.replay(path, format="csv", on_event=None)`
takes a string or path-like input and returns `(engine, report)` for a
separate fresh session. It accepts `"csv"`, `"binary"`, or a `ReplayFormat`.

### Observations

`engine.observation(depth, inventory=0, unrealized_pnl=0).array()` and the
array from `BookEnv` both have shape `(4 * depth + 4,)` and dtype `float32`.
They are 1-D and C-contiguous. Rows are represented as consecutive
`price_ticks, qty_lots` pairs.

| Indices | Field |
| --- | --- |
| `0 : 2N` | N bid pairs, highest price first |
| `2N : 4N` | N ask pairs, lowest price first |
| `4N` | Best ask less best bid in ticks, zero if either side is empty |
| `4N+1` | `(bid lots - ask lots) / (bid lots + ask lots)` across displayed levels, zero when both sides are empty |
| `4N+2` | Signed inventory in lots |
| `4N+3` | Unrealized PnL in tick-lots |

Missing levels are zero pairs. The array is a read-only view of a native
snapshot. Calling `array()` twice on the same snapshot shares the allocation.
Each snapshot owns distinct storage, and the array keeps it alive after
subsequent steps, resets, or deletion of the engine or environment. Native
storage is passed to NumPy without a copy at the language boundary. Exact
integer depth and trade fields remain available through the native bindings
when a value exceeds float32 integer precision.

### Policy environment

```python
from lob_sim import Action, ActionKind, BookEnv

env = BookEnv(depth=3, scenario="synthetic", scenario_steps=4)
obs, info = env.reset(seed=0)
for action in (Action(ActionKind.QUOTE_BUY, size=8.0), 0, 5, 0):
    obs, reward, terminated, truncated, info = env.step(action)
```

`reset(seed=None)` returns `(obs, info)` and starts a new session. The
default seed is zero. Seeded initial depth posts four levels per side around
the 100-tick reference with quantities from 6 to 12 lots. The synthetic
scenario sends a seeded market order of 1–3 lots every second step, after
the agent action. The quiet scenario has only the seeded initial liquidity.

| Discrete code | Action |
| ---: | --- |
| 0 | Hold |
| 1 / 2 | Quote buy / sell for four lots at the reference |
| 3 / 4 | Market buy / sell for four lots |
| 5 + i | Cancel the own live order at slot i |

The continuous form is `Action(kind, size=1, offset=0, cancel_index=None)`.
Sizes and offsets are real-valued controls rounded down to whole lots and
ticks. Size for quote or market actions must round to 1–4,294,967,295 lots.
Offset for a quote must round to 0–99 ticks. Buy quotes use reference minus
offset and sell quotes use reference plus offset. Hold and cancel use the
default size and offset. Markets use the default offset. Cancel requires a
nonnegative slot and removes the order's full remaining quantity. Slots are
zero-based in `info["queue"]`, ordered by submission among live own orders
and compacted after cancellations or fills. Out-of-range or incompatible
actions raise `ValueError` before advancing the episode.

`step(action)` returns `(obs, reward, terminated, truncated, info)`. Natural
completion occurs at `scenario_steps` (32 by default). A smaller `max_steps`
truncates at that step. Natural completion takes precedence when the two
limits coincide. A call before reset or after either end flag raises
`RuntimeError`.

`info["action_result"]` carries the native result for the agent action
(`None` for hold). `info["fills"]` includes all step trades, in agent then
background order. `info["queue"]` lists `(order_id, QueuePosition)` pairs.
It also includes step number, signed inventory, integer cash, mark, realized
PnL, unrealized PnL, and equity. The mark is the best bid/ask midpoint when
both sides are present. With one or both sides empty it uses the last trade
price or keeps the previous mark if there is no trade. An own-account trade
updates the queue and tape and nets to zero in the agent ledger.

Cash starts at zero, decreases on buys, and increases on sells by fill price
times lots. Inventory is marked at the current mark and cost basis uses
average open price. Unrealized PnL is marked inventory value less signed
open cost. Realized PnL is cash plus signed open cost. Reward is the change
in equity (`cash + inventory * mark`) over the step. Matching and cash use
integer arithmetic, while the environment carries fractions for midpoint
and average cost and exports floating-point observations and rewards.

## Performance and qualification

Run the release and sanitizer builds separately from a clean checkout.

```sh
CXX=clang++ python3 scripts/benchmark_lob_sim.py

CXX=clang++ cmake -S cpp -B cpp/build/lob-sanitize \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DFAIRCROSS_ENABLE_ASAN=ON
cmake --build cpp/build/lob-sanitize --target faircross_lob_tests -j2
python3 scripts/check_lob_sim.py --native cpp/build/lob-sanitize/faircross_lob_tests \
  --python .venv/bin/python --output cpp/build/lob-evidence/tests.json
CXX=clang++ ./scripts/check.sh
```

The benchmark uses a single thread, seed 20261120, 64 initial price slots per
side with four order slots per base price, and sequential new IDs. It
refills inactive slots and mixes partial and full cancels, stale-ID
cancels, price and size modifies, and market sweeps. JSON reports the
observed depth and event mix. Latency percentiles use an independent,
seeded 1-in-128 sample of `submit`, with nearest-rank aggregation. Elapsed
time includes workload generation, matching, and sampling. Peak RSS is
whole-process high-water memory. The budgets are below ten seconds and
below 256,000,000 bytes for each of the 5 million and 10 million event runs.

The [tail-latency study](benchmarks/m5-tail-latency.md) reports each event type
at three book depths and includes a separate stack sample.

The verification workflow retains per-platform benchmark JSON, sanitizer
and Python timing evidence, installable wheels and source distributions,
SHA-256 checksums, and the candidate verdict for 90 days. The qualification
job requires both native architectures, both supported Python versions,
and the existing frozen-vector gate. It binds results to the tested
revision and publishes a per-criterion checklist in the qualification
artifact and workflow summary.

## v1.0 release notes

- Added continuous price-time FIFO matching, limit and market execution,
  partial cancels, deterministic modifies, and native depth and queue views.
- Added streaming CSV v1 and little-endian binary v1 replay with diagnostics.
- Added the nanobind Python module, zero-copy NumPy snapshots, and seeded
  `BookEnv` policy episodes for execution and market-making experiments.
- The continuous book is single-threaded and in-memory. Exact ID retirement
  takes one bitmap block per populated group of 64 IDs. Widely separated
  IDs use more memory than the sequential IDs in the reference workload.
  The simulator models one instrument, with no exchange connectivity,
  latency model, fees, margin, or self-trade prevention.

## Prepare the GitHub release

After review and merge, use the verification run for the exact commit on
`main`. Confirm the `lob-sim-qualification` verdict and verify the
`SHA256SUMS` from each packaging job. The four wheels target Ubuntu 24.04
on x86_64 and ARM64, with CPython 3.10 and 3.12. Compare the source archives
before selecting one for the release.

Use the verified commit for the `v1.0.0` tag. Attach the four tested wheels,
the source archive, their checksums, and the qualification verdict. Include
the supported environments and the simulation limits above in the release
description.
