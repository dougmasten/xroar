* SETCMD - issue $C0 command ($50F0) with $FF4A/$FF4B ($50F1/2), status to $50F3
        org     $5100
start   orcc    #$50
        lda     #$43
        sta     $ff40
        ldb     $50f0
        stb     $ff49
        ldx     $50f1
        stx     $ff4a
        lda     #$c0
        sta     $ff48
        exg     x,x
w       lda     $ff48
        bita    #1
        bne     w
        sta     $50f3
        clr     $ff40
        andcc   #$af
        rts
        end     start
