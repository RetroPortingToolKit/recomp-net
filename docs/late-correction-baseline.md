# Correct an older input before accepting a later baseline

When both seats discover wrong predictions, a follower can accept a correction
beginning at tick T before it receives the authoritative row for T-1. The
snapshot at T already contains that wrong input. Replaying from T cannot repair
it. The driver previously recorded `UNAPPLIED stage=sealing`, then reported a
baseline digest fork even though an older snapshot could repair the timeline.

Reconciliation now aborts that episode using the existing REALIGN message when
the newly discovered correction precedes its baseline. The existing owed-row
retry then opens an episode reaching the older tick. This specific retry bypasses
the ordinary 30-tick abort backoff: delaying known repair caused hash-chain
stalls while the wrong timeline kept advancing. Other abort backoff is unchanged.
There are no new protocol messages, platform dependencies or simulation fallbacks.

The existing driver test now includes `dual-inline` and `dual-inline-skew`.
They inject wrong predictions on both seats at intervals20/20 and20/19. Both
use the original independent toy-engine timeline comparison and episode ledger;
the existing no-fork/no-stall criteria are unchanged.

Recorded Linux comparison against8c76fb6:

| Case | Original driver | Candidate |
| --- | --- | --- |
| dual-inline | 1 fork, 1 chain stall | 0 forks, 0 stalls |
| dual-inline-skew | 1 fork, 1 chain stall | 0 forks, 0 stalls |

Both candidate timelines agree through their confirmed frontier and drain
without timeouts. Scheduling changes episode counts; these are observed results,
not a guarantee that every run takes an identical correction path. The existing
driver suite and six input/rollback/control checks also pass. A real X2 pair
first exposed the older-row failure at input99/baseline100; its active gameplay
check supplies separate engine-level qualification.

Reproduce with an ordinary tests-enabled Linux build:

```sh
cmake -S . -B build -DRNET_BUILD_TESTS=ON -DRNET_BUILD_EXAMPLES=OFF
cmake --build build --target rb_driver_test
./build/rb_driver_test dual-inline
```

Run the unfiltered driver test for the existing inline, incremental, mixed,
multi-seat, loss, observer, hold and refusal cases. Matched engine/build identity
checks still apply; this change does not establish cross-build compatibility.
