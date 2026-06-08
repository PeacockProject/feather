#!/bin/sh
# smoke_help.sh — minimal smoke test for the ftr binary.
#
# Verifies:
#   1. --help exits 0 and stdout lists every subcommand
#   2. --version matches FTR_VERSION
#   3. `ftr install --help` exits 0 and prints usage (subcommand
#      dispatch works for the most-used cmd)
#   4. `ftr nonexistent-cmd` errors to stderr listing valid commands
#      and exits 1
#
# Run from the repo root: ./tests/smoke_help.sh

set -eu

FTR="${FTR:-./ftr}"
EXPECTED_VERSION="0.1.0-skeleton"
SUBCMDS="install remove sync upgrade search info list files flavor"

if [ ! -x "$FTR" ]; then
	echo "smoke_help.sh: error: $FTR not found or not executable" >&2
	echo "(run 'make build' first)" >&2
	exit 2
fi

fail() {
	echo "smoke_help.sh: FAIL: $*" >&2
	exit 1
}

# ----------------------------------------------------------------
# 1. --help exits 0 and stdout lists every subcommand
# ----------------------------------------------------------------
help_out=$("$FTR" --help)
help_rc=$?
if [ "$help_rc" -ne 0 ]; then
	fail "--help exited $help_rc, expected 0"
fi
for cmd in $SUBCMDS; do
	if ! printf '%s\n' "$help_out" | grep -qw "$cmd"; then
		fail "--help output missing subcommand '$cmd'"
	fi
done

# ----------------------------------------------------------------
# 2. --version matches FTR_VERSION
# ----------------------------------------------------------------
ver_out=$("$FTR" --version)
ver_rc=$?
if [ "$ver_rc" -ne 0 ]; then
	fail "--version exited $ver_rc, expected 0"
fi
if ! printf '%s\n' "$ver_out" | grep -qw "$EXPECTED_VERSION"; then
	fail "--version output '$ver_out' missing '$EXPECTED_VERSION'"
fi

# ----------------------------------------------------------------
# 3. `ftr install --help` exits 0 with a usage line
# ----------------------------------------------------------------
inst_out=$("$FTR" install --help) || {
	rc=$?
	fail "install --help exited $rc, expected 0"
}
if ! printf '%s\n' "$inst_out" | grep -q "Usage: ftr install"; then
	fail "install --help missing 'Usage: ftr install': $inst_out"
fi

# ----------------------------------------------------------------
# 4. Unknown subcommand errors to stderr + exits 1
# ----------------------------------------------------------------
set +e
unk_err=$("$FTR" nonexistent-cmd 2>&1 1>/dev/null)
unk_rc=$?
set -e
if [ "$unk_rc" -ne 1 ]; then
	fail "unknown command exited $unk_rc, expected 1"
fi
for cmd in $SUBCMDS; do
	if ! printf '%s\n' "$unk_err" | grep -qw "$cmd"; then
		fail "unknown-command error output missing valid subcommand '$cmd'"
	fi
done

echo "smoke_help.sh: PASS"
