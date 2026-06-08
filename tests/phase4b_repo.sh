#!/bin/sh
# phase4b_repo.sh — end-to-end round trip against a file:// repo.
#
# Sets up a tmp dir as a fake repo, generates an index.toml + a few
# .feather archives covering all three install layouts (peacock, app,
# compat), wires a temp feather.conf at file://, then exercises:
#
#   ftr sync     → verifies index lands at sync/<repo>/index.toml
#   ftr search   → verifies match
#   ftr info     → verifies metadata
#   ftr install  → peacock-shell-stub via repo lookup
#   ftr install  → layout=app round trip
#   ftr install  → layout=compat round trip
#   ftr upgrade  → bump peacock-shell-stub from 0.1.0 to 0.2.0
#   ftr remove   → all three packages, verify cleanup
#
# Run from the repo root: ./tests/phase4b_repo.sh

set -eu

FTR="${FTR:-./ftr}"
GEN_KEYPAIR="${GEN_KEYPAIR:-./tools/gen-keypair}"
FTR_SIGN="${FTR_SIGN:-./tools/ftr-sign}"

if [ ! -x "$FTR" ]; then
	echo "phase4b: error: $FTR not found (run 'make build' first)" >&2
	exit 2
fi
if [ ! -x "$GEN_KEYPAIR" ] || [ ! -x "$FTR_SIGN" ]; then
	echo "phase4b: error: keypair/sign tools missing (run 'make tools/gen-keypair tools/ftr-sign')" >&2
	exit 2
fi
if ! command -v tar >/dev/null 2>&1; then
	echo "phase4b: error: tar(1) not in PATH" >&2
	exit 2
fi
if ! command -v sha256sum >/dev/null 2>&1; then
	echo "phase4b: error: sha256sum(1) not in PATH" >&2
	exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

REPO_ROOT="$WORK/repo"
DB_ROOT="$WORK/db"
PEACOCK_PREFIX="$WORK/peacock"
APPS_PREFIX="$WORK/apps"
COMPAT_PREFIX="$WORK/compat"
CONF="$WORK/feather.conf"
KEYS="$WORK/keys"
mkdir -p "$REPO_ROOT" "$DB_ROOT" "$PEACOCK_PREFIX" "$APPS_PREFIX" "$COMPAT_PREFIX" "$KEYS"

export FTR_DB_ROOT="$DB_ROOT"
export FTR_CONFIG="$CONF"

# Generate a deterministic test keypair; point $FTR_PUBKEY at it so
# the in-binary placeholder default isn't even consulted.
"$GEN_KEYPAIR" "phase4b-repo-test-key" "$KEYS/repo.pub" "$KEYS/repo.sec" \
	"phase4b repo test key" >/dev/null
export FTR_PUBKEY="$KEYS/repo.pub"

sign_file() {
	"$FTR_SIGN" "$KEYS/repo.sec" "$1" "$1.sig" "phase4b: $(basename "$1")"
}

fail() {
	echo "phase4b: FAIL: $*" >&2
	exit 1
}

# ----------------------------------------------------------------
# Helper: build a .feather archive from a staging dir.
# Args: <stage-dir> <archive-out-path> [extra-tar-entries...]
# ----------------------------------------------------------------
build_archive() {
	stage=$1
	out=$2
	shift 2
	( cd "$stage" && tar -czf "$out" manifest.toml files "$@" )
}

# ----------------------------------------------------------------
# Stage peacock-shell-stub-0.1.0 (layout=peacock)
# ----------------------------------------------------------------
P1="$WORK/stage-peacock-1"
mkdir -p "$P1/files/bin"
cat >"$P1/manifest.toml" <<'EOF'
[package]
name = "peacock-shell-stub"
version = "0.1.0"
description = "Stub peacock shell — v0.1"
runtime = "peacock"

[install]
layout = "peacock"
EOF
cat >"$P1/files/bin/peacock-shell-stub" <<'EOF'
#!/bin/sh
echo "peacock-shell-stub v0.1"
EOF
chmod +x "$P1/files/bin/peacock-shell-stub"
build_archive "$P1" "$REPO_ROOT/peacock-shell-stub-0.1.0.feather"
sign_file "$REPO_ROOT/peacock-shell-stub-0.1.0.feather"

# ----------------------------------------------------------------
# Stage app-stub-0.1.0 (layout=app)
# ----------------------------------------------------------------
APP="$WORK/stage-app"
mkdir -p "$APP/files/bin"
cat >"$APP/manifest.toml" <<'EOF'
[package]
name = "app-stub"
version = "0.1.0"
description = "Stub user app — layout=app"

[install]
layout = "app"
EOF
cat >"$APP/files/bin/app-stub" <<'EOF'
#!/bin/sh
echo "hello from app-stub"
EOF
chmod +x "$APP/files/bin/app-stub"
build_archive "$APP" "$REPO_ROOT/app-stub-0.1.0.feather"
sign_file "$REPO_ROOT/app-stub-0.1.0.feather"

# ----------------------------------------------------------------
# Stage compat-stub-0.1.0 (layout=compat, runtime=glibc)
# ----------------------------------------------------------------
COMPAT="$WORK/stage-compat"
mkdir -p "$COMPAT/files/lib"
cat >"$COMPAT/manifest.toml" <<'EOF'
[package]
name = "compat-stub"
version = "0.1.0"
description = "Stub compat runtime — glibc"
runtime = "glibc"

[install]
layout = "compat"
EOF
echo "/* stub libc */" >"$COMPAT/files/lib/libc-stub.so"
build_archive "$COMPAT" "$REPO_ROOT/compat-stub-0.1.0.feather"
sign_file "$REPO_ROOT/compat-stub-0.1.0.feather"

# ----------------------------------------------------------------
# Write index.toml
# ----------------------------------------------------------------
write_index() {
	idx_path=$1
	{
		printf '[repo]\n'
		printf 'name = "peacock-stable"\n'
		printf 'generated_at = "2026-06-09T00:00:00Z"\n\n'
		for combo in \
			"peacock-shell-stub|0.1.0|Stub peacock shell — v0.1|peacock|peacock" \
			"app-stub|0.1.0|Stub user app — layout=app||app" \
			"compat-stub|0.1.0|Stub compat runtime — glibc|glibc|compat"
		do
			IFS='|'; set -- $combo; unset IFS
			name=$1; version=$2; desc=$3; runtime=$4; layout=$5
			arch="$name-$version.feather"
			path="$REPO_ROOT/$arch"
			sha=$(sha256sum "$path" | awk '{print $1}')
			size=$(stat -c '%s' "$path")
			printf '[[package]]\n'
			printf 'name = "%s"\n' "$name"
			printf 'version = "%s"\n' "$version"
			printf 'description = "%s"\n' "$desc"
			if [ -n "$runtime" ]; then
				printf 'runtime = "%s"\n' "$runtime"
			fi
			printf 'layout = "%s"\n' "$layout"
			printf 'archive = "%s"\n' "$arch"
			printf 'sha256 = "%s"\n' "$sha"
			printf 'size = %s\n\n' "$size"
		done
	} >"$idx_path"
}
write_index "$REPO_ROOT/index.toml"
sign_file "$REPO_ROOT/index.toml"

# ----------------------------------------------------------------
# Write feather.conf
# ----------------------------------------------------------------
cat >"$CONF" <<EOF
[[repos]]
name = "peacock-stable"
url = "file://$REPO_ROOT"
EOF

# ----------------------------------------------------------------
# ftr sync
# ----------------------------------------------------------------
echo "phase4b: --- ftr sync ---"
sync_out=$("$FTR" sync) || fail "ftr sync exited non-zero"
printf '%s\n' "$sync_out"
[ -f "$DB_ROOT/sync/peacock-stable/index.toml" ] || \
	fail "synced index.toml missing"

# ----------------------------------------------------------------
# ftr search
# ----------------------------------------------------------------
echo "phase4b: --- ftr search ---"
search_out=$("$FTR" search peacock) || fail "ftr search exited non-zero"
printf '%s\n' "$search_out"
echo "$search_out" | grep -q "peacock-stable/peacock-shell-stub 0.1.0" || \
	fail "ftr search missing peacock-shell-stub entry"

# search by description
search_out=$("$FTR" search glibc) || fail "ftr search (glibc) exited non-zero"
echo "$search_out" | grep -q "peacock-stable/compat-stub 0.1.0" || \
	fail "ftr search description match missing"

# ----------------------------------------------------------------
# ftr info
# ----------------------------------------------------------------
echo "phase4b: --- ftr info ---"
info_out=$("$FTR" info peacock-shell-stub) || fail "ftr info exited non-zero"
printf '%s\n' "$info_out"
echo "$info_out" | grep -q "^Name:.*peacock-shell-stub$" || \
	fail "ftr info missing Name line"
echo "$info_out" | grep -q "^Version:.*0.1.0$" || \
	fail "ftr info missing Version line"
echo "$info_out" | grep -q "^Repository:.*peacock-stable$" || \
	fail "ftr info missing Repository line"

# ----------------------------------------------------------------
# ftr install peacock-shell-stub  (by name → repo lookup)
# ----------------------------------------------------------------
echo "phase4b: --- ftr install peacock-shell-stub (by name) ---"
inst_out=$("$FTR" install --peacock-prefix "$PEACOCK_PREFIX" \
                           peacock-shell-stub 2>&1) \
	|| fail "ftr install by name exited non-zero"
printf '%s\n' "$inst_out"
case "$inst_out" in
	*"installed: peacock-shell-stub-0.1.0"*) ;;
	*) fail "expected 'installed: peacock-shell-stub-0.1.0' line" ;;
esac
case "$inst_out" in
	*"(verified by "*) ;;
	*) fail "expected '(verified by <fingerprint>)' note" ;;
esac
[ -x "$PEACOCK_PREFIX/bin/peacock-shell-stub" ] || \
	fail "binary not installed at expected path"
[ -d "$DB_ROOT/local/peacock-shell-stub-0.1.0" ] || \
	fail "DB entry not created"

# ftr list
list_out=$("$FTR" list)
echo "$list_out" | grep -q "^peacock-shell-stub 0.1.0$" || \
	fail "ftr list missing peacock-shell-stub"

# ----------------------------------------------------------------
# ftr install app-stub (layout=app)
# ----------------------------------------------------------------
echo "phase4b: --- ftr install app-stub (layout=app) ---"
inst_out=$("$FTR" install --apps-prefix "$APPS_PREFIX" \
                           app-stub 2>&1) \
	|| fail "ftr install app-stub exited non-zero"
printf '%s\n' "$inst_out"
[ -x "$APPS_PREFIX/app-stub/bin/app-stub" ] || \
	fail "app-stub binary not at <apps>/app-stub/bin/app-stub"
[ -d "$DB_ROOT/local/app-stub-0.1.0" ] || \
	fail "app-stub DB entry missing"

# ----------------------------------------------------------------
# ftr install compat-stub (layout=compat, runtime=glibc)
# ----------------------------------------------------------------
echo "phase4b: --- ftr install compat-stub (layout=compat) ---"
inst_out=$("$FTR" install --compat-prefix "$COMPAT_PREFIX" \
                           compat-stub 2>&1) \
	|| fail "ftr install compat-stub exited non-zero"
printf '%s\n' "$inst_out"
[ -f "$COMPAT_PREFIX/glibc/lib/libc-stub.so" ] || \
	fail "compat-stub libc not at <compat>/glibc/lib/libc-stub.so"
[ -d "$DB_ROOT/local/compat-stub-0.1.0" ] || \
	fail "compat-stub DB entry missing"

# ----------------------------------------------------------------
# upgrade: bump peacock-shell-stub to 0.2.0
# ----------------------------------------------------------------
echo "phase4b: --- ftr upgrade (bump to 0.2.0) ---"
P2="$WORK/stage-peacock-2"
mkdir -p "$P2/files/bin"
cat >"$P2/manifest.toml" <<'EOF'
[package]
name = "peacock-shell-stub"
version = "0.2.0"
description = "Stub peacock shell — v0.2"
runtime = "peacock"

[install]
layout = "peacock"
EOF
cat >"$P2/files/bin/peacock-shell-stub" <<'EOF'
#!/bin/sh
echo "peacock-shell-stub v0.2"
EOF
chmod +x "$P2/files/bin/peacock-shell-stub"
build_archive "$P2" "$REPO_ROOT/peacock-shell-stub-0.2.0.feather"
sign_file "$REPO_ROOT/peacock-shell-stub-0.2.0.feather"
write_index "$REPO_ROOT/index.toml.new"
# append second [[package]] entry; rebuild whole index for simplicity:
{
	printf '[repo]\n'
	printf 'name = "peacock-stable"\n'
	printf 'generated_at = "2026-06-09T01:00:00Z"\n\n'
	for combo in \
		"peacock-shell-stub|0.1.0|Stub peacock shell — v0.1|peacock|peacock" \
		"peacock-shell-stub|0.2.0|Stub peacock shell — v0.2|peacock|peacock" \
		"app-stub|0.1.0|Stub user app — layout=app||app" \
		"compat-stub|0.1.0|Stub compat runtime — glibc|glibc|compat"
	do
		IFS='|'; set -- $combo; unset IFS
		name=$1; version=$2; desc=$3; runtime=$4; layout=$5
		arch="$name-$version.feather"
		path="$REPO_ROOT/$arch"
		sha=$(sha256sum "$path" | awk '{print $1}')
		size=$(stat -c '%s' "$path")
		printf '[[package]]\n'
		printf 'name = "%s"\n' "$name"
		printf 'version = "%s"\n' "$version"
		printf 'description = "%s"\n' "$desc"
		if [ -n "$runtime" ]; then
			printf 'runtime = "%s"\n' "$runtime"
		fi
		printf 'layout = "%s"\n' "$layout"
		printf 'archive = "%s"\n' "$arch"
		printf 'sha256 = "%s"\n' "$sha"
		printf 'size = %s\n\n' "$size"
	done
} >"$REPO_ROOT/index.toml"
sign_file "$REPO_ROOT/index.toml"

"$FTR" sync >/dev/null
up_out=$("$FTR" upgrade --peacock-prefix "$PEACOCK_PREFIX" \
                         peacock-shell-stub 2>&1) \
	|| fail "ftr upgrade exited non-zero"
printf '%s\n' "$up_out"
case "$up_out" in
	*"peacock-shell-stub: 0.1.0 -> 0.2.0"*) ;;
	*) fail "expected 'peacock-shell-stub: 0.1.0 -> 0.2.0', got: $up_out" ;;
esac
[ -d "$DB_ROOT/local/peacock-shell-stub-0.2.0" ] || \
	fail "DB entry for 0.2.0 missing after upgrade"

# upgrade with no newer version: should report up-to-date
up_out=$("$FTR" upgrade --peacock-prefix "$PEACOCK_PREFIX" \
                         peacock-shell-stub 2>&1) || \
	fail "second ftr upgrade exited non-zero"
case "$up_out" in
	*"up to date"*) ;;
	*) fail "expected 'up to date' on no-op upgrade, got: $up_out" ;;
esac

# ----------------------------------------------------------------
# remove every package, verify cleanup
# ----------------------------------------------------------------
echo "phase4b: --- ftr remove (peacock-shell-stub, app-stub, compat-stub) ---"
"$FTR" remove peacock-shell-stub app-stub compat-stub
[ ! -e "$PEACOCK_PREFIX/bin/peacock-shell-stub" ] || \
	fail "peacock binary still present"
[ ! -e "$APPS_PREFIX/app-stub/bin/app-stub" ] || \
	fail "app-stub binary still present"
[ ! -e "$COMPAT_PREFIX/glibc/lib/libc-stub.so" ] || \
	fail "compat-stub lib still present"
[ ! -d "$DB_ROOT/local/peacock-shell-stub-0.2.0" ] || \
	fail "peacock-shell-stub DB entry still present"
[ ! -d "$DB_ROOT/local/app-stub-0.1.0" ] || \
	fail "app-stub DB entry still present"
[ ! -d "$DB_ROOT/local/compat-stub-0.1.0" ] || \
	fail "compat-stub DB entry still present"

# ----------------------------------------------------------------
# negative: tampered sha256 must fail
# ----------------------------------------------------------------
echo "phase4b: --- sha256 tamper rejection ---"
# Corrupt the on-disk archive without updating the index sha256.
printf 'corrupted bytes' >>"$REPO_ROOT/app-stub-0.1.0.feather"
set +e
tam_out=$("$FTR" install --apps-prefix "$APPS_PREFIX" app-stub 2>&1)
tam_rc=$?
set -e
if [ "$tam_rc" -eq 0 ]; then
	fail "tampered archive install should have failed, got rc=0"
fi
case "$tam_out" in
	*"sha256 mismatch"*) ;;
	*) fail "expected 'sha256 mismatch' diagnostic, got: $tam_out" ;;
esac

echo "phase4b_repo.sh: PASS"
