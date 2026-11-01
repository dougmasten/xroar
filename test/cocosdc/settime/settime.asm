* SETTIME - time CoCo SDC disk set commands on real hardware
*
* Run from an SDC disk image in drive 0, in the current directory.  It
* makes a directory SETTEST holding DISK1.DSK and DISK2.DSK (if they don't
* exist), mounts it as a disk set in drive 0, times the '#' and '+' commands,
* then mounts the original image in drive 0 again.
*
* Each result is 4 bytes at RES: a 24 bit loop count and the status byte at
* the end of the wait.  Every wait loop iteration takes 18 cycles; the CPU
* runs at 0.89 MHz with interrupts off.  A count of $FFFFFF means the wait
* timed out (about 5 s).  After the results, at NAME, are the 11 bytes of
* the name 'I' reports for drive 0 after asking for a missing disk.

RES     equ     $5000
NAME    equ     RES+25*4
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

* Name of the image in drive 0, to mount it again at the end
        lda     #$c0
        ldb     #'I
        ldx     #0
        lbsr    issue
        lbsr    wready
        ldx     #blk
        lbsr    rdblk
        ldx     #blk
        ldu     #orig
        ldb     #11
on1     lda     ,x+
        sta     ,u+
        decb
        bne     on1

* Make the set: directory, two blank images (N: on drive 1), unmount
        lda     #$e0
        ldx     #nm_k
        lbsr    blkcmd
        lda     #$e1
        ldx     #nm_d1
        lbsr    blkcmd
        lda     #$e1
        ldx     #nm_d2
        lbsr    blkcmd
        lda     #$e1
        ldx     #nm_u
        lbsr    blkcmd

* Mount the set in drive 0: disk 1
        lda     #$e0
        ldx     #nm_set
        lbsr    blkcmd

* '#': $FF4A = disk number, $FF4B bit 0 = blink the LED
        ldx     #$0200          ; disk 2
        lbsr    setcmd
        ldx     #$0100          ; disk 1
        lbsr    setcmd
        ldx     #$0101          ; disk 1, blink once
        lbsr    setcmd
        ldx     #$0201          ; disk 2, blink twice
        lbsr    setcmd
        ldb     #'+             ; next: disk 1 (wraps), blinks once
        ldx     #0
        lbsr    setcmd2
        ldb     #'+             ; next: disk 2, blinks twice
        ldx     #0
        lbsr    setcmd2
        ldx     #$0300          ; missing disk 3
        lbsr    setcmd

* Which disk is in drive 0 now?
        lda     #$c0
        ldb     #'I
        ldx     #0
        lbsr    issue
        lbsr    wready
        ldx     #blk
        lbsr    rdblk
        ldx     #blk
        ldu     #NAME
        ldb     #11
nm1     lda     ,x+
        sta     ,u+
        decb
        bne     nm1

* A set in drive 1, and N: on a directory
        lda     #$e1
        ldx     #nm_set
        lbsr    blkcmd
        lda     #$e0
        ldx     #nm_nset
        lbsr    blkcmd

* Mount the original image in drive 0 again: "M:NAME.EXT"
        ldx     #blk
        clrb
cl1     clr     ,x+
        decb
        bne     cl1
        ldx     #blk
        ldd     #$4d3a          ; "M:"
        std     ,x++
        ldu     #orig
        ldb     #8
mn1     lda     ,u+
        cmpa    #$20
        beq     mn2
        sta     ,x+
        decb
        bne     mn1
mn2     lda     #'.
        sta     ,x+
        ldu     #orig+8
        ldb     #3
mn3     lda     ,u+
        cmpa    #$20
        beq     mn4
        sta     ,x+
        decb
        bne     mn3
mn4     lda     #$e0
        ldx     #blk
        lbsr    blkcmd

        clr     $ff40
        andcc   #$af
        puls    u,y,pc

* '#' with parameters X, timed until BUSY clears
setcmd  ldb     #'#
setcmd2 lda     #$c0
        bsr     issue
        bra     wnbusy

* Command A with a 256 byte block at X: times accepting the command, then
* completing it
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
wready  clr     hi
        ldx     #0
wr1     lda     -2,y
        bita    #STA_READY|STA_FAILED
        bne     store
        leax    1,x
        bne     wr1
        bsr     wrap
        bra     wr1

* Wait for BUSY to clear (18 cycles per loop)
wnbusy  clr     hi
        ldx     #0
wn1     lda     -2,y
        bita    #STA_BUSY
        beq     store
        leax    1,x
        bne     wn1
        bsr     wrap
        bra     wn1

* Count overflowed 16 bits: give up after about 5 s
wrap    inc     hi
        lda     hi
        cmpa    #4
        blo     wr9
        leas    2,s             ; drop the return to the wait loop
        lda     #$ff
        sta     hi
        ldx     #$ffff
        lda     -2,y
        bra     store
wr9     rts

* Store the count hi:X and status A
store   ldu     rp
        ldb     hi
        stb     ,u+
        stx     ,u++
        sta     ,u+
        stu     rp
        rts

* Read 256 bytes in word mode to X
rdblk   ldb     #128
rb1     ldu     ,y
        stu     ,x++
        decb
        bne     rb1
        rts

* Send 256 bytes from X in word mode
send    ldb     #128
sd1     ldu     ,x++
        stu     ,y
        decb
        bne     sd1
        rts

rp      rmb     2
hi      rmb     1
orig    rmb     11

nm_k    fcc     "K:SETTEST"
        fill    0,256-9
nm_d1   fcc     "N:SETTEST/DISK1.DSK"
        fill    0,256-19
nm_d2   fcc     "N:SETTEST/DISK2.DSK"
        fill    0,256-19
nm_u    fcc     "M:"
        fill    0,256-2
nm_set  fcc     "M:SETTEST"
        fill    0,256-9
nm_nset fcc     "N:SETTEST"
        fill    0,256-9
blk     rmb     256
        end     start
