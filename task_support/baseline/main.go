package main

import (
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"github.com/mandiant/GoReSym/objfile"
)

type function struct {
	Start, End uint64
	FullName string
}
type metadata struct {
	Version, Arch, OS string
	ModuleMeta objfile.ModuleData
	TabMeta struct { VA uint64; Version string }
	UserFunctions []function
}
func check(ok bool, msg string) {
	if !ok { panic(msg) }
}
func main() {
	for _, name := range []string{"linux-amd64", "windows-amd64", "darwin-arm64"} {
		path := ".task-build/" + name
		data, err := os.ReadFile(path+".json"); if err != nil { panic(err) }
		var m metadata
		if err := json.Unmarshal(data, &m); err != nil { panic(err) }
		check(m.TabMeta.VA != 0 && m.ModuleMeta.VA != 0 && m.ModuleMeta.TextVA != 0, "candidate not resolved: "+name)
		var root function
		for _, f := range m.UserFunctions {
			if f.FullName == "main.root" { root = f }
			check(f.FullName != "main.leaf" && f.FullName != "main.middle", "fixture did not inline: "+f.FullName)
		}
		check(root.Start != 0 && root.End > root.Start, "missing physical root: "+name)
		file, err := objfile.Open(path+".bin"); if err != nil { panic(err) }
		tabs, err := file.PCLineTable("", m.TabMeta.VA, m.ModuleMeta.TextVA); if err != nil { panic(err) }
		seen := false
		for tab := range tabs {
			if seen || tab.PclntabVA != m.TabMeta.VA || tab.ParsedPclntab.Go12line.Version.String() != m.TabMeta.Version { continue }
			src, line, fn := tab.ParsedPclntab.PCToLine(root.Start)
			check(fn != nil && fn.Name == "main.root", "physical function mismatch")
			check(strings.HasSuffix(src, "/task_support/fixture/main.go") && line > 0, "file/line missing")
			seen = true
			fmt.Printf("BASELINE_OBJECT_OK %s root_nonempty=true inline_names_absent=true file_line=true version=%s\n", name, m.Version)
		}
		check(seen, "no final table "+name)
		if err := file.Close(); err != nil { panic(err) }
	}
	fmt.Println("BASELINE_THREE_FORMATS_PHYSICAL_PCLNTAB_OK")
}
