# Memory And Process Notes

## Reclaim Metadata

`ENABLE_RECLAIM_METADATA` is set to `0` intentionally. Physical frame
lifetime is owned by page-table teardown, `page_refcount[]`, and `kfree()`;
reverse-map and reclaim descriptors are advisory only. Keeping that metadata
disabled focuses the VM on lazy allocation and exit-time page recycling without
introducing resident-page stealing paths.

## Heap VMA Growth

`sys_brk` uses `vma_extend_heap` to extend only the contiguous heap frontier
tracked by `proc->sz`. Generic anonymous VMA merging remains disabled because
anonymous mappings created by `mmap` or split by `mprotect` can carry distinct
permissions and future metadata ownership. The heap path has narrower
invariants, so it can bound VMA-node growth during lazy `brk` expansion without
coalescing unrelated mappings.
