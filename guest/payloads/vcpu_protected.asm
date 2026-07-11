bits 16
org 0

start:
    xor ax, ax
    mov ds, ax ; DS.base = 0

    ; BX = load base, set by hypervisor (vcpu_setup_regs stores rip in rbx).
    ; SP = load_base + 0xFF0, set by hypervisor

    ; patch gdt_descriptor's 32-bit base to the actual physical address of gdt_start
    xor eax, eax
    mov ax, bx
    add eax, gdt_start ; eax = physical address of gdt_start
    mov [bx + gdt_descriptor + 2], eax

    ; load GDT (DS.base=0, so [bx + offset] = physical address)
    lgdt [bx + gdt_descriptor]

    ; enable protected mode
    mov eax, cr0
    or eax, 1
    mov cr0, eax

    ; far-jump to protected_mode_entry.
    xor eax, eax
    mov ax, bx
    add eax, protected_mode_entry ; eax = physical address of protected_mode_entry
    push dword 0x00000008 ; CS selector (pushed as dword for 32-bit retf)
    push eax ; EIP
    db 0x66, 0xcb ; 32-bit far return (operand-size override + retf)

bits 32
protected_mode_entry:
    mov ax, 0x10 ; flat data segment
    mov ds, ax
    mov es, ax
    mov ss, ax
    lea esp, [ebx + 0xF00] ; per-VCPU stack top (ebx = load_base, preserved across far jump)

    mov dx, 0x3f8
    mov al, '1'
    out dx, al
    mov al, 0x0a
    out dx, al

    hlt

align 8
gdt_start:
    dq 0x0000000000000000 ; null descriptor (mandatory)
    dw 0xFFFF, 0x0000 ; code: limit_low, base_low
    db 0x00, 0x9A, 0xCF, 0x00; code: base_mid, access, flags+limit_hi, base_hi
    dw 0xFFFF, 0x0000 ; data: limit_low, base_low
    db 0x00, 0x92, 0xCF, 0x00 ; data: base_mid, access, flags+limit_hi, base_hi
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1 ; GDT limit
    dd gdt_start ; GDT base — patched at runtime to physical address