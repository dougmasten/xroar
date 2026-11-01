* WT - mount name at $5200 in drive 1 (status $50F0), write LSN 0 of drive 1
* (accept status $50F1, done status $50F2), 'I' attribute byte to $50F3
        org     $5100
start   orcc    #$50
        ldy     #$ff4a
        lda     #$e1
        bsr     cmd
        bmi     m1
        ldx     #$5200
        bsr     send
        bsr     wnb
m1      sta     $50f0
        lda     #$a1
        bsr     cmd
        sta     $50f1
        bmi     w1
        ldx     #$5300
        bsr     send
        bsr     wnb
w1      sta     $50f2
        lda     #$43
        sta     $ff40
        lda     #'I
        sta     $ff49
        ldx     #0
        stx     $ff4a
        lda     #$c1
        sta     $ff48
        bsr     wr
        bmi     ix
        ldx     #$5300
        ldb     #128
r1      ldu     ,y
        stu     ,x++
        decb
        bne     r1
        lda     $530b
ix      sta     $50f3
        clr     $ff40
        andcc   #$af
        rts
cmd     ldb     #$43
        stb     $ff40
        clr     $ff49
        ldx     #0
        stx     $ff4a
        sta     $ff48
wr      lda     $ff48
        bita    #$82
        beq     wr
        tsta
        rts
wnb     lda     $ff48
        bita    #1
        bne     wnb
        rts
send    ldb     #128
s1      ldu     ,x++
        stu     ,y
        decb
        bne     s1
        rts
        end     start
