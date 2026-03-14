#include <kmalloc.h>
#include <kalloc.h>
#include <string.h>

/* ---- SLUB-style slab allocator ---- */

#define SLAB_HDR_SIZE  64  /* reserved at start of each slab page */
#define END_OF_LIST    0xFFFF

struct slab_header {
    struct slab_header *next;    /* next slab in list */
    uint16_t free_count;         /* free objects in this slab */
    uint16_t total_objects;      /* total objects capacity */
    uint16_t next_free;          /* index of first free object */
    uint8_t  cache_idx;          /* index into caches[] that owns this slab */
    uint8_t  _pad;
};

struct kmem_cache {
    uint32_t object_size;
    uint32_t objects_per_slab;
    struct slab_header *current;  /* active slab for allocation */
    struct slab_header *partial;  /* partially used slabs */
    struct slab_header *full;     /* completely used slabs */
};

#define NCACHES 8
static uint32_t cache_sizes[NCACHES] = {16, 32, 64, 128, 256, 512, 1024, 2048};
static struct kmem_cache caches[NCACHES];
static int slub_initialized;

/* Object address within a slab */
static inline void *slab_obj(struct slab_header *slab, uint32_t idx, uint32_t obj_size) {
    return (char *)slab + SLAB_HDR_SIZE + (uint64_t)idx * obj_size;
}

/* Get the embedded freelist index stored in a free object */
static inline uint16_t obj_get_next(void *obj) {
    return *(uint16_t *)obj;
}

/* Set the embedded freelist index in a free object */
static inline void obj_set_next(void *obj, uint16_t next) {
    *(uint16_t *)obj = next;
}

/*
 * Allocate a new slab page and initialize its freelist.
 */
static struct slab_header *slab_create(struct kmem_cache *cache) {
    char *page = kalloc();
    if (!page)
        return NULL;
    memset(page, 0, PAGE_SIZE);

    struct slab_header *slab = (struct slab_header *)page;
    slab->next = NULL;
    slab->total_objects = (uint16_t)cache->objects_per_slab;
    slab->free_count = slab->total_objects;
    slab->next_free = 0;
    slab->cache_idx = (uint8_t)(cache - caches);

    /* Build embedded freelist: each free object points to the next */
    for (uint32_t i = 0; i < cache->objects_per_slab; i++) {
        void *obj = slab_obj(slab, i, cache->object_size);
        if (i + 1 < cache->objects_per_slab)
            obj_set_next(obj, (uint16_t)(i + 1));
        else
            obj_set_next(obj, END_OF_LIST);
    }

    return slab;
}

/*
 * Remove a slab from a linked list. Returns 1 if found.
 */
static int slab_list_remove(struct slab_header **list, struct slab_header *slab) {
    struct slab_header **pp = list;
    while (*pp) {
        if (*pp == slab) {
            *pp = slab->next;
            slab->next = NULL;
            return 1;
        }
        pp = &(*pp)->next;
    }
    return 0;
}

static void *slab_alloc(struct kmem_cache *cache) {
    /* Try current slab */
    if (cache->current && cache->current->free_count > 0) {
        struct slab_header *slab = cache->current;
        uint16_t idx = slab->next_free;
        void *obj = slab_obj(slab, idx, cache->object_size);
        slab->next_free = obj_get_next(obj);
        slab->free_count--;

        /* If current is now full, move to full list */
        if (slab->free_count == 0) {
            cache->current = NULL;
            slab->next = cache->full;
            cache->full = slab;
        }
        return obj;
    }

    /* Current is full or NULL — try promoting a partial slab */
    if (cache->partial) {
        cache->current = cache->partial;
        cache->partial = cache->partial->next;
        cache->current->next = NULL;
        return slab_alloc(cache);  /* retry with new current */
    }

    /* No partial slabs — create a new one */
    struct slab_header *slab = slab_create(cache);
    if (!slab)
        return NULL;
    cache->current = slab;
    return slab_alloc(cache);
}

static void slab_free(struct kmem_cache *cache, void *ptr) {
    /* Determine which slab this object belongs to */
    struct slab_header *slab = (struct slab_header *)PGROUNDDOWN((uint64_t)ptr);
    uint32_t idx = (uint32_t)((char *)ptr - (char *)slab - SLAB_HDR_SIZE) / cache->object_size;

    /* Push onto embedded freelist */
    obj_set_next(ptr, slab->next_free);
    slab->next_free = (uint16_t)idx;
    int was_full = (slab->free_count == 0);
    slab->free_count++;

    /* If slab was full, move it to partial (or make it current) */
    if (was_full) {
        slab_list_remove(&cache->full, slab);
        if (!cache->current) {
            cache->current = slab;
        } else {
            slab->next = cache->partial;
            cache->partial = slab;
        }
    }

    /* If slab is completely free, consider returning it to buddy */
    if (slab->free_count == slab->total_objects) {
        /* Count free slabs to avoid thrashing */
        int free_slabs = 0;
        if (cache->current && cache->current->free_count == cache->current->total_objects)
            free_slabs++;
        struct slab_header *s = cache->partial;
        while (s) {
            if (s->free_count == s->total_objects)
                free_slabs++;
            s = s->next;
        }

        if (free_slabs > 2) {
            /* Return this slab to the page allocator */
            if (slab == cache->current) {
                cache->current = NULL;
            } else {
                slab_list_remove(&cache->partial, slab);
            }
            kfree(slab);
        }
    }
}

/* ---- Public API ---- */

void kmalloc_init(void) {
    for (int i = 0; i < NCACHES; i++) {
        caches[i].object_size = cache_sizes[i];
        caches[i].objects_per_slab = (PAGE_SIZE - SLAB_HDR_SIZE) / cache_sizes[i];
        caches[i].current = NULL;
        caches[i].partial = NULL;
        caches[i].full = NULL;
    }
    slub_initialized = 1;
}

void *kmalloc(uint64_t size) {
    if (size == 0)
        return 0;

    /* Before SLUB init, fall back to page allocator */
    if (!slub_initialized) {
        if (size <= PAGE_SIZE)
            return kalloc();
        return 0;
    }

    for (int i = 0; i < NCACHES; i++) {
        if (caches[i].object_size >= size)
            return slab_alloc(&caches[i]);
    }

    if (size <= PAGE_SIZE)
        return kalloc();
    return 0;
}

void kmfree(void *ptr, uint64_t size) {
    if (!ptr)
        return;

    if (!slub_initialized) {
        kfree(ptr);
        return;
    }

    /* Derive the owning cache from the slab header, not caller-provided size.
     * This prevents freeing into the wrong cache when caller passes a
     * different size than what was used for allocation. */
    struct slab_header *slab = (struct slab_header *)PGROUNDDOWN((uint64_t)ptr);
    uint8_t idx = slab->cache_idx;
    if (idx < NCACHES) {
        slab_free(&caches[idx], ptr);
        return;
    }

    /* Not from a slab cache (e.g. full-page allocation) */
    (void)size;
    kfree(ptr);
}
