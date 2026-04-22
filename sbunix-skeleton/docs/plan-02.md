# Remaining CR07 Remediation Plan

This file records what remains after the current `os-debug-01` checkpoint at
`afa6653`.

## Completed Checkpoint Context

- `rem-safety-abi` landed.
- `rem-pte` landed.
- `rem-signal-time` landed.
- Recent local commits were rewritten to use the required commit prefixes.
- Disk cleanup recovered enough space for further QEMU validation.

## Remaining VM/VMA/Process Lifetime Work

- `CR07-002`: block unsafe eviction of shared, COW, and shared-anon pages until ownership/rmap metadata is authoritative.
- `CR07-003`: implement shared `mm` ownership for `CLONE_VM` or reject unsupported shared-VM semantics consistently.
- `CR07-007`: refuse anon VMA merge unless metadata can be merged losslessly, or implement the metadata merge.
- `CR07-008`: add one VFS close/drop helper and use it for file-backed VMA release.
- `CR07-017`: make `mprotect` rollback-safe by preallocating/linking all VMA segments before mutation, or rolling back exactly on failure.

Recommended workstream: `rem-vm-lifetime`.

## Remaining VFS/Path/Ext2 Work

- `CR07-009`: implement ext2 truncate shrink release, grow zero-fill, and inode persistence.
- `CR07-012`: add one dirfd-aware resolver for implemented `*at` syscalls.
- `CR07-016`: reject directory hardlinks at syscall/VFS level, with defensive ext2 rejection.

Recommended workstreams:

- `rem-vfs-path` for dirfd resolver and directory hardlink policy.
- `rem-ext2` for truncate and persistence behavior.

## Remaining Final Validation/App Compatibility Work

- Run exact external/app rows that local core tests do not cover:
  - Python
  - make
  - TCC
  - BusyBox
  - persistence write/verify
  - interactive shell/job-control probes
- Complete the final `rem-validation` pass:
  - rerun the stub/no-op audit
  - complete the 49-row matrix
  - run final QEMU gates
  - clean merged worktrees and branches after successful gates
- Run the deferred PTE endurance item:
  - exact long `ptextest` loop from the original plan

Recommended workstream: `rem-validation` after the remaining fix branches land.
