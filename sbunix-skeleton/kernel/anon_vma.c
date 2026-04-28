#include "include/mm.h"
#include "include/kalloc.h"
#include "include/kmalloc.h"
#include "include/proc.h"
#include "include/string.h"
#include "include/printk.h"
#include "include/vm.h"

static spinlock_t anon_vma_lock;  // Global anon_vma allocation lock

/* Initialize anonymous VMA subsystem */
void anon_vma_init(void) {
  spin_lock_init(&anon_vma_lock, "anon_vma");
  printk("anon_vma: subsystem initialized\n");
}

/* Allocate a new anon_vma object */
struct anon_vma *anon_vma_alloc(void) {
  struct anon_vma *anon_vma = (struct anon_vma *)kmalloc(sizeof(struct anon_vma));
  if (!anon_vma) return NULL;
  
  memset(anon_vma, 0, sizeof(*anon_vma));
  INIT_LIST_HEAD(&anon_vma->head);
  spin_lock_init(&anon_vma->lock, "anon_vma");
  atomic_set(&anon_vma->refcount, 1);
  anon_vma->root = anon_vma;  // Self is root initially
  anon_vma->parent = NULL;
  anon_vma->num_pages = 0;
  anon_vma->num_vmas = 0;
  
  return anon_vma;
}

/* Free anon_vma when refcount reaches zero */
void anon_vma_free(struct anon_vma *anon_vma) {
  if (!anon_vma) return;
  
  // Debug check - should be empty
  if (!list_empty(&anon_vma->head)) {
    printk("WARNING: anon_vma_free: anon_vma still has chains\n");
  }
  
  /* BF: num_pages is advisory rmap accounting and can be conservative after
   * COW/fork failure cleanup.  Physical lifetime is owned by kfree(), so only
   * num_vmas > 0 indicates a real VMA-chain leak here. */
  if (anon_vma->num_vmas > 0) {
    printk("WARNING: anon_vma_free: anon_vma still has %lu VMAs\n",
           anon_vma->num_vmas);
  }
  
  // Release parent reference if we have one
  if (anon_vma->parent) {
    anon_vma_put(anon_vma->parent);
  }
  
  kmfree(anon_vma, sizeof(struct anon_vma));
}

/* Get reference to anon_vma */
void anon_vma_get(struct anon_vma *anon_vma) {
  if (anon_vma) {
    atomic_inc(&anon_vma->refcount);
  }
}

/* Put reference to anon_vma, free if last reference */
void anon_vma_put(struct anon_vma *anon_vma) {
  if (anon_vma && atomic_dec_and_test(&anon_vma->refcount)) {
    anon_vma_free(anon_vma);
  }
}

/* Create anon_vma for VMA on fork */
int anon_vma_fork(struct vm_area *vma, struct vm_area *parent_vma) {
  struct anon_vma *anon_vma;
  struct anon_vma_chain *avc;
  
  if (!parent_vma->anon_vma) {
    // Parent has no anon_vma, create new one
    anon_vma = anon_vma_alloc();
    if (!anon_vma) return -1;
    vma->anon_vma = anon_vma;
    if (anon_vma_link(vma) < 0) {
      /* BF: anon_vma_link failed before ownership transferred to a chain.
       * Drop the VMA's own reference and leave no dangling pointer for the
       * fork failure cleanup path. */
      anon_vma_put(anon_vma);
      vma->anon_vma = NULL;
      return -1;
    }
    return 0;
  }
  
  // Create child anon_vma inheriting from parent
  anon_vma = anon_vma_alloc();
  if (!anon_vma) return -1;
  
  // Set up inheritance hierarchy
  anon_vma->root = parent_vma->anon_vma->root;
  anon_vma->parent = parent_vma->anon_vma;
  anon_vma_get(parent_vma->anon_vma);  // Parent reference
  
  vma->anon_vma = anon_vma;
  
  // Link to both child and parent anon_vma chains
  if (anon_vma_link(vma) < 0) {
    anon_vma_put(anon_vma);
    vma->anon_vma = NULL;
    return -1;
  }
  
  // Also link to parent's anon_vma for shared COW pages
  avc = (struct anon_vma_chain *)kmalloc(sizeof(struct anon_vma_chain));
  if (!avc) {
    /* BF: anon_vma_unlink consumes both the chain reference and the VMA's
     * own anon_vma reference.  Do not put anon_vma again after unlink. */
    anon_vma_unlink(vma);
    return -1;
  }
  
  avc->anon_vma = parent_vma->anon_vma;
  avc->vma = vma;
  INIT_LIST_HEAD(&avc->anon_vma_node);
  INIT_LIST_HEAD(&avc->vma_node);
  anon_vma_get(parent_vma->anon_vma);
  
  spin_lock(&parent_vma->anon_vma->lock);
  list_add(&avc->anon_vma_node, &parent_vma->anon_vma->head);
  list_add(&avc->vma_node, &vma->anon_vma_chain);
  parent_vma->anon_vma->num_vmas++;
  spin_unlock(&parent_vma->anon_vma->lock);
  
  return 0;
}

/* Link VMA to its anon_vma */
int anon_vma_link(struct vm_area *vma) {
  struct anon_vma *anon_vma = vma->anon_vma;
  struct anon_vma_chain *avc;
  
  if (!anon_vma) return -1;
  
  avc = (struct anon_vma_chain *)kmalloc(sizeof(struct anon_vma_chain));
  if (!avc) return -1;
  
  avc->anon_vma = anon_vma;
  avc->vma = vma;
  INIT_LIST_HEAD(&avc->anon_vma_node);
  INIT_LIST_HEAD(&avc->vma_node);
  anon_vma_get(anon_vma);
  
  spin_lock(&anon_vma->lock);
  list_add(&avc->anon_vma_node, &anon_vma->head);
  list_add(&avc->vma_node, &vma->anon_vma_chain);
  anon_vma->num_vmas++;
  spin_unlock(&anon_vma->lock);
  
  return 0;
}

/* BF: Variant of anon_vma_link that uses a pre-allocated anon_vma_chain
 * node.  Caller must have already kmalloc'd avc.  Used by mprotect
 * preflight allocation to guarantee no allocation failure mid-mutation. */
int anon_vma_link_prealloc(struct vm_area *vma, struct anon_vma_chain *avc) {
  struct anon_vma *anon_vma = vma->anon_vma;
  if (!anon_vma || !avc) return -1;

  avc->anon_vma = anon_vma;
  avc->vma = vma;
  INIT_LIST_HEAD(&avc->anon_vma_node);
  INIT_LIST_HEAD(&avc->vma_node);
  anon_vma_get(anon_vma);

  spin_lock(&anon_vma->lock);
  list_add(&avc->anon_vma_node, &anon_vma->head);
  list_add(&avc->vma_node, &vma->anon_vma_chain);
  anon_vma->num_vmas++;
  spin_unlock(&anon_vma->lock);

  return 0;
}

/* Unlink VMA from all anon_vma chains */
void anon_vma_unlink(struct vm_area *vma) {
  struct list_head *pos = vma->anon_vma_chain.next;

  while (pos != &vma->anon_vma_chain) {
    struct list_head *next = pos->next;
    struct anon_vma_chain *avc = list_entry(pos, struct anon_vma_chain, vma_node);
    struct anon_vma *anon_vma = avc->anon_vma;
    
    spin_lock(&anon_vma->lock);
    list_del(&avc->anon_vma_node);
    list_del(&avc->vma_node);
    if (anon_vma->num_vmas > 0) {
      anon_vma->num_vmas--;
    }
    spin_unlock(&anon_vma->lock);
    
    anon_vma_put(anon_vma);
    kmfree(avc, sizeof(struct anon_vma_chain));
    pos = next;
  }
  
  if (vma->anon_vma) {
    anon_vma_put(vma->anon_vma);
    vma->anon_vma = NULL;
  }
}

/* Add anonymous reverse mapping for page */
int page_add_anon_rmap(struct page *page, struct vm_area *vma, uint64_t address) {
  if (!page || !vma || !vma->anon_vma) return -1;
  
  // Associate page with anon_vma
  page->anon_vma = vma->anon_vma;
  SetPageAnon(page);
  
  // Update mapping count
  page_add_mapping(page);
  
  // Update anon_vma statistics
  spin_lock(&vma->anon_vma->lock);
  vma->anon_vma->num_pages++;
  /* BF: Unconditional increment — the old guard `if (vma->resident_pages)`
   * prevented the first page from ever being counted. */
  vma->resident_pages++;
  spin_unlock(&vma->anon_vma->lock);
  
  return 0;
}

/* Remove reverse mapping for page */
void page_remove_rmap(struct page *page, struct vm_area *vma) {
  if (!page || !PageAnon(page) || !page->anon_vma) return;

  /* BF: Update advisory mapping metadata only.  PTE teardown still calls
   * kfree() once and remains the sole owner of physical lifetime. */
  page_remove_mapping(page);

  // If this was the last mapping, clear anon_vma association
  if (atomic_read(&page->mapcount) == -1) {
    /* BF: Defensive re-check — page->anon_vma may have been cleared
     * between the function entry check and here if another caller
     * ran page_remove_rmap on the same page concurrently (e.g. via
     * CoW fault decrementing mapcount on the shared page). */
    if (!page->anon_vma) return;
    spin_lock(&page->anon_vma->lock);
    if (page->anon_vma->num_pages > 0) {
      page->anon_vma->num_pages--;
    }
    spin_unlock(&page->anon_vma->lock);

    page->anon_vma = NULL;
    ClearPageAnon(page);
  }

  /* BF: Unconditional resident_pages decrement (was gated on non-zero
   * which prevented counting when first page was added unconditionally). */
  if (vma && vma->anon_vma) {
    spin_lock(&vma->anon_vma->lock);
    if (vma->resident_pages > 0) {
      vma->resident_pages--;
    }
    spin_unlock(&vma->anon_vma->lock);
  }
}

/* Find all VMAs that map a given page (for debugging) */
int page_mapped_in_vma_count(struct page *page) {
  struct anon_vma *anon_vma;
  int count = 0;
  
  if (!PageAnon(page) || !page->anon_vma) return 0;
  
  anon_vma = page->anon_vma;
  spin_lock(&anon_vma->lock);
  
  for (struct list_head *pos = anon_vma->head.next;
       pos != &anon_vma->head;
       pos = pos->next) {
    // Each avc represents a potential mapping
    count++;
  }
  
  spin_unlock(&anon_vma->lock);
  return count;
}

/* Debug function to print anon_vma statistics */
void anon_vma_print_stats(struct anon_vma *anon_vma) {
  if (!anon_vma) {
    printk("anon_vma: NULL\n");
    return;
  }
  
  spin_lock(&anon_vma->lock);
  printk("anon_vma %p:\n", anon_vma);
  printk("  refcount=%d num_vmas=%lu num_pages=%lu\n",
         atomic_read(&anon_vma->refcount), anon_vma->num_vmas, anon_vma->num_pages);
  printk("  root=%p parent=%p\n", anon_vma->root, anon_vma->parent);
  
  if (!list_empty(&anon_vma->head)) {
    int chain_count = 0;
    
    for (struct list_head *pos = anon_vma->head.next;
         pos != &anon_vma->head;
         pos = pos->next) {
      chain_count++;
    }
    printk("  chain_count=%d\n", chain_count);
  } else {
    printk("  chain_count=0\n");
  }
  
  spin_unlock(&anon_vma->lock);
}
