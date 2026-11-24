#include "tests/oss-fuzz/session_model.h"
#include "coap3/coap_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
void task_cov_start(void);
unsigned task_cov_hits(void);
#define CHECK(x) do {if (!(x)) {fprintf(stderr,"API consumer line %d\n",__LINE__);return 7;}} while (0)
static const char normal[] =
 "COAP_MODEL_PLAN 1\n"
 "CASE cancel\nSESSION 0 40\nSESSION 1 40\n"
 "SEND 0 0 1 1 10\nSEND 0 0 2 2 20\nSEND 0 0 3 3 30\nSEND 0 1 2 2 25\n"
 "CANCEL 5 0 2\nACK 6 0 1\nACK 7 0 3\nACK 8 1 2\nEND\n"
 "CASE timeout\nSESSION 0 20\nSEND 0 0 1 1 2\nEND\n"
 "CASE quiet\nSESSION 0 1\nEND\n";
static const char reordered[] =
 "COAP_MODEL_PLAN 1\nCASE quiet\nSESSION 0 1\nEND\n"
 "CASE timeout\nSESSION 0 20\nSEND 0 0 1 1 2\nEND\n"
 "CASE cancel\nSESSION 1 40\nSESSION 0 40\n"
 "ACK 8 1 2\nSEND 0 1 2 2 25\nACK 7 0 3\nSEND 0 0 3 3 30\n"
 "CANCEL 5 0 2\nSEND 0 0 2 2 20\nACK 6 0 1\nSEND 0 0 1 1 10\nEND\n";
static int save(const char *p,const char *s) {
 FILE *f=fopen(p,"wb"); if(!f)return 0;
 size_t n=strlen(s); int ok=fwrite(s,1,n,f)==n; return fclose(f)==0 && ok;
}
int main(int argc,char **argv) {
 FILE *f; char *text; long n; int rc;
 CHECK(argc==4); CHECK(save(argv[1],normal)); CHECK(save(argv[2],reordered));
 f=fopen(argv[3],"w+b"); CHECK(f); task_cov_start();
 /* Fixed consumers of both new library symbols; no production implementation. */
 coap_startup(); coap_set_log_level(COAP_LOG_EMERG);
 coap_context_t *ctx=coap_new_context(NULL); CHECK(ctx);
 coap_address_t addr; coap_address_init(&addr);addr.size=sizeof(addr.addr.sin);
 addr.addr.sin.sin_family=AF_INET;addr.addr.sin.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 addr.addr.sin.sin_port=htons(5683);
 coap_session_t *s=coap_new_client_session(ctx,NULL,&addr,COAP_PROTO_UDP);CHECK(s);
 s->max_retransmit=0;
 coap_queue_t *q=coap_new_node();CHECK(q);
 q->pdu=coap_pdu_init(COAP_MESSAGE_CON,COAP_REQUEST_CODE_GET,1,64);CHECK(q->pdu);
 q->id=1;q->timeout=10;
 CHECK(coap_wait_ack_at(ctx,s,q,100)==1);
 CHECK(ctx->sendqueue_basetime==100 && ctx->sendqueue->t==10);
 q=coap_pop_next(ctx);CHECK(q);
 CHECK(coap_retransmit_at(ctx,q,110)==COAP_INVALID_MID);
 CHECK(ctx->sendqueue==NULL);
 coap_session_release(s);coap_free_context(ctx);coap_cleanup();
 rc=coap_model_run((const uint8_t*)normal,strlen(normal),f); CHECK(rc==0);
 CHECK(fflush(f)==0); n=ftell(f); CHECK(n>0 && n<2000000); rewind(f);
 text=malloc((size_t)n+1); CHECK(text); CHECK(fread(text,1,(size_t)n,f)==(size_t)n);text[n]=0;
 CHECK(strstr(text,"COAP_MODEL_COVERAGE 1\n")==text);
 CHECK(strstr(text,"\nSTATE ") && strstr(text,"\nEDGE "));
 CHECK(strstr(text,"CASE cancel sessions=2 acked=3 canceled=1 timeout=0 retries=0\n"));
 CHECK(strstr(text,"CASE timeout sessions=1 acked=0 canceled=0 timeout=1 retries=2\n"));
 CHECK(strstr(text,"CASE quiet sessions=1 acked=0 canceled=0 timeout=0 retries=0\n"));
 free(text); CHECK(fclose(f)==0); CHECK(task_cov_hits()>0);
 printf("API_MODEL_OK real_guard_hits=%u\n",task_cov_hits());return 0;
}
