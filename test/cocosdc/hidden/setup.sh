# Names containing '~' look like the short forms of long names, so the SDC
# hides them; SET only holds such a name, so it isn't a disk set
mkdsk sd/ABC.DSK
mkdsk "sd/AAA~1.DSK"
mkdir sd/SET
mkdsk "sd/SET/G~1.DSK"
