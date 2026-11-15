package essentials
import("errors";"reflect";"sync/atomic";"testing")
func TestCompatibility(t *testing.T){
 a:=[]int{3,1,2};b:=[]string{"c","a","b"};VoodooSort(a,func(i,j int)bool{return a[i]<a[j]},b)
 if !reflect.DeepEqual(a,[]int{1,2,3})||!reflect.DeepEqual(b,[]string{"a","b","c"}){t.Fatal(a,b)}
 Reverse(a);OrderedDelete(&a,1);UnorderedDelete(&a,0);if !reflect.DeepEqual(a,[]int{1}){t.Fatal(a)}
 if MinInt(-3,9,0)!=-3||MaxInt(-3,9,0)!=9{t.Fatal("extrema")}
 e:=errors.New("root");if !errors.Is(AddCtx("context",e),e)||AddCtx("",nil)!=nil{t.Fatal("errors")}
 for _,workers:=range []int{0,1,3,1000}{
  seen:=make([]int32,97);sum:=0
  ReduceConcurrentMap(workers,len(seen),func()(func(int),func()){n:=0;return func(i int){atomic.AddInt32(&seen[i],1);n++},func(){sum+=n}})
  if sum!=97{t.Fatal("reduce",sum)};for _,n:=range seen{if n!=1{t.Fatal("coverage",seen)}}
 }
}
