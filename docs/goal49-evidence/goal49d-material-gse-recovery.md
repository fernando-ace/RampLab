# Goal49D material-positive automatic GSE recovery

Source checkpoint: raw private materiality passes; automatic mode/parity validation follows.

Initial live refs:
```
14d63061ef6884733209de5311e2362d7972bbc2 refs/heads/main
85e6e32adf7fb52be466adba8f90cbf819bab55e refs/heads/goal49-selective-integrated-recovery-rebuild-14d6306
```
The literal `refs/heads/goal4` query returned no ref. Started at the exact Goal49 tip.

Toolchain: `/workspace/ramplab-venv/bin` first in PATH; CMake/CTest 3.31.10,
Ninja 1.13.0.git.kitware.jobserver-pipe-1, g++ 14.2.0. Reused `build/`.

The existing `AutomaticIntegratedGseRecoveryDiagnostic` is the foundation. The
historical fixture already recovered through normal shortest-distance reactive
fleet dispatch and tied at 109 seconds, so it could not establish materiality.

Synthetic facts retain AX101 / TO-AX101 and its critical baggage-unload task 3.
Vehicle 2 outages at 130 seconds; healthy idle compatible Vehicle 3 is the normal
backup. A second idle compatible cart (4) has a nearer depot, with a 100 m access
road that closes at 131 and reopens at 800 seconds. Normal REACTIVE dispatch
selects cart 4 by route distance. Vehicle 3's approach is unaffected. No production
logic, mode semantics, comparison rule, threshold, or other recovery action changed.

At the same 130-second parent boundary, Tier 0 and `vehicle:3:3` both privately
finish safely. Tier 0 worst delay is 532 seconds; reassignment worst delay is 0.
The existing materiality function accepts the 532-second gain against its unchanged
300-second threshold. Parent snapshot and event history remain equal after both
evaluations. The raw focused test passes (see accompanying output).

Categorical failure is unnecessary here: legitimate reactive fallback succeeds,
while the permitted delay criterion establishes material benefit without weakening
normal fleet behavior. KAUO remains DATA_BLOCKED; all operations are synthetic.
Goal49 remains unmerged. Goal50 and the eight-family matrix have not started.
