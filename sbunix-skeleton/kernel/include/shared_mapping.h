#ifndef _SHARED_MAPPING_H
#define _SHARED_MAPPING_H

#include <riscv.h>
#include <vfs.h>

struct vm_area;

struct shared_mapping;

int shared_mapping_create_anon(struct shared_mapping **out);
int shared_mapping_get_file(struct open_file *file, struct shared_mapping **out);
void shared_mapping_get(struct shared_mapping *mapping);
void shared_mapping_put(struct shared_mapping *mapping);
int shared_mapping_map_page(struct vm_area *vma, uint64_t va, char **page_out);
void shared_slot_mark_dirty(struct vm_area *vma, uint64_t va);
void shared_mapping_writeback_range(struct vm_area *vma, uint64_t start,
                                    uint64_t end);

#endif
