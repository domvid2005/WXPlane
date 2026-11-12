#!/usr/bin/env python3
"""Builds WXPlane's SkunkCrafts Updater depot from a finished plugin folder (the CI "WXPlane" bundle: WXPlane/ with lin_x64,
mac_x64, win_x64 and skunkcrafts_updater.cfg).

  skunkcrafts_depot.py <bundle>/WXPlane <depot>/WXPlane [--version 5.0]

The depot is what the updater app reads at the cfg's "module" address:
  skunkcrafts_updater.cfg            the same key|value file as in the download (the newest version)
  skunkcrafts_updater_whitelist.txt  every file the plugin consists of: path|CRC32 (unsigned, decimal)
  skunkcrafts_updater_sizeslist.txt  path|size in bytes
  skunkcrafts_updater_blacklist.txt  files the updater deletes from users' installs (tools/skunkcrafts_blacklist.txt)
  ...and a copy of every file, at its path.
The updater compares each user's files with the whitelist and downloads only what differs. The user's settings (WXPlane.cfg) are
in X-Plane's Output/preferences folder, not in the plugin folder, so an update never touches them."""
import argparse, binascii, os, shutil, sys

CFG = "skunkcrafts_updater.cfg"
CONTROL = {CFG, "skunkcrafts_updater_whitelist.txt", "skunkcrafts_updater_sizeslist.txt", "skunkcrafts_updater_blacklist.txt",
           "skunkcrafts_updater_ignorelist.txt", "skunkcrafts_updater_oncelist.txt", ".DS_Store", "Thumbs.db"}

def crc32(path):
    c = 0
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            c = binascii.crc32(chunk, c)
    return c & 0xFFFFFFFF

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src"); ap.add_argument("dst"); ap.add_argument("--version")
    a = ap.parse_args()
    src, dst = os.path.abspath(a.src), os.path.abspath(a.dst)
    cfgp = os.path.join(src, CFG)
    if not os.path.isfile(cfgp): sys.exit(f"no {CFG} in {src}")
    cfg = {}
    for line in open(cfgp, encoding="utf-8"):
        line = line.strip()
        if line and not line.startswith("#") and "|" in line:
            k, v = line.split("|", 1); cfg[k.strip()] = v.strip()
    if a.version and cfg.get("version") != a.version:
        sys.exit(f"the bundle is version {cfg.get('version')}, not {a.version} (src/version.h)")
    for p in ("lin_x64/WXPlane.xpl", "mac_x64/WXPlane.xpl", "win_x64/WXPlane.xpl"):
        if not os.path.isfile(os.path.join(src, p)): sys.exit(f"missing {p}: the bundle must hold all three platforms")
    if os.path.exists(dst): shutil.rmtree(dst)
    os.makedirs(dst)
    white, sizes = [], []
    for root, dirs, files in os.walk(src):
        dirs.sort()
        for n in sorted(files):
            full = os.path.join(root, n)
            rel = os.path.relpath(full, src).replace("\\", "/")
            if n in CONTROL: continue
            white.append(f"{rel}|{crc32(full)}"); sizes.append(f"{rel}|{os.path.getsize(full)}")
            os.makedirs(os.path.dirname(os.path.join(dst, rel)), exist_ok=True)
            shutil.copy2(full, os.path.join(dst, rel))
    black = []
    bl = os.path.join(os.path.dirname(os.path.abspath(__file__)), "skunkcrafts_blacklist.txt")
    if os.path.isfile(bl):
        black = [l.strip() for l in open(bl, encoding="utf-8") if l.strip() and not l.startswith("#")]
    w = lambda name, lines: open(os.path.join(dst, name), "w", newline="\n", encoding="utf-8").write("".join(l + "\n" for l in lines))
    w("skunkcrafts_updater_whitelist.txt", white)
    w("skunkcrafts_updater_sizeslist.txt", sizes)
    w("skunkcrafts_updater_blacklist.txt", black)
    w(CFG, [f"{k}|{v}" for k, v in cfg.items()])
    print(f"depot {dst}: version {cfg.get('version')}, {len(white)} files, {sum(int(s.rsplit('|', 1)[1]) for s in sizes) / 1e6:.1f} MB")

if __name__ == "__main__":
    main()
