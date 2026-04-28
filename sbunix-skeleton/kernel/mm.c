#include "include/mm.h"
#include "include/kalloc.h"
#include "include/string.h"
#include "include/printk.h"
#include "include/riscv.h"

/* Global page descriptor table */
// Static page descriptor table to avoid bootstrap problems
/* BF: Size descriptors from PHYSTOP so rmap metadata covers all RAM. */
#define MAX_STATIC_PAGES (PHYSMEM_SIZE / PAGE_SIZE)
static struct page static_page_table[MAX_STATIC_PAGES];
struct page *page_table;              // Points to static_page_table
static uint64_t max_pfn;                     // Highest valid page frame number
static spinlock_t page_table_lock;           // Protects page_table operations

/* Enhanced memory statistics */
struct mm_stat mm_stats;

/* Initialize page descriptor system */
void mm_init_page_descriptors(void) {
  
  // Calculate number of physical pages in system
  // Physical memory starts at KERNBASE and ends at PHYSTOP.
  uint64_t phys_start = 0x80000000UL;
  max_pfn = (PHYSTOP - phys_start) >> 12;  // PAGE_SIZE = 4096, so shift by 12
  
  // Use static allocation to avoid bootstrap problems
  page_table = static_page_table;
  if (max_pfn > MAX_STATIC_PAGES) {
    printk("mm: WARNING - limiting max_pfn from %lu to %d to fit in static table\n",
           max_pfn, MAX_STATIC_PAGES);
    max_pfn = MAX_STATIC_PAGES;
  }
  
  printk("mm: using static page descriptor table for %lu pages\n", max_pfn);
  
  // Initialize all page descriptors
  memset(page_table, 0, max_pfn * sizeof(struct page));
  for (uint64_t pfn = 0; pfn < max_pfn; pfn++) {
    struct page *page = &page_table[pfn];
    atomic_set(&page->refcount, 0);
    atomic_set(&page->mapcount, -1);    // -1 means not mapped
    page->flags = 0;
    page->age_score = 0;
    page->access_count = 0;
    page->last_access = 0;
    page->virtual = (void *)(0x80000000UL + (pfn << 12));
    page->pfn = pfn;
    page->anon_vma = NULL;
  }
  
  spin_lock_init(&page_table_lock, "page_table");
  
  // Initialize statistics
  mm_stats.total_pages = max_pfn;
  mm_stats.free_pages = max_pfn;  // Will be adjusted as pages are allocated
  mm_stats.anon_pages = 0;
  mm_stats.file_pages = 0;
  mm_stats.shared_pages = 0;
  mm_stats.clock_scans = 0;
  mm_stats.clock_evictions = 0;
  mm_stats.clock_second_chance = 0;
  mm_stats.pressure_events = 0;
  mm_stats.direct_reclaims = 0;
  mm_stats.background_reclaims = 0;
  mm_stats.reclaim_failures = 0;
  mm_stats.kswapd_wakeups = 0;
  mm_stats.watermark_hits = 0;
  
  printk("mm: initialized %lu page descriptors at %p\n", max_pfn, page_table);
}

/* Convert page frame number to page descriptor */
struct page *pfn_to_page(uint64_t pfn) {
  if (pfn >= max_pfn) {
    return NULL;
  }
  return &page_table[pfn];
}

/* Convert page descriptor to page frame number */
uint64_t page_to_pfn(struct page *page) {
  if (!page || page < page_table || page >= page_table + max_pfn) {
    return ~0UL;  // Invalid PFN
  }
  return page - page_table;
}

/* Convert virtual address to page descriptor */
struct page *virt_to_page(void *addr) {
  uint64_t va = (uint64_t)addr;
  
  // Physical addresses in SBUnix start at 0x80000000
  if (va < 0x80000000UL || va >= PHYSTOP) {
    return NULL;  // Invalid address
  }
  
  uint64_t pfn = (va - 0x80000000UL) >> 12;
  return pfn_to_page(pfn);
}

/* Enhanced reference counting */
void page_get(struct page *page) {
  if (!page) return;
  atomic_inc(&page->refcount);
}

void page_put(struct page *page) {
  if (!page) return;

  /* BF: struct page refcount is reclaim metadata only.  Physical page
   * lifetime is owned by kalloc.c's page_refcount[] and kfree(), so this
   * path must not free a page behind a live PTE. */
  if (atomic_read(&page->refcount) > 0)
    atomic_dec(&page->refcount);
}

/* Mapping reference counting */
void page_add_mapping(struct page *page) {
  if (!page) return;
  
  int old_mapcount = atomic_inc_return(&page->mapcount);
  if (old_mapcount == 0) {
    // First mapping (mapcount was -1, now 0)
    if (PageAnon(page)) {
      mm_stats.anon_pages++;
    } else {
      mm_stats.file_pages++;
    }
  } else if (old_mapcount == 1) {
    // Became shared (mapcount was 0, now 1)
    mm_stats.shared_pages++;
  }
}

void page_remove_mapping(struct page *page) {
  if (!page) return;

  /* BF: Metadata-only.  Rmap teardown may run with incomplete mapcount
   * coverage after COW fork, so clamp at -1 and never free physical memory
   * from this advisory counter. */
  if (atomic_read(&page->mapcount) <= -1)
    return;

  int new_mapcount = atomic_dec_return(&page->mapcount);
  if (new_mapcount == -1) {
    // Last mapping removed (mapcount was 0, now -1)
    if (PageAnon(page)) {
      if (mm_stats.anon_pages > 0) mm_stats.anon_pages--;
    } else {
      if (mm_stats.file_pages > 0) mm_stats.file_pages--;
    }
  } else if (new_mapcount == 0) {
    // No longer shared (mapcount was 1, now 0)
    if (mm_stats.shared_pages > 0) mm_stats.shared_pages--;
  }
}

/* Check if page is shared (multiple mappings) */
int page_mapcount(struct page *page) {
  if (!page) return -1;
  return atomic_read(&page->mapcount);
}

/* Called by page_put when page should be freed */
void mm_free_page(struct page *page) {
  if (!page) return;
  
  void *pa = page->virtual;
  
  // Debug check - ensure page is not still mapped
  if (atomic_read(&page->mapcount) != -1) {
    printk("WARNING: freeing page with mapcount=%d\n", 
           atomic_read(&page->mapcount));
  }
  
  // Reset page descriptor state
  page->flags = 0;
  page->age_score = 0;
  page->access_count = 0;
  page->last_access = 0;
  page->anon_vma = NULL;
  
  // Free the actual physical page using original kfree
  kfree(pa);
  
  // Update statistics
  mm_stats.free_pages++;
  
}

/* Enhanced kalloc integration - implemented in kalloc.c */

/* Debug function to print page descriptor information */
void print_page_info(struct page *page) {
  if (!page) {
    printk("Page: NULL\n");
    return;
  }
  
  printk("Page pfn=%lu pa=%p:\n", page_to_pfn(page), page->virtual);
  printk("  refcount=%d mapcount=%d\n", 
         atomic_read(&page->refcount), atomic_read(&page->mapcount));
  printk("  flags=0x%lx age_score=%u access_count=%u\n",
         page->flags, page->age_score, page->access_count);
  printk("  last_access=%lu\n", page->last_access);
}

/* Print memory management statistics */
void mm_print_stats(void) {
  printk("=== Memory Management Statistics ===\n");
  printk("Total pages:     %lu (%lu MB)\n", 
         mm_stats.total_pages, 
         mm_stats.total_pages * PAGE_SIZE / 1024 / 1024);
  printk("Free pages:      %lu (%lu MB)\n",
         mm_stats.free_pages, 
         mm_stats.free_pages * PAGE_SIZE / 1024 / 1024); 
  printk("Anonymous pages: %lu\n", mm_stats.anon_pages);
  printk("File pages:      %lu\n", mm_stats.file_pages);
  printk("Shared pages:    %lu\n", mm_stats.shared_pages);
  
  printk("\n=== Enhanced Clock Statistics ===\n");
  printk("Clock scans:     %lu\n", mm_stats.clock_scans);
  printk("Evictions:       %lu\n", mm_stats.clock_evictions);
  printk("Second chances:  %lu\n", mm_stats.clock_second_chance);
  
  printk("\n=== Memory Pressure Statistics ===\n");
  printk("Pressure events: %lu\n", mm_stats.pressure_events);
  printk("Direct reclaims: %lu\n", mm_stats.direct_reclaims);
  printk("Background reclaims: %lu\n", mm_stats.background_reclaims);
  printk("Reclaim failures: %lu\n", mm_stats.reclaim_failures);
  printk("Watermark hits:  %lu\n", mm_stats.watermark_hits);
  
  printk("\n=== Memory Watermarks ===\n");
  printk("Min free:        %lu pages\n", mm_watermarks.min_free);
  printk("Low free:        %lu pages\n", mm_watermarks.low_free);
  printk("High free:       %lu pages\n", mm_watermarks.high_free);
  
  int pressure = mm_check_memory_pressure();
  printk("Current pressure level: %d ", pressure);
  switch(pressure) {
    case 0: printk("(none)\n"); break;
    case 1: printk("(moderate)\n"); break;  
    case 2: printk("(high)\n"); break;
    case 3: printk("(critical)\n"); break;
    default: printk("(unknown)\n"); break;
  }
}
/* External tick counter access function */
extern unsigned long get_ticks(void);

/* Enhanced page access tracking for improved clock algorithm */
void mark_page_accessed(struct page *page) {
  if (!page) return;
  
  unsigned long current_ticks = get_ticks();
  
  // Update access tracking
  page->last_access = current_ticks;
  page->access_count++;
  
  // Increase age score based on recent access
  if (page->age_score < MM_AGE_MAX) {
    page->age_score += 10;  // Give significant boost for access
  }
  
  // Set accessed flag
  SetPageAccessed(page);
}

/* Age pages gradually over time (called by timer interrupt) */
void mm_age_pages(void) {
  static unsigned long last_age_tick = 0;
  unsigned long current_ticks = get_ticks();
  
  /* BF: Keep aging frequent enough that memory-pressure tests do not run
   * with stale page age data for long stretches. */
  if (current_ticks - last_age_tick < 10) {
    return;
  }
  last_age_tick = current_ticks;
  
  for (uint64_t pfn = 0; pfn < max_pfn; pfn++) {
    struct page *page = pfn_to_page(pfn);
    if (!page || atomic_read(&page->refcount) == 0) {
      continue;
    }
    
    // Pages that haven't been accessed recently lose age score
    if (current_ticks - page->last_access > 50) {
      if (page->age_score > 0) {
        page->age_score--;
      }
      
      // Also decay access count slowly
      if (page->access_count > 0 && (current_ticks % 10) == 0) {
        page->access_count = (page->access_count * 3) / 4;
      }
    }
  }
}

/* Memory pressure watermarks */
struct mm_watermarks mm_watermarks;

/* Initialize memory watermark thresholds */
void mm_init_watermarks(void) {
  // Set watermarks as percentages of total memory
  // min_free: 2% of memory (critical threshold)
  // low_free: 5% of memory (start background reclaim)  
  // high_free: 8% of memory (stop background reclaim)
  
  mm_watermarks.min_free = (mm_stats.total_pages * 2) / 100;
  if (mm_watermarks.min_free < 8) mm_watermarks.min_free = 8;  // Minimum 8 pages
  
  mm_watermarks.low_free = (mm_stats.total_pages * 5) / 100;
  if (mm_watermarks.low_free < 16) mm_watermarks.low_free = 16;  // Minimum 16 pages
  
  mm_watermarks.high_free = (mm_stats.total_pages * 8) / 100;
  if (mm_watermarks.high_free < 32) mm_watermarks.high_free = 32;  // Minimum 32 pages
  
  // Ensure watermarks are ordered correctly
  if (mm_watermarks.low_free <= mm_watermarks.min_free) {
    mm_watermarks.low_free = mm_watermarks.min_free + 8;
  }
  if (mm_watermarks.high_free <= mm_watermarks.low_free) {
    mm_watermarks.high_free = mm_watermarks.low_free + 16;
  }
  
  printk("mm: watermarks - min: %lu, low: %lu, high: %lu pages\n",
         mm_watermarks.min_free, mm_watermarks.low_free, mm_watermarks.high_free);
}

/* Check if system is under memory pressure */
int mm_check_memory_pressure(void) {
  uint64_t free_pages = mm_stats.free_pages;
  
  if (free_pages <= mm_watermarks.min_free) {
    return 3;  // Critical pressure
  } else if (free_pages <= mm_watermarks.low_free) {
    return 2;  // High pressure
  } else if (free_pages <= mm_watermarks.high_free) {
    return 1;  // Moderate pressure
  }
  
  return 0;  // No pressure
}

/* Trigger memory reclaim when needed */
void mm_trigger_reclaim(int pages_needed) {
  int pressure_level = mm_check_memory_pressure();
  
  if (pressure_level == 0 && pages_needed == 1) {
    return;  // No pressure and only need 1 page
  }
  
  mm_stats.watermark_hits++;
  
  if (pressure_level >= 2 || pages_needed > 4) {
    // High/critical pressure or large allocation - try direct reclaim
    mm_direct_reclaim(pages_needed);
  } else {
    // Moderate pressure - trigger background reclaim
    mm_background_reclaim();
  }
}

/* Direct reclaim - immediately try to free pages */
int mm_direct_reclaim(int pages_needed) {
  mm_stats.direct_reclaims++;
  mm_stats.reclaim_failures++;
  (void)pages_needed;
  return -1;
}

/* Background reclaim - gentle page reclaim */
void mm_background_reclaim(void) {
  mm_stats.background_reclaims++;
  mm_stats.kswapd_wakeups++;
  mm_stats.reclaim_failures++;
}
