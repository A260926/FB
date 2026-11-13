#define _DARWIN_C_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "archive.h"
#include "archive_entry.h"
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
static struct archive_entry *entry(const char *name, int type, int size) {
 struct archive_entry *e=archive_entry_new(); assert(e);
 archive_entry_set_pathname(e,name); archive_entry_set_mode(e,type|0750);
 archive_entry_set_size(e,size); return e;
}
int main(void) {
 char dir[]="check-baseline-XXXXXX"; struct archive *a; struct archive_entry *e;
 struct stat st,st2; char data[8]={0}; FILE *f;
 assert(mkdtemp(dir)); assert(chdir(dir)==0); assert(mkdir("outside",0700)==0);
 f=fopen("outside/victim","wb"); assert(f); assert(fwrite("SAFE",1,4,f)==4); fclose(f);
 assert(mkdir("root",0700)==0); assert(chdir("root")==0);
 a=archive_write_disk_new(); assert(a);
 assert(archive_write_disk_set_options(a,ARCHIVE_EXTRACT_PERM|ARCHIVE_EXTRACT_SECURE_SYMLINKS|ARCHIVE_EXTRACT_SECURE_NODOTDOT|ARCHIVE_EXTRACT_SECURE_NOABSOLUTEPATHS)==ARCHIVE_OK);
 e=entry("tree",AE_IFDIR,0); assert(archive_write_header(a,e)==ARCHIVE_OK);
 assert(archive_write_finish_entry(a)==ARCHIVE_OK); archive_entry_free(e);
 e=entry("tree/data",AE_IFREG,4); assert(archive_write_header(a,e)==ARCHIVE_OK);
 assert(archive_write_data(a,"DATA",4)==4); assert(archive_write_finish_entry(a)==ARCHIVE_OK); archive_entry_free(e);
 e=entry("tree/alias",AE_IFREG,0); archive_entry_set_hardlink(e,"tree/data");
 assert(archive_write_header(a,e)==ARCHIVE_OK); assert(archive_write_finish_entry(a)==ARCHIVE_OK); archive_entry_free(e);
 e=entry("escape",AE_IFLNK,0); archive_entry_set_symlink(e,"../outside");
 assert(archive_write_header(a,e)==ARCHIVE_OK); assert(archive_write_finish_entry(a)==ARCHIVE_OK); archive_entry_free(e);
 e=entry("escape/victim",AE_IFREG,4); assert(archive_write_header(a,e)==ARCHIVE_FAILED); archive_entry_free(e);
 assert(archive_write_close(a)==ARCHIVE_OK); assert(archive_write_free(a)==ARCHIVE_OK);
 assert(stat("tree/data",&st)==0); assert(stat("tree/alias",&st2)==0); assert(st.st_ino==st2.st_ino && st.st_dev==st2.st_dev);
 assert(stat("tree",&st)==0); assert((st.st_mode&0777)==0750);
 f=fopen("tree/data","rb"); assert(f); assert(fread(data,1,4,f)==4); fclose(f); assert(memcmp(data,"DATA",4)==0);
 f=fopen("../outside/victim","rb"); assert(f); assert(fread(data,1,4,f)==4); fclose(f); assert(memcmp(data,"SAFE",4)==0);
 puts("normal_disk_writer=PASS regular+hardlink+deferred_permissions");
 puts("existing_symlink_guard=PASS outside_sentinel=SAFE");
 puts("target_root_fd=NOT_IMPLEMENTED baseline_only"); return 0;
}
