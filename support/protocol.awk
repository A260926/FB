# Parse completion records outside the target process, in execution order.
BEGIN {g[1]="index";g[2]="pixels";g[3]="budget";phase=0;current=0;total=0;checks=0;done=0;bad=0}
function reject(){bad=1;exit 1}
$1=="GROUP_BEGIN" {
 if(done||phase!=0||NF!=2||current>=3||$2!=g[current+1])reject();
 ++current;phase=1;n=0;c=0;next
}
$1=="CASE" {
 if(done||phase!=1||NF!=4||$2!=g[current]||$4!~/^[1-9][0-9]*$/||seen[$3]++)reject();
 ++n;c+=$4;next
}
$1=="GROUP_END" {
 if(done||phase!=1||NF!=4||$2!=g[current]||n<2||$3!=n||$4!=c)reject();
 total+=n;checks+=c;phase=0;next
}
$1=="TARGET_COMPLETE" {
 if(done||current!=3||phase!=0||NF!=3||$2!="cases="total||$3!="checks="checks)reject();
 done=1;next
}
END {if(bad||!done||total<6||checks<6)exit 1;print "TARGET_PROTOCOL_OK groups=3 cases="total" checks="checks}
