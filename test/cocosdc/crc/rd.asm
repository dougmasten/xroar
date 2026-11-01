* RD - mount name at $5200 in drive 1 (status $50F0), read LSN $50F8-$50FA
* of drive 1: status at ready $50F1, status after the block $50F2, the
* block at $5300
        org     $5100
start   orcc    #$50
        ldy     #$ff4a
        lda     #$43
        sta     $ff40
        clr     $ff49
        ldx     #0
        stx     $ff4a
        lda     #$e1
        sta     $ff48
        bsr     wr
        bmi     m1
        ldx     #$5200
        ldb     #128
s1      ldu     ,x++
        stu     ,y
        decb
        bne     s1
        bsr     wnb
m1      sta     $50f0
        lda     #$43
        sta     $ff40
        lda     $50f8
        sta     $ff49
        ldx     $50f9
        stx     $ff4a
        lda     #$81
        sta     $ff48
        bsr     wr
        sta     $50f1
        bmi     r9
        ldx     #$5300
        ldb     #128
r1      ldu     ,y
        stu     ,x++
        decb
        bne     r1
        lda     $ff48
r9      sta     $50f2
        clr     $ff40
        andcc   #$af
        rts
wr      lda     $ff48
        bita    #$82
        beq     wr
        tsta
        rts
wnb     lda     $ff48
        bita    #1
        bne     wnb
        rts
        end     start
