package main
import (
 "fmt"
 "go/ast"
 "go/parser"
 "go/token"
 "golang.org/x/tools/go/cfg"
 "golang.org/x/tools/go/analysis"
 "golang.org/x/tools/go/analysis/passes/copylock"
)
func main(){
 f,e:=parser.ParseFile(token.NewFileSet(),"local.go",`package p; func f(x bool) { if x { a() } else { b() }; c() }`,0);if e!=nil{panic(e)}
 g:=cfg.New(f.Decls[0].(*ast.FuncDecl).Body,func(*ast.CallExpr)bool{return true})
 live,edges:=0,0;for _,b:=range g.Blocks{if b.Live{live++;edges+=len(b.Succs)}}
 if live!=4||edges!=4{panic(fmt.Sprint(live,edges))}
 if e=analysis.Validate([]*analysis.Analyzer{copylock.Analyzer});e!=nil{panic(e)}
 fmt.Println("TOOLS_BASELINE_OK live=4 edges=4 copylock-valid")
}
