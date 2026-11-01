# Five "+" commands step through the set (4 is missing), then "#"
# selects disks 3, 4 (missing: 3 stays) and 9 (out of range)
# A disk set with images 1, 2, 3 and 5; each holds a file naming it
mkdir sd/SET
for n in 1 2 3 5; do
	echo "$n" >G$n
	mkdsk sd/SET/GAME$n.DSK G$n
done
echo x >sd/SET/README.TXT
# Results go to drive 1, which keeps its disk while drive 0 changes
mkdsk sd/OUT.DSK
printf '0=T.DSK\r\n1=OUT.DSK\r\n' >sd/STARTUP.CFG
