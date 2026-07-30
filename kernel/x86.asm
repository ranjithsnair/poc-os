; Routines to let C code use special x86 instructions.
;
; These used to be GCC "static inline" functions containing inline
; assembly (one per instruction) directly in include/x86.h - the compiler
; inlined them at every call site with no call overhead. Written out here
; as real NASM functions instead, they are now ordinary out-of-line calls
; using the standard 32-bit cdecl convention: arguments are on the stack
; above the return address ([esp+4], [esp+8], ...), the return value (if
; any) comes back in eax, and eax/ecx/edx are free to clobber while
; ebx/esi/edi/ebp must be restored before returning.
;
; A handful of these names (stosb, lgdt, lidt, ltr, cli, sti, xchg) are
; themselves x86 instruction mnemonics. NASM's parser treats the bare word
; as that instruction even when it's meant as a label, so those labels are
; written with a leading "$" to force plain-identifier parsing; it does not
; change the symbol name that ends up in the object file.

BITS 32
section .text

; uchar inb(ushort port)
global inb
inb:
  mov dx, [esp+4]
  in al, dx
  movzx eax, al
  ret

; void insl(int port, void *addr, int cnt)
global insl
insl:
  push edi
  mov dx, [esp+8]     ; port
  mov edi, [esp+12]   ; addr
  mov ecx, [esp+16]   ; cnt
  cld
  rep insd
  pop edi
  ret

; void outb(ushort port, uchar data)
global outb
outb:
  mov dx, [esp+4]     ; port
  mov al, [esp+8]     ; data
  out dx, al
  ret

; void outw(ushort port, ushort data)
global outw
outw:
  mov dx, [esp+4]     ; port
  mov ax, [esp+8]     ; data
  out dx, ax
  ret

; void outsl(int port, const void *addr, int cnt)
global outsl
outsl:
  push esi
  mov dx, [esp+8]     ; port
  mov esi, [esp+12]   ; addr
  mov ecx, [esp+16]   ; cnt
  cld
  rep outsd
  pop esi
  ret

; void stosb(void *addr, int data, int cnt)
global $stosb
$stosb:
  push edi
  mov edi, [esp+8]    ; addr
  mov eax, [esp+12]   ; data
  mov ecx, [esp+16]   ; cnt
  cld
  rep stosb
  pop edi
  ret

; void stosl(void *addr, int data, int cnt)
global stosl
stosl:
  push edi
  mov edi, [esp+8]    ; addr
  mov eax, [esp+12]   ; data
  mov ecx, [esp+16]   ; cnt
  cld
  rep stosd
  pop edi
  ret

; void lgdt(struct segdesc *p, int size)
; Builds the 6-byte {limit:16, base:32} pseudo-descriptor LGDT expects
; in a small scratch area on the stack, then loads it.
global $lgdt
$lgdt:
  push ebp
  mov ebp, esp
  sub esp, 6
  mov eax, [ebp+12]   ; size
  dec eax
  mov [ebp-6], ax      ; pd.limit = size-1
  mov eax, [ebp+8]    ; p
  mov [ebp-4], eax     ; pd.base = p
  lgdt [ebp-6]
  mov esp, ebp
  pop ebp
  ret

; void lidt(struct gatedesc *p, int size)
; Same layout trick as lgdt, above, but for the IDT register.
global $lidt
$lidt:
  push ebp
  mov ebp, esp
  sub esp, 6
  mov eax, [ebp+12]   ; size
  dec eax
  mov [ebp-6], ax      ; pd.limit = size-1
  mov eax, [ebp+8]    ; p
  mov [ebp-4], eax     ; pd.base = p
  lidt [ebp-6]
  mov esp, ebp
  pop ebp
  ret

; void ltr(ushort sel)
global $ltr
$ltr:
  mov ax, [esp+4]
  ltr ax
  ret

; uint readeflags(void)
global readeflags
readeflags:
  pushfd
  pop eax
  ret

; void loadgs(ushort v)
global loadgs
loadgs:
  mov ax, [esp+4]
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
  mov ecx, [esp+4]    ; addr
  mov eax, [esp+8]    ; newval
  lock xchg [ecx], eax
  ret

; void clearlock(volatile uint *p)
; Sets *p to 0 with a single plain store. Used by spinlock.c's release()
; instead of a C assignment: the call itself (like the inline asm it
; replaces) acts as a compiler barrier, so the store can't be reordered
; or elided relative to the __sync_synchronize() release fence just
; before it.
global clearlock
clearlock:
  mov eax, [esp+4]
  mov dword [eax], 0
  ret

; uint rcr2(void)
global rcr2
rcr2:
  mov eax, cr2
  ret

; void lcr3(uint val)
global lcr3
lcr3:
  mov eax, [esp+4]
  mov cr3, eax
  ret
