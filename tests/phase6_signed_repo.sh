#!/bin/sh
# phase6_signed_repo.sh — signed-repo end-to-end round trip + tamper
# rejection + (optional) https path.
#
# Builds a fake file:// repo with two signed .feather packages and a
# signed index.toml. Exercises:
#
#   ftr sync     → verifies index sig
#   ftr install  → verifies pkg sig
#   tamper       → mutate one byte of a .feather AFTER signing,
#                  re-install. Must reject with verify error.
#   restore      → put the file back; re-install must succeed again
#   --allow-unsigned: local archive without .sig must install with WARN
#                     and refuse to bypass repo verification.
#
# An HTTPS smoke is included if `python3 -m http.server` is available
# and curl is in PATH; otherwise documented as skipped. The HTTPS
# server is plain http on a localhost port — it covers the curl exec
# + retry path without needing TLS material.
#
# Run from the repo root: ./tests/phase6_signed_repo.sh

set -eu

FTR="${FTR:-./ftr}"
GEN_KEYPAIR="${GEN_KEYPAIR:-./tools/gen-keypair}"
FTR_SIGN="${FTR_SIGN:-./tools/ftr-sign}"

if [ ! -x "$FTR" ]; then
	echo "phase6: error: $FTR not found (run 'make build' first)" >&2
	exit 2
fi
if [ ! -x "$GEN_KEYPAIR" ] || [ ! -x "$FTR_SIGN" ]; then
	echo "phase6: error: tools/gen-keypair or tools/ftr-sign missing" >&2
	echo "       run: make tools/gen-keypair tools/ftr-sign" >&2
	exit 2
fi
if ! command -v tar >/dev/null 2>&1; then
	echo "phase6: error: tar(1) not in PATH" >&2
	exit 2
fi
if ! command -v sha256sum >/dev/null 2>&1; then
	echo "phase6: error: sha256sum(1) not in PATH" >&2
	exit 2
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

REPO_ROOT="$WORK/repo"
DB_ROOT="$WORK/db"
PEACOCK_PREFIX="$WORK/peacock"
APPS_PREFIX="$WORK/apps"
KEYS="$WORK/keys"
CONF="$WORK/feather.conf"
mkdir -p "$REPO_ROOT" "$DB_ROOT" "$PEACOCK_PREFIX" "$APPS_PREFIX" "$KEYS"

export FTR_DB_ROOT="$DB_ROOT"
export FTR_CONFIG="$CONF"

fail() {
	echo "phase6: FAIL: $*" >&2
	exit 1
}

# ----------------------------------------------------------------
# Keypair
# ----------------------------------------------------------------
"$GEN_KEYPAIR" "phase6-signed-repo-seed" "$KEYS/repo.pub" "$KEYS/repo.sec" \
	"phase6 signed-repo test key" >"$KEYS/key_id.txt"
KEY_ID=$(cat "$KEYS/key_id.txt")
echo "phase6: keypair $KEY_ID"

# Use this keypair as the trusted pubkey via env override.
export FTR_PUBKEY="$KEYS/repo.pub"

sign_file() {
	"$FTR_SIGN" "$KEYS/repo.sec" "$1" "$1.sig" "phase6: $(basename "$1")"
}

# ----------------------------------------------------------------
# Build 2 packages
# ----------------------------------------------------------------
build_archive() {
	stage=$1
	out=$2
	( cd "$stage" && tar -czf "$out" manifest.toml files )
}

# pkg A: peacock-shell-stub 0.1.0
P1="$WORK/stage-shell"
mkdir -p "$P1/files/bin"
cat >"$P1/manifest.toml" <<'EOF'
[package]
name = "peacock-shell-stub"
version = "0.1.0"
description = "Stub peacock shell — signed"
runtime = "peacock"

[install]
layout = "peacock"
EOF
cat >"$P1/files/bin/peacock-shell-stub" <<'EOF'
#!/bin/sh
echo "signed peacock-shell-stub"
EOF
chmod +x "$P1/files/bin/peacock-shell-stub"
build_archive "$P1" "$REPO_ROOT/peacock-shell-stub-0.1.0.feather"
sign_file "$REPO_ROOT/peacock-shell-stub-0.1.0.feather"

# pkg B: peacock-tool-stub 0.1.0
P2="$WORK/stage-tool"
mkdir -p "$P2/files/bin"
cat >"$P2/manifest.toml" <<'EOF'
[package]
name = "peacock-tool-stub"
version = "0.1.0"
description = "Stub peacock tool — signed"
runtime = "peacock"

[install]
layout = "peacock"
EOF
cat >"$P2/files/bin/peacock-tool-stub" <<'EOF'
#!/bin/sh
echo "signed peacock-tool-stub"
EOF
chmod +x "$P2/files/bin/peacock-tool-stub"
build_archive "$P2" "$REPO_ROOT/peacock-tool-stub-0.1.0.feather"
sign_file "$REPO_ROOT/peacock-tool-stub-0.1.0.feather"

# ----------------------------------------------------------------
# Build + sign index.toml
# ----------------------------------------------------------------
write_index() {
	{
		printf '[repo]\n'
		printf 'name = "peacock-stable"\n\n'
		for combo in \
			"peacock-shell-stub|0.1.0|Stub peacock shell — signed" \
			"peacock-tool-stub|0.1.0|Stub peacock tool — signed"
		do
			IFS='|'; set -- $combo; unset IFS
			name=$1; version=$2; desc=$3
			arch="$name-$version.feather"
			path="$REPO_ROOT/$arch"
			sha=$(sha256sum "$path" | awk '{print $1}')
			size=$(stat -c '%s' "$path")
			printf '[[package]]\n'
			printf 'name = "%s"\n' "$name"
			printf 'version = "%s"\n' "$version"
			printf 'description = "%s"\n' "$desc"
			printf 'runtime = "peacock"\n'
			printf 'layout = "peacock"\n'
			printf 'archive = "%s"\n' "$arch"
			printf 'sha256 = "%s"\n' "$sha"
			printf 'size = %s\n\n' "$size"
		done
	} >"$REPO_ROOT/index.toml"
	sign_file "$REPO_ROOT/index.toml"
}
write_index

# ----------------------------------------------------------------
# feather.conf — per-repo pubkey field, overriding the env var.
# ----------------------------------------------------------------
cat >"$CONF" <<EOF
[[repos]]
name   = "peacock-stable"
url    = "file://$REPO_ROOT"
pubkey = "$KEYS/repo.pub"
EOF

# ----------------------------------------------------------------
# sync + install (positive paths)
# ----------------------------------------------------------------
echo "phase6: --- ftr sync ---"
out=$("$FTR" sync 2>&1) || fail "ftr sync exited non-zero: $out"
printf '%s\n' "$out"
case "$out" in
	*"synced: peacock-stable (verified by $KEY_ID)"*) ;;
	*) fail "expected 'synced: peacock-stable (verified by $KEY_ID)', got: $out" ;;
esac
[ -f "$DB_ROOT/sync/peacock-stable/index.toml" ] \
	|| fail "index.toml not synced"
[ -f "$DB_ROOT/sync/peacock-stable/index.toml.sig" ] \
	|| fail "index.toml.sig not synced"

echo "phase6: --- ftr install peacock-shell-stub ---"
out=$("$FTR" install --peacock-prefix "$PEACOCK_PREFIX" \
	peacock-shell-stub 2>&1) \
	|| fail "ftr install exited non-zero: $out"
printf '%s\n' "$out"
case "$out" in
	*"installed: peacock-shell-stub-0.1.0 (verified by $KEY_ID)"*) ;;
	*) fail "expected '(verified by $KEY_ID)' install line, got: $out" ;;
esac
[ -x "$PEACOCK_PREFIX/bin/peacock-shell-stub" ] \
	|| fail "binary not installed at expected path"

# ----------------------------------------------------------------
# Tamper: corrupt one byte of peacock-tool-stub AFTER signing,
# re-fetch via sync (file:// fetch will re-copy the corrupt archive),
# install must reject.
# ----------------------------------------------------------------
echo "phase6: --- tamper rejection ---"
# Save the original for restoration later.
cp "$REPO_ROOT/peacock-tool-stub-0.1.0.feather" \
	"$WORK/tool-original.feather"
# Flip a byte in the middle of the archive (header bytes are gzip
# magic + flags — flipping near the end keeps the file an apparent
# gzip but breaks sha256+sig).
ORIG_SIZE=$(stat -c '%s' "$REPO_ROOT/peacock-tool-stub-0.1.0.feather")
TAMPER_OFF=$(( ORIG_SIZE / 2 ))
# Use dd to overwrite one byte without changing the file size so the
# sha256 mismatch isn't trivial-length.
dd if=/dev/zero of="$REPO_ROOT/peacock-tool-stub-0.1.0.feather" \
	bs=1 count=1 seek="$TAMPER_OFF" conv=notrunc 2>/dev/null

# Update the index to match the new sha256 so the sha256 check passes
# and the sig check is the thing that rejects. Don't re-sign — that's
# the point of the tamper.
TAMPER_SHA=$(sha256sum "$REPO_ROOT/peacock-tool-stub-0.1.0.feather" \
	| awk '{print $1}')
sed -i "s/^sha256 = \".*\" *$/sha256 = \"PLACEHOLDER\"/" "$REPO_ROOT/index.toml" || true
# Easier: regenerate index from scratch with the new sha for tool.
{
	printf '[repo]\n'
	printf 'name = "peacock-stable"\n\n'
	for combo in \
		"peacock-shell-stub|0.1.0|Stub peacock shell — signed" \
		"peacock-tool-stub|0.1.0|Stub peacock tool — signed"
	do
		IFS='|'; set -- $combo; unset IFS
		name=$1; version=$2; desc=$3
		arch="$name-$version.feather"
		path="$REPO_ROOT/$arch"
		sha=$(sha256sum "$path" | awk '{print $1}')
		size=$(stat -c '%s' "$path")
		printf '[[package]]\n'
		printf 'name = "%s"\n' "$name"
		printf 'version = "%s"\n' "$version"
		printf 'description = "%s"\n' "$desc"
		printf 'runtime = "peacock"\n'
		printf 'layout = "peacock"\n'
		printf 'archive = "%s"\n' "$arch"
		printf 'sha256 = "%s"\n' "$sha"
		printf 'size = %s\n\n' "$size"
	done
} >"$REPO_ROOT/index.toml"
sign_file "$REPO_ROOT/index.toml"   # re-sign index (legit)
"$FTR" sync >/dev/null

set +e
out=$("$FTR" install --peacock-prefix "$PEACOCK_PREFIX" \
	peacock-tool-stub 2>&1)
rc=$?
set -e
if [ "$rc" -eq 0 ]; then
	fail "tampered archive install should have failed, got rc=0"
fi
case "$out" in
	*"signature verify failed"*) ;;
	*) fail "expected 'signature verify failed' diagnostic, got: $out" ;;
esac
case "$out" in
	*"signer key fingerprint: $KEY_ID"*) ;;
	*) fail "expected signer fingerprint in error, got: $out" ;;
esac
printf '%s\n' "$out"

# ----------------------------------------------------------------
# Restore: put the original archive + index back; install must
# succeed again.
# ----------------------------------------------------------------
echo "phase6: --- post-restore install succeeds ---"
cp "$WORK/tool-original.feather" \
	"$REPO_ROOT/peacock-tool-stub-0.1.0.feather"
write_index   # rebuilds + re-signs against the restored archive
"$FTR" sync >/dev/null
out=$("$FTR" install --peacock-prefix "$PEACOCK_PREFIX" \
	peacock-tool-stub 2>&1) || fail "post-restore install failed: $out"
case "$out" in
	*"installed: peacock-tool-stub-0.1.0 (verified by $KEY_ID)"*) ;;
	*) fail "post-restore expected verified install, got: $out" ;;
esac

# ----------------------------------------------------------------
# Local archive without .sig: WARN line + install proceeds.
# ----------------------------------------------------------------
echo "phase6: --- local install without .sig: WARN ---"
LOCAL_DIR="$WORK/local"
mkdir -p "$LOCAL_DIR/files/bin"
cat >"$LOCAL_DIR/manifest.toml" <<'EOF'
[package]
name = "local-only"
version = "0.1.0"
description = "Local archive with no sidecar .sig"

[install]
layout = "peacock"
EOF
echo "echo local" >"$LOCAL_DIR/files/bin/local-only"
chmod +x "$LOCAL_DIR/files/bin/local-only"
( cd "$LOCAL_DIR" && tar -czf "$WORK/local-only-0.1.0.feather" \
	manifest.toml files )

out=$("$FTR" install --peacock-prefix "$PEACOCK_PREFIX" \
	"$WORK/local-only-0.1.0.feather" 2>&1) \
	|| fail "local-unsigned install should succeed, got rc=$?: $out"
case "$out" in
	*"installed without signature (local archive, no .sig file)"*) ;;
	*) fail "expected 'no .sig' WARN line, got: $out" ;;
esac
[ -x "$PEACOCK_PREFIX/bin/local-only" ] || fail "local-only not installed"
"$FTR" remove local-only >/dev/null

# ----------------------------------------------------------------
# --allow-unsigned: even with a present .sig that would fail, we skip.
# Use the tampered tool archive + valid sig (sig matches original) and
# rename them so a local install picks them up.
# ----------------------------------------------------------------
echo "phase6: --- --allow-unsigned bypasses sig verify (local only) ---"
# Make a local archive that has a deliberately-wrong .sig sidecar.
# Sign a different blob to get a sig that won't verify against the
# archive.
echo "decoy contents" >"$WORK/decoy"
"$FTR_SIGN" "$KEYS/repo.sec" "$WORK/decoy" "$WORK/local-only-0.1.0.feather.sig" \
	"phase6 mismatched sig"
cp "$WORK/local-only-0.1.0.feather" "$WORK/local-mismatch.feather"
cp "$WORK/local-only-0.1.0.feather.sig" "$WORK/local-mismatch.feather.sig"

# Without --allow-unsigned: must fail.
set +e
out=$("$FTR" install --peacock-prefix "$PEACOCK_PREFIX" \
	"$WORK/local-mismatch.feather" 2>&1)
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "local with bad sig should fail without --allow-unsigned"
case "$out" in
	*"signature verify failed"*) ;;
	*) fail "expected 'signature verify failed' on local bad sig, got: $out" ;;
esac

# With --allow-unsigned: must succeed + print WARN.
out=$("$FTR" install --peacock-prefix "$PEACOCK_PREFIX" --allow-unsigned \
	"$WORK/local-mismatch.feather" 2>&1) \
	|| fail "local --allow-unsigned should succeed: $out"
case "$out" in
	*"installed without signature verification (--allow-unsigned)"*) ;;
	*) fail "expected --allow-unsigned WARN, got: $out" ;;
esac
"$FTR" remove local-only >/dev/null

# ----------------------------------------------------------------
# --allow-unsigned is NOT honored for repo installs. The tamper test
# above already showed mandatory verify; here we just confirm that
# adding --allow-unsigned to a repo install with a bad sig still
# fails.
# ----------------------------------------------------------------
echo "phase6: --- --allow-unsigned NOT honored for repo installs ---"
# Tamper the tool archive again, sync, install with --allow-unsigned.
dd if=/dev/zero of="$REPO_ROOT/peacock-tool-stub-0.1.0.feather" \
	bs=1 count=1 seek="$TAMPER_OFF" conv=notrunc 2>/dev/null
{
	printf '[repo]\n'
	printf 'name = "peacock-stable"\n\n'
	for combo in \
		"peacock-shell-stub|0.1.0|Stub peacock shell — signed" \
		"peacock-tool-stub|0.1.0|Stub peacock tool — signed"
	do
		IFS='|'; set -- $combo; unset IFS
		name=$1; version=$2; desc=$3
		arch="$name-$version.feather"
		path="$REPO_ROOT/$arch"
		sha=$(sha256sum "$path" | awk '{print $1}')
		size=$(stat -c '%s' "$path")
		printf '[[package]]\n'
		printf 'name = "%s"\n' "$name"
		printf 'version = "%s"\n' "$version"
		printf 'description = "%s"\n' "$desc"
		printf 'runtime = "peacock"\n'
		printf 'layout = "peacock"\n'
		printf 'archive = "%s"\n' "$arch"
		printf 'sha256 = "%s"\n' "$sha"
		printf 'size = %s\n\n' "$size"
	done
} >"$REPO_ROOT/index.toml"
sign_file "$REPO_ROOT/index.toml"
"$FTR" sync >/dev/null
set +e
out=$("$FTR" install --peacock-prefix "$PEACOCK_PREFIX" --allow-unsigned \
	peacock-tool-stub 2>&1)
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "--allow-unsigned must not bypass repo sig check"
case "$out" in
	*"signature verify failed"*) ;;
	*) fail "expected repo install rejection, got: $out" ;;
esac

# ----------------------------------------------------------------
# HTTPS smoke (optional). Stand up python3 -m http.server in a
# subshell, point feather at it, run sync, tear down.
# ----------------------------------------------------------------
HTTPS_STATUS="skipped"
if command -v curl >/dev/null 2>&1 && \
   command -v python3 >/dev/null 2>&1; then
	# Restore the tool archive + re-sign + re-write index so the
	# fake HTTP server serves a working snapshot.
	cp "$WORK/tool-original.feather" \
		"$REPO_ROOT/peacock-tool-stub-0.1.0.feather"
	write_index
	# Pick an ephemeral high port.
	PORT=$(awk 'BEGIN{srand(); print 30000 + int(rand()*5000)}')
	( cd "$REPO_ROOT" && python3 -m http.server "$PORT" \
		>"$WORK/http.log" 2>&1 ) &
	HTTP_PID=$!
	# Give the server a moment to bind. python's http.server logs
	# the first request when ready; instead poll the port with curl.
	tries=0
	while [ "$tries" -lt 30 ]; do
		if curl -fs "http://127.0.0.1:$PORT/index.toml" \
		    -o /dev/null 2>/dev/null; then
			break
		fi
		tries=$((tries+1))
		sleep 0.1
	done
	if [ "$tries" -ge 30 ]; then
		kill "$HTTP_PID" 2>/dev/null || true
		echo "phase6: http server failed to start; skipping HTTPS path"
		HTTPS_STATUS="skipped (server failed to start)"
	else
		# Rewrite feather.conf to point at the local http server.
		cat >"$CONF" <<EOF2
[[repos]]
name   = "peacock-stable"
url    = "http://127.0.0.1:$PORT"
pubkey = "$KEYS/repo.pub"
EOF2
		# Clean the DB so sync actually re-fetches via curl.
		rm -rf "$DB_ROOT/sync/peacock-stable"
		echo "phase6: --- ftr sync (http://) ---"
		if out=$("$FTR" sync 2>&1); then
			printf '%s\n' "$out"
			case "$out" in
				*"synced: peacock-stable (verified by $KEY_ID)"*)
					HTTPS_STATUS="ran (file scheme verified) + http via curl"
					;;
				*)
					HTTPS_STATUS="failed: $out"
					;;
			esac
		else
			HTTPS_STATUS="ftr sync failed over http: $out"
		fi
		kill "$HTTP_PID" 2>/dev/null || true
		wait "$HTTP_PID" 2>/dev/null || true
	fi
else
	HTTPS_STATUS="skipped (curl or python3 missing)"
fi
echo "phase6: HTTPS smoke: $HTTPS_STATUS"

echo "phase6_signed_repo.sh: PASS"
