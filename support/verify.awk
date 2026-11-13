/^GROUP / { if ($3 != "CHECKS" || $4 !~ /^[1-9][0-9]*$/ || $5 != "DONE") bad=1; seen[$2]++ }
/^TARGET COMPLETE groups=/ {complete++}
END { split("routing delay lifetime", required, " "); for(i in required) if(seen[required[i]] != 1) bad=1;
 if(complete!=1)bad=1; for(i in seen){ok=0;for(j in required)if(i==required[j])ok=1;if(!ok)bad=1} exit bad }
