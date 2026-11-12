package rdf
import("strings";"testing";"bytes")
func TestBaseline(t *testing.T){
 d:=NewTripleDecoder(strings.NewReader("@prefix e: <https://example.test/> . e:s e:p [ e:q \"v\" ] ."),Turtle)
 ts,err:=d.DecodeAll();if err!=nil||len(ts)!=2 {t.Fatalf("decode: %v %v",ts,err)}
 var b bytes.Buffer;e:=NewTripleEncoder(&b,NTriples);if e.EncodeAll(ts)!=nil||e.Close()!=nil {t.Fatal("encode")}
 back:=NewTripleDecoder(strings.NewReader(b.String()),NTriples);again,err:=back.DecodeAll();if err!=nil||len(again)!=2 {t.Fatal("roundtrip",err)}
 t.Log("RDF_BASELINE_COLLECTION_AND_ROUNDTRIP_OK")
}
