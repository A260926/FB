# Validate the fixed driver's transcript outside the target process.
function fail(message) {
    print "TARGET_TRANSCRIPT_REJECTED line=" NR ": " message > "/dev/stderr"
    bad = 1
    exit 1
}
function positive(value) { return value ~ /^[1-9][0-9]*$/ }
function field_value(value, key, parts) {
    if (split(value, parts, "=") != 2 || parts[1] != key || !positive(parts[2]))
        fail("invalid " key)
    return parts[2] + 0
}
BEGIN { wanted["assembly"] = wanted["emission"] = wanted["lifetime"] = 1 }
{
    if (finished && NF) fail("output after final completion")
    if ($1 == "CASE") {
        if (phase || NF != 4 || !($2 in wanted) || $3 !~ /^[A-Za-z0-9_.:-]+$/ || !positive($4))
            fail("invalid or late CASE")
        if (seen[$3]++) fail("duplicate case name")
        ++cases[$2]; checks[$2] += $4; ++total_cases; total_checks += $4
    } else if ($1 == "GROUP_COMPLETE") {
        phase = 1
        if (NF != 4 || !($2 in wanted) || completed[$2]++) fail("invalid or duplicate group completion")
        if (!cases[$2] || field_value($3, "cases") != cases[$2] || field_value($4, "checks") != checks[$2])
            fail("group counts disagree with CASE records")
        ++groups
    } else if ($1 == "TARGET_COMPLETE") {
        if (NF != 3 || groups != 3 || total_cases < 6 || !total_checks)
            fail("incomplete cases or groups")
        if (field_value($2, "cases") != total_cases || field_value($3, "checks") != total_checks)
            fail("final counts disagree with CASE records")
        finished = 1
    }
}
END {
    if (bad) exit 1
    if (!finished) {
        print "TARGET_TRANSCRIPT_REJECTED: missing final completion" > "/dev/stderr"
        exit 1
    }
    print "TARGET_TRANSCRIPT_OK cases=" total_cases " checks=" total_checks " groups=" groups
}
