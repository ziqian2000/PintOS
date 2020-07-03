#include "vm/swap.h"
#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include <devices/block.h>
#include <stdio.h>

void
swap_init ()
{
  lock_init (&swap_lock);
  swap_block_device = block_get_role (BLOCK_SWAP);
  swap_map = bitmap_create (block_size (swap_block_device) / SECTOR_PER_PAGE);
  bitmap_set_all (swap_map, 0);
}

void
swap_load (struct spt_entry *spte)
{
  lock_acquire (&swap_lock);
  if (bitmap_test (swap_map, spte->swap_index) != 0)
  {
    bitmap_set (swap_map, spte->swap_index, 0);
    for (size_t i = 0; i < SECTOR_PER_PAGE; i++)
      block_read (swap_block_device, spte->swap_index * SECTOR_PER_PAGE + i, spte->addr + BLOCK_SECTOR_SIZE * i);
    lock_release(&swap_lock);
  }
  else
  {
    PANIC ("Swap block is free");
  }
}

size_t
swap_dump (void *frame)
{
  lock_acquire (&swap_lock);
  size_t free_index = bitmap_scan_and_flip (swap_map, 0, 1, 0);
  if (free_index == BITMAP_ERROR)
  {
    PANIC ("Swap device is full");
  }
  else
  {
    for (size_t i = 0; i < SECTOR_PER_PAGE; i++)
      block_write (swap_block_device, free_index * SECTOR_PER_PAGE + i, frame + BLOCK_SECTOR_SIZE * i);
    lock_release (&swap_lock);
    return free_index;
  }
}