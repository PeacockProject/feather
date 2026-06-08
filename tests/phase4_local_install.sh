#!/bin/sh
# phase4_local_install.sh — round-trip test for local .feather
# install + remove (layout=peacock).
#
# Builds a tiny .feather archive on the fly, installs it into a
# sandbox prefix (--peacock-prefix) with a sandbox DB ($FTR_DB_ROOT),
# verifies the files appeared and the DB lists the package, then
# removes it and verifies cleanup.
#
# Also covers the negative case: a layout=app archive must be
# rejected with "not yet supported in phase 4".
#
# Run from the repo root: ./tests/phase4_local_install.sh

set -eu

FTR="${FTR:-./ftr}"

if [ ! -x "$FTR" ]; then
	echo "phase4: error: $FTR not found (run 'make build' first)" >&2
	exit 2
fi
if ! command -v tar >/dev/null 2>&1; then
	echo "phase4: error: tar(1) not in PATH" >&2
	exit 2
fi

# ----------------------------------------------------------------
# scratch dirs
# ----------------------------------------------------------------
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

DB_ROOT="$WORK/db"
PREFIX_SANDBOX="$WORK/peacock"
mkdir -p "$DB_ROOT" "$PREFIX_SANDBOX"
export FTR_DB_ROOT="$DB_ROOT"

fail() {
	echo "phase4: FAIL: $*" >&2
	exit 1
}

# ----------------------------------------------------------------
# build a tiny .feather archive: layout=peacock
# ----------------------------------------------------------------
STAGE_OK="$WORK/stage-ok"
mkdir -p "$STAGE_OK/files/bin" \
         "$STAGE_OK/files/share/peacock-shell-stub" \
         "$STAGE_OK/hooks"

cat >"$STAGE_OK/manifest.toml" <<'EOF'
[package]
name = "peacock-shell-stub"
version = "0.1.0"
description = "Stub package for phase 4 verification"
flavor = ["arch", "debian", "alpine"]
runtime = "peacock"

[install]
layout = "peacock"
prefix = "/peacock"

[provides]
peacock-shell = "0.1"
EOF

cat >"$STAGE_OK/files/bin/peacock-shell-stub" <<'EOF'
#!/bin/sh
echo "peacock-shell-stub here, prefix=$FEATHER_PREFIX"
EOF
chmod +x "$STAGE_OK/files/bin/peacock-shell-stub"

cat >"$STAGE_OK/files/share/peacock-shell-stub/README" <<'EOF'
peacock-shell-stub: a placeholder for the real peacock-shell port.
EOF

cat >"$STAGE_OK/hooks/post-install.sh" <<'EOF'
#!/bin/sh
echo "post-install: installed $FEATHER_PKG_NAME-$FEATHER_PKG_VERSION at $FEATHER_PREFIX"
EOF
chmod +x "$STAGE_OK/hooks/post-install.sh"

ARCHIVE_OK="$WORK/peacock-shell-stub.feather"
( cd "$STAGE_OK" && tar -czf "$ARCHIVE_OK" manifest.toml files hooks )

# ----------------------------------------------------------------
# round trip: install
# ----------------------------------------------------------------
echo "phase4: --- install round trip ---"
inst_out=$("$FTR" install --peacock-prefix "$PREFIX_SANDBOX" \
                          "$ARCHIVE_OK" 2>&1) || {
	rc=$?
	printf '%s\n' "$inst_out"
	fail "ftr install exited $rc"
}
printf '%s\n' "$inst_out"

case "$inst_out" in
	*"installed: peacock-shell-stub-0.1.0"*) ;;
	*) fail "expected 'installed: peacock-shell-stub-0.1.0' line, got: $inst_out" ;;
esac

[ -x "$PREFIX_SANDBOX/bin/peacock-shell-stub" ] || \
	fail "installed binary missing: $PREFIX_SANDBOX/bin/peacock-shell-stub"
[ -f "$PREFIX_SANDBOX/share/peacock-shell-stub/README" ] || \
	fail "installed README missing"
[ -f "$DB_ROOT/local/peacock-shell-stub-0.1.0/manifest.toml" ] || \
	fail "DB manifest.toml missing"
[ -f "$DB_ROOT/local/peacock-shell-stub-0.1.0/files" ] || \
	fail "DB files list missing"
[ -f "$DB_ROOT/local/peacock-shell-stub-0.1.0/installed.txt" ] || \
	fail "DB installed.txt missing"
[ -x "$DB_ROOT/local/peacock-shell-stub-0.1.0/hooks/post-install.sh" ] || \
	fail "DB hook copy missing"

# ----------------------------------------------------------------
# ftr list
# ----------------------------------------------------------------
list_out=$("$FTR" list) || fail "ftr list exited non-zero"
printf '%s\n' "$list_out" | grep -q "^peacock-shell-stub 0.1.0$" || \
	fail "ftr list missing entry: $list_out"

# ----------------------------------------------------------------
# ftr files
# ----------------------------------------------------------------
files_out=$("$FTR" files peacock-shell-stub) || \
	fail "ftr files exited non-zero"
echo "$files_out" | grep -q "$PREFIX_SANDBOX/bin/peacock-shell-stub$" || \
	fail "ftr files missing bin entry: $files_out"
echo "$files_out" | grep -q "$PREFIX_SANDBOX/share/peacock-shell-stub/README$" || \
	fail "ftr files missing README entry: $files_out"

# ----------------------------------------------------------------
# remove
# ----------------------------------------------------------------
echo "phase4: --- remove round trip ---"
rm_out=$("$FTR" remove peacock-shell-stub 2>&1) || {
	rc=$?
	printf '%s\n' "$rm_out"
	fail "ftr remove exited $rc"
}
printf '%s\n' "$rm_out"

case "$rm_out" in
	*"removed: peacock-shell-stub-0.1.0"*) ;;
	*) fail "expected 'removed: peacock-shell-stub-0.1.0' line, got: $rm_out" ;;
esac

[ ! -e "$PREFIX_SANDBOX/bin/peacock-shell-stub" ] || \
	fail "binary still present after remove"
[ ! -e "$PREFIX_SANDBOX/share/peacock-shell-stub/README" ] || \
	fail "README still present after remove"
[ ! -d "$DB_ROOT/local/peacock-shell-stub-0.1.0" ] || \
	fail "DB entry still present after remove"

list_out=$("$FTR" list)
if [ -n "$list_out" ]; then
	fail "ftr list non-empty after remove: $list_out"
fi

# ----------------------------------------------------------------
# negative case: layout=app must be rejected
# ----------------------------------------------------------------
echo "phase4: --- layout=app rejection ---"
STAGE_APP="$WORK/stage-app"
mkdir -p "$STAGE_APP/files"
cat >"$STAGE_APP/manifest.toml" <<'EOF'
[package]
name = "com.example.notes"
version = "0.1.0"

[install]
layout = "app"
EOF
echo "placeholder" >"$STAGE_APP/files/placeholder"
ARCHIVE_APP="$WORK/com.example.notes.feather"
( cd "$STAGE_APP" && tar -czf "$ARCHIVE_APP" manifest.toml files )

set +e
app_out=$("$FTR" install --peacock-prefix "$PREFIX_SANDBOX" \
                         "$ARCHIVE_APP" 2>&1)
app_rc=$?
set -e

if [ "$app_rc" -eq 0 ]; then
	fail "layout=app install should have failed, got rc=0: $app_out"
fi
case "$app_out" in
	*"not yet supported in phase 4"*) ;;
	*) fail "expected 'not yet supported in phase 4', got: $app_out" ;;
esac

echo "phase4_local_install.sh: PASS"
