# Counts are verified outside the test process so exit(0) cannot bypass completion.
BEGIN { required["order"]=1; required["instances"]=1; required["frontier"]=1; required["render"]=1; bad=0; complete=0; }
$1=="ORDERED_CASE" {
  if(NF!=4 || !($2 in required) || $4 !~ /^[0-9]+$/ || $4+0<1 || seen[$3]++ || complete) bad=1;
  cases[$2]++; checks[$2]+=$4; n++;
}
$1=="ORDERED_GROUP" {
  if(NF!=4 || !($2 in required) || done[$2]++ || $3+0<2 || $3+0!=cases[$2] || $4+0!=checks[$2] || complete) bad=1;
}
$1=="ORDERED_COMPLETE" { if(NF!=2 || ++complete!=1 || $2+0!=n) bad=1; }
END { for(g in required) if(done[g]!=1 || cases[g]<2 || checks[g]<2) bad=1; if(complete!=1 || n<8) bad=1; if(bad) { print "ORDERED_OUTPUT_REJECTED" > "/dev/stderr"; exit 1; } }
