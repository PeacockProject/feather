#!/bin/sh
# phase10_keyring.sh — pacman-key-style trust keyring (ftr key).
#
# Builds a signed file:// repo whose feather.conf pins NO pubkey, so the
# only possible trust source is the keyring. Exercises:
#
#   ftr key fingerprint   prints a pubkey file's id (no keyring change)
#   ftr key add / list     import + trust; appears in the listing
#   ftr sync   (no pin)    verifies the index via the keyring key
#   ftr install (no pin)   verifies the package via the keyring key
#   ftr key remove         revokes; a fresh sync then FAILS (no trusted signer)
#   re-add                 trust restored; sync succeeds again
#
# Run from the repo root: ./tests/phase10_keyring.sh
set -eu

FTR="${FTR:-./ftr}"
GEN_KEYPAIR="${GEN_KEYPAIR:-./tools/gen-keypair}"
FTR_SIGN="${FTR_SIGN:-./tools/ftr-sign}"

for t in "$FTR" "$GEN_KEYPAIR" "$FTR_SIGN"; do
	[ -x "$t" ] || { echo "phase10: error: $t missing (make build + tools)" >&2; exit 2; }
done
command -v sha256sum >/dev/null 2>&1 || { echo "phase10: need sha256sum" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

REPO="$WORK/repo"; DB="$WORK/db"; KEYS="$WORK/keys"; CONF="$WORK/feather.conf"
PREFIX="$WORK/peacock"
mkdir -p "$REPO" "$DB" "$KEYS" "$PREFIX"

export FTR_DB_ROOT="$DB"
export FTR_CONFIG="$CONF"
# Crucial: do NOT set FTR_PUBKEY — the keyring must be the trust source.
unset FTR_PUBKEY 2>/dev/null || true

fail() { echo "phase10: FAIL: $*" >&2; exit 1; }

# --- keypair (signs the repo; will be the trusted keyring key) ---
"$GEN_KEYPAIR" "phase10-keyring-seed" "$KEYS/repo.pub" "$KEYS/repo.sec" \
	"phase10 keyring test key" >"$KEYS/key_id.txt"
KEY_ID=$(cat "$KEYS/key_id.txt")
echo "phase10: keypair $KEY_ID"

sign() { "$FTR_SIGN" "$KEYS/repo.sec" "$1" "$1.sig" "phase10: $(basename "$1")"; }

# --- one signed package + signed index ---
P="$WORK/stage"; mkdir -p "$P/files/bin"
cat >"$P/manifest.toml" <<'EOF'
[package]
name = "keyring-pkg"
version = "0.1.0"
description = "package trusted purely via the keyring"

[install]
layout = "peacock"
EOF
echo '#!/bin/sh' >"$P/files/bin/keyring-pkg"; chmod +x "$P/files/bin/keyring-pkg"
( cd "$P" && tar -czf "$REPO/keyring-pkg-0.1.0.feather" manifest.toml files )
sign "$REPO/keyring-pkg-0.1.0.feather"

SHA=$(sha256sum "$REPO/keyring-pkg-0.1.0.feather" | awk '{print $1}')
SIZE=$(stat -c '%s' "$REPO/keyring-pkg-0.1.0.feather")
{
	printf '[repo]\nname = "peacock-stable"\n\n'
	printf '[[package]]\nname = "keyring-pkg"\nversion = "0.1.0"\n'
	printf 'layout = "peacock"\narchive = "keyring-pkg-0.1.0.feather"\n'
	printf 'sha256 = "%s"\nsize = %s\n' "$SHA" "$SIZE"
} >"$REPO/index.toml"
sign "$REPO/index.toml"

# feather.conf with NO pubkey field — trust can only come from the keyring.
cat >"$CONF" <<EOF
[[repos]]
name = "peacock-stable"
url  = "file://$REPO"
EOF

# --- ftr key fingerprint (no keyring change) ---
echo "phase10: --- ftr key fingerprint ---"
out=$("$FTR" key fingerprint "$KEYS/repo.pub" 2>&1) || fail "key fingerprint: $out"
case "$out" in *"$KEY_ID"*) ;; *) fail "fingerprint != $KEY_ID: $out" ;; esac

# --- before trust: sync must FAIL (empty keyring, built-in default won't match) ---
echo "phase10: --- sync before trust (must fail) ---"
set +e; out=$("$FTR" sync 2>&1); rc=$?; set -e
[ "$rc" -eq 0 ] && fail "sync should fail with empty keyring + no pin: $out"
echo "phase10: (correctly refused) "

# --- add to keyring + list ---
echo "phase10: --- ftr key add + list ---"
out=$("$FTR" key add "$KEYS/repo.pub" 2>&1) || fail "key add: $out"
case "$out" in *"trusted: $KEY_ID"*) ;; *) fail "key add line: $out" ;; esac
out=$("$FTR" key list 2>&1) || fail "key list: $out"
case "$out" in *"$KEY_ID"*) ;; *) fail "key list missing $KEY_ID: $out" ;; esac

# --- sync now verifies via the keyring ---
echo "phase10: --- ftr sync (trust via keyring) ---"
out=$("$FTR" sync 2>&1) || fail "sync after trust: $out"
case "$out" in
	*"synced: peacock-stable (verified by $KEY_ID)"*) ;;
	*) fail "expected verify-by-$KEY_ID, got: $out" ;;
esac

# --- install also verifies via the keyring ---
echo "phase10: --- ftr install (trust via keyring) ---"
out=$("$FTR" install --peacock-prefix "$PREFIX" keyring-pkg 2>&1) || fail "install: $out"
case "$out" in
	*"installed: keyring-pkg-0.1.0 (verified by $KEY_ID)"*) ;;
	*) fail "expected verified install, got: $out" ;;
esac
[ -x "$PREFIX/bin/keyring-pkg" ] || fail "package not installed"

# --- revoke: a fresh sync must FAIL ---
echo "phase10: --- ftr key remove (revoke) → sync fails ---"
"$FTR" key remove "$KEY_ID" >/dev/null || fail "key remove"
rm -rf "$DB/sync"
set +e; out=$("$FTR" sync 2>&1); rc=$?; set -e
[ "$rc" -eq 0 ] && fail "sync must fail after revocation: $out"
echo "phase10: (revocation collapsed trust)"

# --- re-add: trust restored ---
echo "phase10: --- re-add → sync succeeds again ---"
"$FTR" key add "$KEYS/repo.pub" >/dev/null || fail "re-add"
out=$("$FTR" sync 2>&1) || fail "sync after re-add: $out"
case "$out" in *"verified by $KEY_ID"*) ;; *) fail "re-add verify: $out" ;; esac

echo "phase10_keyring.sh: PASS"
