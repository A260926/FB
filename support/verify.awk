# Fixed process-external verifier. A CASE is accepted only after a matching BEGIN.
function fail(s) { print "PROTOCOL_REJECT " s > "/dev/stderr"; bad=1 }
function positive(s) { return s ~ /^[1-9][0-9]*$/ }
BEGIN {
 n=split(required,order,","); for(i=1;i<=n;i++) allowed[order[i]]=1;
 stage=1; active=""; done=0; total=0; sums=0
}
$1=="BEGIN" {
 if(done || NF!=3 || active!="" || $2!=order[stage] || $3!~/^[A-Za-z_][A-Za-z0-9_]*$/ || names[$3]++) fail("bad BEGIN");
 active=$2" "$3; next
}
$1=="CASE" {
 if(done || NF!=4 || active!=$2" "$3 || !positive($4) || $2!=order[stage]) fail("bad CASE");
 active=""; count[$2]++; sum[$2]+=$4; total++; sums+=$4; next
}
$1=="GROUP" {
 if(done || NF!=4 || active!="" || $2!=order[stage] || groups[$2]++ || !positive($3) || !positive($4) || $3!=count[$2] || $4!=sum[$2] || $3<per_group) fail("bad GROUP");
 stage++; next
}
$1=="TARGET_COMPLETE" {
 if(done++ || NF!=3 || active!="" || stage!=n+1 || $2!~/^cases=[1-9][0-9]*$/ || $3!~/^checks=[1-9][0-9]*$/) fail("early or malformed summary");
 split($2,a,"=");split($3,b,"="); if(a[2]!=total || b[2]!=sums || total<minimum) fail("summary mismatch");next
}
END {
 if(active!="" || done!=1 || stage!=n+1) fail("incomplete execution");
 for(i=1;i<=n;i++) if(groups[order[i]]!=1 || count[order[i]]<per_group || sum[order[i]]<=0) fail("missing group");
 if(bad) exit 1;
 print "PROCESS_VERIFIED cases="total" checks="sums
}
