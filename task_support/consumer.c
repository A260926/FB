/* Fixed public-API consumer. It supplies no production guidance implementation. */
#include "theft.h"
#include <inttypes.h>
#include <string.h>
#include <unistd.h>

#define REQUIRE(c, m) do { if (!(c)) { fprintf(stderr,"CONSUMER: %s\n",m); exit(17); } } while (0)
#define TRIALS 20
struct sample { uint32_t shape, value, tag; };
struct environment;
struct alloc_env { struct environment *e; unsigned slot; };
struct record { struct sample x[2]; uint64_t donor, admitted, mask, novel; size_t size; };
struct environment {
    pid_t parent;
    bool forked, force_fail;
    unsigned allocs, frees, posts, guides, mutations, shape_changes, extensions, repeats;
    struct sample latest[2];
    struct record trace[TRIALS];
    uint64_t seen, next_id, cursor;
    struct { uint64_t id; struct sample x[2]; } corpus[4];
    size_t count;
};
static uint32_t width(const struct sample *s) { return 4 + 4*s->shape; }
static unsigned used(const struct sample *s) { return 2 + width(s) + 16; }
static uint64_t tape(const struct sample *s) {
    return s->shape | ((uint64_t)s->value << 2) | ((uint64_t)s->tag << (2+width(s)));
}
static uint64_t mask_for(const struct sample *a, const struct sample *b) {
    return UINT64_C(1) | (UINT64_C(1)<<((a->value^b->tag)&31)) | (UINT64_C(1)<<(32+a->shape));
}
static enum theft_alloc_res allocate(struct theft *t, void *v, void **out) {
    struct alloc_env *a=v;
#ifdef GUIDED_API
    REQUIRE(!theft_cover(t,62),"allocator cannot publish features");
#endif
    struct sample *s=calloc(1,sizeof(*s));
    if (!s) return THEFT_ALLOC_ERROR;
    s->shape=(uint32_t)theft_random_bits(t,2);
    s->value=(uint32_t)theft_random_bits(t,(uint8_t)width(s));
    s->tag=(uint32_t)theft_random_bits(t,16);
    a->e->latest[a->slot]=*s;
    a->e->allocs++;
    *out=s;
    return THEFT_ALLOC_OK;
}
static void release(void *p, void *v) {
    struct alloc_env *a=v;
    if (p) { a->e->frees++; free(p); }
}
static enum theft_trial_res property(struct theft *t, void *a, void *b) {
    struct environment *e=theft_hook_get_env(t);
    REQUIRE((getpid()!=e->parent)==e->forked,"property process identity");
    const struct sample *x=a,*y=b;
    REQUIRE(x->shape<4 && y->shape<4,"legal generated shape");
    REQUIRE(x->value < (UINT32_C(1)<<width(x)),"legal generated value");
#ifdef GUIDED_API
    const bool accepted=theft_cover(t,0);
    if (!e->force_fail) REQUIRE(accepted,"primary property accepts feature zero");
    REQUIRE(!theft_cover(t,64),"feature range");
    if (accepted) {
        REQUIRE(theft_cover(t,(uint8_t)((x->value^y->tag)&31)),"property value feature");
        REQUIRE(theft_cover(t,(uint8_t)(32+x->shape)),"property shape feature");
        REQUIRE(theft_cover(t,0),"duplicate feature is idempotent");
    }
#endif
    return e->force_fail ? THEFT_TRIAL_FAIL : THEFT_TRIAL_PASS;
}
static enum theft_hook_fork_post_res fork_post(const struct theft_hook_fork_post_info *i, void *v) {
    (void)v;
#ifdef GUIDED_API
    REQUIRE(!theft_cover(i->t,61),"fork_post is outside property");
#else
    (void)i;
#endif
    return THEFT_HOOK_FORK_POST_CONTINUE;
}
static enum theft_hook_trial_post_res trial_post(const struct theft_hook_trial_post_info *i, void *v) {
    struct environment *e=v;
#ifdef GUIDED_API
    REQUIRE(!theft_cover(i->t,60),"trial_post is outside property");
#endif
    if (i->repeat) { e->repeats++; return THEFT_HOOK_TRIAL_POST_CONTINUE; }
    REQUIRE(i->trial_id<TRIALS,"bounded trial id");
    e->posts++;
    if (!e->force_fail) {
        REQUIRE(i->result==THEFT_TRIAL_PASS || i->result==THEFT_TRIAL_DUP,"normal primary verdict");
        memcpy(e->trace[i->trial_id].x,e->latest,sizeof(e->latest));
    }
    return e->force_fail ? THEFT_HOOK_TRIAL_POST_REPEAT_ONCE : THEFT_HOOK_TRIAL_POST_CONTINUE;
}
static enum theft_hook_counterexample_res counterexample(const struct theft_hook_counterexample_info *i, void *v) {
    (void)i; (void)v; return THEFT_HOOK_COUNTEREXAMPLE_CONTINUE;
}
#ifdef GUIDED_API
static void guidance(const struct theft_guidance_info *i, void *v) {
    struct environment *e=v;
    REQUIRE(getpid()==e->parent,"guidance callback runs in parent");
    REQUIRE(i->trial_id==e->guides && i->trial_id<TRIALS,"one ordered primary guidance decision");
    e->guides++;
    struct record *r=&e->trace[i->trial_id];
    r->donor=i->donor_id; r->admitted=i->admitted_id;
    r->mask=i->feature_mask; r->novel=i->novel_mask; r->size=i->corpus_size;
    memcpy(r->x,e->latest,sizeof(e->latest));
    bool mutation=e->count && i->trial_id%4;
    REQUIRE(i->mutated==mutation,"feedback determines fresh versus donor generation");
    if (mutation) {
        size_t d=0;
        while (d<e->count && e->corpus[d].id<e->cursor) d++;
        if (d==e->count) d=0;
        REQUIRE(i->donor_id==e->corpus[d].id,"actual FIFO donor rotation");
        e->cursor=i->donor_id+1;
        unsigned desc=(unsigned)((i->trial_id-1)%6);
        unsigned arg=desc/3, req=desc%3;
        const struct sample *old=&e->corpus[d].x[arg];
        unsigned w=req==0?2:req==1?width(old):16;
        unsigned bit=(unsigned)(((i->trial_id-1)/6)%w);
        REQUIRE(i->mutation_arg==arg && i->mutation_request==req && i->mutation_bit==bit,"selected request coordinates");
        unsigned offset=req==0?0:req==1?2:2+width(old);
        for (unsigned k=0;k<2;k++) {
            uint64_t expected=tape(&e->corpus[d].x[k]);
            if (k==arg) expected^=UINT64_C(1)<<(offset+bit);
            unsigned n=used(&e->corpus[d].x[k]);
            if (used(&e->latest[k])<n) n=used(&e->latest[k]);
            uint64_t keep=(UINT64_C(1)<<n)-1;
            REQUIRE((tape(&e->latest[k])&keep)==(expected&keep),"real allocator consumed mutated continuous tape");
        }
        e->mutations++;
        e->shape_changes+=(old->shape!=e->latest[arg].shape);
        e->extensions+=(used(&e->latest[arg])>used(old));
    } else {
        REQUIRE(i->donor_id==UINT64_MAX && i->mutation_arg==UINT8_MAX && i->mutation_request==SIZE_MAX && i->mutation_bit==UINT32_MAX,"fresh sentinels");
    }
    const uint64_t actual=i->result==THEFT_TRIAL_DUP?0:mask_for(&e->latest[0],&e->latest[1]);
    REQUIRE(i->feature_mask==actual,"features belong to actual primary input");
    uint64_t novel=i->result==THEFT_TRIAL_PASS?actual&~e->seen:0;
    REQUIRE(i->novel_mask==novel,"only new PASS features admit input");
    if (novel) {
        REQUIRE(i->admitted_id==e->next_id++,"monotonic admission identity");
        e->seen|=actual;
        if (e->count==4) { memmove(e->corpus,e->corpus+1,3*sizeof(e->corpus[0])); e->count--; }
        e->corpus[e->count].id=i->admitted_id;
        memcpy(e->corpus[e->count].x,e->latest,sizeof(e->latest));
        e->count++;
    } else REQUIRE(i->admitted_id==UINT64_MAX,"no admission without novelty");
    REQUIRE(i->corpus_size==e->count,"bounded live corpus after decision");
}
#endif
static void run(struct environment *e,bool forked,bool failure) {
    memset(e,0,sizeof(*e)); e->parent=getpid(); e->forked=forked; e->force_fail=failure;
    struct alloc_env ae[2]={{e,0},{e,1}};
    struct theft_type_info ti[2]; memset(ti,0,sizeof(ti));
    for (unsigned k=0;k<2;k++) {
        ti[k].alloc=allocate; ti[k].free=release; ti[k].env=&ae[k];
        ti[k].autoshrink_config.enable=true;
        ti[k].autoshrink_config.max_failed_shrinks=8;
    }
    struct theft_run_config cfg={.prop2=property,.type_info={&ti[0],&ti[1]},
        .trials=failure?1:TRIALS,.seed=UINT64_C(0xa600d64b175eed),
        .fork={.enable=forked,.timeout=1000},
        .hooks={.trial_post=trial_post,.fork_post=fork_post,.counterexample=counterexample,.env=e}};
#ifdef GUIDED_API
    struct theft_guidance_config g={.corpus_limit=4,.post=guidance,.env=e};
    enum theft_run_res result=theft_run_guided(&cfg,&g);
#else
    enum theft_run_res result=theft_run(&cfg);
#endif
    REQUIRE(result==(failure?THEFT_RUN_FAIL:THEFT_RUN_PASS),"real theft run result");
    REQUIRE(e->allocs>0 && e->allocs==e->frees,"real argument ownership balanced");
    REQUIRE(e->posts==(failure?1:TRIALS),"real trial completion count");
#ifdef GUIDED_API
    REQUIRE(e->guides==(failure?1:TRIALS),"guidance cannot be delegated to legacy theft_run");
    if (!failure) {
        REQUIRE(e->mutations>=10 && e->shape_changes>0,"real request mutation reached dynamic allocator");
        REQUIRE(e->extensions>0,"dynamic request growth must consume extension bits");
    } else {
        REQUIRE(e->count==0 && e->repeats==1,"shrinking and rerun do not publish a corpus");
    }
#endif
}
int main(void) {
    struct environment a,b;
    run(&a,false,false); run(&b,true,false);
    REQUIRE(!memcmp(a.trace,b.trace,sizeof(a.trace)),"fork and in-process inputs/feedback are identical");
#ifdef GUIDED_API
    printf("GUIDED real_mutations=%u shape_changes=%u extensions=%u admissions=%" PRIu64 "\n",a.mutations,a.shape_changes,a.extensions,a.next_id);
    run(&a,true,true);
    puts("GUIDED production_feedback=ok fork_replay=ok shrink_isolation=ok");
#else
    printf("THEFT_LEGACY normal_trials=%u fork_trials=%u balanced=1 first_shapes=%u,%u\n",a.posts,b.posts,a.trace[0].x[0].shape,a.trace[0].x[1].shape);
    run(&a,true,true);
    REQUIRE(a.repeats==1,"normal failure rerun");
    puts("THEFT_LEGACY autoshrink=ok normal_failure_rerun=ok");
#endif
    return 0;
}
