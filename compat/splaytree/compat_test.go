package splaytree
import("math/rand";"sort";"testing")
type number int
func(n number)Compare(x number)int{if n<x{return -1};if n>x{return 1};return 0}
func TestOrderedBag(t *testing.T){
 r:=rand.New(rand.NewSource(21));var tree Tree[number];bag:=[]int{}
 check:=func(){sort.Ints(bag);i:=0;tree.Iterate(func(x number)bool{if i>=len(bag)||int(x)!=bag[i]{t.Fatalf("order at %d",i)};i++;return true});if i!=len(bag)||tree.Len()!=len(bag){t.Fatal("count")};if i>0&&(int(tree.Min())!=bag[0]||int(tree.Max())!=bag[i-1]){t.Fatal("extrema")}}
 for i:=0;i<400;i++{v:=r.Intn(40);tree.Insert(number(v));bag=append(bag,v)};check()
 for len(bag)>0{i:=r.Intn(len(bag));tree.Delete(number(bag[i]));bag=append(bag[:i],bag[i+1:]...);check()}
 if tree.Root!=nil{t.Fatal("empty")}
}
