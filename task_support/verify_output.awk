# Fixed-driver protocol check in the parent process; no target algorithm.
function fail() { bad=1 }
function positive(s) { return s ~ /^[1-9][0-9]*$/ }
function value(s,key, t) { if(index(s,key "=")!=1) {fail();return 0}; t=substr(s,length(key)+2);if(!positive(t))fail();return t+0 }
function case_record(g,name,c,key) {
 if(!(g in required)||done||passed[g]||!positive(c))fail();
 key=(style=="A"?name:g "/" name);if(names[key]++)fail();
 cases[g]++;checks[g]+=c;totalcases++;totalchecks+=c;
}
BEGIN { count=split(groups,list," ");for(i=1;i<=count;i++){if(list[i] in required)fail();required[list[i]]=1} }
/^CASE_PASS / {
 if(style!="A"||NF!=4)fail();c=value($4,"checks");case_record($2,$3,c);next;
}
/^case / {
 if(style!="C"||NF!=3)fail();pos=index($2,"/");if(pos<2||pos==length($2))fail();
 g=substr($2,1,pos-1);name=substr($2,pos+1);c=value($3,"checks");case_record(g,name,c);next;
}
/^GROUP_PASS / {
 if(style!="A"||NF!=4||done||!($2 in required)||passed[$2]++)fail();
 n=value($3,"cases");c=value($4,"checks");if(n!=cases[$2]||c!=checks[$2])fail();next;
}
/^PASS / {
 g=$2;if(style=="A"||done||!(g in required)||passed[g]++)fail();
 if(style=="B") {if(NF!=3||!positive($3))fail();cases[g]=1;checks[g]=$3+0;totalcases++;totalchecks+=$3}
 else {if(NF!=4)fail();n=value($3,"cases");c=value($4,"checks");if(n!=cases[g]||c!=checks[g])fail()}
 next;
}
/^TARGET_COMPLETE/ {
 if(done++)fail();for(g in required)if(passed[g]!=1||cases[g]<1||checks[g]<1)fail();
 if(style=="A") {if(NF!=3)fail();n=value($2,"cases");c=value($3,"checks");if(n!=totalcases||c!=totalchecks)fail()}
 else if(NF!=1||$1!="TARGET_COMPLETE")fail();next;
}
END {
 if(done!=1)fail();for(g in required)if(passed[g]!=1||cases[g]<1||checks[g]<1)fail();
 if(bad){print "target output incomplete or inconsistent" > "/dev/stderr";exit 1}
}
