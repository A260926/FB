package pogreb
import("bytes";"os";"path/filepath";"strings";"sync";"testing";"time";"github.com/akrylysov/pogreb/fs")
type baselineBlockingFS struct {fs.FileSystem; prefix string; entered,release chan struct{}; once sync.Once}
type baselineBlockingFile struct {fs.File; owner *baselineBlockingFS}
func(b *baselineBlockingFS) OpenFile(n string,f int,m os.FileMode)(fs.File,error){
 x,e:=b.FileSystem.OpenFile(n,f,m);if e==nil&&strings.HasPrefix(n,b.prefix)&&strings.HasSuffix(n,".psg")&&f&os.O_CREATE!=0{return &baselineBlockingFile{x,b},nil};return x,e
}
func(f *baselineBlockingFile) Write(p []byte)(int,error){f.owner.once.Do(func(){close(f.owner.entered);<-f.owner.release});return f.File.Write(p)}
func TestBaselineBackup(t *testing.T){
 dir:=t.TempDir();dst:=filepath.Join(dir,"backup")
 b:=&baselineBlockingFS{FileSystem:fs.OS,prefix:dst+string(os.PathSeparator),entered:make(chan struct{}),release:make(chan struct{})}
 db,e:=Open(filepath.Join(dir,"live"),&Options{FileSystem:b,maxSegmentSize:256,compactionMinSegmentSize:64,compactionMinFragmentation:0.01});if e!=nil{t.Fatal(e)}
 defer db.Close()
 for i:=0;i<12;i++ {if e=db.Put([]byte{byte(i)},bytes.Repeat([]byte{byte(i)},80));e!=nil{t.Fatal(e)}}
 if e=db.Put([]byte{0},[]byte("before"));e!=nil{t.Fatal(e)}
 done:=make(chan error,1);go func(){done<-db.Backup(dst)}()
 defer func(){select{case <-b.release:default:close(b.release)}}()
 select{case <-b.entered:case <-time.After(3*time.Second):t.Fatal("backup did not reach destination")}
 _,e=db.Compact();if e!=errBusy{close(b.release);<-done;t.Fatalf("want current busy result, got %v",e)}
 if e=db.Put([]byte{0},[]byte("after"));e!=nil{t.Fatal(e)}
 close(b.release);if e=<-done;e!=nil{t.Fatal(e)}
 snap,e:=Open(dst,&Options{FileSystem:fs.OS});if e!=nil{t.Fatal(e)};defer snap.Close()
 v,e:=snap.Get([]byte{0});if e!=nil||string(v)!="before"{t.Fatalf("snapshot %q %v",v,e)}
 t.Log("BASELINE_COPY_BLOCKS_COMPACTION")
 t.Log("BASELINE_BACKUP_REOPENS_SNAPSHOT")
}
