# SPDX-License-Identifier: GPL-2.0
# Build-time provider metadata for consumers with an audited stock ABI view.
# The native Module.symvers and built ELF files are never modified.
BEGIN { FS = "\t" }
NR == FNR {
    crc[$2] = $1
    provider[$2] = $3
    export_type[$2] = $4
    next
}
$2 in crc {
    n = split($3, parts, "/")
    if (parts[n] != provider[$2] || $4 != export_type[$2] || $5 != "" || seen[$2]++) {
        print "stock305 ABI: unexpected provider metadata for " $2 > "/dev/stderr"
        failed = 1
    }
    sub(/^[^\t]+/, crc[$2])
}
{ print }
END {
    for (symbol in crc) {
        if (!seen[symbol]) {
            print "stock305 ABI: missing provider symbol " symbol > "/dev/stderr"
            failed = 1
        }
    }
    exit failed
}
