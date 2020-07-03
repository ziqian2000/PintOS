#include "vm/frame.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/page.h"
#include "vm/swap.h"
#include <stdio.h>


void 
frame_table_init (void)
{
  list_init (&frame_table);
  lock_init (&frame_table_lock);
  frame_table_index = malloc (sizeof (struct frame_entry *) * init_ram_pages);
}

void *
frame_get (struct spt_entry *spte, bool ZERO)
{
  lock_acquire (&frame_table_lock);
 
  void *addr = palloc_get_page (ZERO ? PAL_USER | PAL_ZERO : PAL_USER);

  while (addr == NULL)
    addr = frame_evict (ZERO ? PAL_USER | PAL_ZERO : PAL_USER); 
  
  if (addr != NULL)
  {
    struct frame_entry *fe = malloc (sizeof (struct frame_entry));
    fe->frame_addr = addr;
    fe->owner_thread = thread_current ();
    fe->spte = spte;
    list_push_back (&frame_table, &fe->elem);
    frame_table_index [pg_no ((void *)vtop (addr))] = fe;
    lock_release (&frame_table_lock);
  } 
  else
  {
    PANIC ("frame is short");   
  }
  return addr;
}

void
frame_free (void *frame)
{
  if (frame == NULL) return;
  lock_acquire (&frame_table_lock);

  intptr_t frame_pgno = pg_no ((void *)vtop (frame));
  if (frame_table_index [frame_pgno] == NULL)
  {
    PANIC ("frame not exists");
  }
  else
  {
    struct frame_entry *fe = frame_table_index [frame_pgno];
    list_remove (&fe->elem);
    palloc_free_page (frame);
    free (fe);
    frame_table_index [frame_pgno] = NULL;
  }

  lock_release (&frame_table_lock);
}

void *
frame_evict (enum palloc_flags flags)
{
  while(1)
  {
    struct list_elem *e = list_begin (&frame_table);
    for (e = list_begin (&frame_table); e != list_end (&frame_table); e = list_next (e))
    {
      struct frame_entry *fe = list_entry (e, struct frame_entry, elem); 
    
      if (!fe->spte->pinned)
      {
        uint32_t *pd = fe->owner_thread->pagedir;
        uint8_t *upage = fe->spte->addr;
        if (pagedir_is_accessed (pd, upage)) 
          pagedir_set_accessed (pd, upage, false);
        else
        {
          pagedir_clear_page (pd, upage);
          if (fe->spte->type == PAGE_MMAP)
          {
            if (pagedir_is_dirty (pd, upage))
            {
              lock_acquire (&filesys_lock);
              file_write_at (fe->spte->file, fe->frame_addr, fe->spte->read_bytes, fe->spte->ofs);
              lock_release (&filesys_lock);
            }
          }
          else if (fe->spte->type == PAGE_SWAP)
          {
            fe->spte->swap_index = swap_dump (fe->frame_addr);
          }
          else if (fe->spte->type == PAGE_ELF)
          {
            if (pagedir_is_dirty (pd, upage))
            {
              fe->spte->type = PAGE_SWAP;
              fe->spte->swap_index = swap_dump (fe->frame_addr);
            }
          } 
          fe->spte->is_present = false;
          palloc_free_page (fe->frame_addr);
          list_remove (&fe->elem);
          frame_table_index [pg_no ((void*)vtop (fe->frame_addr))] = NULL;
          free (fe);
          return palloc_get_page (flags);
        }
      }
    }
  }

}