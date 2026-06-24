#!/bin/sh
# phase7_resolve.sh — dependency resolver, metapackages, conflicts, arch.
#
# Builds a signed file:// repo whose index carries depends/conflicts/arch
# metadata, then checks:
#
#   install prp-meta   → pulls base-a, base-b (transitive) in deps-first
#                        order; the metapackage installs with no files.
#   install conflictor → rejected because it conflicts with installed base-b.
#   install --arch     → an arch-tagged package is found only for its arch.
#
# Run from the repo root: ./tests/phase7_resolve.sh

set -eu

FTR="${FTR:-./ftr}"
GEN_KEYPAIR="${GEN_KEYPAIR:-./tools/gen-keypair}"
FTR_SIGN="${FTR_SIGN:-./tools/ftr-sign}"

for t in "$FTR" "$GEN_KEYPAIR" "$FTR_SIGN"; do
	[ -x "$t" ] || { echo "phase7: error: $t missing (make build + tools)" >&2; exit 2; }
done
command -v tar >/dev/null 2>&1 || { echo "phase7: error: tar missing" >&2; exit 2; }
command -v sha256sum >/dev/null 2>&1 || { echo "phase7: error: sha256sum missing" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

REPO="$WORK/repo"
DB="$WORK/db"
PFX="$WORK/peacock"
KEYS="$WORK/keys"
CONF="$WORK/feather.conf"
mkdir -p "$REPO" "$DB" "$PFX" "$KEYS"

export FTR_DB_ROOT="$DB"
export FTR_CONFIG="$CONF"

fail() { echo "phase7: FAIL: $*" >&2; exit 1; }

"$GEN_KEYPAIR" "phase7-resolve-seed" "$KEYS/repo.pub" "$KEYS/repo.sec" \
	"phase7 resolve test key" >"$KEYS/key_id.txt"
KEY_ID=$(cat "$KEYS/key_id.txt")
export FTR_PUBKEY="$KEYS/repo.pub"
echo "phase7: keypair $KEY_ID"

sign_file() { "$FTR_SIGN" "$KEYS/repo.sec" "$1" "$1.sig" "phase7: $(basename "$1")"; }

# --- build a peacock package with one file ---
build_pkg() {
	name=$1; ver=$2
	stage="$WORK/stage-$name"
	mkdir -p "$stage/files/bin"
	cat >"$stage/manifest.toml" <<EOF
[package]
name = "$name"
version = "$ver"

[install]
layout = "peacock"
EOF
	echo "#!/bin/sh" >"$stage/files/bin/$name"
	chmod +x "$stage/files/bin/$name"
	( cd "$stage" && tar -czf "$REPO/$name-$ver.feather" manifest.toml files )
	sign_file "$REPO/$name-$ver.feather"
}

# --- build a metapackage (manifest only, no files/) ---
build_meta() {
	name=$1; ver=$2
	stage="$WORK/stage-$name"
	mkdir -p "$stage"
	cat >"$stage/manifest.toml" <<EOF
[package]
name = "$name"
version = "$ver"

[install]
layout = "meta"
EOF
	( cd "$stage" && tar -czf "$REPO/$name-$ver.feather" manifest.toml )
	sign_file "$REPO/$name-$ver.feather"
}

build_pkg  base-a    0.1.0
build_pkg  base-b    0.1.0
build_pkg  conflictor 0.1.0
build_pkg  archpkg   0.1.0
build_meta prp-meta  0.1.0

# --- write index.toml with depends/conflicts/arch metadata ---
emit_entry() {  # name ver extra_lines...
	name=$1; ver=$2; shift 2
	path="$REPO/$name-$ver.feather"
	sha=$(sha256sum "$path" | awk '{print $1}')
	size=$(stat -c '%s' "$path")
	printf '[[package]]\n'
	printf 'name = "%s"\n' "$name"
	printf 'version = "%s"\n' "$ver"
	printf 'archive = "%s-%s.feather"\n' "$name" "$ver"
	printf 'sha256 = "%s"\n' "$sha"
	printf 'size = %s\n' "$size"
	for line in "$@"; do printf '%s\n' "$line"; done
	printf '\n'
}

{
	printf '[repo]\n'
	printf 'name = "peacock-stable"\n\n'
	emit_entry base-a     0.1.0 'layout = "peacock"'
	emit_entry base-b     0.1.0 'layout = "peacock"' 'depends = ["base-a"]'
	emit_entry conflictor 0.1.0 'layout = "peacock"' 'conflicts = ["base-b"]'
	emit_entry archpkg    0.1.0 'layout = "peacock"' 'arch = "aarch64test"'
	emit_entry prp-meta   0.1.0 'layout = "meta"'    'depends = ["base-b"]'
} >"$REPO/index.toml"
sign_file "$REPO/index.toml"

cat >"$CONF" <<EOF
[[repos]]
name   = "peacock-stable"
url    = "file://$REPO"
pubkey = "$KEYS/repo.pub"
EOF

"$FTR" sync >/dev/null || fail "sync failed"

# ----------------------------------------------------------------
# 1. metapackage pulls transitive deps, deps-first
# ----------------------------------------------------------------
echo "phase7: --- install prp-meta (deps: base-a -> base-b -> prp-meta) ---"
out=$("$FTR" install --peacock-prefix "$PFX" prp-meta 2>&1) \
	|| fail "install prp-meta failed: $out"
printf '%s\n' "$out"
# topological order: base-a before base-b before prp-meta
echo "$out" | grep -q "install: 3 package(s): base-a base-b prp-meta" \
	|| fail "expected deps-first plan 'base-a base-b prp-meta', got: $out"
[ -x "$PFX/bin/base-a" ] || fail "base-a not installed"
[ -x "$PFX/bin/base-b" ] || fail "base-b not installed"
[ -d "$DB/local/prp-meta-0.1.0" ] || fail "metapackage not recorded in DB"

# Re-installing an already-satisfied dep alone is a no-op.
out=$("$FTR" install --peacock-prefix "$PFX" base-a 2>&1) || fail "reinstall base-a failed"
# base-a is an explicit target → reprocessed (not 'nothing to do'); fine either way.

# ----------------------------------------------------------------
# 2. conflict rejection (base-b already installed)
# ----------------------------------------------------------------
echo "phase7: --- install conflictor (conflicts base-b) must fail ---"
set +e
out=$("$FTR" install --peacock-prefix "$PFX" conflictor 2>&1)
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "conflictor install should have failed (conflicts installed base-b)"
echo "$out" | grep -q "conflicts with 'base-b'" \
	|| fail "expected conflict diagnostic, got: $out"
printf '%s\n' "$out"

# ----------------------------------------------------------------
# 3. arch filtering
# ----------------------------------------------------------------
echo "phase7: --- arch filter ---"
set +e
out=$("$FTR" install --peacock-prefix "$PFX" --arch x86test archpkg 2>&1)
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "archpkg should not resolve for the wrong arch"
echo "$out" | grep -q "cannot resolve dependency 'archpkg'" \
	|| fail "expected arch-filtered resolve failure, got: $out"

out=$("$FTR" install --peacock-prefix "$PFX" --arch aarch64test archpkg 2>&1) \
	|| fail "archpkg should install for its arch: $out"
[ -x "$PFX/bin/archpkg" ] || fail "archpkg not installed for matching arch"

echo "phase7_resolve.sh: PASS"
