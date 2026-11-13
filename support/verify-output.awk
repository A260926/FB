# Fixed driver transcript: only returned, nonempty cases count as completed.
function reject(reason) {
    print "TARGET_TRANSCRIPT_REJECTED: " reason > "/dev/stderr"
    failed = 1
    exit 1
}
function positive(n) { return n ~ /^[1-9][0-9]*$/ }
function group(g) { return g == "checked" || g == "ownership" || g == "reset" }
{
    if (complete) reject("output after final completion")
    if ($1 == "CASE") {
        if (phase || NF != 4 || !group($2) || $3 !~ /^[A-Za-z0-9_.-]+$/ || !positive($4))
            reject("invalid or late case")
        if (names[$3]++) reject("duplicate case name")
        cases[$2]++; checks[$2] += $4; total_cases++; total_checks += $4
    }
    else if ($1 == "GROUP") {
        phase = 1
        if (NF != 4 || !group($2) || completed[$2]++ || $3 !~ /^cases=[1-9][0-9]*$/ || $4 !~ /^checks=[1-9][0-9]*$/)
            reject("invalid or duplicate group completion")
        split($3, c, "="); split($4, a, "=")
        if (c[2] != cases[$2] || a[2] != checks[$2]) reject("group totals differ")
    }
    else if ($1 == "TARGET_COMPLETE") {
        if (NF != 3 || $2 !~ /^cases=[1-9][0-9]*$/ || $3 !~ /^checks=[1-9][0-9]*$/)
            reject("invalid final completion")
        split($2, c, "="); split($3, a, "=")
        if (!completed["checked"] || !completed["ownership"] || !completed["reset"])
            reject("missing group completion")
        if (total_cases < 6 || c[2] != total_cases || a[2] != total_checks)
            reject("final totals differ or fewer than six cases")
        complete = 1
    }
    else if ($1 ~ /^(CASE|GROUP|TARGET_COMPLETE)/) reject("malformed protocol record")
}
END {
    if (!failed && !complete) {
        print "TARGET_TRANSCRIPT_REJECTED: missing final completion" > "/dev/stderr"
        exit 1
    }
    if (failed) exit 1
}
