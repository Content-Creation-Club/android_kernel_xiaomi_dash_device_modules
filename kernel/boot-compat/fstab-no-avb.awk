# First-stage AVB policy stays in ramdisk, without a kernel PID1-orange view.
/^[[:space:]]*#/ || NF == 0 { print; next }
{
    count = split($5, flags, ",")
    kept = ""
    for (i = 1; i <= count; i++) {
        if (flags[i] ~ /^avb(=|$)|^avb_keys=|^avb_hashtree_digest=/)
            continue
        kept = kept (kept == "" ? "" : ",") flags[i]
    }
    $5 = kept
    print
}
