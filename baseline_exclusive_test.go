package etree
import("bytes";"testing")
func TestBaselineSerialization(t *testing.T) {
 d:=NewDocument();d.ReadSettings.PreserveCData=true
 if e:=d.ReadFromString(`<outer xmlns:p="urn:p" xml:lang="fr"><p:payload z="2" a="1"><![CDATA[a&b]]><p:child/></p:payload></outer>`);e!=nil{t.Fatal(e)}
 e:=d.Root().ChildElements()[0]
 s:=WriteSettings{CanonicalEndTags:true,CanonicalText:true,CanonicalAttrVal:true}
 var out bytes.Buffer;e.WriteTo(&out,&s)
 want:=`<p:payload z="2" a="1"><![CDATA[a&b]]><p:child></p:child></p:payload>`
 if out.String()!=want{t.Fatalf("unexpected existing serialization: %q",out.String())}
 if e.NamespaceURI()!="urn:p"{t.Fatal("ancestor namespace lookup failed")}
 t.Log("BASELINE_ANCESTOR_BINDING_RESOLVED")
 t.Log("BASELINE_CANONICAL_FLAGS_ARE_NOT_EXCLUSIVE_C14N")
}
