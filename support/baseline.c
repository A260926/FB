#include "lmdb.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv) {
 MDB_env *e; MDB_txn *w,*r; MDB_dbi d; MDB_stat st; char k[16],v[200];
 MDB_val key,data; memset(v,'x',sizeof v); assert(argc==2);
 assert(!mdb_env_create(&e)); assert(!mdb_env_set_maxdbs(e,2));
 assert(!mdb_env_set_mapsize(e,32*1024*1024)); assert(!mdb_env_open(e,argv[1],MDB_NOTLS,0600));
 assert(!mdb_txn_begin(e,0,0,&w)); assert(!mdb_dbi_open(w,"events",MDB_CREATE,&d));
 for(int i=0;i<4000;i++){snprintf(k,sizeof k,"%08d",i);key=(MDB_val){8,k};data=(MDB_val){sizeof v,v};assert(!mdb_put(w,d,&key,&data,0));}
 assert(!mdb_txn_commit(w)); assert(!mdb_txn_begin(e,0,MDB_RDONLY,&r));
 assert(!mdb_stat(r,d,&st)); assert(st.ms_entries==4000 && st.ms_leaf_pages>1);
 assert(!mdb_txn_begin(e,0,0,&w));
 for(int i=1000;i<3000;i++){snprintf(k,sizeof k,"%08d",i);key=(MDB_val){8,k};assert(!mdb_del(w,d,&key,0));}
 assert(!mdb_txn_commit(w)); assert(!mdb_stat(r,d,&st)); assert(st.ms_entries==4000);
 mdb_txn_abort(r);assert(!mdb_txn_begin(e,0,MDB_RDONLY,&r));assert(!mdb_stat(r,d,&st));assert(st.ms_entries==2000);
 puts("POINT_DELETE=2000 OLD_SNAPSHOT=4000 NEW_SNAPSHOT=2000");
 mdb_txn_abort(r);mdb_env_close(e);return 0;
}
