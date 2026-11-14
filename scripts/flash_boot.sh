#!/usr/bin/env bash
# Flash a boot image to an SM-A032F over SSH.
#
# This script writes ONLY the boot partition. It is intentionally conservative:
# it copies the image, verifies the SHA-256 on the phone, asks for confirmation,
# and uses conv=fsync + sync before rebooting.
#
# Example:
#   ./flash_boot.sh --image boot-oc.img --phone 192.0.2.10 --port 22 \
#       --user termux --key ~/.ssh/id_ed25519
#
# Never commit real phone addresses, ports, users, or keys to this repository.

set -euo pipefail

IMAGE=""
PHONE=""
PORT="22"
USER=""
KEY=""
REMOTE_DIR="/sdcard"
PARTITION="/dev/block/by-name/boot"
REBOOT=1
ASSUME_YES=0
DRY_RUN=0

usage() {
  cat <<'EOF'
usage: flash_boot.sh --image FILE --phone HOST --user USER [options]

required:
  --image FILE        boot image to write
  --phone HOST        phone IP or hostname
  --user USER         SSH/Termux user

options:
  --port N            SSH port (default: 22)
  --key FILE          SSH private key
  --remote-dir DIR    remote staging directory (default: /sdcard)
  --partition PATH    boot partition (default: /dev/block/by-name/boot)
  --no-reboot         do not reboot after writing
  --yes               skip the confirmation prompt
  --dry-run           print the commands but do not run them
  -h, --help          show this help
EOF
}

die() { echo "error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
  case "$1" in
    --image) IMAGE="${2:-}"; shift 2 ;;
    --phone) PHONE="${2:-}"; shift 2 ;;
    --port) PORT="${2:-}"; shift 2 ;;
    --user) USER="${2:-}"; shift 2 ;;
    --key) KEY="${2:-}"; shift 2 ;;
    --remote-dir) REMOTE_DIR="${2:-}"; shift 2 ;;
    --partition) PARTITION="${2:-}"; shift 2 ;;
    --no-reboot) REBOOT=0; shift ;;
    --yes) ASSUME_YES=1; shift ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) die "unknown argument: $1" ;;
  esac
done

[ -n "$IMAGE" ] || die "--image is required"
[ -n "$PHONE" ] || die "--phone is required"
[ -n "$USER" ] || die "--user is required"
[ -f "$IMAGE" ] || die "image not found: $IMAGE"

# Keep remote paths simple so the shell quoting below is unambiguous.
for value in "$REMOTE_DIR" "$PARTITION"; do
  case "$value" in
    *[!A-Za-z0-9_./-]*) die "unsafe remote path: $value" ;;
  esac
done

SSH_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -p "$PORT")
SCP_OPTS=(-o BatchMode=yes -o ConnectTimeout=10 -P "$PORT")
if [ -n "$KEY" ]; then
  SSH_OPTS+=(-i "$KEY")
  SCP_OPTS+=(-i "$KEY")
fi

REMOTE_IMAGE="${REMOTE_DIR}/$(basename "$IMAGE")"
HOST_SHA="$(sha256sum "$IMAGE" | awk '{print $1}')"

echo "image      : $IMAGE"
echo "sha256     : $HOST_SHA"
echo "target     : $USER@$PHONE:$PORT"
echo "remote     : $REMOTE_IMAGE"
echo "partition  : $PARTITION"
echo

if [ "$ASSUME_YES" -ne 1 ]; then
  echo "This writes the boot partition. A bad image can prevent the phone from booting."
  printf "Type 'FLASH' to continue: "
  read -r answer
  [ "$answer" = "FLASH" ] || die "aborted"
fi

run() {
  if [ "$DRY_RUN" -eq 1 ]; then
    printf 'dry-run:'
    printf ' %q' "$@"
    printf '\n'
  else
    "$@"
  fi
}

run scp "${SCP_OPTS[@]}" "$IMAGE" "$USER@$PHONE:$REMOTE_IMAGE"

if [ "$DRY_RUN" -eq 0 ]; then
  REMOTE_SHA="$(ssh "${SSH_OPTS[@]}" "$USER@$PHONE" "sha256sum '$REMOTE_IMAGE'" | awk '{print $1}')"
  echo "remote sha : $REMOTE_SHA"
  [ "$REMOTE_SHA" = "$HOST_SHA" ] || die "SHA-256 mismatch on the phone"
fi

WRITE_CMD="su -c 'dd if=$REMOTE_IMAGE of=$PARTITION bs=1M conv=fsync; sync"
if [ "$REBOOT" -eq 1 ]; then
  WRITE_CMD="$WRITE_CMD; reboot'"
else
  WRITE_CMD="$WRITE_CMD'"
fi

run ssh "${SSH_OPTS[@]}" "$USER@$PHONE" "$WRITE_CMD"
echo "done."
