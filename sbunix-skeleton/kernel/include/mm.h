#ifndef _MM_H
#define _MM_H

#include "riscv.h"

/* Page aging constants for enhanced clock algorithm */
#define MM_AGE_MAX 255
#define MM_AGE_YOUNG 200
#define MM_AGE_OLD 32

/* Simple atomic type for uniprocessor kernel */
typedef struct {
  int counter;
} atomic_t;

/* Simple spinlock type (placeholder for uniprocessor) */
typedef struct {
  int locked;
  const char *name;
} spinlock_t;

/* Simple doubly-linked list implementation */
struct list_head {
  struct list_head *next, *prev;
};

#define LIST_HEAD_INIT(name) { &(name), &(name) }
#define LIST_HEAD(name) struct list_head name = LIST_HEAD_INIT(name)

static inline void INIT_LIST_HEAD(struct list_head *list) {
  list->next = list;
  list->prev = list;
}

static inline void __list_add(struct list_head *new, struct list_head *prev, struct list_head *next) {
  next->prev = new;
  new->next = next;
  new->prev = prev;
  prev->next = new;
}

static inline void list_add(struct list_head *new, struct list_head *head) {
  __list_add(new, head, head->next);
}

static inline void list_add_tail(struct list_head *new, struct list_head *head) {
  __list_add(new, head->prev, head);
}

static inline void __list_del(struct list_head *prev, struct list_head *next) {
  next->prev = prev;
  prev->next = next;
}

static inline void list_del(struct list_head *entry) {
  __list_del(entry->prev, entry->next);
  entry->next = NULL;
  entry->prev = NULL;
}

static inline int list_empty(const struct list_head *head) {
  return head->next == head;
}

#define list_entry(ptr, type, member) \
  ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))

#define list_for_each_entry(pos, head, member) \
  for (pos = list_entry((head)->next, typeof(*pos), member); \
       &pos->member != (head); \
       pos = list_entry(pos->member.next, typeof(*pos), member))

#define list_for_each_entry_safe(pos, n, head, member) \
  for (pos = list_entry((head)->next, typeof(*pos), member), \
       n = list_entry(pos->member.next, typeof(*pos), member); \
       &pos->member != (head); \
       pos = n, n = list_entry(n->member.next, typeof(*n), member))

/* Forward declarations */
struct anon_vma;
struct vm_area;

/* Enhanced page descriptor for rich per-page metadata */
struct page {
  /* Reference counting */
  atomic_t refcount;              // Allocation references (kalloc/kfree)
  atomic_t mapcount;              // Virtual mapping count (-1=unmapped, 0=1 mapping, N=shared)
  
  /* State flags */
  uint64_t flags;                 // Page state flags (see PG_* below)
  
  /* Enhanced clock algorithm data */
  unsigned int age_score;         // Multi-level aging score (0-255)
  unsigned int access_count;      // Recent access frequency counter
  uint64_t last_access;           // Timestamp of last access (in ticks)
  
  /* Physical page information */
  void *virtual;                  // Kernel virtual address for this page
  uint64_t pfn;                   // Page frame number
  
  /* Reverse mapping (Phase 2) */
  struct anon_vma *anon_vma;      // Anonymous VMA this page belongs to
  
  /* Swap backing */
  union {
    uint32_t swap_entry;          // Swap slot if page is swapped out
    void *freelist_next;          // Next in free page list
  };
};

/* Page flags for enhanced state tracking */
#define PG_locked     0    // Page is locked for I/O
#define PG_dirty      1    // Page contains modified data
#define PG_accessed   2    // Page has been accessed recently
#define PG_anon       3    // Page contains anonymous memory
#define PG_swapcache  4    // Page is in swap cache
#define PG_reclaim    5    // Page is being reclaimed
#define PG_active     6    // Page is in active set (for future LRU migration)
#define PG_reserved   7    // Page is reserved (kernel, etc.)

/* Page flag manipulation - simple bit operations for uniprocessor */
static inline int test_bit(int bit, uint64_t *flags) {
  return (*flags & (1UL << bit)) != 0;
}

static inline void set_bit(int bit, uint64_t *flags) {
  *flags |= (1UL << bit);
}

static inline void clear_bit(int bit, uint64_t *flags) {
  *flags &= ~(1UL << bit);
}

/* Page flag manipulation macros */
#define PageLocked(page)      test_bit(PG_locked, &(page)->flags)
#define SetPageLocked(page)   set_bit(PG_locked, &(page)->flags)
#define ClearPageLocked(page) clear_bit(PG_locked, &(page)->flags)

#define PageDirty(page)       test_bit(PG_dirty, &(page)->flags)
#define SetPageDirty(page)    set_bit(PG_dirty, &(page)->flags) 
#define ClearPageDirty(page)  clear_bit(PG_dirty, &(page)->flags)

#define PageAccessed(page)    test_bit(PG_accessed, &(page)->flags)
#define SetPageAccessed(page) set_bit(PG_accessed, &(page)->flags)
#define ClearPageAccessed(page) clear_bit(PG_accessed, &(page)->flags)

#define PageAnon(page)        test_bit(PG_anon, &(page)->flags)
#define SetPageAnon(page)     set_bit(PG_anon, &(page)->flags)
#define ClearPageAnon(page)   clear_bit(PG_anon, &(page)->flags)

#define PageReclaim(page)     test_bit(PG_reclaim, &(page)->flags)
#define SetPageReclaim(page)  set_bit(PG_reclaim, &(page)->flags)
#define ClearPageReclaim(page) clear_bit(PG_reclaim, &(page)->flags)

/* Simple atomic operations for uniprocessor kernel */
static inline void atomic_set(atomic_t *a, int val) {
  a->counter = val;
}

static inline int atomic_read(atomic_t *a) {
  return a->counter;
}

static inline void atomic_inc(atomic_t *a) {
  a->counter++;
}

static inline void atomic_dec(atomic_t *a) {
  a->counter--;
}

static inline int atomic_dec_and_test(atomic_t *a) {
  return --a->counter == 0;
}

static inline int atomic_inc_return(atomic_t *a) {
  return ++a->counter;
}

static inline int atomic_dec_return(atomic_t *a) {
  return --a->counter;
}

/* Spinlock operations for uniprocessor kernel.
 * BF: Kept as lightweight no-ops.  This kernel is cooperative single-hart
 * with no preemptive scheduling; timer interrupts only set a flag and
 * never touch anon_vma/page data structures.  Real interrupt masking
 * was attempted (push_off/pop_off) but caused livelock in the swap
 * eviction path because the interrupt disable/restore changed timing
 * assumptions throughout the memory subsystem.  If SMP or preemptive
 * scheduling is added, revisit with full interrupt-safe locking. */
static inline void spin_lock_init(spinlock_t *lock, const char *name) {
  lock->locked = 0;
  lock->name = name;
}

static inline void spin_lock(spinlock_t *lock) {
  (void)lock;
}

static inline void spin_unlock(spinlock_t *lock) {
  (void)lock;
}

/* Enhanced memory statistics */
struct mm_stat {
  uint64_t total_pages;         // Total system pages
  uint64_t free_pages;          // Currently free pages
  uint64_t anon_pages;          // Anonymous pages in use
  uint64_t file_pages;          // File-backed pages in use
  uint64_t swap_pages;          // Pages in swap
  uint64_t shared_pages;        // Shared pages (mapcount > 0)
  
  /* Enhanced clock algorithm statistics */
  uint64_t clock_scans;         // Total clock scans performed
  uint64_t clock_evictions;     // Pages evicted by clock
  uint64_t clock_second_chance; // Pages given second chance
  
  /* Memory pressure statistics */
  uint64_t pressure_events;     // Times memory pressure detected
  uint64_t direct_reclaims;     // Direct reclaim attempts
  uint64_t background_reclaims; // Background reclaim cycles
  uint64_t reclaim_failures;    // Failed reclaim attempts
  
  /* kswapd statistics */
  uint64_t kswapd_wakeups;      // Times kswapd was awakened
  uint64_t watermark_hits;      // Times watermarks were hit
};

/* Memory pressure watermarks */
struct mm_watermarks {
  uint64_t min_free;            // Minimum free pages (critical)
  uint64_t low_free;            // Low watermark (start background reclaim)
  uint64_t high_free;           // High watermark (stop background reclaim)
};

extern struct mm_watermarks mm_watermarks;

extern struct mm_stat mm_stats;

/* Global page descriptor table */
extern struct page *page_table;

/* Core page management functions */
void mm_init_page_descriptors(void);
struct page *pfn_to_page(uint64_t pfn);
uint64_t page_to_pfn(struct page *page);
struct page *virt_to_page(void *addr);

/* Enhanced reference counting */
void page_get(struct page *page);
void page_put(struct page *page);
void page_add_mapping(struct page *page);
void page_remove_mapping(struct page *page);
int page_mapcount(struct page *page);

/* Anonymous VMA object for reverse mapping */
struct anon_vma {
  struct list_head head;           // List of anon_vma_chain entries
  spinlock_t lock;                 // Protects the anon_vma structure
  atomic_t refcount;               // Reference count
  
  /* Inheritance hierarchy for fork() */
  struct anon_vma *root;           // Root of inheritance tree
  struct anon_vma *parent;         // Parent anon_vma (for inheritance)
  
  /* Statistics */
  uint64_t num_pages;              // Total pages in this anon_vma
  uint64_t num_vmas;               // Number of VMAs using this anon_vma
};

/* Chain linking anon_vma to VMA */
struct anon_vma_chain {
  struct list_head anon_vma_node;  // Node in anon_vma->head list
  struct list_head vma_node;       // Node in vma->anon_vma_chain list
  struct anon_vma *anon_vma;       // The anon_vma object
  struct vm_area *vma;             // The VMA
};

/* Anonymous VMA management functions */
void anon_vma_init(void);
struct anon_vma *anon_vma_alloc(void);
void anon_vma_free(struct anon_vma *anon_vma);
void anon_vma_get(struct anon_vma *anon_vma);
void anon_vma_put(struct anon_vma *anon_vma);
int anon_vma_fork(struct vm_area *vma, struct vm_area *parent_vma);
int anon_vma_link(struct vm_area *vma);
int anon_vma_link_prealloc(struct vm_area *vma, struct anon_vma_chain *avc);
void anon_vma_unlink(struct vm_area *vma);

/* Reverse mapping operations */
int page_add_anon_rmap(struct page *page, struct vm_area *vma, uint64_t address);
void page_remove_rmap(struct page *page, struct vm_area *vma);
int page_mapped_in_vma_count(struct page *page);

/* Enhanced page access tracking */
void mark_page_accessed(struct page *page);
void mm_age_pages(void);

/* Memory pressure detection */
int mm_check_memory_pressure(void);
void mm_init_watermarks(void);
void mm_trigger_reclaim(int pages_needed);
int mm_direct_reclaim(int pages_needed);
void mm_background_reclaim(void);

/* System tick access */
extern unsigned long get_ticks(void);

/* Internal page management */
void mm_free_page(struct page *page);

/* Enhanced kalloc integration */
void *enhanced_kalloc(void);
#endif /* _MM_H */
