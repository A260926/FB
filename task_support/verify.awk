/^GROUP / {
    if (NF != 5 || $3 != "CHECKS" || $4 !~ /^[1-9][0-9]*$/ || $5 != "DONE") bad=1;
    seen[$2]++;
}
/^TARGET COMPLETE/ { if ($0 != "TARGET COMPLETE groups=3") bad=1; complete++ }
END {
    split("density transport passes", required, " ");
    for (i in required) if (seen[required[i]] != 1) bad=1;
    for (name in seen) {
        valid=0;
        for (i in required) if (name == required[i]) valid=1;
        if (!valid) bad=1;
    }
    if (complete != 1) bad=1;
    if (bad) print "INCOMPLETE_TARGET_EXECUTION" > "/dev/stderr";
    exit bad;
}
