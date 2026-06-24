#!/bin/sh
# phase8_gpg.sh — GPG-verified repo index.
#
# A repo may anchor index trust in a detached GPG signature
# (index.toml.asc) instead of minisign. Packages stay minisign-signed;
# the index is GPG-signed. Checks:
#
#   ftr sync   → verifies index.toml.asc via gpg(1) → "(gpg-verified)"
#   ftr install→ package still verified via minisign (sha256 + sig)
#   tamper     → mutate index after signing → gpg verify fails, old kept
#
# Skips gracefully if gpg(1) is unavailable.
#
# Run from the repo root: ./tests/phase8_gpg.sh

set -eu

FTR="${FTR:-./ftr}"
GEN_KEYPAIR="${GEN_KEYPAIR:-./tools/gen-keypair}"
FTR_SIGN="${FTR_SIGN:-./tools/ftr-sign}"

for t in "$FTR" "$GEN_KEYPAIR" "$FTR_SIGN"; do
	[ -x "$t" ] || { echo "phase8: error: $t missing" >&2; exit 2; }
done
if ! command -v gpg >/dev/null 2>&1; then
	echo "phase8: gpg(1) not available — SKIP"
	echo "phase8_gpg.sh: PASS"
	exit 0
fi
command -v tar >/dev/null 2>&1 || { echo "phase8: tar missing" >&2; exit 2; }
command -v sha256sum >/dev/null 2>&1 || { echo "phase8: sha256sum missing" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

REPO="$WORK/repo"; DB="$WORK/db"; PFX="$WORK/peacock"; KEYS="$WORK/keys"
CONF="$WORK/feather.conf"; GNUPGHOME="$WORK/gnupg"
mkdir -p "$REPO" "$DB" "$PFX" "$KEYS" "$GNUPGHOME"
chmod 700 "$GNUPGHOME"
export GNUPGHOME

export FTR_DB_ROOT="$DB"
export FTR_CONFIG="$CONF"

fail() { echo "phase8: FAIL: $*" >&2; exit 1; }

# --- minisign key for PACKAGES ---
"$GEN_KEYPAIR" "phase8-seed" "$KEYS/repo.pub" "$KEYS/repo.sec" "phase8 key" \
	>"$KEYS/key_id.txt"
KEY_ID=$(cat "$KEYS/key_id.txt")
export FTR_PUBKEY="$KEYS/repo.pub"
sign_pkg() { "$FTR_SIGN" "$KEYS/repo.sec" "$1" "$1.sig" "phase8: $(basename "$1")"; }

# --- GPG key for the INDEX ---
echo "phase8: generating ephemeral gpg key…"
gpg --batch --quiet --gen-key >/dev/null 2>&1 <<EOF || fail "gpg key gen failed"
%no-protection
Key-Type: eddsa
Key-Curve: ed25519
Name-Real: phase8 index signer
Expire-Date: 0
%commit
EOF
gpg --batch --quiet --armor --export >"$KEYS/gpg.pub" 2>/dev/null
[ -s "$KEYS/gpg.pub" ] || fail "gpg pubkey export empty"
gpg_sign_index() {
	rm -f "$REPO/index.toml.asc"
	gpg --batch --yes --quiet --detach-sign --armor \
		-o "$REPO/index.toml.asc" "$REPO/index.toml" \
		|| fail "gpg index signing failed"
}

# --- build one minisign-signed package ---
P="$WORK/stage-shell"
mkdir -p "$P/files/bin"
cat >"$P/manifest.toml" <<'EOF'
[package]
name = "peacock-shell-stub"
version = "0.1.0"

[install]
layout = "peacock"
EOF
echo "#!/bin/sh" >"$P/files/bin/peacock-shell-stub"
chmod +x "$P/files/bin/peacock-shell-stub"
( cd "$P" && tar -czf "$REPO/peacock-shell-stub-0.1.0.feather" manifest.toml files )
sign_pkg "$REPO/peacock-shell-stub-0.1.0.feather"

write_index() {
	path="$REPO/peacock-shell-stub-0.1.0.feather"
	sha=$(sha256sum "$path" | awk '{print $1}')
	size=$(stat -c '%s' "$path")
	{
		printf '[repo]\nname = "peacock-stable"\n\n'
		printf '[[package]]\n'
		printf 'name = "peacock-shell-stub"\n'
		printf 'version = "0.1.0"\n'
		printf 'layout = "peacock"\n'
		printf 'archive = "peacock-shell-stub-0.1.0.feather"\n'
		printf 'sha256 = "%s"\n' "$sha"
		printf 'size = %s\n\n' "$size"
	} >"$REPO/index.toml"
}
write_index
gpg_sign_index

cat >"$CONF" <<EOF
[[repos]]
name   = "peacock-stable"
url    = "file://$REPO"
pubkey = "$KEYS/repo.pub"
gpgkey = "$KEYS/gpg.pub"
EOF

# ----------------------------------------------------------------
# sync → gpg-verified
# ----------------------------------------------------------------
echo "phase8: --- ftr sync (gpg) ---"
out=$("$FTR" sync 2>&1) || fail "ftr sync failed: $out"
printf '%s\n' "$out"
echo "$out" | grep -q "synced: peacock-stable (gpg-verified)" \
	|| fail "expected '(gpg-verified)' sync line, got: $out"
[ -f "$DB/sync/peacock-stable/index.toml" ] || fail "index not synced"
[ -f "$DB/sync/peacock-stable/index.toml.asc" ] || fail "asc not synced"

# ----------------------------------------------------------------
# install → package minisign-verified
# ----------------------------------------------------------------
echo "phase8: --- ftr install (package via minisign) ---"
out=$("$FTR" install --peacock-prefix "$PFX" peacock-shell-stub 2>&1) \
	|| fail "install failed: $out"
echo "$out" | grep -q "installed: peacock-shell-stub-0.1.0 (verified by $KEY_ID)" \
	|| fail "expected minisign-verified install, got: $out"
[ -x "$PFX/bin/peacock-shell-stub" ] || fail "binary not installed"

# ----------------------------------------------------------------
# tamper the index AFTER signing → gpg verify must reject, keep old
# ----------------------------------------------------------------
echo "phase8: --- tamper index → gpg rejection ---"
printf '\n# sneaky edit\n' >>"$REPO/index.toml"   # do NOT re-sign
set +e
out=$("$FTR" sync 2>&1)
rc=$?
set -e
echo "$out" | grep -q "GPG verification failed" \
	|| fail "expected GPG verification failure, got: $out"
# old index is retained (rc 0 with WARN since a previous good index exists)
[ -f "$DB/sync/peacock-stable/index.toml" ] || fail "old index should be retained"

# re-sign the tampered index → sync should accept it again
gpg_sign_index
out=$("$FTR" sync 2>&1) || fail "re-signed sync failed: $out"
echo "$out" | grep -q "synced: peacock-stable (gpg-verified)" \
	|| fail "re-signed index should sync, got: $out"

echo "phase8_gpg.sh: PASS"
