#!/usr/bin/env bash
# Rollback helper: write a known-good boot image back to the boot partition.
#
# This is a thin wrapper around flash_boot.sh with a louder warning. The
# mechanics are identical: copy, verify SHA-256, write with conv=fsync, sync.
#
# Example:
#   ./rollback_boot.sh --image boot-stock.img --phone 192.0.2.10 \
#       --port 22 --user termux --key ~/.ssh/id_ed25519
#
# Keep at least one rollback image on the phone and one on the host.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

cat <<'EOF'
============================================================
 ROLLBACK: this writes a boot image to the boot partition.
 Make sure the image is the stock or last known-good build.
============================================================
EOF

exec "$SCRIPT_DIR/flash_boot.sh" "$@"
