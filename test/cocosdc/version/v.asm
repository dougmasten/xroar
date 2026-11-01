        org     $5100
start   orcc    #$50
        lda     #$43
        sta     $ff40
        lda     #'V
        sta     $ff49
        ldx     #0
        stx     $ff4a
        lda     #$c0
        sta     $ff48
w       lda     $ff48
        bita    #1
        bne     w
        ldd     $ff4a
        std     $5000
        clr     $ff40
        andcc   #$af
        rts
        end     start
