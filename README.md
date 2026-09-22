# Fabric Upgrade Manager

Vendor-neutral **Fabric OS upgrade intent and execution lifecycle runtime** from
Summon Software Labs.

Fabric Upgrade Manager (FUM) owns *upgrade intent and execution lifecycle* for
software, firmware and control-plane components: what is being upgraded, against
which artifact, on which targets, under which authority, at which generation,
with which evidence, and with what rollback exposure. It does **not** own the
knowledge or the mechanisms that belong to other Fabric systems.

```
                 Fabric Compatibility Registry      Change Planner / Rollout Fabric
                              |                                  |
                              v                                  v
   Drain/Maintenance Fabric -> [        Fabric Upgrade Manager        ] <- Configuration Fabric
                              ^                                  ^
                              |                                  |
                        target adapters (vendor mechanics)   artifact sources
```

## Systems boundary (what this repository is, and is not)

Owned here:

* upgrade campaigns, stages, attempts and their lifecycle;
* the preflight decision (inventory + compatibility + integrity + skew + claims);
* execution admission, fencing, idempotent retry and restart reconciliation;
* post-activation verification and the success criterion;
* rollback eligibility, irreversible-step marking and refusal;
* the provenance ledger of artifact -> target -> campaign generation;
* deterministic decisions and explanations.

Deliberately **not** implemented here (bound through ports, or refused as
`not_integrated`):

| System | Port | Behaviour when not wired |
| --- | --- | --- |
| Fabric Compatibility Registry | `CompatibilityRegistryPort` | preflight refuses: compatibility cannot be asserted |
| Change Planner / Rollout Fabric | `RolloutFabricPort` | no external sequencing; admission is refused when configured as required |
| Drain / Maintenance Fabric | `DrainFabricPort` | a plan that needs service removal is refused at preflight |
| Configuration Fabric | `ConfigurationFabricPort` | configuration delivery is skipped, and said so in the decision trail |
| Inventory source | `InventorySourcePort` | required: the runtime will not invent targets |
| Artifact source | `ArtifactSourcePort` | artifact bytes cannot be resolved: preflight refuses |
| Vendor installation mechanics | `TargetAdapterPort` | strategy is refused unless the adapter truthfully claims it |

## Implemented capabilities

### Identities and typed semantics

Strongly typed identities for every domain object: `ComponentId`, `ArtifactId`,
`BuildId`, `TargetId`, `CampaignId`, `StageId`, `AttemptId`, `AuthorityId`,
`PolicyId`, `EvidenceId`, `DecisionId`, `StepId`, `RecordId`, `AdapterId`,
`SignerId`, plus monotonic `Generation`, `Epoch`, `Incarnation`, `Revision` and
`Sequence` counters and a validated SHA-256 `Digest`. Identities are never
interchangeable strings or integers.

### Artifact metadata

Digest (SHA-256) and size, component identity and type, build identity,
platform constraints, capability requirements, schema/protocol versions, signer
provenance hooks (signer, key id, algorithm, attestation reference, source
revision, build system) and rollback compatibility including the declared
irreversible boundary and the explicit irreversible step identities. An artifact
that is not reversible **must** declare its boundary: the model refuses it
otherwise.

### Preflight

Before anything executes, preflight:

1. resolves the current inventory (freshness is bound to the process incarnation);
2. verifies the artifact payload against the declared digest and size;
3. queries the compatibility registry per target;
4. checks platform, capability, strategy claims and rollback availability;
5. assesses the planned version skew against the declared budget;
6. issues a ticket bound to the campaign generation, epoch and incarnation.

A failed preflight leaves the campaign blocked, and `start` refuses.

### Lifecycle

`proposed -> validated -> prepared -> staged -> activating -> verifying ->
completed`, with `blocked`, `paused`, `failed`, `rollback-planned`,
`rolling-back` and `rolled-back`. Every transition has explicit guards
(preflight currency, artifact integrity, recorded rollback plan, irreversible
acknowledgement, adapter strategy support, staging completeness, activation
completeness, fresh passing verification, skew budget, drain obligations,
rollback eligibility, rollback completeness). Guards default to "not proven":
a transition is refused unless the caller established the precondition.

### Strategies and truthful adapter claims

`in-place`, `restart-based`, `redundant-pair-rolling` and
`control-plane-generation-handoff`. A strategy is planned only when every
target's adapter claims it; the claim set itself is validated for internal
consistency (for example: generation handoff requires the handoff capability,
rolling replacement requires a health probe, parallel activation limits are
bounded). When an adapter cannot do something, the plan is rejected instead of
the runtime pretending.

### Rollback eligibility and irreversible steps

The rollback plan, the return artifact and the eligibility verdict are recorded
**before** the first activation. An artifact that declares an irreversible
boundary cannot be rolled back after that boundary is crossed, and the refusal
is explicit (`irreversible` with the declared boundary). Irreversible steps are
named in the plan and must be acknowledged by the operator before execution.

### Health gates and verification

Activation is gated on health evidence: from the configured health probe, or
from the adapter's own observation. Evidence freshness is a function of the
observing incarnation, the observation time and the declared validity window; a
target that has no observable health (for example a process that is not running
yet) is recorded as *not applicable* rather than silently passing.

Success is post-activation verification, never installer exit alone: the
observed version must match the artifact, the build must be reported, the
evidence must be fresh and the target must be healthy.

### Fencing, idempotency and reconciliation

Every attempt carries a fence token (campaign, generation, incarnation, attempt,
authority) and every commit re-checks it: stale attempts cannot mutate current
state, and a rejected commit is recorded as a fencing decision. Adapter
operations carry idempotency keys; a lost acknowledgement is resolved by
observing the target before anything is repeated, so an activation that already
took effect is reported exactly once. Restart reconciliation abandons work owned
by a previous incarnation and blocks campaigns that were mid-flight, requiring a
fresh preflight before execution resumes.

### Version skew budgets

A budget declares how far a live target may trail the newest version in flight
(major, minor, patch) plus explicit waivers. Skew is assessed before activation
and after every activation; exceeding it blocks the campaign rather than rolling
forward. A target running ahead of the campaign artifact is a violation. Skew is
never taken from persisted state: it is recomputed from live attempts and the
current inventory, so a deserialized assessment can never masquerade as fresh
evidence.

### Provenance ledger

Append-only, hash-chained records of what artifact and version were applied to
which target, under which campaign, generation, authority and incarnation, with
the outcome (`applied`, `verified`, `rolled-back`, `failed`, `aborted`,
`superseded`). The chain is verified on load and on inspection; tampering,
reordering and truncation are detected.

### Durable state

A versioned, integrity-checked journal (magic + format version + per-record
CRC-32C + SHA-256 hash chain + strict sequence numbers), with conservative
recovery: replay stops at the first record that cannot be trusted, nothing after
it is used, the damage is reported with its offset and cause, and appending after
damage is refused until an operator explicitly authorises dropping the
unverifiable bytes. Compaction rewrites a snapshot and continues the chain head,
so continuity is provable across compaction. Records that replay structurally but
cannot be applied to state are counted and reported, never silently dropped.

### Adapters

`TargetAdapterPort` separates generic governance (all of the above) from
vendor-specific installation mechanics: `observe`, `prepare`, `activate`,
`verify`, `rollback`, plus declared claims. Adapters never see campaign state,
policy or the ledger; they receive a target, an artifact, a fence token, an
idempotency key and a payload path.

### CLI

```
fum plan        # build and print a plan from a request document
fum run         # create, preflight, start and report in one process
fum create      # create a campaign
fum preflight   # resolve inventory, query compatibility, decide admission
fum start       # admit and execute
fum pause | resume | abort
fum rollback    # plan or execute a rollback
fum status | explain | inventory | provenance | decisions | stats
```

Global options (`--state`, `--inventory`, `--registry`, `--artifacts`,
`--control-plane`, `--executor`, `--json`, ...) may appear before or after the
command. Exit codes: 0 success, 1 runtime failure, 2 usage, 3 refused by policy
or precondition, 4 not found, 5 not integrated or unsupported.

Because a preflight ticket is bound to the process incarnation that issued it,
`fum start` re-runs preflight inside its own process when no current ticket
exists, and reports that it did.

## Architecture

```
include/fum/
  core/      result, checked arithmetic, ids, digest, sha-256, crc-32c, json, time, log, fs
  model/     version, artifact, target, evidence, lifecycle, skew, provenance, decision,
             policy, campaign (plan, stage, attempt, ticket, observation)
  ports/     compatibility registry, rollout, drain, configuration, health, artifact
             source, provenance ledger, target adapter + adapter registry
  store/     journal (framed, CRC + hash chain), durable state (recovery, compaction)
  runtime/   bounded worker pool with cancellation and honest shutdown accounting
  engine/    planner, preflight, verification, reconciliation, engine (lifecycle driver)
  adapters/  synthetic fabric (deterministic), local_process (real OS processes)
src/cli/     file-backed integration ports and the command line
```

### Concurrency audit

An explicit audit of the concurrency surface, performed on the final code:

* **One engine mutex.** All durable state (`DurableStore`) is guarded by a single
  `std::mutex`. There is no second state lock and no lock hierarchy to invert.
* **No I/O under the lock.** Adapter calls, port calls, logger calls and worker
  pool submission never happen while the engine mutex is held. Work is planned
  under the lock, executed outside it, and committed under the lock again with a
  fence check.
* **No callbacks under the lock.** Adapters, ports and user code are never
  invoked from a locked region; decisions recorded during a locked commit are
  appended to the journal under the same lock (a bounded, synchronous local write)
  and published through the logger after the lock is released.
* **Read-modify-write.** Every commit re-reads the campaign and attempt records
  from the store and re-checks the fence before mutating, so a stale snapshot can
  never overwrite newer state. Recovery reads attempts from the store at decision
  time rather than from a snapshot taken earlier in the same commit.
* **Nested acquisition.** The only nesting is "engine mutex held, journal file
  writer used", which is a leaf: the journal takes no locks of its own beyond the
  OS file handle. The worker pool lock is never held while the engine mutex is
  taken (pool submission happens outside the engine lock), and the engine mutex
  is never taken from a pool thread while the pool lock is held.
* **Shutdown joins.** `Engine::shutdown` stops admission (`shutting_down_`),
  then stops the pool (which discards queued work and joins workers), then closes
  the journal. In-flight adapter calls must return for the join to complete; that
  is a property of external adapters, and it is documented rather than hidden
  behind a timeout. Commands called after shutdown fail with `cancelled`.
* **Worker pool self-join.** `WorkerPool::drain()` refuses to run on a pool
  thread (that would be a self-join deadlock); the engine driver executes one
  item of every batch on its own thread, so even a single-worker pool makes
  progress.
* **Cancellation.** A cancelled pool discards queued work without executing it
  and counts it; a cancelled or shut-down engine never publishes success. Tests
  assert that a stale in-flight commit produces no provenance record.
* **Read paths.** Inspection methods (`status`, `campaigns`, `explain`,
  `provenance`, `decisions`, `stats`) take the same mutex; they never upgrade it
  and never call out.

## Testing and proof surfaces

Every test runs plainly; **no test uses a timeout** and CTest is configured with
no timeout property. A hanging test is treated as a defect.

| Suite | What it proves |
| --- | --- |
| `test_core` | SHA-256/CRC vectors, strict bounded JSON parser (malformed, truncated, over-deep, over-large), identity validation, version algebra, timestamps, checked arithmetic, atomic and bounded file I/O |
| `test_model` | lifecycle transitions and refusals, terminal states, guard requirements, evidence freshness (incarnation, expiry, future stamps), skew budgets, ledger chain tampering, decision fingerprint determinism, artifact/target/policy/campaign serialization round trips |
| `test_engine_lifecycle` | rolling/in-place/pair-rolling/handoff campaigns, incompatible matrices, bad digest, unacknowledged irreversible steps, rollback eligibility and execution, pause/resume/abort, stale tickets, skew blocking, unsupported strategies, health-gate hold, drain delegation, not-integrated refusals |
| `test_engine_faults` | lost acknowledgements (with and without effect), activation crash, installer success without a version change, stale and foreign-incarnation verification evidence, prepare failure, controller restart reconciliation (real reopen and seeded mid-flight state), stale commit fencing against a paused campaign, shutdown semantics, configuration/rollout refusals |
| `test_store` | journal round trip and chain continuity, torn tail, bit flips, reordering/edited history, record and file bounds, store compaction, refused extension after damage, ledger across restarts, store bounds |
| `test_concurrency` | bounded pool accounting, self-join refusal, deterministic cancellation, concurrent campaigns on 4 worker threads, concurrent readers during execution, repeated open/close cycles, sequential campaign churn |
| `test_property` | 12 seeded randomized state machines (random faults and operator actions) asserting lifecycle, evidence, ledger and terminal-state invariants; cross-instance plan and decision determinism; preflight refusal never executes; skew never exceeded while running |
| `test_adversarial` | 4000 seeded JSON mutations (parse-or-refuse, re-serialize stability), 4000 random identifier/version/digest/timestamp inputs, absurd artifact metadata, journal header corruption and every truncation length, corrupted record payloads, hostile inventory/registry answers, bounded resources end to end |
| `test_local_process` | the real local-process path: an actual control-plane executable is staged, restarted and verified over TCP loopback; kill/restart with fresh process incarnations; incarnation and token fencing over the wire; malformed and oversized frames do not kill the server; lost acknowledgement; activation crash; two-stage rollback back to the previous build |

### REAL / SYNTHETIC / UNSUPPORTED

* **REAL**: OS processes (create, terminate, restart), TCP loopback sockets with
  framed transport, file I/O and atomic replacement, the durable journal and its
  integrity checks, threads and worker pools, MSVC builds, the CMake package and
  a downstream consumer.
* **SYNTHETIC**: the synthetic fabric (adapter, inventory, registry, artifact
  source, health probe, drain fabric, rollout fabric, configuration fabric,
  ledger) used to exercise governance deterministically, including fault
  injection. It is labelled synthetic everywhere it appears and claims no vendor
  behaviour.
* **UNSUPPORTED / NOT CLAIMED**: vendor firmware or hardware, switch or NIC
  interactions, RDMA/NVLink/multi-GPU, multi-node clustering, real compatibility
  databases, real change-management or drain systems, signature verification
  (FUM records and enforces declared provenance hooks; it is not a signing
  authority). The local-process adapter upgrades a *test control plane* it owns;
  it makes no hardware claim.

## Benchmarks

`fum_bench_upgrade` measures **completed** upgrades with durable journaling on
(every record fsynced), never submission latency:

```
scenario: campaigns=20 targets=8 workers=1 executor=deterministic-inline
  completed campaigns : 20
  verified targets    : 160
  provenance records  : 320
  elapsed             : 8.567 s
  completed upgrades/s: 2.3
  verified targets/s  : 18.7

scenario: campaigns=20 targets=8 workers=4 executor=thread-pool
  completed campaigns : 20
  verified targets    : 160
  provenance records  : 320
  elapsed             : 8.358 s
  completed upgrades/s: 2.4
  verified targets/s  : 19.1
```

Durable journaling dominates: the throughput is bounded by fsync per record, not
by the executor. The threaded scenario is intentionally not faster — it
demonstrates that concurrency does not weaken durability or governance. Numbers
are from a Windows 11 x64 development host with MSVC 19.44; treat them as a
characterisation, not a guarantee.

## Building

```
cmake -S . -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel
ctest --test-dir build/release --output-on-failure
```

Requirements: CMake 3.25+, a C++20 compiler (MSVC 19.3x with `/W4 /WX`, or GCC
11+/Clang 14+ with `-Wall -Wextra -Werror`), and Ninja or any other generator.
First-party code compiles with zero warnings under the configured warning set.

Options: `FUM_BUILD_TESTS`, `FUM_BUILD_EXAMPLES`, `FUM_BUILD_BENCHMARKS`,
`FUM_BUILD_LOCAL_PROCESS_ADAPTER`, `FUM_WARNINGS_AS_ERRORS`, `FUM_ENABLE_ASAN`.

### Installation and downstream use

```
cmake --install build/release --prefix /some/prefix
```

```cmake
find_package(FabricUpgradeManager 1.0 REQUIRED)
target_link_libraries(my_tool PRIVATE fum::core fum::synthetic)
```

The exported targets are `fum::core`, `fum::synthetic`, `fum::local_process`
and `fum::warnings`. `tests/downstream` is an independent consumer that is
configured, built and run against the *installed* package.

## Examples

* `examples/rolling_upgrade.cpp` — a three-target rolling upgrade with plan,
  preflight, execution and provenance output (synthetic).
* `examples/custom_adapter.cpp` — implementing `TargetAdapterPort`: an adapter
  that claims in-place only, refuses rollback honestly, and shows the runtime
  rejecting a strategy it does not claim.

## Operational notes and limitations

* One engine process owns a state directory. The journal is a single-writer
  append log; concurrent writers are not supported and are not claimed.
* A preflight ticket is valid only inside the process incarnation that issued it.
  This is deliberate: evidence must be re-established after a restart.
* Restart reconciliation blocks campaigns that were mid-flight. Resuming requires
  a fresh preflight and an explicit operator release.
* The engine mutex serialises journal writes, so durable throughput is bounded by
  the storage device. This is a deliberate trade of throughput for a provable
  ordering.
* The local-process adapter is Windows-only in this release (process creation and
  supervision use the Win32 API); the rest of the runtime is portable.
* Signature *verification* is out of scope: FUM enforces declared provenance
  metadata and the artifact digest, and leaves trust establishment to the fabric.

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
