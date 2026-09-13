#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""Source-only regression checks; does not compile or exercise the driver."""

import re
from pathlib import Path


def check(source):
    def body(name):
        match = re.search(r"\b" + name + r"\([^;]*?\n\{\n(.*?)\n\}", source, re.S)
        assert match, name
        return match[1]

    def array(name):
        match = re.search(r"\b" + name + r"\[16\] = \{([^}]+)\}", source)
        assert match, name
        data = bytearray(16)
        index = 0
        for entry in match[1].split(","):
            entry = entry.strip()
            if "=" in entry:
                target, entry = entry.split("=")
                index = int(target.strip()[1:-1], 0)
            data[index] = int(entry, 0)
            index += 1
        return data

    # Stock hr_show: CDB at sp+8, length byte at sp+0x17.
    assert array("read_sdr") == bytes.fromhex("c0 40 00 00 01 0c 00 00 00 00 00 00 00 00 00 1c")
    assert array("read_osv") == bytes.fromhex("c0 40 00 00 01 0a 00 00 00 00 00 00 00 00 00 4c")

    vendor = body("read_vendor_hr")
    for field, older, newer in ((1, 3, 15), (2, 88, 83), (11, 82, 110)):
        match = re.search(
            rf"cdb\[{field}\] = hba->ufs_version < 0x400 \? (\w+) : (\w+);", vendor)
        assert match, f"SKhynix field {field} must use controller version"
        assert tuple(int(value, 0) for value in match.groups()) == (older, newer)
    assert "wspecversion" not in vendor

    # Flags 4 in stock's __pm_runtime_resume/__pm_runtime_idle mean sync get/put.
    for name in ("ufs_read_desc_param", "ufs_get_string_desc", "read_descriptor", "read_vendor_hr"):
        calls = re.findall(r"pm_runtime_(get_sync|put_sync)\(hba->dev\)", body(name))
        expected = ["get_sync", "put_sync"]
        if name == "ufs_get_string_desc":
            expected = ["get_sync", "get_sync", "put_sync", "put_sync"]
        assert calls == expected, (name, calls)
    assert "pm_runtime_resume_and_get" not in source
    assert "pm_runtime_put(" not in source
    descriptor = body("ufs_read_desc_param")
    assert re.search(r"if \(ret\)\s+return -EINVAL;", descriptor)

    assert '{ 0x06, "SKHYNIX" }' in source
    assert 'DRAM_SHOW(ddr_size, "%d", ddr_size)' in source
    assert '?: ""' not in body("mv_proc_show")
    for text in (
        "get sdr result error", "get hr_inquiry result error",
        "hr write buffer  error", "hr read buffer  error",
        "get skhynix result error", "scsi_ss_set_pwd error", "set pwd fail",
        "scsi_ss_enter_vendor_mode error", "enter vendor mode fail",
        "get osv result error", "ger hr fail fail", "exit vendor mode fail",
    ):
        assert text in source, text

    # No embedded NUL: serial: is seven bytes, hex overwrites byte seven.
    serial = body("dump_string_desc_serial_show")
    assert 'sysfs_emit_at(buf, len, "serial:")' in serial
    stock = bytearray(0x003a6c6169726573.to_bytes(8, "little"))
    stock[7:] = b"0041\n\0"
    assert stock[:-1] == b"serial:0041\n"
    for text, size in (("dram_info is NULL\n", 18), ("get desc info fail\n", 19),
                       ("get health info fail\n", 21)):
        assert len(text) == size

    # Stock's U+00E9 example fills its allocation, leaving no terminator byte.
    payload = "é".encode("utf-16-be")
    stock_capacity = len(payload) // 2 + 1
    assert len(payload.decode("utf-16-be").encode("utf-8")) == stock_capacity


if __name__ == "__main__":
    check(Path(__file__).with_name("mi_memory.c").read_text())
    print("PASS: mi_memory source contract (no compile/runtime validation)")
