{ for(i=1;i<=NF;i++) { x[++n]=$i; } }
END {
 if(n!=3076 || x[1]!="P3" || x[2]!=32 || x[3]!=32 || x[4]!=255) exit 1;
 different=0;
 for(i=5;i<=n;i++) { if(x[i]!~/^[0-9]+$/ || x[i]+0>255) exit 1; if(i>7 && x[i]!=x[5+(i-5)%3]) different=1; }
 if(!different) exit 1;
 print "ORDERED_IMAGE_OK 32x32";
}
