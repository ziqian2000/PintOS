#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <bitmap.h>
#include <devices/block.h>
#include "threads/synch.h"
#include "vm/page.h"

#define SECTOR_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

struct lock swap_lock;
struct bitmap *swap_map;
struct block *swap_block_device;

void swap_init (void);
void swap_load (struct spt_entry *spte);
size_t swap_dump (void *);

#endif