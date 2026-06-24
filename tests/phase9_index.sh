#!/bin/sh
# phase9_index.sh — `ftr index` generates a resolver-usable index.toml.
#
# Builds two packages (base-b depends base-a), generates the index with
# `ftr index` (not hand-written), signs it, then sync + install base-b
# and confirm base-a was pulled in. Proves the generated index carries
# depends/arch/sha256 the resolver can use.
#
# Run from the repo root: ./tests/phase9_index.sh

set -eu

FTR="${FTR:-./ftr}"
GEN_KEYPAIR="${GEN_KEYPAIR:-./tools/gen-keypair}"
FTR_SIGN="${FTR_SIGN:-./tools/ftr-sign}"

for t in "$FTR" "$GEN_KEYPAIR" "$FTR_SIGN"; do
	[ -x "$t" ] || { echo "phase9: error: $t missing" >&2; exit 2; }
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
REPO="$WORK/repo"; DB="$WORK/db"; PFX="$WORK/peacock"; KEYS="$WORK/keys"; CONF="$WORK/feather.conf"
mkdir -p "$REPO" "$DB" "$PFX" "$KEYS"
export FTR_DB_ROOT="$DB"; export FTR_CONFIG="$CONF"
fail() { echo "phase9: FAIL: $*" >&2; exit 1; }

"$GEN_KEYPAIR" "phase9-seed" "$KEYS/repo.pub" "$KEYS/repo.sec" "phase9 key" >"$KEYS/id.txt"
export FTR_PUBKEY="$KEYS/repo.pub"
sign() { "$FTR_SIGN" "$KEYS/repo.sec" "$1" "$1.sig" "phase9"; }

build_pkg() {  # name depends-line
	name=$1; dep=$2
	stage="$WORK/stage-$name"; mkdir -p "$stage/files/bin"
	{
		printf '[package]\nname = "%s"\nversion = "0.1.0"\n' "$name"
		[ -n "$dep" ] && printf '%s\n' "$dep"
		printf '\n[install]\nlayout = "peacock"\n'
	} >"$stage/manifest.toml"
	echo "#!/bin/sh" >"$stage/files/bin/$name"; chmod +x "$stage/files/bin/$name"
	( cd "$stage" && tar -czf "$REPO/$name-0.1.0.feather" manifest.toml files )
	sign "$REPO/$name-0.1.0.feather"
}

build_pkg base-a ""
build_pkg base-b 'depends = ["base-a"]'

# Generate the index with the tool under test.
"$FTR" index --arch aarch64test --name peacock-stable "$REPO" >/dev/null || fail "ftr index failed"
[ -f "$REPO/index.toml" ] || fail "index.toml not written"

echo "phase9: --- generated index.toml ---"
cat "$REPO/index.toml"
grep -q 'depends = \["base-a"\]' "$REPO/index.toml" || fail "depends not emitted"
grep -q 'arch = "aarch64test"' "$REPO/index.toml" || fail "arch not emitted"
grep -q 'layout = "peacock"' "$REPO/index.toml" || fail "layout not emitted"
grep -q '^sha256 = "' "$REPO/index.toml" || fail "sha256 not emitted"

sign "$REPO/index.toml"
cat >"$CONF" <<EOF
[[repos]]
name   = "peacock-stable"
url    = "file://$REPO"
pubkey = "$KEYS/repo.pub"
EOF

"$FTR" sync >/dev/null || fail "sync of generated index failed"

# Install base-b for the matching arch → base-a must be pulled in first.
out=$("$FTR" install --peacock-prefix "$PFX" --arch aarch64test base-b 2>&1) \
	|| fail "install base-b failed: $out"
printf '%s\n' "$out"
echo "$out" | grep -q "install: 2 package(s): base-a base-b" \
	|| fail "expected resolver to pull base-a from generated index, got: $out"
[ -x "$PFX/bin/base-a" ] || fail "base-a not installed"
[ -x "$PFX/bin/base-b" ] || fail "base-b not installed"

echo "phase9_index.sh: PASS"
