bits 16
org 0

start:
    ; don't rely on reset defaults, set ds ourselves
    xor ax, ax
    mov ds, ax

    mov dx, 0x3f8              ; COM1, reused for every 'out' below

    ; --- near write: just past our own code, segment 0 ---
    mov byte [0x200], 0xAA      ; guest phys 0x200

    ; --- far write: near the top of backed RAM, different segment ---
    mov ax, 0x9000              ; segment 0x9000 * 16 = 0x90000
    mov ds, ax
    mov byte [0xFFF0], 0xBB     ; 0x90000 + 0xFFF0 = 0x9FFF0 (still < 0xA0000)

    ; visible "still alive" signal, not a computed value like phase 3/4
    mov al, '5'
    out dx, al
    mov al, 0x0a
    out dx, al

    ; --- deliberate out-of-region touch ---
    ; host only registers RAM up to 0xA0000, so this address has no
    ; backing memory region at all. should trigger KVM_EXIT_MMIO,
    ; not corrupt memory or hang.
    mov ax, 0xA000               ; segment 0xA000 * 16 = 0xA0000 exactly
    mov ds, ax
    mov byte [0x0], 0xCC         ; guest phys 0xA0000 (classic VGA start)

    hlt