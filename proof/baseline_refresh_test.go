package proof_test
import("fmt";"testing";"github.com/transparency-dev/merkle/proof";"github.com/transparency-dev/merkle/rfc6962";"github.com/transparency-dev/merkle/testonly")
func TestBaselineRefresh(t *testing.T) {
 h:=rfc6962.DefaultHasher;tr:=testonly.New(h)
 for i:=0;i<13;i++ {tr.AppendData([]byte(fmt.Sprintf("entry-%d",i)))}
 old,e:=tr.InclusionProof(4,7);if e!=nil{t.Fatal(e)}
 if e=proof.VerifyInclusion(h,4,7,tr.LeafHash(4),old,tr.HashAt(7));e!=nil{t.Fatal(e)}
 c,e:=tr.ConsistencyProof(7,13);if e!=nil{t.Fatal(e)}
 if e=proof.VerifyConsistency(h,7,13,c,tr.HashAt(7),tr.HashAt(13));e!=nil{t.Fatal(e)}
 fresh,e:=tr.InclusionProof(4,13);if e!=nil{t.Fatal(e)}
 if e=proof.VerifyInclusion(h,4,13,tr.LeafHash(4),fresh,tr.HashAt(13));e!=nil{t.Fatal(e)}
 if e=proof.VerifyInclusion(h,4,13,tr.LeafHash(4),old,tr.HashAt(13));e==nil{t.Fatal("old proof unexpectedly valid for new root")}
 t.Log("BASELINE_OLD_INCLUSION_AND_CONSISTENCY_OK")
 t.Log("BASELINE_REISSUED_PROOF_REQUIRED")
}
