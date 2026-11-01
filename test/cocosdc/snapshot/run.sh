# Run straight through, then for each checkpoint: stop there and snapshot,
# restore the snapshot and finish.  Every run should give the same results.
cp -a sd base
xr -type "$TYPE"
ref=$(out_txt)
echo "$ref"
lwasm -9 -f decb -o /dev/null --symbol-dump=syms "$T/snt.asm"
for cp in CP1 CP2 CP3 CP4; do
	addr=$(sed -n "s/^$cp EQU \\\$//p" syms)
	rm -rf sd
	cp -a base sd
	xr -type "$TYPE" -trap "pc=0x$addr" -trap-snap $cp.sna -trap-timeout 0
	xr -load $cp.sna
	res=$(out_txt)
	[ "$res" = "$ref" ] && echo "$cp same" || echo "$cp differs: $res"
done
