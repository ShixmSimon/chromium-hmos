# OHOS V8 source overlays

These three files are based on V8 revision
`b3d6849efaf11696198e15b32290e6d6b0d540c1`, pinned by Chromium 154.0.8037.97.
Keep their upstream content synchronized when changing the V8 DEPS revision.

On OHOS, an emulated address subspace can map shared buffers within its owned
reservation. Private/shared allocations use the same region allocator, and
freeing a shared mapping restores the PROT_NONE reservation before reuse.
This avoids failed random shared-mapping hints beyond the phone's usable
address space without disabling the V8 sandbox or returning external memory.
Other platforms retain upstream behavior.

The corresponding V8 unit test occupies the unreserved half, then checks shared
aliases, offsets, repeated private/shared reuse and preservation of reservations.
It was added for the V8 test target; it has not been run locally.

Use the existing source-overlay delivery and push-triggered native build.
Do not edit CI workflows, CI scripts, or build orchestration for this change.
