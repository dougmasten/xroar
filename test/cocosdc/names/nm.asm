* NM - issue command $50F0 with the name at $5200, status to $50F1, then the
* 'I' name of drive 1 (11 bytes) to $50F2
        org     $5100
start   orcc    #$50
        ldy     #$ff4a
        lda     #$43
        sta     $ff40
        clr     $ff49
        ldx     #0
        stx     $ff4a
        lda     $50f0
        sta     $ff48
w1      lda     $ff48
        bita    #$82
        beq     w1
        bmi     dn
        ldx     #$5200
        ldb     #128
s1      ldu     ,x++
        stu     ,y
        decb
        bne     s1
w2      lda     $ff48
        bita    #1
        bne     w2
dn      sta     $50f1
        ldx     #$50f2          ; blank name if 'I' fails
        ldb     #11
        lda     #$20
b1      sta     ,x+
        decb
        bne     b1
        lda     #$43
        sta     $ff40
        lda     #'I
        sta     $ff49
        ldx     #0
        stx     $ff4a
        lda     #$c1
        sta     $ff48
w3      lda     $ff48
        bita    #$82
        beq     w3
        bmi     ix
        ldx     #$5300
        ldb     #128
r1      ldu     ,y
        stu     ,x++
        decb
        bne     r1
        ldx     #$5300
        ldu     #$50f2
        ldb     #11
c1      lda     ,x+
        sta     ,u+
        decb
        bne     c1
ix      clr     $ff40
        andcc   #$af
        rts
        end     start
