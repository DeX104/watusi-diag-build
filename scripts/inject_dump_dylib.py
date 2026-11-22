#!/usr/bin/env python3
"""
Inject WatusiDump.dylib into a Watusi IPA for theme diagnostics.

Usage:
    python3 inject_dump_dylib.py \\
        --dump-dylib  WatusiDump.dylib \\
        --src-ipa     watusi_original_duplicate.ipa \\
        --out-ipa     Watusi_diag.ipa

Strategy for adding LC_LOAD_WEAK_DYLIB to Watusi.dylib:
  Watusi.dylib has 3 non-essential 16-byte LCs at the very end:
    LC_VERSION_MIN_IPHONEOS  - redundant (LC_BUILD_VERSION present)
    LC_DYLD_ENVIRONMENT      - empty/corrupt env-var string
    LC_SOURCE_VERSION        - debug metadata only
  Total freed: 48 bytes = exactly one LC_LOAD_WEAK_DYLIB + 24-byte name.
  No binary content is shifted; sizeofcmds stays the same.
"""
import sys, struct, zipfile, plistlib, os, argparse

INSTALL_NAME       = b"@rpath/WatusiDump.dylib\x00"  # 24 bytes
LC_LOAD_WEAK_DYLIB = 0x80000018
DUMP_ENTRY         = "Payload/WhatsApp.app/Frameworks/WatusiDump.dylib"
WATUSI_ENTRY       = "Payload/WhatsApp.app/Watusi.dylib"
PLIST_ENTRY        = "Payload/WhatsApp.app/Info.plist"

def _u32(d, o): return struct.unpack_from("<I", d, o)[0]


def build_lc_load_weak_dylib(install_name: bytes, cmdsize: int) -> bytes:
    lc  = struct.pack("<II", LC_LOAD_WEAK_DYLIB, cmdsize)
    lc += struct.pack("<IIII", 24, 0, 0x10000, 0x10000)
    lc += install_name
    if len(lc) < cmdsize:
        lc += b'\x00' * (cmdsize - len(lc))
    return lc[:cmdsize]


def patch_watusi_add_weak_load(data: bytes) -> bytes:
    ncmds      = _u32(data, 16)
    sizeofcmds = _u32(data, 20)
    hdr_size   = 32

    off = hdr_size
    lc_list = []
    for _ in range(ncmds):
        if off + 8 > len(data): break
        cmd = _u32(data, off)
        sz  = _u32(data, off + 4)
        lc_list.append((off, cmd, sz))
        off += sz
        if sz == 0: break

    SAFE_TO_REMOVE = {0x00000026, 0x00000029, 0x0000001d}
    tail3 = lc_list[-3:]
    for lc_off, cmd, sz in tail3:
        if cmd not in SAFE_TO_REMOVE:
            raise RuntimeError(f"Unexpected tail LC 0x{cmd:08x} at 0x{lc_off:x}")
        if sz != 16:
            raise RuntimeError(f"Tail LC 0x{cmd:08x} at 0x{lc_off:x}: size={sz} ≠ 16")

    NEW_SIZE  = 48
    start_off = tail3[0][0]
    new_lc    = build_lc_load_weak_dylib(INSTALL_NAME, NEW_SIZE)
    assert len(new_lc) == NEW_SIZE

    data = bytearray(data)
    data[start_off : start_off + NEW_SIZE] = new_lc
    struct.pack_into("<I", data, 16, ncmds - 2)
    print(f"    Tail LCs replaced at 0x{start_off:04x}  ncmds: {ncmds} -> {ncmds-2}  sizeofcmds 0x{sizeofcmds:x} unchanged")
    return bytes(data)


def patch_plist(data: bytes) -> bytes:
    pl = plistlib.loads(data)
    pl["UIFileSharingEnabled"] = True
    pl["LSSupportsOpeningDocumentsInPlace"] = True
    return plistlib.dumps(pl, fmt=plistlib.FMT_XML)


def find_entry(names, target):
    for n in names:
        if n.replace("\\", "/").lower() == target.lower():
            return n
    return None


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dump-dylib", required=True)
    p.add_argument("--src-ipa",    required=True)
    p.add_argument("--out-ipa",    required=True)
    args = p.parse_args()

    print(f"[*] Loading {os.path.basename(args.dump_dylib)} ...")
    with open(args.dump_dylib, "rb") as f:
        dump_bytes = f.read()
    print(f"    {len(dump_bytes):,} bytes")

    with zipfile.ZipFile(args.src_ipa, "r") as zin:
        names = zin.namelist()

    watusi_name = find_entry(names, WATUSI_ENTRY)
    plist_name  = find_entry(names, PLIST_ENTRY)
    if not watusi_name: sys.exit(f"[!] Not found: {WATUSI_ENTRY}")
    if not plist_name:  sys.exit(f"[!] Not found: {PLIST_ENTRY}")
    print(f"    Watusi: {watusi_name}  Plist: {plist_name}")

    with zipfile.ZipFile(args.src_ipa, "r") as zin:
        watusi_orig  = zin.read(watusi_name)
        plist_orig   = zin.read(plist_name)
        watusi_info  = zin.getinfo(watusi_name)
        plist_info   = zin.getinfo(plist_name)

    print(f"[*] Patching Watusi.dylib ({len(watusi_orig):,} bytes) ...")
    watusi_patched = patch_watusi_add_weak_load(watusi_orig)

    print(f"[*] Patching Info.plist ...")
    plist_patched = patch_plist(plist_orig)

    print(f"[*] Repacking -> {os.path.basename(args.out_ipa)} ...")
    tmp      = args.out_ipa + ".tmp"
    skip_low = {watusi_name.lower(), plist_name.lower()}

    with zipfile.ZipFile(args.src_ipa, "r") as zin, \
         zipfile.ZipFile(tmp, "w", compression=zipfile.ZIP_DEFLATED, allowZip64=True) as zout:

        info_d = zipfile.ZipInfo(DUMP_ENTRY)
        info_d.compress_type = zipfile.ZIP_STORED
        info_d.external_attr = (0o100755 << 16)
        zout.writestr(info_d, dump_bytes)
        print(f"    [+] Added  {DUMP_ENTRY}")

        info_w = zipfile.ZipInfo(watusi_name)
        info_w.compress_type = zipfile.ZIP_STORED
        info_w.date_time     = watusi_info.date_time
        info_w.external_attr = watusi_info.external_attr
        zout.writestr(info_w, watusi_patched)
        print(f"    [+] Patched Watusi.dylib")

        info_p = zipfile.ZipInfo(plist_name)
        info_p.compress_type = zipfile.ZIP_DEFLATED
        info_p.date_time     = plist_info.date_time
        info_p.external_attr = plist_info.external_attr
        zout.writestr(info_p, plist_patched)
        print(f"    [+] Patched Info.plist")

        for item in zin.infolist():
            if item.filename.lower() in skip_low:
                continue
            zout.writestr(item, zin.read(item.filename))

    if os.path.exists(args.out_ipa):
        os.unlink(args.out_ipa)
    os.rename(tmp, args.out_ipa)
    size = os.path.getsize(args.out_ipa)
    print(f"\n[+] {args.out_ipa}  ({size/1024/1024:.1f} MB)")


if __name__ == "__main__":
    main()
