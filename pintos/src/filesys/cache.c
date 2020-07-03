#include "cache.h"
#include "filesys.h"
#include "../threads/malloc.h"
#include "../threads/synch.h"
#include "../threads/thread.h"
#include "../devices/timer.h"

#define INVALID_SECTOR ((block_sector_t) - 1)

struct cache_entry
    {
        struct lock entry_lock;   
        struct condition no_need;  // no readers & writers
        struct condition no_writers;

        int read_cnt, write_cnt;   // number of readers/writers
        int read_wait_cnt, write_wait_cnt; // number of readers/writers that is waiting. 
        
        /*
            Sector number
            sector == INVALID_SECTOR means this cache block is free
        */
        block_sector_t sector;


        bool is_up_to_date;  
        bool is_dirty;

        struct lock data_lock;
        uint8_t data[BLOCK_SECTOR_SIZE];
    };


/* Cache. */
#define CACHE_MAX 64
struct cache_entry cache[CACHE_MAX];

struct lock cache_sync;

static int evict_hand = 0;


static void flush_daemon_init(void);
static void read_ahead_daemon_init(void);

void 
cache_init(void)
{
    lock_init(&cache_sync);

    for(int i = 0; i<CACHE_MAX;i++)
    {
        struct cache_entry *b = &cache[i];
        lock_init(&b->entry_lock);
        lock_init(&b->data_lock);
        cond_init(&b->no_need);
        cond_init(&b->no_writers);
        b->sector = INVALID_SECTOR; // initially free
        b->write_cnt = b->write_wait_cnt = 0;
        b->read_cnt = b->read_wait_cnt = 0;
        lock_init(&b->data_lock);
    }
    //flush_daemon_init();
    //read_ahead_daemon_init();
}


void 
cache_flush(void)
{
    for(int i = 0; i<CACHE_MAX; i++)
    {
        struct cache_entry *b = &cache[i];
        block_sector_t sector;

        lock_acquire(&b->entry_lock);
        sector = b->sector;
        lock_release(&b->entry_lock);

        if(sector !=INVALID_SECTOR) 
        {
            b = cache_lock(sector, EXCLUSIVE);
            if(b->is_up_to_date && b->is_dirty)
            {
                block_write(fs_device, b->sector, b->data);
                b->is_dirty = false;
            }
            cache_unlock(b);
        }
    }
}


/*The call must hold <cache_sync>*/
struct cache_entry *
cache_find(block_sector_t sector, enum lock_type type)
{
    for(int i = 0;i<CACHE_MAX;i++)
    {
        struct cache_entry *b = &cache[i];
        lock_acquire(&b->entry_lock);
        if(b->sector == sector)
        {  // found
            lock_release(&cache_sync);
            if(type == NON_EXCLUSIVE)
            {
                b->read_wait_cnt++;
                if(b->write_cnt || b->write_wait_cnt)
                    do
                    {
                        cond_wait(&b->no_writers, &b->entry_lock);
                    } while (b->write_cnt);
                
                b->read_cnt++;
                b->read_wait_cnt--;
            }
            else 
            {
                b->write_wait_cnt++;  
                if(b->read_cnt || b->read_wait_cnt || b->write_cnt)
                    do
                    {
                        cond_wait(&b->no_need, &b->entry_lock);
                    } while (b->read_cnt || b->write_cnt);
                b->write_cnt++;
                b->write_wait_cnt--;    
            }

            lock_release(&b->entry_lock);
            return b;
        }   
        lock_release(&b->entry_lock);
    }
    return NULL; // not found
}

struct cache_entry *
cache_try_lock(block_sector_t sector, enum lock_type type, bool *evicted)
{
    lock_acquire(&cache_sync);

    struct cache_entry *entry = cache_find(sector, type);
    if(entry != NULL)
        return entry;

    // Not in cache. Find a free entry;
    for(int i = 0;i<CACHE_MAX;i++)
    {
        struct cache_entry *b = &cache[i];
        lock_acquire(&b->entry_lock);
        if(b->sector == INVALID_SECTOR)
        {
            lock_release(&b->entry_lock);

            b->sector = sector;
            b->is_up_to_date = false;
            ASSERT (b->read_cnt == 0 && b->write_cnt == 0);
            if(type == EXCLUSIVE)
                b->write_cnt = 1;
            else 
                b->read_cnt = 1;

            lock_release(&cache_sync);    
            return b;
        }
        lock_release(&b->entry_lock);
    }

    // no empty entry, try evicting, still holding <cache_sync>
    for(int i = 0; i < CACHE_MAX; i++)
    {
        struct cache_entry *b = &cache[evict_hand];
        if(++evict_hand >= CACHE_MAX) evict_hand = 0;

        lock_acquire(&b->entry_lock);
        if(!b->read_cnt && !b->write_cnt && !b->write_wait_cnt && !b->read_wait_cnt)
        {  // ok for evicting  
            b->write_cnt = 1;
            lock_release(&b->entry_lock);
            lock_release(&cache_sync);

            if(b->is_up_to_date && b->is_dirty)   // write back to disk
            {
                block_write(fs_device, b->sector, b->data);
                b->is_dirty = false;
            }

            lock_acquire(&b->entry_lock);
            b->write_cnt = 0;
            if(!b->read_wait_cnt && !b->write_wait_cnt)  // no one is waiting for it, ok for evicting
                b->sector = INVALID_SECTOR;
            else{  
                if(b->read_wait_cnt)// give to the waiter
                    cond_broadcast(&b->no_writers, &b->entry_lock);
                else 
                    cond_signal(&b->no_need, &b->entry_lock);
            }

            *evicted = true;
            lock_release(&b->entry_lock);
            return NULL;
        }
        lock_release(&b->entry_lock);
    }

    lock_release(&cache_sync);
    return NULL;
}

struct cache_entry *
cache_lock(block_sector_t sector, enum lock_type type)
{
    while(true)         // try again after 1000ms if fails 
    {
       struct cache_entry *entry = NULL;
       while (true)   // retry immediately if eviction happens
       {
            bool evicted = false;
            entry = cache_try_lock(sector, type, &evicted);
            if(!evicted) break;
       }

       if(entry != NULL) return entry;
       timer_sleep(1000);
    }
}

void *
cache_read(struct cache_entry *b)
{
    lock_acquire(&b->data_lock);
    if(!b->is_up_to_date)    // fetch from disk
    {
        block_read(fs_device, b->sector, b->data);
        b->is_up_to_date = true;
        b->is_dirty = false;
    }
    lock_release(&b->data_lock);
    return b->data;
}

void *
cache_setzero(struct cache_entry * b)
{
    memset(b->data, 0, BLOCK_SECTOR_SIZE);
    b->is_up_to_date =  true;
    b->is_dirty = true;

    return b->data;
}

void 
cache_dirty(struct cache_entry *b)  // mark as dirty
{
    b->is_dirty = true;
}

void 
cache_unlock(struct cache_entry *b)
{
    lock_acquire(&b->entry_lock);
    if(b->read_cnt)
    {
        if(--b->read_cnt == 0)
            cond_signal(&b->no_need, &b->entry_lock);
    }
    else if(b->write_cnt)
    {
        b->write_cnt--;
        if(b->read_wait_cnt)
            cond_broadcast(&b->no_writers, &b->entry_lock);
        else 
            cond_signal(&b->no_need, &b->entry_lock);
    }
    else 
        NOT_REACHED();
    
    lock_release(&b->entry_lock);
}

void 
cache_free(block_sector_t sector)
{
    lock_acquire(&cache_sync);
    for(int i =0 ;i<CACHE_MAX;i++)
    {
        struct cache_entry *b = &cache[i];

        lock_acquire(&b->entry_lock);
        if(b->sector == sector)
        {
            if(!b->read_cnt && !b->write_cnt && !b->read_wait_cnt && !b->write_wait_cnt)
                b->sector = INVALID_SECTOR;
            lock_release(&b->entry_lock);  
            break; 
        }
        lock_release(&b->entry_lock);
    }
    lock_release(&cache_sync);
}


/*flush daemon thread*/

static void 
flush_daemon(void *aux)  // flush cache periodically
{
    while(true)
    {
        timer_sleep(30000);
        cache_flush();
    }
}

static void flush_daemon_init(void)
{
    thread_create("flush_daemon", PRI_MIN, flush_daemon, NULL);
}

/*readahead daemon thread*/

struct readahead_block
    {
        struct list_elem list_elem;    // element in <readahead_list>
        block_sector_t sector;
    };

static struct lock readahead_lock;

static struct condition readahead_list_nonempty;

static struct list readahead_list;

static void 
readahead_daemon(void *aux)
{
    while(true)
    {
        struct readahead_block *b;
        struct cache_entry *entry;

        lock_acquire(&readahead_lock);
        while(list_empty(&readahead_list))
            cond_wait(&readahead_list_nonempty, &readahead_lock);
        
        b = list_entry(list_pop_front(&readahead_list), struct readahead_block, list_elem);

        lock_release(&readahead_lock);

        // read the block into cache
        entry = cache_lock(b->sector, NON_EXCLUSIVE);
        cache_read(entry);
        cache_unlock(entry);
        
        free(b);
    }
}

static void read_ahead_daemon_init(void)
{
    lock_init(&readahead_lock);
    cond_init(&readahead_list_nonempty);
    list_init(&readahead_list);
    thread_create("readahead_daemon", PRI_MIN, readahead_daemon, NULL);
}

void 
cache_readahead(block_sector_t sector)
{
    struct readahead_block *b = malloc(sizeof *b);
    if(b == NULL) return;

    b->sector = sector;

    // Add <b> to the list;
    lock_acquire(&readahead_lock);
    list_push_back(&readahead_list, &b->list_elem);
    cond_signal(&readahead_list_nonempty, &readahead_lock);
    lock_release(&readahead_lock);
}

