* SDCTIME - time CoCo SDC commands on real hardware
*
* Run from an SDC disk image in drive 0 (at least 35 tracks).  Drive 1 gets
* a scratch image, TIMING.DSK, which is created if it doesn't exist.
*
* Each result is 3 bytes at RES: a 16 bit loop count and the status byte at
* the end of the wait.  Every wait loop iteration takes 18 cycles; the CPU
* runs at 0.89 MHz with interrupts off.  A count of $FFFF means the wait
* timed out (about 1.3 s), status $FF means the test was skipped.

RES     equ     $5000
STA_BUSY equ    $01
STA_READY equ   $02
STA_FAILED equ  $80

        org     $5100
start   pshs    u,y
        orcc    #$50
        sta     $ffd8           ; slow speed: 0.89 MHz
        ldy     #$ff4a
        ldx     #RES
        stx     rp

* Commands that don't touch the SD card
        lda     #$c0            ; V: firmware version
        ldb     #'V
        ldx     #0
        lbsr    issue
        lbsr    wnbusy
        lda     #$c0            ; Q: size of the image in drive 0
        ldb     #'Q
        ldx     #0
        lbsr    issue
        lbsr    wnbusy
        lda     #$c0            ; I: info for drive 0
        ldb     #'I
        ldx     #0
        lbsr    issue
        lbsr    wready
        lbsr    drain

* Sector reads from drive 0
        ldx     #rdtab
        lda     #$80
        lbsr    reads

* Mount the scratch image in drive 1, creating it if needed
        lda     #$e1
        ldx     #nm_m
        lbsr    blkcmd
        bita    #STA_FAILED
        bne     mk
        ldb     #2
        lbsr    skip
        bra     wt
mk      lda     #$e1
        ldx     #nm_n
        lbsr    blkcmd
        bita    #STA_FAILED
        beq     wt
        ldb     #16             ; no scratch image: skip writes, read back
        lbsr    skip
        bra     dir

* Sector writes to drive 1: time to accept the data, then to commit it
wt      ldx     #wrtab
        stx     tp
wtl     ldx     tp
        ldb     ,x
        cmpb    #$ff
        beq     wtx
        leax    3,x
        stx     tp
        ldx     -2,x
        lda     #$a1
        lbsr    issue
        lbsr    wready
        ldx     #$a000          ; any 256 bytes will do
        lbsr    send
        lbsr    wnbusy
        bra     wtl
wtx     ldx     #rbtab          ; read back from drive 1
        lda     #$81
        lbsr    reads

* Directory listing: L: then one page
dir     lda     #$e0
        ldx     #nm_l
        lbsr    blkcmd
        lda     #$c0
        ldb     #'>
        ldx     #0
        lbsr    issue
        lbsr    wready
        lbsr    drain

* Stream drive 0: first block, 4 more blocks, then abort
        lda     #$90
        clrb
        ldx     #0
        lbsr    issue
        lbsr    wready
        lda     #4
        sta     cnt
stl     lbsr    drain
        lbsr    drain
        lbsr    wready
        dec     cnt
        bne     stl
        lda     #$d0
        sta     -2,y
        lbsr    wnbusy

* Unmount drive 1
        lda     #$e1
        ldx     #nm_u
        lbsr    blkcmd

        clr     $ff40
        andcc   #$af
        puls    u,y,pc

* Read sectors listed at X (3 byte LSNs, $FF ends) with command A
reads   sta     rcmd
        stx     tp
rdl     ldx     tp
        ldb     ,x
        cmpb    #$ff
        beq     rdx
        leax    3,x
        stx     tp
        ldx     -2,x
        lda     rcmd
        bsr     issue
        bsr     wready
        bsr     drain
        bra     rdl
rdx     rts

* Command A with a 256 byte block at X: times accepting the command, then
* completing it.  Returns the final status in A.
blkcmd  pshs    x
        clrb
        ldx     #0
        bsr     issue
        bsr     wready
        puls    x
        bsr     send
        bra     wnbusy

* Enter command mode and issue command A with parameters B, X
issue   pshs    a
        lda     #$43
        sta     -10,y           ; $FF40
        stb     -1,y            ; $FF49
        stx     ,y              ; $FF4A/$FF4B
        puls    a
        sta     -2,y            ; $FF48
        rts

* Wait for READY or FAILED (18 cycles per loop)
wready  ldx     #0
wr1     lda     -2,y
        bita    #STA_READY|STA_FAILED
        bne     store
        leax    1,x
        bne     wr1
        leax    -1,x
        bra     store

* Wait for BUSY to clear (18 cycles per loop)
wnbusy  ldx     #0
wn1     lda     -2,y
        bita    #STA_BUSY
        beq     store
        leax    1,x
        bne     wn1
        leax    -1,x

* Store loop count X and status A
store   ldu     rp
        stx     ,u++
        sta     ,u+
        stu     rp
        rts

* Store B skipped results
skip    ldu     rp
sk1     clr     ,u+
        clr     ,u+
        lda     #$ff
        sta     ,u+
        decb
        bne     sk1
        stu     rp
        rts

* Read 256 bytes in word mode
drain   ldb     #128
dr1     ldx     ,y
        decb
        bne     dr1
        rts

* Send 256 bytes from X in word mode
send    ldb     #128
sd1     ldu     ,x++
        stu     ,y
        decb
        bne     sd1
        rts

rp      rmb     2
tp      rmb     2
cnt     rmb     1
rcmd    rmb     1

* LSN 0 twice (repeat), 1 (other half of the same SD block), 2 and 3 (next
* block), 300 and 301 (far away), 18 (next track)
rdtab   fcb     0
        fdb     0
        fcb     0
        fdb     0
        fcb     0
        fdb     1
        fcb     0
        fdb     2
        fcb     0
        fdb     3
        fcb     0
        fdb     300
        fcb     0
        fdb     301
        fcb     0
        fdb     18
        fcb     $ff
* LSN 0 four times, 1, 2, then 300 (may extend a new file)
wrtab   fcb     0
        fdb     0
        fcb     0
        fdb     0
        fcb     0
        fdb     0
        fcb     0
        fdb     0
        fcb     0
        fdb     1
        fcb     0
        fdb     2
        fcb     0
        fdb     300
        fcb     $ff
rbtab   fcb     0
        fdb     0
        fcb     0
        fdb     300
        fcb     $ff

nm_m    fcc     "M:TIMING.DSK"
        fill    0,256-12
nm_n    fcc     "N:TIMING.DSK"
        fill    0,256-12
nm_l    fcc     "L:*.*"
        fill    0,256-5
nm_u    fcc     "M:"
        fill    0,256-2
        end     start
