/* rhh_b16kv.c ---
 *
 * Filename: rhh_b16kv.c
 * Description:
 * Author: Felix Chern
 * Maintainer:
 * Copyright: (c) 2017 Felix Chern
 * Created: Sun Apr 30 16:05:21 2017 (-0700)
 * Version:
 * Package-Requires: ()
 * Last-Updated:
 *           By:
 *     Update #: 0
 * URL:
 * Doc URL:
 * Keywords:
 * Compatibility:
 *
 */

/* Commentary:
 *
 *
 *
 */

/* Change Log:
 *
 *
 */

/* This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or (at
 * your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

/* Code: */


#include <stdio.h> // TODO use op_log instead
#include <inttypes.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include "opic/common/op_assert.h"
#include "opic/common/op_utils.h"
#include "opic/common/op_log.h"
#include "opic/op_malloc.h"
#include "rhh_b16kv.h"

#define PROBE_STATS_SIZE 64

OP_LOGGER_FACTORY(logger, "benchmark.robin_hood.rhh_b16kv");

struct RHH_b16kv
{
  uint64_t objcnt;
  uint64_t objcnt_high;
  uint8_t capacity_clz;    // leading zeros of capacity
  uint8_t capacity_ms4b;   // most significant 4 bits
  uint16_t longest_probes;
  size_t keysize;
  size_t valsize;
  uint32_t stats[PROBE_STATS_SIZE];
  uint8_t* bucket;
};

bool
RHH_b16kv_New(OPHeap* heap, RHH_b16kv ** rhh,
              uint64_t num_objects, double load,
              size_t keysize, size_t valsize)
{
  uint64_t capacity;
  uint32_t capacity_clz, capacity_ms4b, capacity_msb;
  size_t alloc_size, header_size;
  const size_t sb_size = keysize + valsize;
  const size_t lb_size = sb_size * 8 + 2;
  uintptr_t rhh_base;

  op_assert(load > 0.0 && load < 1.0,
            "load %lf must within close interval (0.0, 1.0)\n", load);
  capacity = (uint64_t)(num_objects / load);
  if (capacity < 8)
    capacity = 8;
  capacity_clz = __builtin_clzl(capacity);
  capacity_msb = 64 - capacity_clz;
  capacity_ms4b = round_up_div(capacity, 1UL << (capacity_msb - 4));
  capacity = (uint64_t)capacity_ms4b << (capacity_msb - 4);

  header_size = sizeof(RHH_b16kv);
  alloc_size = header_size + round_up_div(capacity, 8) * lb_size;
  OP_LOG_INFO(logger, "alloc size %zu", alloc_size);

  *rhh = OPCalloc(heap, 1, alloc_size);
  if (!*rhh)
    return false;
  rhh_base = (uintptr_t)(*rhh);

  (*rhh)->bucket = (uint8_t*)(rhh_base + header_size);

  (*rhh)->capacity_clz = capacity_clz;
  (*rhh)->capacity_ms4b = capacity_ms4b;
  (*rhh)->objcnt_high = (uint64_t)(capacity * load);
  (*rhh)->keysize = keysize;
  (*rhh)->valsize = valsize;
  return true;
}

void
RHH_b16kv_Destroy(RHH_b16kv* rhh)
{
  OPDealloc(rhh);
}

static inline uintptr_t
hash_with_probe(RHH_b16kv* rhh, uint64_t key, int probe)
{
  uintptr_t mask = (1ULL << (64 - rhh->capacity_clz)) - 1;

  // quadratic probing
  uint64_t probed_hash = key + probe * probe * 2;

  // Fast mod and scale
  return (probed_hash & mask) * rhh->capacity_ms4b >> 4;
}

static inline int
findprobe(RHH_b16kv* rhh, OPHash hasher, uintptr_t idx)
{
  const size_t keysize = rhh->keysize;
  const size_t valsize = rhh->valsize;
  const size_t sb_size = keysize + valsize;
  const size_t lb_size = sb_size * 8 + 2;
  const uintptr_t lb_idx = idx / 8;
  const uintptr_t sb_idx = idx % 8;
  const uintptr_t key_idx = lb_idx * lb_size + 2 + sb_idx * sb_size;
  uint64_t hashed_key;

  hashed_key = hasher(&rhh->bucket[key_idx],
                      keysize);
  for (int i = 0; i <= rhh->longest_probes; i++)
    {
      if (hash_with_probe(rhh, hashed_key, i) == idx)
        return i;
    }
  OP_LOG_ERROR(logger, "Didn't find any match probe!\n");
  return -1;
}

bool RHH_b16kv_PutCustom(RHH_b16kv* rhh,
                         OPHash hasher, void* key, void* val)
{
  const size_t keysize = rhh->keysize;
  const size_t valsize = rhh->valsize;
  const size_t sb_size = keysize + valsize;
  const size_t lb_size = sb_size * 8 + 2;
  uintptr_t idx, lb_idx, sb_idx, key_idx;
  int probe, old_probe;
  uint8_t bucket_cpy[sb_size];
  uint8_t bucket_tmp[sb_size];
  uint64_t hashed_key;
  uint16_t* bmap;

  if (rhh->objcnt > rhh->objcnt_high)
    return false;

  memcpy(bucket_cpy, key, keysize);
  memcpy(&bucket_cpy[keysize], val, valsize);

  probe = 0;
  while (true)
    {
      hashed_key = hasher(bucket_cpy, keysize);
      idx = hash_with_probe(rhh, hashed_key, probe);
      lb_idx = idx / 8;
      sb_idx = idx % 8;
      bmap = (uint16_t*)&rhh->bucket[lb_idx * lb_size];
      key_idx = lb_idx * lb_size + 2 + sb_idx * sb_size;
      if (((*bmap >> (sb_idx * 2)) & 0x03) != 1)
        {
          memcpy(&rhh->bucket[key_idx], bucket_cpy, sb_size);
          *bmap &= ~(1U << (sb_idx * 2 + 1));
          *bmap |= 1U << (sb_idx * 2);
          rhh->longest_probes = probe > rhh->longest_probes ?
            probe : rhh->longest_probes;
          rhh->objcnt++;
          if (probe < PROBE_STATS_SIZE)
            rhh->stats[probe]++;
          else
            OP_LOG_WARN(logger, "Large probe: %d\n", probe);
          return true;
        }
      else if (!memcmp(&rhh->bucket[key_idx],
                       bucket_cpy, keysize))
        {
          memcpy(&rhh->bucket[key_idx], bucket_cpy, sb_size);
          return true;
        }
      old_probe = findprobe(rhh, hasher, idx);

      if (probe > old_probe)
        {
          if (old_probe < PROBE_STATS_SIZE)
            rhh->stats[old_probe]--;
          if (probe < PROBE_STATS_SIZE)
            rhh->stats[probe]++;
          else
            OP_LOG_WARN(logger, "Large probe: %d\n", probe);

          memcpy(bucket_tmp, &rhh->bucket[key_idx], sb_size);
          memcpy(&rhh->bucket[key_idx], bucket_cpy, sb_size);
          memcpy(bucket_cpy, bucket_tmp, sb_size);

          rhh->longest_probes = probe > rhh->longest_probes ?
            probe : rhh->longest_probes;
          probe = old_probe;
        }
        probe++;
    }
}

static inline bool
RHHSearchIdx(RHH_b16kv* rhh, OPHash hasher, void* key,
             uintptr_t* lb_idx, uintptr_t* sb_idx)
{
  const size_t keysize = rhh->keysize;
  const size_t valsize = rhh->valsize;
  const size_t sb_size = keysize + valsize;
  const size_t lb_size = sb_size * 8 + 2;
  uintptr_t idx, key_idx;
  uint64_t hashed_key;
  uint16_t* bmap;

  hashed_key = hasher(key, keysize);

  for (int probe = 0; probe <= rhh->longest_probes; probe++)
    {
      idx = hash_with_probe(rhh, hashed_key, probe);
      *lb_idx = idx / 8;
      *sb_idx = idx % 8;
      bmap = (uint16_t*)&rhh->bucket[*lb_idx * lb_size];
      key_idx = *lb_idx * lb_size + 2 + *sb_idx * sb_size;

      switch((*bmap >> (*sb_idx * 2)) & 0x03)
        {
        case 0: return false;
        case 2: continue;
        default: (void)0;
        }
      if (!memcmp(key, &rhh->bucket[key_idx], keysize))
        return true;
    }
  return false;
}

void* RHH_b16kv_GetCustom(RHH_b16kv* rhh, OPHash hasher, void* key)
{
  const size_t keysize = rhh->keysize;
  const size_t valsize = rhh->valsize;
  const size_t sb_size = keysize + valsize;
  const size_t lb_size = sb_size * 8 + 16;
  uintptr_t lb_idx, sb_idx, key_idx;
  if (RHHSearchIdx(rhh, hasher, key, &lb_idx, &sb_idx))
    {
      key_idx = lb_idx * lb_size + 2 + sb_idx * sb_size;
      return &rhh->bucket[key_idx + keysize];
    }
  return NULL;
}

void RHH_b16kv_PrintStat(RHH_b16kv* rhh)
{
  for (int i = 0; i < PROBE_STATS_SIZE; i++)
    if (rhh->stats[i])
      printf("probe %02d: %d\n", i, rhh->stats[i]);
}

/* rhh_b16kv.c ends here */
