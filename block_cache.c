/*

MIT License

Copyright (c) 2025 Michael Neil (Far Left Lane)

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "block_cache.h"

#include <ff.h>
#include <string.h>
#include <diskio.h>
#include <stdbool.h>

#if IO_STATS
#include <stdio.h>
#include "hw_config.h"

uint32_t s_block_cache_read_count = 0;
uint32_t s_block_cache_read_ahead_count = 0;
uint32_t s_block_cache_read_hit_count = 0;
uint32_t s_block_cache_read_free_count = 0;
uint32_t s_block_cache_read_evict_count = 0;
uint32_t s_block_cache_read_miss_count = 0;

uint32_t s_block_cache_write_count = 0;
uint32_t s_block_cache_write_hit_count = 0;
uint32_t s_block_cache_write_free_count = 0;
uint32_t s_block_cache_write_evict_count = 0;
uint32_t s_block_cache_write_flush_count = 0;
#endif


#define BLOCK_SIZE          512
#define CACHE_SIZE          128          // Number of cache entries, 128 = 64K bytes
#define HASH_SIZE           257          // Prime number bucket count

/* -------------------------------
    cache_entry
    block identifies and state
    hash chain
    lru  double linked list
    block data
   ------------------------------- */

typedef struct cache_entry 
{
    LBA_t       sector;             //  Sector (can be 64-bit or 32-bit based on FF_LBA64 from ff.h)
    BYTE        pdrv;               //  Drive number
    bool        dirty;              //  Needs write-back
    bool        valid;              //  Entry active

    struct cache_entry *hash_next;  //  hash chain

    struct cache_entry *lru_prev;   //  LRU doubly-linked list
    struct cache_entry *lru_next;

    uint8_t  data[BLOCK_SIZE];      //  Cached contents
} cache_entry;


//  cache storage
static cache_entry s_cache[CACHE_SIZE];         
static cache_entry *s_hash_table[HASH_SIZE];
static bool s_dirty_blocks = true;

//  LRU list: entry at head = most recently used. tail = eviction candidate
static cache_entry *s_lru_head = NULL;
static cache_entry *s_lru_tail = NULL;

//  Free list
static cache_entry *s_free_head = NULL;


//  LRU functions

static void lru_remove(cache_entry *e)
{
    if (e->lru_prev) e->lru_prev->lru_next = e->lru_next;
    if (e->lru_next) e->lru_next->lru_prev = e->lru_prev;

    if (s_lru_head == e) s_lru_head = e->lru_next;
    if (s_lru_tail == e) s_lru_tail = e->lru_prev;

    e->lru_prev = e->lru_next = NULL;
}

static void lru_insert_front(cache_entry *e)
{
    e->lru_next = s_lru_head;
    e->lru_prev = NULL;

    if (s_lru_head) s_lru_head->lru_prev = e;
    s_lru_head = e;

    if (!s_lru_tail)
        s_lru_tail = e;
}

//  Move entry to front on page hit
static void lru_touch(cache_entry *e)
{
    if (s_lru_head == e) return;
    lru_remove(e);
    lru_insert_front(e);
}

//  free functions, we re-use the hash pointers

static cache_entry *free_get(void)
{
    if (s_free_head == NULL)
        return NULL;
    
    cache_entry *result = s_free_head;;
    s_free_head = result->hash_next;

    return result;
}

static void free_insert(cache_entry *e)
{
    e->valid = false;
    e->hash_next = s_free_head;
    s_free_head = e;
}

//  Hash table functions

static uint32_t hash_key(BYTE pdrv, LBA_t sector)
{
    //  Need to colapse 5 or 9 bytes into 4 for hashing 
#if FF_LBA64
    uint32_t value = (sector & 0xFFFFFFFF) ^ (sector >> 32) ^ (pdrv << (32 - sizeof(BYTE)));
#else
    uint32_t value = (sector) ^ (pdrv << (32 - sizeof(BYTE)));
#endif
    
    return (value * 2654435761u) % HASH_SIZE;           //  Simple, fast multiplicative hash
}

static cache_entry *hash_lookup(BYTE pdrv, LBA_t sector)
{
    cache_entry *e = s_hash_table[hash_key(pdrv, sector)];
    while (e) 
    {
        if ((e->valid) && (e->sector == sector) && (e->pdrv == pdrv))
            return e;
        e = e->hash_next;
    }
    return NULL;
}

static void hash_insert(cache_entry *e)
{
    uint32_t h = hash_key(e->pdrv, e->sector);
    e->hash_next = s_hash_table[h];
    s_hash_table[h] = e;
}

static void hash_remove(cache_entry *e)
{
    uint32_t h = hash_key(e->pdrv, e->sector);
    cache_entry **pp = &s_hash_table[h];

    while (*pp) 
    {
        if (*pp == e) 
        {
            *pp = e->hash_next;
            e->hash_next = NULL;
            return;
        }
        pp = &(*pp)->hash_next;
    }
}


//  Cache functions

static DRESULT s_evict_error = RES_OK;

static cache_entry *evict_entry(void)
{
    cache_entry *e = s_lru_tail;

    if (!e)
    {
        s_evict_error = RES_PARERR;
        return NULL;
    }

    //  Write back if dirty
    if (e->valid && e->dirty)
    {
        s_evict_error = disk_write_no_cache (e->pdrv, e->data, e->sector, 1);          //  1 is a 512 byte sector
        if (s_evict_error != RES_OK)
        {
            return NULL;
        }
    }

    //  Remove from cache
    hash_remove(e);
    lru_remove(e);

    e->valid = false;
    e->dirty = false;

    return e;
}


//  Public functions

void block_cache_init(void)
{
    memset(s_cache, 0, sizeof(s_cache));
    memset(s_hash_table, 0, sizeof(s_hash_table));

    s_lru_head = NULL;              //  LRU list initially empty; entries inserted when used
    s_lru_tail = NULL;

    for (int i=0; i<CACHE_SIZE; i++)            //  Populate the free list
    {
        if (!s_cache[i].valid) 
        {
            free_insert(&s_cache[i]);
        }
    }

}

DRESULT block_cache_read_block(BYTE pdrv, LBA_t sector, BYTE *out_data)
{
#if IO_STATS
    if (out_data == NULL)
        s_block_cache_read_ahead_count++;
    else
        s_block_cache_read_count++;
#endif

    cache_entry *e = hash_lookup(pdrv, sector);

    if (e) 
    {
        //  Cache hit
        lru_touch(e);

        if(out_data != NULL)
            memcpy(out_data, e->data, BLOCK_SIZE);

#if IO_STATS
    if (out_data != NULL)
        s_block_cache_read_hit_count++;
#endif

        return 0;
    }

    //  Cache miss → find free entry or evict
    cache_entry *free_entry = free_get();

#if IO_STATS
    if (free_entry != NULL)
        s_block_cache_read_free_count++;
#endif

    if (!free_entry)
    {
        free_entry = evict_entry();
#if IO_STATS
        s_block_cache_read_evict_count++;
#endif
    }
    if (!free_entry)
    {
        return s_evict_error;
    }

#if IO_STATS
    if (out_data != NULL)
        s_block_cache_read_miss_count++;
#endif

    //  Load block from device
    DRESULT result = disk_read_no_cache(pdrv, free_entry->data, sector, 1);
    if (result != RES_OK)
        return result;

    free_entry->sector = sector;
    free_entry->pdrv = pdrv;
    free_entry->dirty = false;
    free_entry->valid = true;

    //  Add to hash + LRU
    hash_insert(free_entry);
    lru_insert_front(free_entry);
    
    if(out_data != NULL)
        memcpy(out_data, free_entry->data, BLOCK_SIZE);
    
    return RES_OK;
}

DRESULT block_cache_write_block(BYTE pdrv, LBA_t sector, const BYTE *in_data)
{
#if IO_STATS
    s_block_cache_write_count++;
#endif

    cache_entry *e = hash_lookup(pdrv, sector);

    if (e) 
    {
        //  Cache hit
        memcpy(e->data, in_data, BLOCK_SIZE);
        e->dirty = true;
        s_dirty_blocks = true;
        lru_touch(e);

#if IO_STATS
            s_block_cache_write_hit_count++;
#endif

        return 0;
    }

    //  Cache miss → free or evict
    cache_entry *free_entry = free_get();

#if IO_STATS
    if (free_entry != NULL)
        s_block_cache_write_free_count++;
#endif

    if (!free_entry)
    {
        free_entry = evict_entry();
#if IO_STATS
            s_block_cache_write_evict_count++;
#endif
    }

    if (!free_entry)
    {
        return s_evict_error;
    }

    //  No need to read from device for write
    memcpy(free_entry->data, in_data, BLOCK_SIZE);
    free_entry->sector = sector;
    free_entry->pdrv = pdrv;
    free_entry->dirty = true;
    free_entry->valid = true;
    s_dirty_blocks = true;

    //  Add to structures
    hash_insert(free_entry);
    lru_insert_front(free_entry);

    return 0;
}

DRESULT block_cache_flush(bool flush_all, bool invalidate_all)
{
    if (s_dirty_blocks == false)                    //  Optimization
        return RES_OK;
    
    for (int i=0; i<CACHE_SIZE; i++) 
    {
        if (s_cache[i].valid && s_cache[i].dirty) 
        {
            DRESULT result = disk_write_no_cache (s_cache[i].pdrv, s_cache[i].data, s_cache[i].sector, 1);          //  1 is a 512 byte sector
            if (result != RES_OK)
            {
                return result;
            }
#if IO_STATS
            s_block_cache_write_flush_count++;
#endif
            s_cache[i].dirty = false;

            if (flush_all == false)
                return RES_OK;                      //  Flush only one
        }

        if (invalidate_all)                         //  Flush all of the cache entries
            s_cache[i].valid = false;
    }

    s_dirty_blocks = false;                         //  Clear the flag

    return RES_OK;
}

void block_cache_print_stats(void)
{
#if IO_STATS
    printf("block_cache:, Reads, %d, Ahead, %d, Hit, %d, Free, %d, Evict, %d, Miss, %d, ---, Writes, %d, Hit, %d, Free, %d, Evict, %d, Flush, %d,\n", 
        s_block_cache_read_count, s_block_cache_read_ahead_count, s_block_cache_read_hit_count, s_block_cache_read_free_count, s_block_cache_read_evict_count, s_block_cache_read_miss_count,
        s_block_cache_write_count, s_block_cache_write_hit_count, s_block_cache_write_free_count, s_block_cache_write_evict_count, s_block_cache_write_flush_count
        );
#endif
}