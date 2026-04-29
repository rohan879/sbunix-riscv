# Memory And Process Notes

## Reclaim Metadata

`ENABLE_RECLAIM_METADATA` is set to `0` intentionally. Physical frame
lifetime is owned by page-table teardown, `page_refcount[]`, and `kfree()`;
reverse-map and reclaim descriptors are advisory only. Keeping that metadata
disabled focuses the VM on lazy allocation and exit-time page recycling without
introducing resident-page stealing paths.
