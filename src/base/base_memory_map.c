// Copyright (c) Epic Games Tools
// Licensed under the MIT license (https://opensource.org/license/mit/)

////////////////////////////////
//~ rjf: Memory Map Functions

internal void
memory_map_push(Arena *arena, MemoryMap *map, Rng1U64 vaddr_range, void *data)
{
  MemoryMapRangeNode *n = push_array(arena, MemoryMapRangeNode, 1);
  n->v.vaddr_range = vaddr_range;
  n->v.base = data;
  // NOTE(rjf): newer ranges are pushed to the front of the list, so a walk
  // from first_range visits the most recently pushed fragments first.
  SLLQueuePushFront(map->first_range, map->last_range, n);
}

internal U64
memory_map_read(MemoryMap *map, Rng1U64 range, void *dst)
{
  // NOTE(rjf): reads walk the requested range one byte at a time. fragments
  // are visited newest-first, so newer fragments byte-wise override older,
  // larger fragments wherever they overlap. reading stops at the first byte
  // not covered by any fragment, and bytes after a gap are never copied: the
  // returned length is always the length of a contiguous, covered prefix.
  U64 bytes_read = 0;
  U64 read_size = dim_1u64(range);
  for(U64 byte_idx = 0; byte_idx < read_size; byte_idx += 1)
  {
    U64 vaddr = range.min + byte_idx;
    B32 found = 0;
    for(MemoryMapRangeNode *n = map->first_range; n != 0; n = n->next)
    {
      if(contains_1u64(n->v.vaddr_range, vaddr))
      {
        U64 src_off = vaddr - n->v.vaddr_range.min;
        ((U8 *)dst)[byte_idx] = ((U8 *)n->v.base)[src_off];
        found = 1;
        break;
      }
    }
    if(!found)
    {
      break;
    }
    bytes_read += 1;
  }
  return bytes_read;
}

internal String8
memory_map_data_from_range(Arena *arena, MemoryMap *map, Rng1U64 range)
{
  String8 result = {0};
  result.size = dim_1u64(range);
  result.str = push_array(arena, U8, result.size);
  memory_map_read(map, range, result.str);
  return result;
}
