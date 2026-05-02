#include <shared_mapping.h>
#include <vm.h>
#include <kalloc.h>
#include <kmalloc.h>
#include <string.h>
#include <errno.h>

enum shared_mapping_kind {
  SHARED_MAPPING_ANON = 1,
  SHARED_MAPPING_FILE = 2,
};

struct shared_slot {
  uint64_t index;
  uint64_t pa;
  int dirty;
  struct shared_slot *next;
};

struct shared_mapping {
  int kind;
  int refcount;
  uint64_t id;
  struct vfs_ops *ops;
  void *fs_data;
  uint64_t st_dev;
  uint64_t st_ino;
  struct open_file *file;
  struct shared_slot *slots;
  struct shared_mapping *next;
};

static struct shared_mapping *shared_registry;
static uint64_t next_anon_id = 1;

static int shared_file_identity(struct open_file *file, uint64_t *dev,
                                uint64_t *ino) {
  uint8_t stbuf[128];

  if (!file || !file->ops || !file->ops->fstat)
    return -ENODEV;
  memset(stbuf, 0, sizeof(stbuf));
  if (file->ops->fstat(file->fs_data, file->private, stbuf) < 0)
    return -ENODEV;
  memcpy(dev, stbuf + 0, sizeof(*dev));
  memcpy(ino, stbuf + 8, sizeof(*ino));
  return 0;
}

static int shared_file_size(struct shared_mapping *mapping, int64_t *size_out) {
  uint8_t stbuf[128];

  if (!mapping || !mapping->file || !mapping->file->ops ||
      !mapping->file->ops->fstat)
    return -1;
  memset(stbuf, 0, sizeof(stbuf));
  if (mapping->file->ops->fstat(mapping->file->fs_data,
                                mapping->file->private, stbuf) < 0)
    return -1;
  memcpy(size_out, stbuf + 48, sizeof(*size_out));
  return 0;
}

static struct shared_slot *shared_slot_find(struct shared_mapping *mapping,
                                            uint64_t index) {
  struct shared_slot *slot = mapping ? mapping->slots : 0;

  while (slot) {
    if (slot->index == index)
      return slot;
    slot = slot->next;
  }
  return 0;
}

static uint64_t shared_vma_page_index(struct vm_area *vma, uint64_t va) {
  uint64_t page_delta = (PGROUNDDOWN(va) - vma->start) / PAGE_SIZE;

  if (vma->file)
    return (vma->file_offset / PAGE_SIZE) + page_delta;
  return vma->shared_offset_pages + page_delta;
}

int shared_mapping_create_anon(struct shared_mapping **out) {
  struct shared_mapping *mapping;

  if (!out)
    return -EINVAL;
  mapping = (struct shared_mapping *)kmalloc(sizeof(*mapping));
  if (!mapping)
    return -ENOMEM;
  memset(mapping, 0, sizeof(*mapping));
  mapping->kind = SHARED_MAPPING_ANON;
  mapping->refcount = 1;
  mapping->id = next_anon_id++;
  if (next_anon_id == 0)
    next_anon_id = 1;
  mapping->next = shared_registry;
  shared_registry = mapping;
  *out = mapping;
  return 0;
}

int shared_mapping_get_file(struct open_file *file, struct shared_mapping **out) {
  uint64_t dev = 0;
  uint64_t ino = 0;
  struct shared_mapping *mapping;
  int rc;

  if (!out)
    return -EINVAL;
  rc = shared_file_identity(file, &dev, &ino);
  if (rc < 0)
    return rc;

  for (mapping = shared_registry; mapping; mapping = mapping->next) {
    if (mapping->kind == SHARED_MAPPING_FILE &&
        mapping->ops == file->ops &&
        mapping->fs_data == file->fs_data &&
        mapping->st_dev == dev &&
        mapping->st_ino == ino) {
      mapping->refcount++;
      *out = mapping;
      return 0;
    }
  }

  mapping = (struct shared_mapping *)kmalloc(sizeof(*mapping));
  if (!mapping)
    return -ENOMEM;
  memset(mapping, 0, sizeof(*mapping));
  mapping->kind = SHARED_MAPPING_FILE;
  mapping->refcount = 1;
  mapping->ops = file->ops;
  mapping->fs_data = file->fs_data;
  mapping->st_dev = dev;
  mapping->st_ino = ino;
  mapping->file = file;
  file->refcount++;
  mapping->next = shared_registry;
  shared_registry = mapping;
  *out = mapping;
  return 0;
}

void shared_mapping_get(struct shared_mapping *mapping) {
  if (mapping)
    mapping->refcount++;
}

void shared_mapping_put(struct shared_mapping *mapping) {
  struct shared_mapping **pp;
  struct shared_slot *slot;

  if (!mapping)
    return;
  mapping->refcount--;
  if (mapping->refcount > 0)
    return;

  pp = &shared_registry;
  while (*pp && *pp != mapping)
    pp = &(*pp)->next;
  if (*pp == mapping)
    *pp = mapping->next;

  if (mapping->kind == SHARED_MAPPING_FILE) {
    /* BF: The last VMA reference owns final persistence for file-backed
     * shared slots.  This runs without a registry lock in SBUnix's
     * single-hart kernel; an SMP version would take registry then object. */
    for (slot = mapping->slots; slot; slot = slot->next) {
      int64_t file_size = -1;
      uint64_t file_off = slot->index * PAGE_SIZE;
      uint64_t write_len = PAGE_SIZE;
      int64_t foff = (int64_t)file_off;

      if (shared_file_size(mapping, &file_size) == 0 && file_size >= 0) {
        if (file_off >= (uint64_t)file_size)
          write_len = 0;
        else if (file_off + write_len > (uint64_t)file_size)
          write_len = (uint64_t)file_size - file_off;
      }
      if (write_len > 0 && mapping->file && mapping->file->ops &&
          mapping->file->ops->write) {
        mapping->file->ops->write(mapping->file->fs_data,
                                  mapping->file->private,
                                  (char *)slot->pa, write_len, &foff);
      }
    }
  }

  slot = mapping->slots;
  while (slot) {
    struct shared_slot *next = slot->next;
    if (slot->pa)
      kfree((void *)slot->pa);
    kmfree(slot, sizeof(*slot));
    slot = next;
  }
  if (mapping->file)
    vfs_close_file(mapping->file);
  kmfree(mapping, sizeof(*mapping));
}

int shared_mapping_map_page(struct vm_area *vma, uint64_t va, char **page_out) {
  struct shared_mapping *mapping;
  struct shared_slot *slot;
  uint64_t index;
  char *page;

  if (!vma || !vma->shared || !page_out)
    return -EINVAL;
  mapping = vma->shared;
  index = shared_vma_page_index(vma, va);
  slot = shared_slot_find(mapping, index);
  if (slot) {
    page_ref_inc(slot->pa);
    *page_out = (char *)slot->pa;
    return 0;
  }

  slot = (struct shared_slot *)kmalloc(sizeof(*slot));
  if (!slot)
    return -ENOMEM;
  memset(slot, 0, sizeof(*slot));
  page = (char *)kalloc();
  if (!page) {
    kmfree(slot, sizeof(*slot));
    return -ENOMEM;
  }
  memset(page, 0, PAGE_SIZE);

  if (mapping->kind == SHARED_MAPPING_FILE && mapping->file &&
      mapping->file->ops && mapping->file->ops->read) {
    int64_t foff = (int64_t)(index * PAGE_SIZE);
    mapping->file->ops->read(mapping->file->fs_data,
                             mapping->file->private, page, PAGE_SIZE, &foff);
  }

  slot->index = index;
  slot->pa = (uint64_t)page;
  slot->dirty = 0;
  slot->next = mapping->slots;
  mapping->slots = slot;

  page_ref_inc(slot->pa);
  *page_out = page;
  return 0;
}

void shared_slot_mark_dirty(struct vm_area *vma, uint64_t va) {
  struct shared_slot *slot;

  if (!vma || !vma->shared)
    return;
  slot = shared_slot_find(vma->shared, shared_vma_page_index(vma, va));
  if (slot)
    slot->dirty = 1;
}

void shared_mapping_writeback_range(struct vm_area *vma, uint64_t start,
                                    uint64_t end) {
  struct shared_mapping *mapping;
  int64_t file_size = -1;
  uint64_t wb_start;
  uint64_t wb_end;

  if (!vma || !vma->shared || !vma->file)
    return;
  mapping = vma->shared;
  if (!mapping->file || !mapping->file->ops || !mapping->file->ops->write)
    return;

  wb_start = start > vma->start ? start : vma->start;
  wb_end = end < vma->end ? end : vma->end;
  if (wb_start >= wb_end)
    return;

  (void)shared_file_size(mapping, &file_size);
  for (uint64_t page_va = PGROUNDDOWN(wb_start);
       page_va < wb_end; page_va += PAGE_SIZE) {
    uint64_t index = shared_vma_page_index(vma, page_va);
    struct shared_slot *slot = shared_slot_find(mapping, index);
    uint64_t page_start = page_va;
    uint64_t page_end = page_va + PAGE_SIZE;
    uint64_t from = wb_start > page_start ? wb_start : page_start;
    uint64_t to = wb_end < page_end ? wb_end : page_end;
    uint64_t page_off;
    uint64_t file_off;
    uint64_t write_len;
    int64_t foff;

    if (!slot || from >= to)
      continue;
    page_off = from - page_start;
    file_off = vma->file_offset + (from - vma->start);
    write_len = to - from;
    if (file_size >= 0) {
      if (file_off >= (uint64_t)file_size)
        write_len = 0;
      else if (file_off + write_len > (uint64_t)file_size)
        write_len = (uint64_t)file_size - file_off;
    }
    if (write_len == 0)
      continue;
    foff = (int64_t)file_off;
    mapping->file->ops->write(mapping->file->fs_data,
                              mapping->file->private,
                              (char *)slot->pa + page_off,
                              write_len, &foff);
    slot->dirty = 0;
  }
}
