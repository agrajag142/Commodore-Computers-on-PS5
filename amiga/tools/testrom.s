; testrom.s - a tiny Amiga test "Kickstart" for the PS5 port's simulator.
; Not an operating system: it only exercises what the port connects -
; a 1-plane lores picture (stripes) through the copper, sprite 0 as a
; pointer moved by the mouse (JOY0DAT), Paula channel 0 playing a tone,
; the left mouse button (background green), the right one (POTGOR: the
; stripes go white) and the port 2 joystick (fire: the tone goes up;
; directions: the stripes' colour) and the keyboard (CIA-A's serial port:
; each key code received is drawn as an 8-line band at the top of the
; picture, its 8 bits as 8 pixels from the left - bit 7 first, set for a
; key let go - the next one below, and so on).
; Build:  vasmm68k_mot -Fbin -o testrom.rom testrom.s   (256K)

CUSTOM  = $dff000
CHIP_BPL = $10000
CHIP_COP = $20000
CHIP_SPR = $30000
CHIP_SND = $40000

        org     $fc0000
        dc.w    $1111,$4ef9             ; as Kickstart 1.3 starts (JMP)
        dc.l    start

start:  lea     CUSTOM,a6
        move.w  #$7fff,$9a(a6)          ; INTENA: all off
        move.w  #$7fff,$96(a6)          ; DMACON: all off
        move.w  #$7fff,$9c(a6)          ; INTREQ: clear
        move.b  #3,$bfe201              ; CIA-A DDRA: OVL and LED outputs
        move.b  #2,$bfe001              ; overlay off: chip RAM at 0
        lea     $7fff0,sp

; ---- bitplane: 16-line bands of alternating stripes (40 bytes x 256)
        lea     CHIP_BPL,a0
        move.w  #255,d1
.row    move.w  d1,d2
        lsr.w   #4,d2
        and.w   #1,d2
        move.w  #$f0f0,d3
        tst.w   d2
        beq.s   .a
        move.w  #$ff00,d3
.a      move.w  #19,d0
.col    move.w  d3,(a0)+
        dbf     d0,.col
        dbf     d1,.row

; ---- sprite 0: a 16 x 16 arrow-ish block
        lea     CHIP_SPR,a0
        clr.l   (a0)+                   ; pos/ctl (set every frame)
        move.w  #15,d0
        move.l  #$ffff0000,d1
.spr    move.l  d1,(a0)+                ; plane A on: colour 1 of the sprite
        lsr.l   #1,d1
        and.l   #$ffff0000,d1
        or.l    #$80000000,d1
        dbf     d0,.spr
        clr.l   (a0)+                   ; end of sprite

; ---- copper list: bitplane and sprite pointers, then end
        lea     CHIP_COP,a0
        move.l  #$00e00001,(a0)+        ; BPL1PTH = $0001
        move.l  #$00e20000,(a0)+        ; BPL1PTL = $0000
        move.l  #$01200003,(a0)+        ; SPR0PTH = $0003
        move.l  #$01220000,(a0)+        ; SPR0PTL = $0000
        move.l  #$fffffffe,(a0)+        ; end
        move.l  #CHIP_COP,$80(a6)       ; COP1LC
        move.w  d0,$88(a6)              ; COPJMP1

; ---- display: 320 x 256, 1 plane
        move.w  #$1200,$100(a6)         ; BPLCON0
        move.w  #0,$102(a6)
        move.w  #0,$104(a6)
        move.w  #$2c81,$8e(a6)          ; DIWSTRT
        move.w  #$2cc1,$90(a6)          ; DIWSTOP
        move.w  #$38,$92(a6)            ; DDFSTRT
        move.w  #$d0,$94(a6)            ; DDFSTOP
        move.w  #0,$108(a6)             ; BPL1MOD
        move.w  #$005,$180(a6)          ; COLOR00 dark blue
        move.w  #$fa0,$182(a6)          ; COLOR01 orange
        move.w  #$fff,$1a2(a6)          ; COLOR17 (sprite 0) white

; ---- sound: a square wave, 32 bytes, on channel 0
        lea     CHIP_SND,a0
        move.w  #15,d0
.s1     move.b  #$60,(a0)+
        dbf     d0,.s1
        move.w  #15,d0
.s2     move.b  #$a0,(a0)+
        dbf     d0,.s2
        move.l  #CHIP_SND,$a0(a6)       ; AUD0LC
        move.w  #16,$a4(a6)             ; AUD0LEN (words)
        move.w  #254,$a6(a6)            ; AUD0PER (~440 Hz)
        move.w  #40,$a8(a6)             ; AUD0VOL

        move.w  #$83a1,$96(a6)          ; DMA: master, bitplanes, copper, sprites, audio 0

; ---- main loop, once a frame
        move.w  #200,d4                 ; pointer x (sprite hstart)
        move.w  #120,d5                 ; pointer y
        move.w  $a(a6),d6               ; last JOY0DAT
        moveq   #0,d7                   ; key codes received
frame   move.w  $1e(a6),d0              ; INTREQR
        btst    #5,d0                   ; vertical blank?
        beq.s   frame
        move.w  #$0020,$9c(a6)          ; clear it

        ; mouse: counter deltas (8 bits each, signed)
        move.w  $a(a6),d0
        move.w  d0,d1
        sub.b   d6,d1                   ; dx (low byte)
        ext.w   d1
        add.w   d1,d4
        move.w  d0,d2
        lsr.w   #8,d2
        move.w  d6,d3
        lsr.w   #8,d3
        sub.b   d3,d2                   ; dy
        ext.w   d2
        add.w   d2,d5
        move.w  d0,d6
        cmp.w   #128,d4
        bge.s   .x1
        move.w  #128,d4
.x1     cmp.w   #440,d4
        ble.s   .x2
        move.w  #440,d4
.x2     cmp.w   #44,d5
        bge.s   .y1
        move.w  #44,d5
.y1     cmp.w   #280,d5
        ble.s   .y2
        move.w  #280,d5
.y2
        ; sprite pos/ctl: VSTART<<8 | HSTART>>1 ; VSTOP<<8 | (HSTART&1)
        move.w  d5,d0
        lsl.w   #8,d0
        move.w  d4,d1
        lsr.w   #1,d1
        or.w    d1,d0
        move.w  d0,CHIP_SPR
        move.w  d5,d0
        add.w   #16,d0
        lsl.w   #8,d0
        move.w  d4,d1
        and.w   #1,d1
        or.w    d1,d0
        move.w  d0,CHIP_SPR+2

        ; left mouse button (CIA-A PRA bit 6, 0 = down): green background
        move.w  #$005,d0
        btst    #6,$bfe001
        bne.s   .nl
        move.w  #$0a0,d0
.nl     move.w  d0,$180(a6)

        ; right mouse button (POTGOR bit 10, 0 = down): white stripes
        move.w  #$fa0,d0
        move.w  $16(a6),d1
        btst    #10,d1
        bne.s   .nr
        move.w  #$fff,d0
.nr
        ; joystick port 2 (JOY1DAT): right = red, left = green, up/down = blue stripes
        move.w  $c(a6),d1
        btst    #1,d1
        beq.s   .j1
        move.w  #$f00,d0
.j1     btst    #9,d1
        beq.s   .j2
        move.w  #$0f0,d0
.j2     move.w  d1,d2
        lsr.w   #1,d2
        eor.w   d1,d2
        and.w   #$0101,d2               ; up (bit 8) or down (bit 0)
        beq.s   .j3
        move.w  #$00f,d0
.j3     move.w  d0,$182(a6)

        ; fire (CIA-A PRA bit 7, 0 = down): a higher tone
        move.w  #254,d0
        btst    #7,$bfe001
        bne.s   .nf
        move.w  #127,d0
.nf     move.w  d0,$a6(a6)

        ; keyboard: a byte in on CIA-A's serial port (ICR bit 3)?
        move.b  $bfed01,d0              ; ICR (reading clears it)
        btst    #3,d0
        beq.s   .nk
        move.b  $bfec01,d0              ; SDR
        not.b   d0
        ror.b   #1,d0                   ; the raw key code, bit 7 = up
        move.w  d7,d1
        and.w   #31,d1
        mulu    #320,d1                 ; 8 lines of 40 bytes
        lea     CHIP_BPL,a0
        add.l   d1,a0
        moveq   #7,d2
.kl     move.b  d0,(a0)
        clr.b   1(a0)
        lea     40(a0),a0
        dbf     d2,.kl
        addq.w  #1,d7
        bset    #6,$bfee01              ; the handshake: serial port to output...
        moveq   #50,d2
.hs     dbf     d2,.hs
        bclr    #6,$bfee01              ; ...and back: the keyboard may send the next
.nk     bra     frame

        ; pad to 256K
        dcb.b   $40000-(*-$fc0000),$ff
