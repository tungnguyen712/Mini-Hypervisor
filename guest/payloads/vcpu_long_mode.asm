bits 16
org 0

start:
    xor ax, ax
    mov ds, ax ; DS.base = 0

    ; BX = load base, set by hypervisor (vcpu_setup_regs stores rip in rbx).
    ; SP = load_base + 0xFF0, also set by hypervisor -> no call/pop trick needed.

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

    ; far-jump to protected_mode_entry (position-independent via 32-bit retf).
    xor eax, eax
    mov ax, bx
    add eax, protected_mode_entry ; eax = physical address of protected_mode_entry
    push dword 0x00000008; CS selector (pushed as dword for 32-bit retf)
    push eax ; EIP
    db 0x66, 0xcb ; 32-bit far return (operand-size override + retf)

bits 32
protected_mode_entry:
    mov ax, 0x10 ; flat data segment
    mov ds, ax
    mov es, ax
    mov ss, ax
    lea esp, [ebx + 0xF00] ; per-VCPU stack top (ebx = load_base, preserved across far jump)

    ; --- identity-mapped page tables at fixed physical addresses ---
    ; PML4 @ 0x5000, PDPT @ 0x6000, PD @ 0x7000.
    ; These are above all VCPU load addresses (VCPU0=0x0, VCPU1=0x1000,
    ; VCPU2=0x3000, VCPU3=0x4000) and within GUEST_MEM_SIZE (0x10000).
    ; All VCPUs write the same values here
    ; each VCPU writes all three entries before loading CR3.
    mov dword [0x5000], 0x6003     ; PML4[0] = PDPT base (0x6000) | Present | RW
    mov dword [0x5004], 0x0
    mov dword [0x6000], 0x7003     ; PDPT[0] = PD base (0x7000) | Present | RW
    mov dword [0x6004], 0x0
    mov dword [0x7000], 0x83       ; PD[0] = Present | RW | PS -> 2MB page, base=0
    mov dword [0x7004], 0x0

    mov eax, 0x5000
    mov cr3, eax ; CR3 = PML4 physical address

    mov eax, cr4
    or eax, 0x20 ; CR4.PAE = 1
    mov cr4, eax

    ; EFER.LME = 1 is pre-set by the host via KVM_SET_SREGS (in vcpu_setup_regs).
    ; WSL2/Hyper-V intercepts EFER and rejects guest wrmsr to it, so the
    ; host sets LME before the VCPU runs. The guest only needs to flip CR0.PG.

    mov eax, cr0
    or eax, 0x80000000 ; CR0.PG = 1 -> LMA = LME & CR4.PAE & PG -> long mode active
    mov cr0, eax

    ; far-jump to long_mode_entry (position-independent).
    ; Same retf trick: in 32-bit mode retf pops EIP then CS (both 32-bit).
    mov eax, ebx
    add eax, long_mode_entry ; eax = physical address of long_mode_entry
    push dword 0x00000018 ; CS selector (64-bit code segment)
    push eax ; EIP
    retf ; loads CS=0x18 (L=1) -> CPU enters 64-bit mode

bits 64
long_mode_entry:
    mov ax, 0x10 ; reuse flat data descriptor; base/limit ignored in long mode
    mov ds, ax
    mov es, ax
    mov ss, ax
    lea rsp, [rbx + 0xF00] ; per-VCPU stack top (rbx = load_base, preserved across far jumps)

    mov dx, 0x3f8
    mov al, '2'
    out dx, al
    mov al, 0x0a
    out dx, al

    hlt

align 8
gdt_start:
    dq 0x0000000000000000 ; null descriptor (mandatory)
    dw 0xFFFF, 0x0000 ; 32-bit code: limit_low, base_low
    db 0x00, 0x9A, 0xCF, 0x00 ; code: base_mid, access, flags+limit_hi, base_hi
    dw 0xFFFF, 0x0000 ; 32-bit data: limit_low, base_low
    db 0x00, 0x92, 0xCF, 0x00 ; data: base_mid, access, flags+limit_hi, base_hi
    dw 0x0000, 0x0000 ; 64-bit code: limit/base ignored in long mode
    db 0x00, 0x9A, 0x20, 0x00 ; 64-bit code: access=0x9A, flags=0x20 (L=1, D=0)
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1 ; GDT limit
    dd gdt_start ; GDT base — patched at runtime to physical address
