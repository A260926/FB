#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <webvtt/parser.h>
#include <webvtt/cue.h>
#include <webvtt/node.h>
static webvtt_cue *cues[8];
static unsigned count, errors;
static void receive(void *p,webvtt_cue *cue){(void)p;if(count>=8)abort();cues[count++]=cue;}
static int error(void *p,webvtt_uint l,webvtt_uint c,webvtt_error e){(void)p;(void)l;(void)c;(void)e;errors++;return 0;}
static void text_nodes(const webvtt_node *n,char *buf,size_t *used){
  if(n->kind==WEBVTT_TEXT){const char *s=webvtt_string_text(&n->data.text);size_t len=strlen(s);if(*used+len>=256)abort();memcpy(buf+*used,s,len+1);*used+=len;}
  else if(!WEBVTT_IS_LEAF(n->kind)&&n->data.internal_data){unsigned j;for(j=0;j<n->data.internal_data->length;j++)text_nodes(n->data.internal_data->children[j],buf,used);}
}
static void check(int x,const char *what){if(!x){fprintf(stderr,"BASELINE_FAIL %s\n",what);exit(1);}}
int main(void){
  static const char input[]="WEBVTT\n\nfirst\n00:00:01.000 --> 00:00:04.000 line:-2 position:25% size:50% align:start vertical:rl\nA &amp; <b>B</b>\nC\n\nsecond\n00:00:02.000 --> 00:00:03.000 line:25% vertical:lr\nD\n\n";
  webvtt_parser p;char text[256]={0};size_t used=0,i;
  check(!WEBVTT_FAILED(webvtt_create_parser(receive,error,0,&p)),"create");
  for(i=0;i<sizeof(input)-1;i++)check(!WEBVTT_FAILED(webvtt_parse_chunk(p,input+i,1)),"chunk");
  check(!WEBVTT_FAILED(webvtt_finish_parsing(p)),"finish");webvtt_delete_parser(p);
  check(count==2&&errors==0,"two real parsed cues");
  check(cues[0]->from==1000&&cues[0]->until==4000,"milliseconds");
  check(cues[0]->snap_to_lines&&cues[0]->settings.line==-2,"negative snap line");
  check(cues[0]->settings.position==25&&cues[0]->settings.size==50&&cues[0]->settings.align==WEBVTT_ALIGN_START,"anchors");
  check(cues[0]->settings.vertical==WEBVTT_VERTICAL_RL,"vertical rl");
  check(!cues[1]->snap_to_lines&&cues[1]->settings.line==25&&cues[1]->settings.vertical==WEBVTT_VERTICAL_LR,"percent lr");
  text_nodes(cues[0]->node_head,text,&used);check(!strcmp(text,"A & B\nC"),"parsed entity and tree text");
  printf("VTT_BASELINE_OK cues=%u time=1000:4000 line=-2 position=25 size=50 rl=1 percent=25 lr=1\n",count);
  printf("VTT_TREE_OK text=A& B rows=2\n");
  for(i=0;i<count;i++)webvtt_release_cue(&cues[i]);
  return 0;
}
