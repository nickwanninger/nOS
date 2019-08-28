
global _start
global start

;; starting page tables, statically allocated in the elf
extern p4_table
extern p3_table
extern p2_table
extern p1_table

extern boot_stack_end ;; static memory from the binary where the stack begins
extern kmain ;; c entry point


; Declare constants for the multiboot header.
MBALIGN  equ  1 << 0            ; align loaded modules on page boundaries
MEMINFO  equ  1 << 1            ; provide memory map
FLAGS    equ  MBALIGN | MEMINFO ; this is the Multiboot 'flag' field
MAGIC    equ  0x1BADB002        ; 'magic number' lets bootloader find the header
CHECKSUM equ -(MAGIC + FLAGS)   ; checksum of above, to prove we are multiboot




;; Multiboot header
section .mbhdr
align 8
header_start:
align 4
	dd MAGIC
	dd FLAGS
	dd CHECKSUM
header_end:



section .boot


[bits 32]
start:
_start:
	cli ; disable interrupts

	mov edi, ebx
	mov esi, eax

	mov eax, gdtr32
	lgdt [eax] ; load GDT register with start address of Global Descriptor Table

	; mov eax, cr0
	; or al, 1       ; set PE (Protection Enable) bit in CR0 (Control Register 0)
	; mov cr0, eax

	;; Perform a long-jump to the 32 bit protected mode
	jmp 0x8:gdt1_loaded

;; the 32bit protected mode gdt was loaded, now we need to go about setting up
;; paging for the 64bit long mode
[bits 32]
gdt1_loaded:
	mov eax, 0x10
	mov ds, ax
	mov ss, ax

	;; setup a starter stack
	mov esp, boot_stack_end - 1
	mov ebp, esp

	;; id map the first 512 pages of ram, so lowkern can work with paging enabled
	call map_lowkern_basic

	;; setup and initialize basic paging
	call longmode_setup

	call paging_setup




	;; now our long mode GDT
	mov eax, gdtr64
	lgdt [eax]

	jmp 0x8:gdt2_loaded




[bits 64]
gdt2_loaded:

	;; now that we are here, the second gdt is loaded and we are in 64bit long mode
	;; and paging is enabled.
	mov eax, 0x10
	mov ds, ax
	mov es, ax
	mov ss, ax
	mov fs, ax
	mov gs, ax

	;; Reset the stack to the initial state
	mov rsp, boot_stack_end - 1
	mov rbp, rsp




	;; and call the c code in boot.c
	call kmain
	hlt
	;; Ideally, we would not get here, but if we do we simply hlt spin


	;; just move a special value into eax, so we can see in the state dumps
	;; that we got here.
	mov rax, qword 0xfcfc

.spin:
	hlt
	jmp .spin





bits 32



;; The startup sequence of the kernel identity maps with 4kb pages, just so we can
;; have all the things we need mapped right away. We will later replace this basic
;; mapping with a better one when we are in C
map_lowkern_basic:

	;; p4_table[0] -> p3_table
	mov eax, p3_table
	or eax, 0x3
	mov [p4_table], eax

	;; p3_table[0] -> p2_table
	mov eax, p2_table
	or eax, 0x3
	mov [p3_table], eax

	;; p2_table[0] -> p1_table
	mov eax, p1_table
	or eax, 0x3
	mov [p2_table], eax

	;; ident map the first 512 pages
	mov ecx, 512
	mov edx, p1_table
	mov eax, 0x3
	;; starting at 0x00

.write_pde:

	mov [edx], eax
	add eax, 4096
	add edx, 0x8 ;; shift the dst by 8 bytes (size of addr)
	loop .write_pde

	xor eax, eax
	ret



longmode_setup:
	mov eax, p4_table
	mov cr3, eax

	;; enable PAE
	mov eax, cr4
	or eax, 1 << 5
	mov cr4, eax

	;; enable lme bit in MSR
	mov ecx, 0xC0000080          ; Set the C-register to 0xC0000080, which is the EFER MSR.
	rdmsr                        ; Read from the model-specific register.
	or eax, 1 << 8               ; Set the LM-bit which is the 9th bit (bit 8).
	wrmsr                        ; Write to the model-specific register.
	ret


paging_setup:

	;; paging enable (set the 31st bit of cr0)
	mov eax, cr0
	or eax, 1 << 31
	mov cr0, eax

	;; make sure we are in "normal cache mode"
	mov ebx, ~(3 << 29)
	and eax, ebx
	ret


align 8
gdt32:
	dq 0x0000000000000000 ;; null
	dq 0x00cf9a000000ffff ;; code
	dq 0x00cf92000000ffff ;; data

align 8
gdt64:
	dq 0x0000000000000000 ;; null
	dq 0x00af9a000000ffff ;; code (note lme bit)
	dq 0x00af92000000ffff ;; data (most entries don't matter)


align 8
gdtr32:
	dw 23
	dd gdt32


align 8
global gdtr64
gdtr64:
	dw 23
	dq gdt64



print_regs:
	out 0x3f, eax
	ret
set_start:
	rdtsc
	out 0xfd, eax
	ret
print_time:
	rdtsc
	out 0xfe, eax
	ret