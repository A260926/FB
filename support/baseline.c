#include <framework/mlt.h>
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"check failed line %d\n",__LINE__);exit(1);}}while(0)
int main(void){
 mlt_profile profile=mlt_profile_init(NULL);CHECK(profile);
 mlt_producer a=mlt_producer_new(profile),b=mlt_producer_new(profile);CHECK(a&&b);
 mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(a),"length",1000);
 mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(b),"length",1000);
 mlt_producer_set_in_and_out(a,0,999);mlt_producer_set_in_and_out(b,0,999);
 mlt_playlist p=mlt_playlist_new(profile);CHECK(p);
 CHECK(!mlt_playlist_append_io(p,a,100,119));CHECK(!mlt_playlist_append_io(p,b,200,229));
 CHECK(!mlt_playlist_mix(p,0,6,NULL));CHECK(mlt_playlist_count(p)==3);
 CHECK(mlt_playlist_clip_is_mix(p,1));CHECK(mlt_playlist_clip_length(p,0)==14);
 CHECK(mlt_playlist_clip_length(p,1)==6);CHECK(mlt_playlist_clip_length(p,2)==24);
 mlt_producer mix=mlt_playlist_get_clip(p,1);
 mlt_tractor t=(mlt_tractor)mlt_producer_cut_parent(mix);
 mlt_producer ta=mlt_tractor_get_track(t,0),tb=mlt_tractor_get_track(t,1);
 CHECK(mlt_producer_get_in(ta)==114 && mlt_producer_get_out(ta)==119);
 CHECK(mlt_producer_get_in(tb)==200 && mlt_producer_get_out(tb)==205);
 CHECK(mlt_producer_get_playtime(MLT_PLAYLIST_PRODUCER(p))==44);
 CHECK(mlt_producer_get_in(a)==0 && mlt_producer_get_out(a)==999);
 CHECK(mlt_producer_get_in(b)==0 && mlt_producer_get_out(b)==999);
 CHECK(mlt_properties_get_data(MLT_TRACTOR_PROPERTIES(t),"mix_in",NULL)==mlt_playlist_get_clip(p,0));
 CHECK(mlt_properties_get_data(MLT_TRACTOR_PROPERTIES(t),"mix_out",NULL)==mlt_playlist_get_clip(p,2));
 printf("MLT_BASELINE_OK body=14 mix=6 body=24 total=44 A_mix=114..119 B_mix=200..205 links=ok parents=unchanged\n");
 mlt_playlist_close(p);mlt_producer_close(a);mlt_producer_close(b);mlt_profile_close(profile);mlt_pool_close();
 return 0;
}
