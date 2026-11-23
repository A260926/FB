/********************************************************************
 *                                                                  *
 * Rewind window for non-seekable Ogg/Vorbis streams.              *
 *                                                                  *
 ********************************************************************/

#include <stdlib.h>
#include <string.h>

#include "rewind_cache.h"

struct rewind_cache {
  rewind_entry *entries;
  int           alloc;     /* allocated slots */
  int           head;      /* oldest live entry */
  int           count;     /* live entries */
  int           at_start;  /* next push is the link's first packet */
  int           origin;    /* entries[head] is the link first packet */
  long          budget;    /* max compressed payload bytes */
  long          payload;   /* current compressed payload bytes */
  long          evictions;
};

rewind_cache *rewind_cache_create(long max_bytes){
  rewind_cache *c;
  if(max_bytes<REWIND_MIN_BYTES || max_bytes>REWIND_MAX_BYTES)
    return NULL;
  c=_ogg_calloc(1,sizeof(*c));
  if(!c)return NULL;
  c->budget=max_bytes;
  c->at_start=1;
  return c;
}

void rewind_cache_clear(rewind_cache *c){
  int i;
  if(!c)return;
  for(i=0;i<c->count;i++)
    if(c->entries[c->head+i].packet)
      _ogg_free(c->entries[c->head+i].packet);
  c->head=0;
  c->count=0;
  c->payload=0;
  c->origin=0;
  c->at_start=1;
}

void rewind_cache_destroy(rewind_cache *c){
  if(!c)return;
  rewind_cache_clear(c);
  _ogg_free(c->entries);
  _ogg_free(c);
}

static void rewind_compact(rewind_cache *c){
  if(c->head){
    memmove(c->entries,c->entries+c->head,
            c->count*sizeof(*c->entries));
    c->head=0;
  }
}

static void rewind_evict_oldest(rewind_cache *c){
  rewind_entry *e=&c->entries[c->head];
  c->payload-=e->bytes;
  if(c->payload<0)c->payload=0;
  _ogg_free(e->packet);
  memset(e,0,sizeof(*e));
  c->head++;
  c->count--;
  c->evictions++;
  c->origin=0; /* the retained head is no longer the link origin */
  if(!c->count)c->head=0;
  /* amortised compaction so head cannot grow without bound */
  if(!c->count || (c->head>16 && c->head>=c->count))
    rewind_compact(c);
}

int rewind_cache_push(rewind_cache *c,const ogg_packet *op,
                      ogg_int64_t begin,ogg_int64_t end){
  rewind_entry *e;
  unsigned char *copy;

  if(!c || !op || op->bytes<0)return -1;

  copy=_ogg_malloc(op->bytes?op->bytes:1);
  if(!copy)return -1;
  if(op->bytes)memcpy(copy,op->packet,op->bytes);

  if(!c->alloc){
    c->entries=_ogg_malloc(16*sizeof(*c->entries));
    if(!c->entries){
      _ogg_free(copy);
      return -1;
    }
    c->alloc=16;
  }else if(c->head+c->count>=c->alloc){
    rewind_compact(c);
    if(c->head+c->count>=c->alloc){
      int nalloc=c->alloc*2;
      rewind_entry *p=_ogg_realloc(c->entries,nalloc*sizeof(*p));
      if(!p){
        _ogg_free(copy);
        return -1;
      }
      c->entries=p;
      c->alloc=nalloc;
    }
  }

  e=&c->entries[c->head+c->count];
  e->packet=copy;
  e->bytes=op->bytes;
  e->packetno=op->packetno;
  e->granulepos=op->granulepos;
  e->e_o_s=op->e_o_s;
  e->begin=begin;
  e->end=end;
  c->payload+=op->bytes;
  if(!c->count)c->origin=c->at_start;
  c->at_start=0;
  c->count++;

  /* honor the budget; a single over-budget packet stays alone and
     simply leaves nothing older recoverable */
  while(c->payload>c->budget && c->count>1)
    rewind_evict_oldest(c);
  return 0;
}

int rewind_cache_range(rewind_cache *c,ogg_int64_t *begin,ogg_int64_t *end){
  if(begin)*begin=0;
  if(end)*end=0;
  if(!c || !c->count)return OV_FALSE;
  {
    rewind_entry *first=&c->entries[c->head];
    rewind_entry *last=&c->entries[c->head+c->count-1];
    ogg_int64_t lo;
    if(c->origin){
      /* the retained head really is the link's first audio packet,
         so it needs no overlap predecessor */
      lo=first->begin;
    }else if(c->count<2){
      /* one packet whose predecessor is gone: its lapping cannot be
         reconstructed, never advertise unrecoverable time */
      return OV_FALSE;
    }else{
      lo=c->entries[c->head+1].begin;
    }
    if(last->end<=lo)return OV_FALSE;
    if(begin)*begin=lo;
    /* end follows the PCM cursor convention: one past the last
       replayable frame, i.e. the number of recoverable frames */
    if(end)*end=last->end;
    return 0;
  }
}

int rewind_cache_locate(rewind_cache *c,ogg_int64_t target){
  int lo_i,hi_i;
  if(!c || !c->count)return -1;
  lo_i=c->origin?c->head:c->head+1;
  hi_i=c->head+c->count-1;
  if(lo_i>hi_i)return -1;
  if(target<c->entries[lo_i].begin)return -1;
  if(target>=c->entries[hi_i].end)return -1;
  /* binary search for the entry whose [begin,end) holds target */
  while(lo_i<=hi_i){
    int mid=(lo_i+hi_i)>>1;
    rewind_entry *e=&c->entries[mid];
    if(target<e->begin)hi_i=mid-1;
    else if(target>=e->end)lo_i=mid+1;
    else return mid-c->head;
  }
  return -1;
}

int rewind_cache_count(rewind_cache *c){
  return c?c->count:0;
}

const rewind_entry *rewind_cache_get(rewind_cache *c,int idx){
  if(!c || idx<0 || idx>=c->count)return NULL;
  return &c->entries[c->head+idx];
}

long rewind_cache_payload_bytes(rewind_cache *c){
  return c?c->payload:0;
}

long rewind_cache_budget(rewind_cache *c){
  return c?c->budget:0;
}

long rewind_cache_evictions(rewind_cache *c){
  return c?c->evictions:0;
}

/* like rewind_cache_clear(), but the next retained packet is not the
   link origin (used after an OV_HOLE: the lost predecessor cannot be
   reconstructed) */
void rewind_cache_discontinuity(rewind_cache *c){
  rewind_cache_clear(c);
  if(c)c->at_start=0;
}

rewind_runtime *rewind_runtime_create(long max_bytes){
  rewind_runtime *rt=_ogg_calloc(1,sizeof(*rt));
  if(!rt)return NULL;
  rt->cache=rewind_cache_create(max_bytes);
  if(!rt->cache){
    _ogg_free(rt);
    return NULL;
  }
  return rt;
}

void rewind_runtime_destroy(rewind_runtime *rt){
  if(!rt)return;
  rewind_cache_destroy(rt->cache);
  _ogg_free(rt->q);
  _ogg_free(rt);
}

void rewind_runtime_reset_link(rewind_runtime *rt){
  if(!rt)return;
  rewind_cache_clear(rt->cache);
  _ogg_free(rt->q);
  rt->q=NULL;
  rt->qcap=rt->qn=rt->qhead=0;
  rt->replay=0;
  rt->watermark=0;
  rt->link_pos=0;
}
