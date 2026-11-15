#!/usr/bin/env bash
# Copies a PS5 title folder (make ps5-classic / ps5-modern) to a jailbroken PS5 over FTP and starts it
# (docs/PS5_HOMEBREW.md). The same steps as UnleashedRecomp-Prospero's `make deploy`.
#   tools/ps5/deploy.sh <PS5 IP> classic|modern [rom.z64]
# A ROM given here goes to /data/blitzrecomp/, where the app imports it at its first start.
# FTP_PORT: etaHEN's FTP server is on 1337 (ftpsrv's on 2121). LOADER_PORT: etaHEN's ELF loader, 9021.
# REGISTER=1 first adds the title to the home screen (tools/ps5/register.c; experimental), for a console where the
# launch alone doesn't find it.
set -euo pipefail

root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
host=${1:?usage: tools/ps5/deploy.sh <PS5 IP> classic|modern [rom]}
case ${2:-} in
    classic) title=PPSA99960; dir=$root/build/ps5/app_classic/$title ;;
    modern) title=PPSA99961; dir=$root/build/ps5/app_modern/$title ;;
    *) echo "usage: tools/ps5/deploy.sh <PS5 IP> classic|modern [rom]" >&2; exit 2 ;;
esac
rom=${3:-}
ftp_port=${FTP_PORT:-1337}
loader_port=${LOADER_PORT:-9021}
[[ -f $dir/eboot.bin ]] || { echo "no $dir/eboot.bin: run make ps5-${2}" >&2; exit 2; }

# One FTP session (Python's ftplib): each file goes up under a temporary name and is renamed into place, so a deploy
# cut short never leaves a half-written eboot.bin. The ROM gets a plain name (blitz.<ext>): FTP and URL tools trip
# over GoodN64-style names with spaces and brackets. Files already there at the same size are skipped (eboot.bin is
# always sent), so an interrupted deploy resumes. One connection at a time: etaHEN's FTP drops a second.
echo "==> $title to ftp://$host:$ftp_port/data/homebrew/$title/"
python3 -I - "$host" "$ftp_port" "$dir" "/data/homebrew/$title" "$rom" <<'PY'
import ftplib, os, sys

host, port, local, remote, rom = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4], sys.argv[5]
ftp = ftplib.FTP()
ftp.connect(host, port, timeout=300)  # the console's FTP server can pause for a while mid-file
ftp.login()

def mkdirs(path):
    here = ""
    for part in path.strip("/").split("/"):
        here += "/" + part
        try:
            ftp.mkd(here)
        except ftplib.error_perm:
            pass

def put(src, dst):
    size = os.path.getsize(src)
    try:  # already there at this size (a rerun resumes)
        if ftp.size(dst) == size and not dst.endswith("eboot.bin"):
            return False
    except ftplib.error_perm:
        pass
    if size > 4 * 1024 * 1024:
        print(f"    {dst} ({size >> 20} MB)", flush=True)
    with open(src, "rb") as f:
        ftp.storbinary(f"STOR {dst}.part", f, blocksize=65536)
    try:
        ftp.delete(dst)
    except ftplib.error_perm:
        pass
    ftp.rename(f"{dst}.part", dst)
    return True

count = 0
for folder, _, files in os.walk(local):
    rel = os.path.relpath(folder, local)
    target = remote if rel == "." else f"{remote}/{rel}"
    mkdirs(target)
    for name in files:
        count += 1 if put(os.path.join(folder, name), f"{target}/{name}") else 0
print(f"    {count} files sent (the rest were already there)")
if rom:
    mkdirs("/data/blitzrecomp")
    ext = os.path.splitext(rom)[1].lower()
    put(rom, f"/data/blitzrecomp/blitz{ext}")
    print(f"==> ROM to /data/blitzrecomp/blitz{ext}")
ftp.quit()
PY

sdk_cc() { LLVM_CONFIG=${LLVM_CONFIG:-/opt/homebrew/opt/llvm/bin/llvm-config} "$root/build/ps5/sdk-m/bin/prospero-clang" "$@"; }
if [[ ${REGISTER:-0} == 1 ]]; then
    reg=$root/build/ps5/host/register-$title.elf
    sdk_cc -DBLITZ_TITLE_ID="\"$title\"" -lSceAppInstUtil -lSceIpmi -lSceUserService -lSceSystemService -o "$reg" \
        "$root/tools/ps5/register.c"
    echo "==> registering $title (port $loader_port)"
    nc -w 3 "$host" "$loader_port" < "$reg"
    sleep 3
fi

# The launch payload: asks the system to start the title (UnleashedRecomp-Prospero's tools/controllers/launch.c).
payload=$root/build/ps5/host/launch-$title.elf
if [[ ! -f $payload ]]; then
    sdk_cc \
        -DBOOTSTRAP_TITLE_ID="\"$title\"" -lSceSystemService -lSceUserService -o "$payload" \
        "$root/build/ps5/deps/unleashed-prospero/tools/controllers/launch.c"
fi
echo "==> launching $title (port $loader_port)"
nc -w 3 "$host" "$loader_port" < "$payload" || echo "Couldn't reach the ELF loader: start $title from the home screen."
