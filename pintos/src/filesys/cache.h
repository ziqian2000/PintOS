#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "../devices/block.h"

void cache_init(void);
void cache_flush(void);

enum lock_type
    {
        EXCLUSIVE,
        NON_EXCLUSIVE
    };

struct cache_entry *cache_lock(block_sector_t , enum lock_type);
void cache_unlock(struct cache_entry *);

void *cache_read(struct cache_entry *);
void *cache_setzero(struct cache_entry *);

void cache_dirty(struct cache_entry *);
void cache_free(block_sector_t); 
void cache_readahead(block_sector_t);

#endif 