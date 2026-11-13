BEGIN { names[1]="recovery"; names[2]="prediction"; names[3]="compat"; n=0; done=0; cases=0; checks=0; bad=0 }
/^GROUP / {
 if(done || NF!=4 || $2!=names[++n] || $3!~/^cases=[1-9][0-9]*$/ || $4!~/^checks=[1-9][0-9]*$/)bad=1;
 split($3,a,"=");split($4,b,"=");cases+=a[2];checks+=b[2];
}
/^COMPLETE / {
 if(done++ || n!=3 || NF!=4 || $2!="groups=3" || $3!~/^cases=[1-9][0-9]*$/ || $4!~/^checks=[1-9][0-9]*$/)bad=1;
 split($3,a,"=");split($4,b,"=");if(a[2]!=cases||b[2]!=checks)bad=1;
}
END { if(bad || n!=3 || done!=1 || cases<3 || checks<3)exit 1 }
