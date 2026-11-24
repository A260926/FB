#include "coap3/coap_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define REQUIRE(x) do {if (!(x)) {fprintf(stderr,"baseline line %d\n",__LINE__);exit(3);}} while (0)
static coap_queue_t *enqueue(coap_context_t *ctx, coap_session_t *s, unsigned id, unsigned when) {
  uint8_t token=(uint8_t)id;
  coap_queue_t *n=coap_new_node(); REQUIRE(n);
  n->pdu=coap_pdu_init(COAP_MESSAGE_CON,COAP_REQUEST_CODE_GET,id,64); REQUIRE(n->pdu);
  REQUIRE(coap_add_token(n->pdu,1,&token));
  n->id=id; n->session=coap_session_reference_lkd(s); n->t=when; n->timeout=when;
  REQUIRE(coap_insert_node(&ctx->sendqueue,n)); return n;
}
int main(int argc,char **argv) {
  coap_context_t *ctx; coap_session_t *s; coap_address_t addr;
  coap_queue_t *n=NULL; unsigned survivor_due=0;
  coap_startup(); coap_set_log_level(COAP_LOG_EMERG);
  ctx=coap_new_context(NULL); REQUIRE(ctx);
  coap_address_init(&addr); addr.size=sizeof(addr.addr.sin); addr.addr.sin.sin_family=AF_INET;
  addr.addr.sin.sin_addr.s_addr=htonl(INADDR_LOOPBACK); addr.addr.sin.sin_port=htons(5683);
  s=coap_new_client_session(ctx,NULL,&addr,COAP_PROTO_UDP); REQUIRE(s);
  enqueue(ctx,s,1,10); enqueue(ctx,s,2,20); enqueue(ctx,s,3,30);
  REQUIRE(ctx->sendqueue->t==10 && ctx->sendqueue->next->t==10 && ctx->sendqueue->next->next->t==10);
  REQUIRE(coap_remove_from_queue(&ctx->sendqueue,s,2,NULL,&n)); REQUIRE(n);
  coap_delete_node(n);
  REQUIRE(ctx->sendqueue->t+ctx->sendqueue->next->t==30);
  printf("QUEUE_NORMAL survivor_due=30 delta_preserved=1\n");
  while ((n=coap_pop_next(ctx))) coap_delete_node(n);
  if (argc==1 || strcmp(argv[1],"--normal")) {
    uint8_t byte=2; coap_bin_const_t token={1,&byte};
    enqueue(ctx,s,1,10); enqueue(ctx,s,2,20); enqueue(ctx,s,3,30);
    coap_cancel_all_messages(ctx,s,&token);
    REQUIRE(ctx->sendqueue && ctx->sendqueue->next && !ctx->sendqueue->next->next);
    survivor_due=(unsigned)(ctx->sendqueue->t+ctx->sendqueue->next->t);
    printf("CANCEL_OBSERVED survivor_due=%u expected_due=30\n",survivor_due);
    while ((n=coap_pop_next(ctx))) coap_delete_node(n);
  }
  coap_session_release(s); coap_free_context(ctx); coap_cleanup();
  puts("BASELINE_COMPLETE"); return 0;
}
