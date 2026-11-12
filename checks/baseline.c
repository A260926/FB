/* Public synthetic-certificate witness, CC0-1.0. No private keys. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bearssl.h"

static unsigned char dn[1024];
static size_t dn_len;
static void append_dn(void *unused, const void *p, size_t n) {
    (void)unused;
    if (n > sizeof dn - dn_len) exit(2);
    memcpy(dn + dn_len, p, n); dn_len += n;
}
static unsigned char *load(const char *name, size_t *n) {
    unsigned char *p;
    long z;
    FILE *f = fopen(name, "rb");
    if (!f || fseek(f, 0, SEEK_END) || (z=ftell(f)) <= 0 || fseek(f,0,SEEK_SET)) exit(2);
    p=malloc((size_t)z); if (!p || fread(p,1,(size_t)z,f)!=(size_t)z || fclose(f)) exit(2);
    *n=(size_t)z; return p;
}
static unsigned verify(const br_x509_trust_anchor *ta, const char *leaf,
        const char *ca, const char *server, size_t chunk) {
    br_x509_minimal_context c;
    const char *files[2] = {leaf,ca};
    size_t i;
    br_x509_minimal_init_full(&c,ta,1);
    /* Fixed instant within synthetic fixtures' 2020..2040 validity. */
    br_x509_minimal_set_time(&c, 739951, 0); /* 2025-12-01, days since 0000-01-01 */
    c.vtable->start_chain(&c.vtable,server);
    for (i=0;i<2;i++) {
        size_t n,pos=0; unsigned char *p=load(files[i],&n);
        c.vtable->start_cert(&c.vtable,(uint32_t)n);
        while (pos<n) {
            size_t z=n-pos; if(z>chunk) z=chunk;
            c.vtable->append(&c.vtable,p+pos,z); pos+=z;
        }
        c.vtable->end_cert(&c.vtable); free(p);
    }
    return c.vtable->end_chain(&c.vtable);
}
int main(void) {
    br_x509_decoder_context dc;
    br_x509_trust_anchor ta;
    br_x509_pkey *pk;
    size_t n;
    unsigned a,b,c,d,e;
    unsigned char *root=load("checks/fixtures/root.der",&n);
    br_x509_decoder_init(&dc,append_dn,NULL);
    br_x509_decoder_push(&dc,root,n); pk=br_x509_decoder_get_pkey(&dc);
    if (!pk || !dn_len) return 2;
    ta.dn.data=dn; ta.dn.len=dn_len; ta.flags=BR_X509_TA_CA; ta.pkey=*pk;
    a=verify(&ta,"checks/fixtures/good.der","checks/fixtures/ca.der","service.example.test",4096);
    b=verify(&ta,"checks/fixtures/outside.der","checks/fixtures/ca.der",NULL,1);
    c=verify(&ta,"checks/fixtures/outside.der","checks/fixtures/ca.der","outside.invalid",4096);
    d=verify(&ta,"checks/fixtures/good.der","checks/fixtures/ca-critical.der",NULL,1);
    e=verify(&ta,"checks/fixtures/good.der","checks/fixtures/ca.der","wrong.invalid",1);
    printf("BASELINE good=%u outside_no_hostname=%u outside_matched_hostname=%u critical=%u hostname_mismatch=%u\n",a,b,c,d,e);
    free(root);
    if(a || b || c || d!=BR_ERR_X509_CRITICAL_EXTENSION || e!=BR_ERR_X509_BAD_SERVER_NAME) return 1;
    puts("BEARSSL_BASELINE_OK"); return 0;
}
