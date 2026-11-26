#include "mongoose.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static const unsigned char v5[]={0x30,7,0,1,'a',2,1,1,'x'};
int main(void){
  struct mg_mqtt_message m; struct mg_mqtt_prop p; size_t off;
  assert(mg_mqtt_parse(v5,sizeof(v5),5,&m)==MQTT_OK);
  memset(&p,0xAA,sizeof(p)); off=0;
  assert(mg_mqtt_prop_next(&m,&p,&off)==1);
  assert(p.id==1 && p.iv==1 && p.val.len==0 && p.key.len==0);
  assert(off==2 && m.props_start==6);
  assert(mg_mqtt_prop_next(&m,&p,&off)==0 && off==2 && p.id==1);
  /* props_size == 0 case: topic, prop len 0, empty payload */
  {
    const unsigned char a[]={0x30,4,0,1,'a',0};
    struct mg_mqtt_message mm; struct mg_mqtt_prop q; size_t o=0;
    assert(mg_mqtt_parse(a,sizeof(a),5,&mm)==MQTT_OK);
    assert(mm.props_size==0 && mm.data.len==0 && mm.props_start==6);
    memset(&q,0x55,sizeof(q));
    assert(mg_mqtt_prop_next(&mm,&q,&o)==0 && o==0 && q.iv==0x55555555u);
    assert(mg_mqtt_next_prop(&mm,&q,0)==0 && q.iv==0x55555555u);
  }
  /* unknown id */
  {
    const unsigned char a[]={0x30,6,0,1,'a',2,0x7F,0x00};
    struct mg_mqtt_message mm; struct mg_mqtt_prop q; size_t o=0;
    assert(mg_mqtt_parse(a,sizeof(a),5,&mm)==MQTT_MALFORMED);
    mm.dgram.buf=(char*)a; mm.dgram.len=8; mm.props_start=6; mm.props_size=2;
    memset(&q,0x33,sizeof(q));
    assert(mg_mqtt_prop_next(&mm,&q,&o)==-1 && o==0 && q.id==0x33);
  }
  /* SIZE_MAX-ish */
  {
    struct mg_mqtt_message mm; struct mg_mqtt_prop q; size_t o=0;
    memset(&mm,0,sizeof(mm)); mm.dgram.buf=(char*)v5; mm.dgram.len=sizeof(v5);
    mm.props_start=SIZE_MAX; mm.props_size=SIZE_MAX;
    memset(&q,0x44,sizeof(q));
    assert(mg_mqtt_prop_next(&mm,&q,&o)==-1 && o==0 && q.id==0x44);
    mm.props_start=sizeof(v5); mm.props_size=SIZE_MAX;
    assert(mg_mqtt_prop_next(&mm,&q,&o)==-1);
    mm.props_start=6; mm.props_size=2; o=SIZE_MAX;
    assert(mg_mqtt_prop_next(&mm,&q,&o)==-1 && o==SIZE_MAX && q.id==0x44);
  }
  /* truncated string inside declared zone */
  {
    const unsigned char a[]={0x30,7,0,1,'a',3,0x03,0x00,0x05};
    struct mg_mqtt_message mm; struct mg_mqtt_prop q; size_t o=0;
    assert(mg_mqtt_parse(a,sizeof(a),5,&mm)==MQTT_MALFORMED);
    mm.dgram.buf=(char*)a; mm.dgram.len=9; mm.props_start=6; mm.props_size=3;
    memset(&q,0x77,sizeof(q)); o=0;
    assert(mg_mqtt_prop_next(&mm,&q,&o)==-1 && o==0 && q.id==0x77);
  }
  /* qos1 publish: topic + id + props + payload */
  {
    const unsigned char a[]={0x32,9,0,1,'a',0,1,2,1,1,'x'};
    struct mg_mqtt_message mm; struct mg_mqtt_prop q; size_t o=0;
    assert(mg_mqtt_parse(a,sizeof(a),5,&mm)==MQTT_OK);
    assert(mm.id==1 && mm.data.len==1 && mm.data.buf[0]=='x' && mm.props_size==2);
    assert(mg_mqtt_prop_next(&mm,&q,&o)==1 && q.id==1&&q.iv==1 && o==2);
    assert(mg_mqtt_prop_next(&mm,&q,&o)==0);
  }
  /* concatenated: only first parsed */
  {
    unsigned char a[32]; size_t n=0; struct mg_mqtt_message mm;
    const unsigned char p1[]={0x30,7,0,1,'a',2,1,1,'x'};
    const unsigned char p2[]={0x30,4,0,1,'a',0};
    memcpy(a,p1,9); memcpy(a+9,p2,6); n=15;
    assert(mg_mqtt_parse(a,n,5,&mm)==MQTT_OK && mm.dgram.len==9 && mm.data.len==1);
  }
  /* incomplete declared packet -> INCOMPLETE; complete-but-bad -> MALFORMED */
  {
    const unsigned char inc[]={0x30,7,0,1,'a',2,1};
    struct mg_mqtt_message mm;
    assert(mg_mqtt_parse(inc,sizeof(inc),5,&mm)==MQTT_INCOMPLETE);
    const unsigned char noPropLen[]={0x30,3,0,1,'a'};
    assert(mg_mqtt_parse(noPropLen,sizeof(noPropLen),5,&mm)==MQTT_MALFORMED);
  }
  /* MQTT4 positive control */
  {
    const unsigned char a4[]={0x30,4,0,1,'a','x'};
    struct mg_mqtt_message mm;
    assert(mg_mqtt_parse(a4,sizeof(a4),4,&mm)==MQTT_OK);
    assert(mm.topic.len==1 && mm.topic.buf[0]=='a' && mm.data.len==1 && mm.data.buf[0]=='x' && mm.props_size==0);
  }
  /* varint non-minimal terminates */
  {
    const unsigned char a[]={0x30,7,0,1,'a',3,0x0B,0x81,0x00};
    struct mg_mqtt_message mm; struct mg_mqtt_prop q; size_t o=0;
    assert(mg_mqtt_parse(a,sizeof(a),5,&mm)==MQTT_OK);
    assert(mg_mqtt_prop_next(&mm,&q,&o)==1 && q.id==0x0B && q.iv==1 && o==3);
  }
  puts("QUICK_OK"); return 0;
}
