#!/bin/sh
# phase4c_symlink_guard.sh — the overlay symlink-traversal guard.
#
# A .feather may carry symlinks. A RELATIVE target that climbs out of the
# install root via "../.." is a path-traversal vector (matters once untrusted /
# remote feathers are installable), so install must REFUSE it. Legitimate links
# — absolute targets (resolve within the root at runtime) and intra-tree
# relative ones — must still install. Guards install.c:symlink_target_escapes.
#
# Run from the repo root: ./tests/phase4c_symlink_guard.sh

set -eu

FTR="${FTR:-./ftr}"
[ -x "$FTR" ] || { echo "phase4c: error: $FTR not found (run 'make build')" >&2; exit 2; }
command -v tar >/dev/null 2>&1 || { echo "phase4c: error: tar(1) not in PATH" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail() { echo "phase4c: FAIL: $*" >&2; exit 1; }

# ----------------------------------------------------------------
# negative: an escaping relative symlink must be REFUSED
# ----------------------------------------------------------------
echo "phase4c: --- escaping symlink is refused ---"
BAD="$WORK/stage-bad"
mkdir -p "$BAD/files/usr/bin"
ln -s ../../../../etc/cron.d/x "$BAD/files/usr/bin/evil"
cat >"$BAD/manifest.toml" <<'EOF'
[package]
name = "escaping-stub"
version = "0.1.0"
description = "symlink that climbs out of the install root"
[install]
layout = "system"
EOF
ARCHIVE_BAD="$WORK/escaping-stub.feather"
( cd "$BAD" && tar -czf "$ARCHIVE_BAD" manifest.toml files )

ROOT_BAD="$WORK/root-bad"; mkdir -p "$ROOT_BAD"
set +e
bad_out=$(FTR_DB_ROOT="$ROOT_BAD/var/lib/feather" \
	"$FTR" install --root "$ROOT_BAD" --allow-unsigned "$ARCHIVE_BAD" 2>&1)
bad_rc=$?
set -e
[ "$bad_rc" -ne 0 ] || fail "escaping symlink install should have failed, got rc=0: $bad_out"
case "$bad_out" in
	*"escapes install root"*) ;;
	*) fail "expected 'escapes install root' rejection, got: $bad_out" ;;
esac
[ ! -e "$ROOT_BAD/usr/bin/evil" ] || fail "escaping symlink was created at $ROOT_BAD/usr/bin/evil"

# ----------------------------------------------------------------
# positive: legit links (absolute + intra-tree relative) install
# ----------------------------------------------------------------
echo "phase4c: --- legit symlinks install ---"
OK="$WORK/stage-ok"
mkdir -p "$OK/files/usr/bin"
ln -s /bin/busybox "$OK/files/usr/bin/sh"   # absolute → resolves within root at runtime
ln -s sh "$OK/files/usr/bin/ash"            # intra-tree relative
cat >"$OK/manifest.toml" <<'EOF'
[package]
name = "links-stub"
version = "0.1.0"
description = "legitimate absolute + relative symlinks"
[install]
layout = "system"
EOF
ARCHIVE_OK="$WORK/links-stub.feather"
( cd "$OK" && tar -czf "$ARCHIVE_OK" manifest.toml files )

ROOT_OK="$WORK/root-ok"; mkdir -p "$ROOT_OK"
FTR_DB_ROOT="$ROOT_OK/var/lib/feather" \
	"$FTR" install --root "$ROOT_OK" --allow-unsigned "$ARCHIVE_OK" >/dev/null 2>&1 \
	|| fail "legit-symlink install failed"
[ "$(readlink "$ROOT_OK/usr/bin/sh")" = "/bin/busybox" ] || fail "absolute symlink not installed"
[ "$(readlink "$ROOT_OK/usr/bin/ash")" = "sh" ] || fail "relative symlink not installed"

echo "phase4c_symlink_guard.sh: PASS"
