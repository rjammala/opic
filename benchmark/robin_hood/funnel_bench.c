/* funnel_bench.c ---
 *
 * Filename: funnel_bench.c
 * Description:
 * Author: Felix Chern
 * Maintainer:
 * Copyright: (c) 2017 Felix Chern
 * Created: Sun Jun 25 00:32:00 2017 (-0700)
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


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include "opic/common/op_assert.h"
#include "opic/op_malloc.h"
#include "opic/hash/op_hash.h"
#include "opic/hash/robin_hood.h"

#include "murmurhash3.h"
#include "spookyhash-c/spookyhash.h"
#include "farmhash-c/farmhash.h"

static int objcnt = 0;
static uint64_t val_sum = 0;

typedef uint64_t (*HashFunc)(void* key, void* context, OPHash hasher);
typedef void (*RunKey)(int size, HashFunc hash_func,
                       void* context, OPHash hasher);

typedef bool (*RHHNew_t)(OPHeap* heap, void* rhh,
                         uint64_t num_objects, double load,
                         size_t keysize, size_t valsize);
typedef void (*RHHDestroy_t)(void* rhh);
typedef void (*RHHPrintStat_t)(void* rhh);

static void run_short_keys(int size, HashFunc hash_func,
                           void* context, OPHash hasher);
static void run_mid_keys(int size, HashFunc hash_func,
                         void* context, OPHash hasher);
static void run_long_keys(int size, HashFunc hash_func,
                          void* context, OPHash hasher);
static void run_long_int(int size, HashFunc hash_func,
                         void* context, OPHash hasher);
static void print_timediff(const char* info,
                           struct timeval start, struct timeval end);

uint64_t murmur3(void* key, size_t size)
{
  uint64_t hashed_val[2];
  MurmurHash3_x64_128(key, size, 421439783, hashed_val);
  return hashed_val[0];
}

uint64_t spooky(void* key, size_t size)
{
  return spookyhash64(key, size, 421439783);
}

uint64_t city(void* key, size_t size)
{
  return cityhash64(key, size);
}

uint64_t farm(void* key, size_t size)
{
  return farmhash64(key, size);
}

uint64_t RHHInsertWrap(void* key, void* context, OPHash hash_impl)
{
  static uint64_t val = 0;
  RHHInsertCustom(context, hash_impl, key, &val);
  val++;
  return 0;
}

uint64_t RHHGetWrap(void* key, void* context, OPHash hash_impl)
{
  return *(uint64_t*)RHHGetCustom(context, hash_impl, key);
}

uint64_t RHHFunnelInsertWrap(void* key, void* context, OPHash hash_impl)
{
  static uint64_t val = 0;
  RHHFunnelInsert(context, key, &val);
  val++;
  return 0;
}

uint64_t RHHFunnelGetWrap(void* key, void* context, OPHash hash_impl)
{
  RHHFunnelGet(context, key, NULL, 0);
  return 0;
}

void funnel_sum_val(void* key, void* value, void* ctx,
                    size_t keysize, size_t valsize, size_t ctxsize)
{
  if (!value) return;
  int* intval = value;
  val_sum += *intval;
}

/*
 * Things we want to benchmark:
 * 1. How does funnel rhh perform compare to standard rhh?
 *   1.1 insertion
 *   1.2 query
 * 2. Join scenario
 * 3. Aggregate scenario (upsert)
 * others?
 */

void help(char* program)
{
  printf
    ("usage: %s [-n power_of_2] [-r repeat] [-k keytype] [-i impl]\n"
     "       [-l load] [-p]\n"
     "Options:\n"
     "  -n num     Number of elements measured in power of 2.\n"
     "             -n 20 => run 2^20 = 1 million elements.\n"
     "             defaults to 20\n"
     "  -r repeat  Repeat the benchmar for `repeat` times.\n"
     "  -k keytype keytype = s_string, m_string, l_string l_int\n"
     "             s_string: 6 bytes, m_string: 32 bytes,\n"
     "             l_string: 256 bytes, l_int: 8 bytes\n"
     "             For now only robin_hood hash supports long_int benchmark\n"
     "  -i impl    impl = rhh, funnel_rhh\n"
     "  -l load    load number for rhh range from 0.0 to 1.0.\n"
     "  -p         print probing stats of RHH\n"
     "  -h         print help.\n"
     ,program);
  exit(1);
}

int main(int argc, char* argv[])
{
  OPHeap* heap;
  void *rhh;
  struct timeval
    i_start, i_end,
    q_start, q_end;

  int num_power, opt;
  int repeat = 1;
  RunKey key_func = run_short_keys;
  int k_len = 6;
  uint64_t num;
  double load = 0.8;
  bool print_stat = false;
  RHHFunnel* funnel;

  RHHNew_t rhh_new = (RHHNew_t)RHHNew;
  RHHDestroy_t rhh_destroy = (RHHDestroy_t)RHHDestroy;
  HashFunc rhh_put = RHHFunnelInsertWrap;
  HashFunc rhh_get = RHHFunnelGetWrap;
  RHHPrintStat_t rhh_printstat = (RHHPrintStat_t)RHHPrintStat;
  OPHash hasher = city;
  size_t funnel_slotsize = 1ULL << 12;
  size_t funnel_partition_size = 1ULL << 12;

  num_power = 20;

  while ((opt = getopt(argc, argv, "a:b:n:r:k:i:l:f:ph")) > -1)
    {
      switch (opt)
        {
        case 'a':
          funnel_slotsize = 1ULL << atoi(optarg);
          break;
        case 'b':
          funnel_partition_size = 1ULL << atoi(optarg);
          break;
        case 'n':
          num_power = atoi(optarg);
          break;
        case 'r':
          repeat = atoi(optarg);
          break;
        case 'k':
          if (!strcmp("s_string", optarg))
            {
              key_func = run_short_keys;
              k_len = 6;
            }
          else if (!strcmp("m_string", optarg))
            {
              key_func = run_mid_keys;
              k_len = 32;
            }
          else if (!strcmp("l_string", optarg))
            {
              key_func = run_long_keys;
              k_len = 256;
            }
          else if (!strcmp("l_int", optarg))
            {
              key_func = run_long_int;
              k_len = 8;
            }
          else
            help(argv[0]);
          break;
        case 'i':
          /* if (!strcmp("rhh", optarg)) */
          /*   { */
          /*     printf("Using official robin_hood\n"); */
          /*   } */
          /* else if (!strcmp("rhh_b_k_v", optarg)) */
          /*   { */
          /*     printf("Using rhh_b_k_v\n"); */
          /*     rhh_new = (RHHNew_t)RHH_b_k_v_New; */
          /*     rhh_destroy = (RHHDestroy_t)RHH_b_k_v_Destroy; */
          /*     rhh_put = RHH_b_k_v_PutWrap; */
          /*     rhh_get = RHH_b_k_v_GetWrap; */
          /*     rhh_printstat = (RHHPrintStat_t)RHH_b_k_v_PrintStat; */
          /*   } */
          /* else */
          /*   help(argv[0]); */
          break;
        case 'l':
          load = atof(optarg);
          break;
        case 'f':
          if (!strcmp("murmur3", optarg))
            {
              printf("using murmur3 hasher\n");
              hasher = murmur3;
            }
          else if (!strcmp("spooky", optarg))
            {
              printf("using spookyhash\n");
              hasher = spooky;
            }
          else if (!strcmp("city", optarg))
            {
              printf("using cityhash\n");
              hasher = city;
            }
          else if (!strcmp("farm", optarg))
            {
              printf("using farmhash\n");
              hasher = farm;
            }
          else
            help(argv[0]);
          break;
        case 'p':
          print_stat = true;
          break;
        case 'h':
        case '?':
        default:
            help(argv[0]);
        }
    }

  num = 1UL << num_power;
  printf("running elements %" PRIu64 "\n", num);

  op_assert(OPHeapNew(&heap), "Create OPHeap\n");

  for (int i = 0; i < repeat; i++)
    {
      printf("attempt %d\n", i + 1);
      op_assert(rhh_new(heap, &rhh, num,
                        load, k_len, 8), "Create RobinHoodHash\n");

      funnel = RHHFunnelNewCustom(rhh, hasher, NULL,
                                  funnel_slotsize, funnel_partition_size);
      gettimeofday(&i_start, NULL);
      key_func(num_power, rhh_put, funnel, hasher);
      RHHFunnelInsertFlush(funnel);
      gettimeofday(&i_end, NULL);
      RHHFunnelDestroy(funnel);

      printf("insert finished\n");

      funnel = RHHFunnelNewCustom(rhh, hasher, funnel_sum_val,
                                  funnel_slotsize, funnel_partition_size);
      gettimeofday(&q_start, NULL);
      key_func(num_power, rhh_get, funnel, hasher);
      RHHFunnelGetFlush(funnel);
      gettimeofday(&q_end, NULL);
      RHHFunnelDestroy(funnel);

      print_timediff("Funnel Insert time: ", i_start, i_end);
      print_timediff("Funnel Query time: ", q_start, q_end);

      if (print_stat)
        {
          rhh_printstat(rhh);
        }
      rhh_destroy(rhh);
    }
  printf("objcnt: %d val_sum: %" PRIu64 "\n", objcnt, val_sum);

  return 0;
}

void run_short_keys(int size, HashFunc hash_func,
                    void* context, OPHash hasher)
{
  op_assert(size >= 12, "iteration size must > 2^12\n");
  int i_bound = 1 << (size - 12);
  char uuid [] = "!!!!!!";
  uint64_t counter = 0;
  for (int i = 0; i < i_bound; i++)
    {
      for (int j = 2, val = counter >> 12; j < 6; j++, val>>=6)
        {
          uuid[j] = 0x21 + (val & 0x3F);
        }
      for (int j = 0; j < 64; j++)
        {
          uuid[1] = 0x21 + j;
          for (int k = 0; k < 64; k++)
            {
              uuid[0] = 0x21 + k;
              counter++;
              hash_func(uuid, context, hasher);
            }
        }
    }
}

void run_mid_keys(int size, HashFunc hash_func,
                  void* context, OPHash hasher)
{
  op_assert(size >= 12, "iteration size must > 2^12\n");
  int i_bound = 1 << (size - 12);
  char uuid [] = "!!!!!!--!!!!!!--!!!!!!--!!!!!!--";
  uint64_t counter = 0;
  for (int i = 0; i < i_bound; i++)
    {
      for (int j = 2, val = counter >> 12; j < 6; j++, val>>=6)
        {
          uuid[j] = 0x21 + (val & 0x3F);
          uuid[j+8] = 0x21 + (val & 0x3F);
          uuid[j+16] = 0x21 + (val & 0x3F);
          uuid[j+24] = 0x21 + (val & 0x3F);
        }
      for (int j = 0; j < 64; j++)
        {
          uuid[1] = 0x21 + j;
          uuid[1+8] = 0x21 + j;
          uuid[1+16] = 0x21 + j;
          uuid[1+24] = 0x21 + j;
          for (int k = 0; k < 64; k++)
            {
              uuid[0] = 0x21 + k;
              uuid[0+8] = 0x21 + k;
              uuid[0+16] = 0x21 + k;
              uuid[0+24] = 0x21 + k;
              counter++;
              hash_func(uuid, context, hasher);
            }
        }
    }
}

void run_long_keys(int size, HashFunc hash_func,
                   void* context, OPHash hasher)
{
  op_assert(size >= 12, "iteration size must > 2^12\n");
  int i_bound = 1 << (size - 12);
  char uuid [] =
    "!!!!!!--!!!!!!--!!!!!!--!!!!!!--"
    "!!!!!!--!!!!!!--!!!!!!--!!!!!!--"
    "!!!!!!--!!!!!!--!!!!!!--!!!!!!--"
    "!!!!!!--!!!!!!--!!!!!!--!!!!!!--"
    "!!!!!!--!!!!!!--!!!!!!--!!!!!!--"
    "!!!!!!--!!!!!!--!!!!!!--!!!!!!--"
    "!!!!!!--!!!!!!--!!!!!!--!!!!!!--"
    "!!!!!!--!!!!!!--!!!!!!--!!!!!!--";
  uint64_t counter = 0;
  for (int i = 0; i < i_bound; i++)
    {
      for (int j = 2, val = counter >> 12; j < 6; j++, val>>=6)
        {
          for (int k = 0; k < 256; k+=8)
            uuid[j+k] = 0x21 + (val & 0x3F);
        }
      for (int j = 0; j < 64; j++)
        {
          for (int h = 0; h < 256; h+=8)
            uuid[h+1] = 0x21 + j;
          for (int k = 0; k < 64; k++)
            {
              for (int h = 0; h < 256; h+=8)
                uuid[h] = 0x21 + k;
              counter++;
              hash_func(uuid, context, hasher);
            }
        }
    }
}

void run_long_int(int size, HashFunc hash_func,
                  void* context, OPHash hasher)
{
  op_assert(size >= 12, "iteration size must > 2^12\n");
  uint64_t i_bound = 1 << size;
  for (uint64_t i = 0; i < i_bound; i++)
    {
      hash_func(&i, context, hasher);
    }
}

void print_timediff(const char* info, struct timeval start, struct timeval end)
{
  long int second = end.tv_sec - start.tv_sec;
  unsigned int usec;
  if (end.tv_usec < start.tv_usec)
    {
      second--;
      usec = end.tv_usec + 1000000 - start.tv_usec;
    }
  else
    usec = end.tv_usec - start.tv_usec;

  printf("%s%ld.%06u\n", info, second, usec);
}


/* funnel_bench.c ends here */
