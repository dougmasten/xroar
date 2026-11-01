* SNT - exercise SDC state that snapshots must keep.  Results at RES as
* 16 bit words; CP1-CP4 are checkpoints for snapshot traps.
RES     equ     $5000
        org     $5100
start   orcc    #$50
        ldy     #$ff4a
        ldu     #RES
        stu     rp
* D:SUB, L:*.* and the first page of the listing
        lda     #$e0
        ldx     #nm_d
        lbsr    blkcmd
        lbsr    putst
        lda     #$e0
        ldx     #nm_l
        lbsr    blkcmd
        lda     #'>
        lbsr    ccmd
        lbsr    sumblk
CP1     nop
        lda     #'>
        lbsr    ccmd
        lbsr    sumblk
* m:RAW.BIN in drive 1, then LSN 3 of it
        lda     #$e1
        ldx     #nm_m
        lbsr    blkcmd
        lbsr    putst
CP2     nop
        lda     #$81
        clrb
        ldx     #3
        lbsr    issue
        lbsr    wready
        lbsr    sumblk
* D:/ then M:SET in drive 0, #2 and the name 'I' gives
        lda     #$e0
        ldx     #nm_root
        lbsr    blkcmd
        lda     #$e0
        ldx     #nm_set
        lbsr    blkcmd
        lbsr    putst
        lda     #$c0
        ldb     #'#
        ldx     #$0200
        lbsr    issue
        lbsr    wnbusy
        lbsr    putst
CP3     nop
        lda     #'I
        lbsr    ccmd
        lbsr    sumblk
        lda     #$e0
        ldx     #nm_t
        lbsr    blkcmd
        lbsr    putst
* Stream drive 1: four blocks, checkpoint after two
        lda     #$91
        clrb
        ldx     #0
        lbsr    issue
        ldd     #0
        std     sum
        lbsr    wready
        lbsr    sblk
        lbsr    wready
        lbsr    sblk
CP4     nop
        lbsr    wready
        lbsr    sblk
        lbsr    wready
        lbsr    sblk
        lda     #$d0
        sta     -2,y
        lbsr    wnbusy
        ldd     sum
        lbsr    putd
* Bank 3 of the flash
        clr     $ff40
        lda     $ff43
        anda    #7
        pshs    a
        lda     #3
        sta     $ff43
        ldx     #$c000
        ldd     #0
fl1     addb    ,x+
        adca    #0
        cmpx    #$c100
        bne     fl1
        puls    b
        stb     $ff43
        bsr     putd
        andcc   #$af
        rts

* Command A with a 256 byte block at X, waiting for it to complete
blkcmd  pshs    x
        clrb
        ldx     #0
        bsr     issue
        bsr     wready
        puls    x
        ldb     #128
bc1     ldu     ,x++
        stu     ,y
        decb
        bne     bc1
        bra     wnbusy
* $C0 command B=A on drive 0, wait for its block
ccmd    tfr     a,b
        lda     #$c0
        ldx     #0
        bsr     issue
        bra     wready
issue   pshs    a
        lda     #$43
        sta     -10,y
        stb     -1,y
        stx     ,y
        puls    a
        sta     -2,y
        rts
wready  lda     -2,y
        bita    #$82
        beq     wready
        rts
wnbusy  lda     -2,y
        bita    #1
        bne     wnbusy
        rts
* Sum a 256 byte block into RES
sumblk  bita    #$80
        bne     sb9
        ldd     #0
        std     sum
        bsr     sblk2
        ldd     sum
        bra     putd
sb9     ldd     #$ffff
        bra     putd
* Add a 512 byte stream block to sum
sblk    clrb
        bra     sbl
sblk2   ldb     #128
sbl     pshs    b
        ldd     ,y
        addd    sum
        std     sum
        puls    b
        decb
        bne     sbl
        rts
putst   clra
        ldb     -2,y
putd    ldu     rp
        std     ,u++
        stu     rp
        rts
rp      rmb     2
sum     rmb     2
nm_d    fcc     "D:SUB"
        fill    0,256-5
nm_l    fcc     "L:*.*"
        fill    0,256-5
nm_m    fcc     "m:/RAW.BIN"
        fill    0,256-10
nm_root fcc     "D:/"
        fill    0,256-3
nm_set  fcc     "M:SET"
        fill    0,256-5
nm_t    fcc     "M:/T.DSK"
        fill    0,256-8
        end     start
