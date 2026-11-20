; nwsnes test ROM - an original, freely redistributable SNES program.
;
; It is not a game: it is a scene built to look like what a typical 2D game
; asks of the hardware, so the emulator can be tested and timed without any
; commercial ROM.
;   - mode 1: two scrolling 4bpp layers, a 2bpp HUD layer with priority
;   - 24 moving 16x16 sprites
;   - an HDMA sky gradient through colour math, optional layer blending (A)
;   - DMA for every graphics upload and for the sprite table, every frame
;   - the audio CPU boot upload handshake (the emulator has no audio CPU)
;   - 8 KB copied to work RAM and left alone, like decompressed graphics
;   - battery RAM written now and then
;   - a main loop that polls a flag set by the NMI handler
;
; Build: see the Makefile (ca65/ld65 from cc65).

.p816
.smart -

; ---- zero page ----
frame    = $10   ; 16 bit frame counter
pad      = $12   ; 16 bit joypad state
scrollx  = $14   ; 16 bit
tmp      = $16
tmp2     = $18
index    = $1a
nmiflag  = $1f
OAM      = $0200 ; 544 byte sprite table mirror

.segment "CODE"

.a8
.i16
reset:
  sei
  clc
  xce                   ; native mode
  rep #$38              ; 16 bit a/x/y, decimal off
  .a16
  ldx #$1fff
  txs
  lda #$0000
  tcd
  sep #$20
  .a8
  lda #$8f
  sta $2100             ; screen off
  stz $4200             ; no interrupts yet
  stz $420b
  stz $420c

  ; clear the variables
  ldx #$0000
: stz $00,x
  inx
  cpx #$0020
  bne :-

  ; 8 KB that will sit in work RAM untouched
  rep #$20
  .a16
  lda #$1fff
  ldx #.loword(blob)
  ldy #$2000
  mvn #$00, #$7e        ; from bank 00 to bank 7e
  sep #$20
  .a8
  phk
  plb                   ; mvn changed the data bank

  jsr apu_upload

  ; ---- video setup ----
  lda #$09
  sta $2105             ; mode 1, bg3 on top
  lda #$61
  sta $2107             ; bg1 map at $6000, 64x32
  lda #$68
  sta $2108             ; bg2 map at $6800
  lda #$6c
  sta $2109             ; bg3 map at $6c00
  stz $210b             ; bg1/bg2 tiles at $0000
  lda #$01
  sta $210c             ; bg3 tiles at $1000
  lda #$02
  sta $2101             ; sprites 8x8/16x16, tiles at $4000
  lda #$80
  sta $2115             ; vram: increment after the high byte

  ; palette
  stz $2121
  ldx #.loword(palette)
  ldy #512
  lda #$22
  jsr dma_to_ppu

  ldx #$0000
  stx $2116
  ldx #.loword(tiles4)
  ldy #4096
  jsr dma_to_vram
  ldx #$1000
  stx $2116
  ldx #.loword(tiles2)
  ldy #1024
  jsr dma_to_vram
  ldx #$4000
  stx $2116
  ldx #.loword(sprites)
  ldy #2048
  jsr dma_to_vram
  ldx #$6000
  stx $2116
  ldx #.loword(bg1map)
  ldy #4096
  jsr dma_to_vram
  ldx #$6800
  stx $2116
  ldx #.loword(bg2map)
  ldy #2048
  jsr dma_to_vram
  ldx #$6c00
  stx $2116
  ldx #.loword(bg3map)
  ldy #2048
  jsr dma_to_vram

  ; hide all sprites, then make the first 24 of them 16x16
  ldx #$0000
  lda #$f0
: sta OAM+1,x
  inx
  inx
  inx
  inx
  cpx #$0200
  bne :-
  ldx #$0000
: stz OAM+$200,x
  inx
  cpx #$0020
  bne :-
  lda #$aa              ; size bit set for 4 sprites per byte
  ldx #$0000
: sta OAM+$200,x
  inx
  cpx #$0006
  bne :-

  lda #$17
  sta $212c             ; main screen: bg1 bg2 bg3 sprites
  lda #$02
  sta $212d             ; sub screen: bg2
  stz $2130             ; colour math against the fixed colour
  lda #$20
  sta $2131             ; ...added to the backdrop: the sky gradient

  ; hdma channel 1: one byte per entry to the fixed colour register
  stz $4310
  lda #$32
  sta $4311
  ldx #.loword(gradient)
  stx $4312
  stz $4314
  lda #$02
  sta $420c

  lda #$81
  sta $4200             ; nmi + automatic joypad read
  lda #$0f
  sta $2100             ; screen on, full brightness

main:
  ; the usual way games wait for the next frame: poll a flag set by the nmi
: lda nmiflag
  beq :-
  stz nmiflag
  jsr update
  bra main

; ---- once per frame, outside the interrupt: move the sprites ----
update:
  rep #$30
  .a16
  .i16
  stz index
  ldx #$0000
@loop:
  sep #$20
  .a8
  ; x = sine[frame*2 + index*11]
  lda index
  asl
  asl
  asl
  adc index
  adc index
  adc index
  sta tmp
  lda frame
  asl
  clc
  adc tmp
  rep #$10
  phx
  rep #$20
  .a16
  and #$00ff
  tay
  sep #$20
  .a8
  lda sine,y
  lsr
  clc
  adc #$30
  plx
  sta OAM,x
  ; y = 48 + sine[frame + index*23] / 2
  lda index
  asl
  asl
  asl
  asl
  clc
  adc index
  adc index
  adc index
  adc index
  adc index
  adc index
  adc index
  clc
  adc frame
  phx
  rep #$20
  .a16
  and #$00ff
  tay
  sep #$20
  .a8
  lda sine,y
  lsr
  clc
  adc #$28
  plx
  sta OAM+1,x
  ; tile: ball, star or box
  lda index
  and #$03
  cmp #$03
  bne :+
  lda #$00
: asl
  sta OAM+2,x
  ; attributes: priority 2 or 3, palette from the index
  lda index
  and #$07
  asl
  ora #$20
  sta tmp
  lda index
  and #$08
  asl
  ora tmp
  sta OAM+3,x
  inx
  inx
  inx
  inx
  inc index
  lda index
  cmp #24
  beq @done
  jmp @loop
@done:
  ; battery ram: one byte every 64 frames
  lda frame
  and #$3f
  bne :+
  rep #$20
  .a16
  lda frame
  lsr
  lsr
  lsr
  lsr
  lsr
  lsr
  and #$00ff
  tax
  sep #$20
  .a8
  lda frame+1
  sta $700000,x
:
  sep #$20
  .a8
  rts

; ---- vertical blank ----
nmi:
  rep #$30
  .a16
  .i16
  pha
  phx
  phy
  phb
  phd
  lda #$0000
  tcd
  sep #$20
  .a8
  phk
  plb
  lda $4210             ; acknowledge

  ; sprite table
  stz $2102
  stz $2103
  ldx #OAM
  ldy #544
  lda #$04
  jsr dma_to_ppu

  ; wait for the automatic joypad read, then fetch the buttons
: lda $4212
  and #$01
  bne :-
  rep #$20
  .a16
  lda $4218
  sta pad
  inc frame
  ; scrolling: forward on its own, faster with right, backwards with left
  lda scrollx
  inc
  sta scrollx
  lda pad
  bit #$0100            ; right
  beq :+
  inc scrollx
  inc scrollx
: bit #$0200            ; left
  beq :+
  dec scrollx
  dec scrollx
  dec scrollx
  dec scrollx
:
  sep #$20
  .a8
  lda scrollx
  sta $210d
  lda scrollx+1
  sta $210d             ; bg1 horizontal scroll
  rep #$20
  .a16
  lda scrollx
  lsr
  lsr
  sep #$20
  .a8
  sta $210f
  xba
  sta $210f             ; bg2 scrolls four times slower
  lda #$ff
  sta $210e
  stz $210e             ; bg1/bg2/bg3 one line up, the usual
  sta $2110
  stz $2110
  sta $2112
  stz $2112

  ; holding A makes the ground translucent over the hills
  lda pad
  and #$80
  beq :+
  lda #$02
  sta $2130             ; maths against the sub screen (bg2)...
  lda #$61              ; ...averaged with bg1 and the backdrop
  bra :++
: stz $2130
  lda #$20
: sta $2131

  ; hud: frame counter and buttons as 4 hex digits each
  ldx #$6c68
  stx $2116
  rep #$20
  .a16
  lda frame
  sta tmp
  sep #$20
  .a8
  lda #$3c
  sta tmp2
  jsr hex4
  ldx #$6c56
  stx $2116
  rep #$20
  .a16
  lda pad
  sta tmp
  sep #$20
  .a8
  lda #$38
  sta tmp2
  jsr hex4

  lda #$01
  sta nmiflag
  rep #$30
  .a16
  .i16
  pld
  plb
  ply
  plx
  pla
  rti

; writes tmp (16 bit) as 4 hex digit tiles with attribute tmp2
.a8
.i16
hex4:
  lda tmp+1
  lsr
  lsr
  lsr
  lsr
  jsr digit
  lda tmp+1
  jsr digit
  lda tmp
  lsr
  lsr
  lsr
  lsr
  jsr digit
  lda tmp
digit:
  and #$0f
  inc                   ; tile 1 is '0'
  sta $2118
  lda tmp2
  sta $2119
  rts

; x = source, y = length, a = destination register ($21xx), fixed address
dma_to_ppu:
  sta $4301
  stz $4300
  stx $4302
  stz $4304
  sty $4305
  lda #$01
  sta $420b
  rts

; x = source, y = length: two registers ($2118/$2119), words
dma_to_vram:
  lda #$01
  sta $4300
  lda #$18
  sta $4301
  stx $4302
  stz $4304
  sty $4305
  lda #$01
  sta $420b
  rts

; The standard boot conversation with the audio CPU: wait until it says it
; is ready, send a block of 16 bytes, then tell it to run them. Real games
; upload their whole sound driver this way and hang if nobody answers.
apu_upload:
  rep #$20
  .a16
  lda #$bbaa
: cmp $2140
  bne :-
  sep #$20
  .a8
  lda #$01
  sta $2141             ; data follows
  ldx #$0200
  stx $2142             ; destination in audio ram
  lda #$cc
  sta $2140
: cmp $2140
  bne :-
  ldx #$0000
@byte:
  lda sine,x            ; any 16 bytes will do
  sta $2141
  txa
  sta $2140             ; counter
: cmp $2140
  bne :-
  inx
  cpx #$0010
  bne @byte
  stz $2141             ; no more data: run
  ldx #$0200
  stx $2142
  txa
  clc
  adc #$12
  sta $2140
: cmp $2140
  bne :-
  rts

dummy:
  rti

.segment "RODATA"
; fixed colour per group of 8 lines: blue fades in towards the horizon
gradient:
  .repeat 28, i
  .byte 8, $80 | (i / 2), 8 | 0, $40 | (i / 4)
  .endrepeat
  .byte 0
palette: .incbin "palette.bin"
tiles4:  .incbin "tiles4.bin"
tiles2:  .incbin "tiles2.bin"
sprites: .incbin "sprites.bin"
bg1map:  .incbin "bg1map.bin"
bg2map:  .incbin "bg2map.bin"
bg3map:  .incbin "bg3map.bin"
sine:    .incbin "sine.bin"
blob:    .incbin "blob.bin"

.segment "HEADER"
  .byte "NWSNES TEST ROM      "   ; 21 characters
  .byte $20                      ; LoROM, slow
  .byte $02                      ; rom + ram + battery
  .byte $05                      ; 32 KB
  .byte $01                      ; 2 KB of battery ram
  .byte $01                      ; region: north america (ntsc)
  .byte $00
  .byte $00
  .word $ffff                    ; checksum complement, fixed up after linking
  .word $0000                    ; checksum

.segment "VECTORS"
  .word 0, 0, dummy, dummy, dummy, nmi, 0, dummy       ; native
  .word 0, 0, dummy, 0, dummy, dummy, reset, dummy     ; emulation
