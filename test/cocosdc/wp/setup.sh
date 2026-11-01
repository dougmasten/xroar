# Write protection from the SDF header, from a read-only host file, and none
mksdf -w sd/WP.SDF
mkdsk sd/RO.DSK
chmod a-w sd/RO.DSK
mkdsk sd/RW.DSK
