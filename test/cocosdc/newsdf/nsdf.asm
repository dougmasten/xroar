* NSDF - create SDF images with N: and store the status bytes at $5000
        org     $5100
start   pshs    u,y
        orcc    #$50
        ldy     #$ff4a
        ldu     #$5000
        ldb     #35             ; $FF49 = tracks
        ldx     #$0100          ; $FF4A = sides
        leax    nm_ok,pcr
        pshs    x
        ldx     #$0100
        bsr     create
        ldb     #40
        ldx     #$0300          ; 3 sides: rejected
        leax    nm_bad,pcr
        pshs    x
        ldx     #$0300
        bsr     create
        clr     $ff40
        andcc   #$af
        puls    u,y,pc

* B = tracks, X = sides:0, name pointer on the stack above the return
create  lda     #$43
        sta     -10,y
        stb     -1,y
        stx     ,y
        lda     #$e1
        sta     -2,y
cw1     lda     -2,y
        bita    #$82            ; ready or failed
        beq     cw1
        bmi     cx
        ldx     2,s
        lda     #128
        sta     cnt
cs1     ldd     ,x++
        std     ,y
        dec     cnt
        bne     cs1
cw2     lda     -2,y
        bita    #1
        bne     cw2
cx      sta     ,u+
        puls    x
        leas    2,s
        jmp     ,x

cnt     rmb     1
nm_ok   fcc     "N:NEW.SDF"
        fill    0,256-9
nm_bad  fcc     "N:BAD.SDF"
        fill    0,256-9
        end     start
