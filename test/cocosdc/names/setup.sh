# Short, lower case, wildcard, extensionless, long (hidden) and nested names
mkdsk sd/GAME.DSK
mkdsk sd/game2.dsk
mkdsk sd/GAMMA.DSK
mkdsk sd/NOEXT
mkdsk "sd/Long Name Disk.dsk"
echo x >sd/README.TXT
mkdir sd/DEMO
mkdsk sd/DEMO/X.DSK
TIMEOUT=90
