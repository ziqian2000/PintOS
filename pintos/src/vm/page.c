#include "vm/page.h"
#include "threads/thread.h"
#include <stdio.h>

static void spte_clear (struct hash_elem *e, void *aux UNUSED);

void 
spt_init (struct hash *spt)
{
  hash_init (spt, spte_addr, spte_addr_less, NULL);
}

void
spt_clear (struct hash *spt)
{
  hash_destroy (spt, spte_clear);
}

unsigned
spte_addr (const struct hash_elem *e, void *aux UNUSED)
{
  struct spt_entry *x = hash_entry (e, struct spt_entry, elem);
  return hash_int ((int) x->addr);
}

bool
spte_addr_less (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
  struct spt_entry *x = hash_entry (a, struct spt_entry, elem);
  struct spt_entry *y = hash_entry (b, struct spt_entry, elem);
  return x->addr < y->addr;
}

static void
spte_clear (struct hash_elem *e, void *aux UNUSED)
{
  struct spt_entry *spte = hash_entry (e, struct spt_entry, elem);
  if (spte->is_present)
  {
    frame_free (pagedir_get_page (thread_current ()->pagedir, spte->addr));
    pagedir_clear_page (thread_current ()->pagedir, spte->addr);
  }
  free (spte);
}

struct spt_entry * 
get_spte (const void *addr)
{
  struct spt_entry spte;
  spte.addr = pg_round_down (addr);
  struct hash_elem *elem = hash_find(&thread_current ()->spt, &spte.elem);
  if (elem != NULL) return hash_entry (elem, struct spt_entry, elem);
  else return NULL;
}

static bool
spt_load_file (struct spt_entry *spte)
{
  struct file *file = spte->file;
  uint8_t *upage = spte->addr;
  off_t ofs = spte->ofs;
  uint32_t page_read_bytes = spte->read_bytes;
  uint32_t page_zero_bytes = spte->zero_bytes;
  bool writable = spte->writeable;
  
  uint8_t *kpage = frame_get (spte, page_read_bytes == 0);

  if (kpage == NULL)
    return false;

  lock_acquire (&filesys_lock);
  if (file_read_at (file, kpage, page_read_bytes, ofs) != (int) page_read_bytes)
  {
    frame_free (kpage);
    lock_release (&filesys_lock);
    return false; 
  }
  lock_release (&filesys_lock);

  memset (kpage + page_read_bytes, 0, page_zero_bytes);
  if (!install_page (upage, kpage, writable)) 
  {
    frame_free (kpage);
    return false; 
  }
  
  spte->is_present = true;

  return true;
}

static bool
spt_load_swap (struct spt_entry *spte)
{
  uint8_t *frame = frame_get (spte, false);
  
  if (frame != NULL)
  {
    if (install_page (spte->addr, frame, spte->writeable))
    {
      swap_load (spte);
      goto success;
    }
  }
  frame_free (frame);
  return false;
success:
  spte->is_present = true;
  return true;
}

bool 
spt_load (struct spt_entry *spte)
{
  spte->pinned = true;
  if (spte->is_present) return true;
  switch (spte->type)
    {
      case PAGE_ELF  : 
      case PAGE_MMAP : 
      {
        return spt_load_file (spte);
      }
      case PAGE_SWAP :
      {
        return spt_load_swap (spte);
      }
    }
  return false;
}

bool
spt_stack_growth (void *addr)
{
//puts("!!!");

  if ((size_t)(PHYS_BASE - pg_round_down(addr)) > ULIMIT_STACK) return false;
  struct spt_entry *spte = malloc (sizeof (struct spt_entry));

  //printf("======[1]  %d\n",*(int *)addr);

  spte->type = PAGE_SWAP;
  spte->addr = pg_round_down (addr);
  spte->pinned = true;
  spte->writeable = true;
  spte->is_present = true;

  uint8_t *frame_addr = frame_get (spte, false);

  //printf("======[2]  %d\n",*frame_addr);

  if (frame_addr != NULL)
  { 
    if (install_page(spte->addr, frame_addr, spte->writeable))
    {
      //printf("======[1]  %d\n",&thread_current()->spt.elem_cnt);
      struct hash_elem *old = hash_insert (&thread_current ()->spt, &spte->elem);
      //puts("ASDDDDD");
      if (old == NULL) goto success;
    }
  }
  free (spte);
  frame_free (frame_addr);
  return false;
success:
//puts("####ASDDDDD");
  return true;
}

void
spt_remove (struct spt_entry *spte)
{
  hash_delete (&thread_current ()->spt, &spte->elem);
  free (spte);
}

bool
spt_link_elf (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  struct spt_entry *spte = malloc (sizeof (struct spt_entry));
  spte->type = PAGE_ELF;
  spte->addr = upage;
  spte->pinned = false;
  spte->writeable = writable;
  spte->is_present = false;
  spte->file = file;
  spte->ofs = ofs;
  spte->read_bytes = read_bytes;
  spte->zero_bytes = zero_bytes;
  return hash_insert (&thread_current ()->spt, &spte->elem) == NULL;
}

bool 
spt_link_mmap (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable)
{
  struct spt_entry *spte = malloc (sizeof (struct spt_entry));
  spte->type = PAGE_MMAP;
  spte->addr = upage;
  spte->pinned = false;
  spte->writeable = writable;
  spte->is_present = false;
  spte->file = file;
  spte->ofs = ofs;
  spte->read_bytes = read_bytes;
  spte->zero_bytes = zero_bytes;

  struct mmap_entry *me = malloc (sizeof (struct mmap_entry));
  me->spte = spte;
  me->mapid = thread_current() ->mapid;
  list_push_back (&thread_current ()->mmap_list, &me->elem);

  return hash_insert (&thread_current ()->spt, &spte->elem) == NULL;
}
