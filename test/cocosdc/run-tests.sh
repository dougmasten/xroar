#!/bin/bash
# Regression tests for the CoCo SDC cartridge emulation.
#
# usage: run-tests.sh [-k] [-u] XROAR SDCDOS-ROM [TEST...]
#
#   XROAR       XRoar binary built with --enable-experimental
#   SDCDOS-ROM  SDC-DOS for the CoCo 3 (bank 0 of the cartridge flash)
#   -k          keep each test's working directory
#   -u          update each test's expect.txt from its output
#
# Needs lwasm, decb (ToolShed) and python3.  See README for how tests work.

set -u

keep=0
update=0
while getopts ku opt; do
	case $opt in
	k) keep=1 ;;
	u) update=1 ;;
	*) exit 2 ;;
	esac
done
shift $((OPTIND - 1))
if [ $# -lt 2 ]; then
	sed -n '4,9s/^# \{0,1\}//p' "$0" >&2
	exit 2
fi

XROAR=$(realpath "$1")
ROM=$(realpath "$2")
shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
if [ $# -eq 0 ]; then
	set -- $(cd "$HERE" && ls -d */ | tr -d /)
fi
for tool in lwasm decb python3; do
	command -v $tool >/dev/null || { echo "$tool not found" >&2; exit 2; }
done

# Helpers for setup.sh and run.sh, which run in the test's working directory.

# Make an empty Disk BASIC image, copying in any files given.
mkdsk() {
	local img=$1
	shift
	decb dskini "$img" || return 1
	local f
	for f in "$@"; do
		decb copy -1 -a "$f" "$img,$(basename "$f" | tr a-z A-Z)" || return 1
	done
}

mksdf() {
	python3 -I "$HERE/mksdf.py" "$@"
}

# Run XRoar with the SDC on the SD card directory sd, typing $TYPE.
xr() {
	"$XROAR" -ui null -ao null -no-ratelimit -m coco3 \
		-cart cocosdc -cart-rom "$ROM" -load-hd0 "$W/sd" \
		-timeout "$TIMEOUT" "$@" >>"$W/xroar.log" 2>&1
}

# Print the file OUT.TXT from the program disk (drive 0 unless given).
out_txt() {
	python3 -I "$HERE/getfile.py" "${1:-sd/T.DSK}" OUT.TXT
}

pass=0
fail=0
failed=
for t in "$@"; do
	T=$HERE/$t
	if [ ! -f "$T/t.bas" ]; then
		echo "$t: no such test" >&2
		fail=$((fail + 1))
		failed="$failed $t"
		continue
	fi
	keep_w=$keep
	W=$(mktemp -d "${TMPDIR:-/tmp}/cocosdc-$t.XXXXXX")
	mkdir "$W/sd"
	(
		cd "$W" || exit 1
		TYPE='RUN"T"\r'
		TIMEOUT=60
		mkdsk sd/T.DSK >/dev/null || exit 1
		decb copy -0 -a -l "$T/t.bas" sd/T.DSK,T.BAS || exit 1
		for a in "$T"/*.asm; do
			[ -e "$a" ] || continue
			b=$(basename "$a" .asm | tr a-z A-Z).BIN
			lwasm -9 -f decb -o "$b" "$a" >lwasm.log 2>&1 || { cat lwasm.log; exit 1; }
			decb copy -2 -b "$b" "sd/T.DSK,$b" || exit 1
		done
		echo "0=T.DSK" >sd/STARTUP.CFG
		if [ -f "$T/setup.sh" ]; then
			. "$T/setup.sh" || exit 1
		fi
		# Fixed dates: the SDC reports them in directory listings
		find sd -exec touch -h -d 2024-01-02T03:04:05 {} +
		if [ -f "$T/run.sh" ]; then
			. "$T/run.sh"
		else
			xr -type "$TYPE"
			out_txt
		fi
	) >"$W/result.txt"
	if [ $update -eq 1 ]; then
		cp "$W/result.txt" "$T/expect.txt"
		echo "$t: updated"
	elif cmp -s "$T/expect.txt" "$W/result.txt"; then
		echo "$t: ok"
		pass=$((pass + 1))
	else
		echo "$t: FAILED"
		diff -u "$T/expect.txt" "$W/result.txt" | sed 's/^/    /'
		fail=$((fail + 1))
		failed="$failed $t"
		keep_w=1
	fi
	if [ $keep_w -eq 1 ]; then
		echo "    kept $W"
	else
		chmod -R u+w "$W"
		rm -rf "$W"
	fi
done

[ $update -eq 1 ] && exit 0
echo "$pass passed, $fail failed${failed:+:$failed}"
[ $fail -eq 0 ]
