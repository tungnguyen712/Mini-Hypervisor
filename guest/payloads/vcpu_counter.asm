bits 16
org 0

%define COUNTER_ADDR 0x2000
%define LOOP_COUNT   1000

start:
    xor ax, ax
    mov ds, ax

    mov cx, LOOP_COUNT

.loop:
    lock inc word [COUNTER_ADDR]
    loop .loop

    hlt