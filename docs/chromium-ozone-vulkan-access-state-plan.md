# Chromium Ozone Vulkan Access-State Plan

Last updated: 2026-07-18

## Implementation status

The refactor is implemented and staged as of 2026-07-18. The Chromium/CEF
source tree used by the completed build was deliberately left untouched while
the work was developed in:

```text
/tmp/noctalia-cef-layout-refactor
```

The authoritative result has been folded into the existing patch owners:

- `0001-viz-offscreen-output-transport.patch` owns the generic
  Ozone/Vulkan/Dawn/Ganesh, compound-backing, Viz export, error-contract, and
  state-machine test changes;
- `0002-cef-osr-dmabuf-and-frame-scheduling.patch` owns CEF's output-generation
  recovery.

The regenerated stack was verified by reconstructing the exact post-CEF,
pre-Nucleus source state from the last build's saved patch stack, reversing
that stack, and applying the new eight-patch stack from the beginning. The
resulting source files exactly match the staged implementation.

Implemented behavior:

- `OzoneImageBacking::ScopedAccess` now owns access arbitration, rollback
  fences, acquire commitment, and the optional external Vulkan transfer as one
  transaction.
- queue-family ownership is singular even for abstract concurrent reads;
  another backing access cannot start while an explicit Vulkan owner holds the
  transfer.
- pre-acquire failure restores the removed fences and transfer; post-acquire
  failure invalidates the backing and marks the shared context lost.
- Graphite/Dawn consumes and returns Dawn's exact layout pair and
  `VK_QUEUE_FAMILY_EXTERNAL_KHR`.
- Ganesh/Vulkan imports the producer fence into Ganesh, records the exact
  external-to-local transition, and returns the actual layout and external
  queue family after its caller applies the end state.
- GL and ordinary native-overlay accesses carry an existing transfer through
  unchanged.
- the raw Vulkan representation fails closed when it encounters tracked
  external ownership. Its only production caller is the ChromeOS protected
  overlay detiler, whose legacy API cannot report the exact barrier pair and
  is not part of the Linux Noctalia/CEF path. It is no longer allowed to
  consume or overwrite authoritative state by guessing.
- the offscreen exporter publishes no frame without an exact transfer and
  acquire fence, and distinguishes lost synchronization state from allocation,
  context, and protocol errors.
- a terminal export error closes only that output generation. CEF retires the
  old compositor/output endpoints asynchronously, terminates any outstanding
  external BeginFrame transaction, creates a fresh compositor generation, and
  requests fresh damage.
- focused state-machine coverage now exercises round trips, rollback, implicit
  carry-through, post-acquire invalidation, and concurrent-owner rejection.

All modified C++ translation units have passed the exact optimized CEF Release
compiler in syntax-only mode, including the state-machine test source. A full
CEF rebuild, test execution, Vulkan-validation run, and live animated-art
acceptance run are intentionally the next step; they were not started while
the previous build artifact remained in use.

## Decision

Replace the current best-effort Ozone layout cache with one backing-owned,
scoped Vulkan ownership-transfer contract. The transfer state and its fence
must enter and leave an access together.

This is the proper resolution of Chromium's
`DawnOzoneImageRepresentation` layout-tracking TODO. It also removes the
offscreen-output debug-label exception, makes internal Graphite/Dawn image
reuse content-preserving, and gives the CEF export path the same state model as
the rest of Ozone.

The immediate compatibility behavior remains valid while this work is being
implemented:

- use a tracked layout pair when one is available;
- retain upstream's `UNDEFINED` first/discard access when no pair is available;
- keep the export output strict and refuse to publish a frame without an exact
  state.

That fallback is a bridge, not the final state model.

## Finding

Ozone currently arbitrates access with:

- read/write exclusion;
- per-stream sync fences;
- the last writer stream;
- the SharedImage cleared state.

It does not authoritatively track the Vulkan queue-family ownership transfer
that accompanies those fences.

Dawn's Linux DMA-BUF `SharedTextureMemory` import uses
`VK_QUEUE_FAMILY_EXTERNAL_KHR`. At `EndAccess`, Dawn returns the exact
`oldLayout`/`newLayout` pair used by its release. The next Vulkan acquire must
use the matching pair. Upstream Chromium main still discards this information
and supplies `UNDEFINED`/`UNDEFINED` at the next Dawn access.

For an initialized texture, Dawn interprets that missing new layout as a
discard transition. Vulkan permits `oldLayout = UNDEFINED`, but the prior
contents may be lost. This is unsafe for an initialized image whose contents
must survive, including reusable Viz render-pass backings.

The current fork improves the Dawn-to-overlay-to-Dawn path by caching the
pair in `OzoneImageBacking`. It is incomplete as a global solution because:

- state is taken separately from `BeginAccess` and restored manually on some
  failures;
- the state does not record its external queue family;
- other Ozone representations do not consume and return it;
- native overlay and compound-wrapper lifetimes are not represented by the
  state object itself;
- absence cannot distinguish a legal first discard from an initialized image
  whose state was lost;
- Skia/Vulkan contains an output-specific debug-label layout exception.

The recent internal `RenderPassBacking` failure was a consequence of making
missing state globally fatal before every access path supplied it. The narrow
fallback fixes that regression. The long-term fix is to finish the ownership
model, not to retain a permanent global guess.

## Terminology

The tracked value is an ownership-transfer recipe, not simply a current image
layout:

```cpp
struct VulkanOwnershipTransfer {
  VkImageLayout old_layout;
  VkImageLayout new_layout;
  uint32_t external_queue_family;
};
```

The pair is the exact pair used by the releasing side of a Vulkan
queue-family ownership transfer. The acquiring side must use the same pair.
`external_queue_family` is normally `VK_QUEUE_FAMILY_EXTERNAL_KHR` for Linux
DMA-BUF imports, but it must not be inferred by a remote consumer.

The SharedImage cleared state remains the authority for whether content is
initialized. Do not add a second content-valid flag to the transfer type.

## Required invariants

1. Fence and ownership-transfer state belong to one scoped backing access.
2. An explicit Vulkan access consumes the prior transfer exactly once and
   returns the next transfer when it ends.
3. A non-Vulkan external access carries an existing transfer through
   unchanged unless it explicitly participates in Vulkan ownership.
4. An initialized, preservation-requiring Vulkan access cannot begin without
   a known transfer.
5. A missing transfer is legal only for an uncleared image whose first access
   explicitly permits discard/full initialization, or for a documented
   platform import contract that seeds the initial state.
6. After a Vulkan acquire has been submitted, failure cannot restore the old
   transfer as though it were never consumed. The backing becomes invalid or
   context-lost until recreated.
7. Cross-owner concurrent reads are not allowed. Queue ownership is singular
   even when the abstract SharedImage access is read-only.
8. Compound wrappers forward the same access transaction to the concrete
   backing; they do not copy or infer transfer state.
9. Exported frames remain strict: no exact transfer and no acquire fence means
   no publication.
10. No debug label, image size, slot index, or usage guess selects layout
    behavior.

## Target access API

Add a small scoped transaction owned by `OzoneImageBacking`. Its exact C++
shape can follow Chromium conventions, but it must provide these operations:

```cpp
class OzoneImageBacking::ScopedAccess {
 public:
  const std::vector<gfx::GpuFenceHandle>& begin_fences() const;
  bool needs_end_fence() const;

  // Marks this as an explicit Vulkan ownership access and moves the matching
  // acquire recipe to the caller.
  std::optional<VulkanOwnershipTransfer>
  TakeVulkanOwnershipTransfer();

  // Ends an ordinary GL/native-overlay access. Any unconsumed Vulkan transfer
  // is retained unchanged.
  void End(gfx::GpuFenceHandle fence);

  // Ends an explicit Vulkan ownership access with its newly released state.
  void EndVulkan(gfx::GpuFenceHandle fence,
                 VulkanOwnershipTransfer release);

  // Valid only before the acquire was submitted. Restores the access,
  // fences, and transfer without pretending GPU work occurred.
  void AbortBeforeAcquire();
};
```

`BeginAccess` creates this object only after read/write arbitration succeeds.
The transaction owns any state removed from the backing. Its destructor must
either complete a defined abort or detect an incomplete access; it must not
silently fabricate a release.

This replaces independent `Get`, `Take`, and `Set` calls. It also makes error
paths local: Dawn no longer has to remember which combination of fence and
layout state to restore after each failed step.

## Representation behavior

### Graphite/Dawn

`DawnOzoneImageRepresentation::BeginAccess`:

1. starts an Ozone scoped access;
2. takes the Vulkan ownership transfer;
3. if a transfer exists, gives its exact pair to
   `SharedTextureMemory::BeginAccess`;
4. if none exists, permits `UNDEFINED` only when the image is uncleared and
   the access can initialize it completely;
5. marks the acquire committed only after Dawn accepts the begin descriptor.

`EndAccess`:

1. submits Graphite work before ending the Dawn access;
2. obtains Dawn's returned layout pair and fence;
3. validates both layouts and the expected external queue family;
4. ends the Ozone access with the fence and returned transfer.

If Dawn fails after consuming the acquire, invalidate the backing instead of
putting the old transfer back.

### External offscreen consumer

Move the external Vulkan metadata from
`OverlayImageRepresentation` itself onto its `ScopedReadAccess`.

The export device takes the transfer from that access and publishes:

- exact old and new layouts;
- exact external queue family;
- the matching acquire fence;
- existing frame/slot identity.

When the consumer returns its release fence, the same scoped access ends with
the returned transfer. Noctalia currently normalizes to and releases with
`GENERAL`/`GENERAL`, so that remains the returned pair.

Native overlay clients that do not perform an explicit Vulkan ownership
transfer never take the recipe. Their access ends normally and carries it
through unchanged.

### Skia/Vulkan and raw Vulkan

Both Vulkan representations must use the same scoped ownership access.

Before returning ownership, normalize the image locally to a known layout,
then perform a same-layout ownership release. `GENERAL`/`GENERAL` is the
portable default for exportable color images. This avoids requiring a remote
consumer to infer the internal renderer's previous layout.

The representation returns that exact release pair to the backing. Remove:

```cpp
debug_label() == "OffscreenOutputExport"
```

and all other output-specific layout selection.

If an existing raw Vulkan caller cannot report the release it recorded, change
that caller's scoped-access API to provide it. Do not guess in the backing.

### GL and implicit external users

GL/EGL, native scanout, and other non-Vulkan users retain an existing Vulkan
transfer through their access because they do not record a competing Vulkan
layout transition.

An initialized pixmap imported from an external producer without any prior
Chromium Vulkan release needs an explicit factory/import policy:

- seed the transfer defined by that producer/platform contract; or
- copy/initialize it into a Chromium-owned backing with known state; or
- reject a preservation-requiring Vulkan access.

Do not treat every initialized external pixmap as
`GENERAL`/`GENERAL` without a contract.

### CompoundImageBacking

The concrete backing owns the transaction and state. Compound representations
forward the scoped access object.

Copies between backing types are ordinary source-read and destination-write
transactions. A completed destination Vulkan write establishes the
destination's next transfer. Clear/content IDs remain separate and do not
imply a Vulkan layout.

## Implementation order

### 1. Introduce the value type and scoped transaction

- expand the current value with `external_queue_family`;
- put it next to the Ozone access implementation rather than in a generic
  overlay API;
- convert backing fence/read/write bookkeeping to the scoped object;
- add focused state-machine tests before changing representations.

### 2. Convert Dawn

- remove the upstream `UNDEFINED` TODO path for initialized retained content;
- consume and return the scoped transfer;
- distinguish pre-acquire abort from post-acquire invalidation;
- test first initialization, repeated Dawn access, and begin/end failure.

This is the first functional milestone and the most likely change to improve
internal Graphite/Dawn render-pass preservation.

### 3. Convert the offscreen overlay export

- attach transfer state to `OverlayImageRepresentation::ScopedReadAccess`;
- derive frame queue-family metadata from the transfer;
- return Noctalia's release transfer through that same access;
- keep missing state/fence a named export error.

No Noctalia transport format change is required unless the newly tracked
queue-family field differs from the existing field type.

### 4. Convert native Vulkan representations

- make Skia/Vulkan normalize and explicitly release to the external family;
- extend raw Vulkan scoped access so callers return their release state;
- remove the debug-label special case;
- cover Vulkan-to-Dawn and Dawn-to-Vulkan round trips.

### 5. Convert implicit-external paths

- carry state through GL and native overlay reads;
- define initialized external-import policies per producer;
- reject unsupported write paths that cannot establish a safe next state;
- cover compound backing copies and wrapper forwarding.

At the end of this phase, an initialized Ozone image cannot reach Dawn with an
unknown layout.

### 6. Tighten failure and recovery

- report a synchronization/state error separately from ordinary context loss;
- stop publishing the affected output generation;
- recreate its SharedImages and request a fresh frame;
- recreate the Vulkan/Graphite GPU process once for a terminal device/context
  failure;
- never fall back to Ganesh/GL or CPU paint.

A state-contract violation should fail one output generation clearly. It
should not cause a three-crash GPU-process loop followed by an incompatible
renderer fallback.

### 7. Remove compatibility code

After all Ozone access paths used by the build participate:

- delete the initialized-image `UNDEFINED` fallback;
- delete independent state getters/setters;
- delete debug-label layout selection;
- retain `UNDEFINED` only for explicit first-use discard/full initialization;
- keep the export path's strict checks.

## Tests

### State-machine unit tests

- uncleared first write accepts discard;
- cleared first read without a transfer fails;
- Dawn end state is consumed by the next Dawn begin;
- external read returns `GENERAL`/`GENERAL` to Dawn;
- ordinary native overlay access preserves the existing transfer;
- failed begin restores state and fences;
- failed end invalidates state and cannot restore a stale transfer;
- cross-owner concurrent reads are rejected;
- resize/destruction cannot leak state into a new backing.

### Representation integration tests

- Dawn write -> Dawn read;
- Dawn write -> overlay/Vulkan consumer -> Dawn write;
- Skia/Vulkan write -> Dawn read;
- Dawn write -> Skia/Vulkan read;
- GL/native overlay read between two Dawn accesses;
- CompoundImageBacking source/destination copy;
- initialized imported pixmap with and without a defined producer policy.

Each test must verify pixels as well as state transitions. State-only tests can
pass while an accidental discard corrupts retained content.

### Runtime acceptance

- Vulkan and Dawn validation remain clean;
- no `Initialized shared image has no external Vulkan layout state`;
- no Graphite `insertRecording` failure or GPU-process restart;
- continuously animated artwork remains complete and flicker-free;
- transparent-root CSS backdrop blur remains correct;
- resize, fractional scaling, panel hide/show, and browser recreation work;
- FD counts and the four-slot producer pool remain stable.

Run a retained-content test that redraws only a small changing region of an
initialized render-pass image. That is the direct oracle for the class of bug
hidden by `UNDEFINED`.

## Related optimizations after correctness

Do not mix these into the ownership-state implementation:

- evaluate `VK_EXT_external_memory_acquire_unmodified` for a genuinely
  read-only external interval;
- evaluate all-stage ownership-transfer semantics from Vulkan maintenance8
  only if both sides support and use the same contract;
- preserve full-frame CEF export until damage-aware slot preservation has its
  own correctness proof;
- measure whether normalizing every native-Vulkan release to `GENERAL` has a
  meaningful cost before considering a more specialized stable layout.

The layout tracker is primarily a correctness and recovery improvement. It may
also remove intermittent retained-content corruption and wasted GPU-process
restarts, but it is not expected to be a large steady-state frame-time
optimization by itself.

## Patch placement

Keep this work in the existing
`0001-viz-offscreen-output-transport.patch`, which already owns every affected
Ozone, representation, compound-backing, and Viz export source area. Do not add
a second patch that overlaps those files.

Keep the implementation generic and suitable for Chromium Ozone/Dawn. CEF and
application-specific naming belongs only at the API/transport boundary.

## Completion criterion

The work is complete when every initialized Ozone image used by this build has
one of two explicit histories:

1. a known Vulkan ownership transfer that moves atomically with its fence and
   scoped access; or
2. a documented non-Vulkan producer/import contract that establishes the
   first safe Vulkan acquire.

There must be no initialized preservation path whose answer is “use
`UNDEFINED` because no one recorded what happened.”
