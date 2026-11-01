# A directory to list, a raw file to stream, and a disk set
mkdir sd/SUB sd/SET
for n in $(seq 1 20); do echo "file $n" >sd/SUB/F$n.TXT; done
python3 -I -c 'import sys; sys.stdout.buffer.write(bytes(i * 7 & 255 for i in range(4096)))' >sd/RAW.BIN
mkdsk sd/SET/G1.DSK
mkdsk sd/SET/G2.DSK
