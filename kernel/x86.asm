; Routines to let C code use special x86 instructions, using the SysV
; AMD64 calling convention (arguments in rdi, rsi, rdx, rcx, r8, r9,
; not the stack). A couple of instructions - lgdt/lidt, rcr2/lcr3 -
; genuinely behave differently in 64-bit mode (a 10-byte pseudo-
; descriptor instead of 6; a 64-bit CR2/CR3 instead of 32-bit), not
; just differently-sized.
;
; A "$" - prefixed - label is used for a handful of these names since
; they're themselves x86 instruction mnemonics that NASM's parser
; would otherwise swallow.

BITS 64
section .text

; uchar inb(ushort port)
global inb
inb:
  mov dx, di
  in al, dx
  movzx eax, al
  ret

; void insl(int port, void *addr, int cnt)
global insl
insl:
  mov r10, rdi    ; stash port (rdi is about to become the destination)
  mov rdi, rsi    ; addr
  mov rcx, rdx    ; cnt
  mov dx, r10w    ; port
  cld
  rep insd
  ret

; void outb(ushort port, uchar data)
global outb
outb:
  mov dx, di
  mov al, sil
  out dx, al
  ret

; void outw(ushort port, ushort data)
global outw
outw:
  mov dx, di
  mov ax, si
  out dx, ax
  ret

; void outsl(int port, const void *addr, int cnt)
global outsl
outsl:
  mov r10, rdi    ; stash port
  mov rcx, rdx    ; cnt
  mov dx, r10w    ; port
  cld
  rep outsd       ; source is [rsi], which already holds addr (arg 2)
  ret

; void stosb(void *addr, int data, int cnt)
global $stosb
$stosb:
  mov rcx, rdx    ; cnt
  mov eax, esi    ; data
  cld
  rep stosb       ; dest is [rdi], which already holds addr (arg 1)
  ret

; void stosl(void *addr, int data, int cnt)
global stosl
stosl:
  mov rcx, rdx    ; cnt
  mov eax, esi    ; data
  cld
  rep stosd       ; dest is [rdi], which already holds addr (arg 1)
  ret

; void lgdt(struct segdesc *p, int size)
; Builds the 10-byte {limit:16, base:64} pseudo-descriptor LGDT expects
; in 64-bit mode (wider than the 32-bit build's 6-byte {limit:16,
; base:32} - see x86.asm) in a small scratch area on the stack, then
; loads it.
global $lgdt
$lgdt:
  push rbp
  mov rbp, rsp
  sub rsp, 16
  mov eax, esi      ; size
  dec eax
  mov [rbp-16], ax   ; pd.limit = size-1
  mov [rbp-14], rdi  ; pd.base = p
  lgdt [rbp-16]
  mov rsp, rbp
  pop rbp
  ret

; void lidt(struct gatedesc *p, int size)
; Same layout trick as lgdt, above, but for the IDT register.
global $lidt
$lidt:
  push rbp
  mov rbp, rsp
  sub rsp, 16
  mov eax, esi
  dec eax
  mov [rbp-16], ax
  mov [rbp-14], rdi
  lidt [rbp-16]
  mov rsp, rbp
  pop rbp
  ret

; void ltr(ushort sel)
global $ltr
$ltr:
  mov ax, di
  ltr ax
  ret

; uint readeflags(void)
global readeflags
readeflags:
  pushfq
  pop rax
  ret

; void loadgs(ushort v)
global loadgs
loadgs:
  mov ax, di
  mov gs, ax
  ret

; void cli(void)
global $cli
$cli:
  cli
  ret

; void sti(void)
global $sti
$sti:
  sti
  ret

; uint xchg(volatile uint *addr, uint newval)
; Atomically swaps *addr with newval and returns the old value; used by
; the spinlock implementation.
global $xchg
$xchg:
  mov eax, esi
  lock xchg [rdi], eax
  ret

; void clearlock(volatile uint *p)
; Sets *p to 0 with a single plain store. Used by spinlock.c's release()
; instead of a C assignment: the call itself (like the inline asm it
; replaces) acts as a compiler barrier, so the store can't be reordered
; or elided relative to the __sync_synchronize() release fence just
; before it.
global clearlock
clearlock:
  mov dword [rdi], 0
  ret

; uintp rcr2(void)
; CR2 (the faulting address on a page fault) is a full 64-bit register
; in long mode - unlike the 32-bit build's rcr2, this returns the whole
; thing in rax, not just eax.
global rcr2
rcr2:
  mov rax, cr2
  ret

; void lcr3(uintp val)
global lcr3
lcr3:
  mov cr3, rdi
  ret

; void wrmsr(uint msr, uint64 val)
; wrmsr itself takes the MSR index in ecx and the 64-bit value split as
; edx:eax (high:low), not as one 64-bit register - the "$" prefix is the
; same reserved-mnemonic dodge as $lgdt/$ltr/etc above.
global $wrmsr
$wrmsr:
  mov ecx, edi   ; msr
  mov rax, rsi   ; val (also sets eax = val's low 32 bits)
  mov rdx, rax
  shr rdx, 32    ; edx = val's high 32 bits
  wrmsr
  ret
