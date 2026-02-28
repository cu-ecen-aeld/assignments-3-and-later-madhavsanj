# Assignment 7: Faulty driver kernel oops analysis
## Command used echo "hello_world" > /dev/faulty

```
Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
Mem abort info:
  ESR = 0x0000000096000045
  EC = 0x25: DABT (current EL), IL = 32 bits
  SET = 0, FnV = 0
  EA = 0, S1PTW = 0
  FSC = 0x05: level 1 translation fault
Data abort info:
  ISV = 0, ISS = 0x00000045
  CM = 0, WnR = 1
user pgtable: 4k pages, 39-bit VAs, pgdp=0000000041bca000
[0000000000000000] pgd=0000000000000000, p4d=0000000000000000, pud=0000000000000000
Internal error: Oops: 0000000096000045 [#2] SMP
Modules linked in: hello(O) faulty(O) scull(O)
CPU: 0 PID: 127 Comm: sh Tainted: G      D    O       6.1.44 #1
Hardware name: linux,dummy-virt (DT)
pstate: 80000005 (Nzcv daif -PAN -UAO -TCO -DIT -SSBS BTYPE=--)
pc : faulty_write+0x10/0x20 [faulty]
lr : vfs_write+0xc8/0x390
sp : ffffffc008de3d20
x29: ffffffc008de3d80 x28: ffffff8001de4f80 x27: 0000000000000000
x26: 0000000000000000 x25: 0000000000000000 x24: 0000000000000000
x23: 000000000000000c x22: 000000000000000c x21: ffffffc008de3dc0
x20: 000000555f18ae00 x19: ffffff8001c18f00 x18: 0000000000000000
x17: 0000000000000000 x16: 0000000000000000 x15: 0000000000000000
x14: 0000000000000000 x13: 0000000000000000 x12: 0000000000000000
x11: 0000000000000000 x10: 0000000000000000 x9 : 0000000000000000
x8 : 0000000000000000 x7 : 0000000000000000 x6 : 0000000000000000
x5 : 0000000000000001 x4 : ffffffc000787000 x3 : ffffffc008de3dc0
x2 : 000000000000000c x1 : 0000000000000000 x0 : 0000000000000000
Call trace:
 faulty_write+0x10/0x20 [faulty]
 ksys_write+0x74/0x110
 __arm64_sys_write+0x1c/0x30
 invoke_syscall+0x54/0x130
 el0_svc_common.constprop.0+0x44/0xf0
 do_el0_svc+0x2c/0xc0
 el0_svc+0x2c/0x90
 el0t_64_sync_handler+0xf4/0x120
 el0t_64_sync+0x18c/0x190
Code: d2800001 d2800000 d503233f d50323bf (b900003f) 
---[ end trace 0000000000000000 ]---
```

## Explanation
-The kernel attempted to access memory at virtual address 0x0000000000000000, which is the NULL address. 

-The “Data abort info” shows WnR = 1, meaning the fault happened on a write access. So the driver attempted to write through a NULL pointer.
Modules linked in: ... faulty(O) ... shows the faulty module is loaded.

-The program counter is inside the module:
pc : faulty_write+0x10/0x20 [faulty]
-From This means the crash occurred 16 bytes into faulty_write, whose total size is 0x20 bytes. The invalid access is therefore very early in faulty_write and corresponds to the instruction shown in the “Code:” line.

To map this to a source line, you can rebuild the module with debug symbols and use objdump or addr2line on faulty.ko. For example (host-side cross tools):

Disassemble with source:

aarch64-linux-gnu-objdump -dS faulty.ko | less

Find faulty_write and locate the instruction at offset +0x10.

Or map an address to a line (if you have the right address/symbols):

aarch64-linux-gnu-addr2line -e faulty.ko <address>

Because this lab’s faulty driver is intentionally broken, the NULL dereference is typically an explicit write through a NULL pointer in faulty_write (e.g., writing to *(char *)0 = ... or similar). The oops confirms the bug is exactly that invalid write.

4) Extra context from the trace

EC = 0x25: DABT indicates a data abort on ARM64 in the current exception level (kernel).

FSC = 0x05: level 1 translation fault indicates the MMU could not translate the virtual address (because it is NULL / unmapped).

The process was sh (PID 127), which makes sense because echo is executed via a shell.
