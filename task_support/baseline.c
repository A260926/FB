#include "mongoose.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void) {
  const unsigned char v4[]={0x30,4,0,1,'a','x'};
  const unsigned char v5[]={0x30,7,0,1,'a',2,1,1,'x'};
  struct mg_mqtt_message m; struct mg_mqtt_prop p;
  assert(mg_mqtt_parse(v4,sizeof(v4),4,&m)==MQTT_OK);
  assert(m.topic.len==1 && m.topic.buf[0]=='a' && m.data.len==1 && m.data.buf[0]=='x');
  assert(mg_mqtt_parse(v5,sizeof(v5),5,&m)==MQTT_OK);
  assert(m.dgram.len==sizeof(v5) && m.props_size==2 && m.data.len==1);
  memset(&p,0,sizeof(p));
  assert(mg_mqtt_next_prop(&m,&p,0)==2 && p.id==1 && p.iv==1);
  puts("MQTT_ORIGINAL_VALID_PACKETS_OK"); return 0;
}
