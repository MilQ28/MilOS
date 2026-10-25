# Dual-Architecture Kernel Foundation

## Decision

Keep the existing x86-64 kernel as the working reference implementation.
Develop AArch64 as a separate platform port, not as a second set of conditionals
inside the x86 page-table and device code. The repository already has
architecture-specific compiler/linker settings, image names, Limine EFI
payloads, and `src/arch/$(ARCH)` source discovery. Do not reorganize all source
files just to establish this boundary.

The AArch64 target now builds a minimal, architecture-isolated kernel ELF and
Limine ISO. It includes only `src/arch/aarch64/` and the freestanding runtime;
the current x86-64 `main.cpp`, PMM, VMM, and tests are not linked into it. A
bounded Limine/QEMU boot on `virt` has reached the kernel and printed both
semihosting messages. This validates the minimal virtual boot path, not an
AArch64 memory manager or smartphone compatibility.

## Proposed source and build boundary

Retain `kernel/src/` for code that is genuinely architecture-neutral and add
platform implementations under:

```text
kernel/src/
  arch/
    x86_64/
      entry.S
      interrupts/
      serial.cpp
      vmm.cpp
    aarch64/
      entry.S
      exceptions.cpp
      serial.cpp
      vmm.cpp
  platform/
    boot_info.hpp
    console.hpp
```

The AArch64 build currently uses `src/arch/aarch64/entry.S` and
`src/arch/aarch64/main.cpp` as an isolated source set. Other architectures
continue to use common source discovery plus `src/arch/$(ARCH)`. Build outputs
are segregated by architecture (`obj-$(ARCH)` and `bin-$(ARCH)`); keep
`ARCH=x86_64` as the default. The destination tree is not a request to move
existing files wholesale. Shared code should receive validated boot
information and platform services through small typed interfaces, rather than
include architecture registers, Limine response globals, or device addresses
directly.

The root makefile already selects Limine EFI images, OVMF firmware and a QEMU
machine based on `ARCH`. For AArch64 it currently selects `virt`, `cortex-a72`,
`BOOTAA64.EFI`, and `ovmf-code-aarch64.fd`. This is a suitable initial virtual
platform; no physical-phone image or compatibility claim follows from it.

## Code reuse assessment

| Classification | Existing code | Notes |
|---|---|---|
| Architecture-independent and reusable | `memory.cpp` and `memory.hpp` | Freestanding byte operations use no CPU instructions. Keep their C ABI and verify generated code remains freestanding on both targets. |
| Architecture-independent and reusable | Fixed-width types, simple framebuffer color/pixel calculations | Reuse only after boot-provided framebuffer format, mapping, and bounds are validated per platform. |
| Reusable after refactoring | PMM bitmap and allocation policy in `pmm.cpp` | The bitmap algorithm is portable, but `init()` currently consumes Limine memory-map/HHDM directly and assumes 4 KiB pages. Separate allocator policy from boot-map ingestion and virtual access; reserve non-usable regions and its own metadata explicitly. |
| Reusable after refactoring | PMM/VMM boot diagnostics and kernel initialization in `main.cpp` | It directly owns Limine requests, global constructors, framebuffer output, PMM setup, and x86 VMM tests. Split boot-protocol acquisition, architecture startup, common initialization, and tests. |
| Reusable after refactoring | Logging API in `serial.hpp` | The API shape can be retained, but its current implementation is x86 COM1 port I/O and its non-x86 implementation silently discards output. Provide an explicit platform console backend and report initialization failure. |
| Architecture-specific | `vmm.cpp` | CR3/CR4, x86 paging entries, x86 feature discovery, `invlpg`, and x86 page-table validation must not be reused for AArch64. Implement an independent AArch64 translation-table backend. |
| Architecture-specific | `serial.hpp` hardware backend and `hcf()` | `inb`/`outb` and `hlt` are x86-specific; AArch64 uses memory-mapped devices and `wfi`. |
| Architecture-specific | Linker scripts, compiler flags, and entry/exception conventions | Keep separate scripts and flags. The AArch64 high-half ELF, split early stacks, and controlled exception vector have been exercised through Limine on QEMU `virt`. |
| Uncertain / investigate | Higher-half and HHDM assumptions on AArch64 | Limine specifies higher-half executable mappings and a discovered HHDM, but the actual AArch64 boot mappings and kernel link must be inspected at runtime before relying on particular address ranges. |
| Uncertain / investigate | Phone hardware, firmware, and boot chain | No target handset, SoC, device tree, firmware, secure-boot policy, or vendor boot protocol has been selected. QEMU `virt` is not a smartphone validation platform. |

There is no interrupt-controller setup, scheduler, or AArch64-specific memory
manager in the current sources. The exception-vector implementation and
bounded FDT parser have controlled QEMU tests; the runtime DTB path has been
verified using Limine's documented DTB override. The PMM and VMM checks are
kernel boot diagnostics (the VMM also contains synthetic tests), not a separate
host-test suite.

## Intended AArch64 entry and initialization contract

The first AArch64 port uses the repository's Limine protocol rather than direct
firmware entry. Its ELF `_start` shim aligns the Limine-provided stack to the
AAPCS64 16-byte boundary and calls the architecture-specific C++ entry.

The current request asks for Limine base revision 6. The checked-in protocol
specification states that AArch64 entry is little-endian AArch64 using AAPCS64
without FP/SIMD; SP is provided with at least 64 KiB; general registers
including X29/X30 are zero; and the entry must not return. At base revision 6,
entry may be at EL1 or at EL2 with VHE, depending on firmware handoff. Do not
assume EL1, enable FP/SIMD before configuring CPACR, or assume interrupts are
enabled. The vector installer reads `CurrentEL` and writes VBAR only for EL1
or EL2; other levels fail closed.

Limine enters with the MMU and instruction/data caches enabled. It provides
4 KiB translation granules; TTBR1_EL1 contains bootloader higher-half mappings.
At base revision 6, TCR_EL1 describes the implemented physical-address width
and 4 KiB granule. TTBR0_EL1 is unspecified for base revisions after 0; do not
use or replace it based on assumptions about identity mappings. Keep the
bootloader's mappings intact until an AArch64-specific paging design and tests
exist. Obtain the HHDM offset and memory map through validated Limine requests;
never bake in an HHDM address.

Initialization should proceed in this order:

1. Enter through an AArch64 assembly shim and align the Limine-provided stack.
   Check the exception level before EL-specific initialization and establish
   the early exception-vector base before enabling exceptions.
2. Validate Limine responses and form a small boot-information object containing
   the memory map, HHDM offset, DTB pointer, and optional framebuffer data.
3. Initialize a platform console, then common memory helpers and PMM metadata.
   Use the Limine memory map as the RAM authority; do not infer usable RAM from
   device-tree `memory@` nodes.
4. Keep the existing translation tables unchanged while implementing and
   testing AArch64 read-only translation. Later page-table mutation must use
   AArch64 descriptors, MAIR/TCR attributes, required barriers, and the
   appropriate local/remote TLB invalidation protocol.
5. Initialize broader exception/interrupt handling and devices only after
   vector and memory-attribute behavior is verified.

The entry shim partitions Limine's guaranteed minimum 64-KiB boot stack into
two 32-KiB regions: the normal stack begins at the midpoint and the same-EL
exception stack begins at the original stack top. It selects SP_EL1 or
SP_EL2 only long enough to initialize that bank, then returns to the normal
stack. This avoids relying on an unspecified SP_ELx value. Both stacks remain
in Limine bootloader-reclaimable memory and are not reclaimed by this kernel.
There is no guard page; each stack has a fixed 32-KiB budget.

The kernel installs a 2048-byte-aligned exception-vector table at the current
EL (EL1 or EL2) and captures general registers plus ESR/ELR/SPSR for synchronous
exceptions; FAR is read only for abort exception classes. Limine leaves VBAR
undefined, so the kernel does not rely on its entry value. Fatal exceptions
halt rather than returning into a repeated exception. The exception frame is
312 bytes in C++ and the assembly reserves 320 bytes to preserve 16-byte stack
alignment; compile-time assertions check the member offsets and size.

This remains single-CPU diagnostic code, not an SMP-safe exception subsystem.
There is no interrupt-controller setup, per-CPU exception stack, or remote
diagnostic coordination. The original opt-in UDF test faulted while saving its
frame: `ESR_EL1=0x96000047` decodes as a same-EL data abort (`EC=0x25`) with a
level-3 translation fault (`DFSC=0x07`), not an alignment fault. It arrived at
vector slot 4, which uses the same-EL SPx stack. Limine guarantees the entry
stack pointer but does not establish a separate SP_EL1 exception stack. The
first frame store follows `sub sp, sp, #320`; the observed fault address
`0xffffffff80008000` therefore implies an exception SPx of
`0xffffffff80008140` before the prologue. In the ELF captured when reproducing
the original fault (before subsequent code-layout changes), `.bss` starts at
`0xffffffff80007000` and occupies 0x19 bytes, while the faulting store is at
the next page boundary. This is consistent with an uninitialized SP_EL1
pointing outside the mapped kernel image; the actual pre-fix SP_EL1 register
was not independently sampled, so that value is derived from FAR and the
assembly sequence rather than a direct register dump.

The fixed, opt-in UDF test explicitly selects SP_EL1 before executing the
faulting instruction, so it exercises vector 4 and the initialized exception
stack. It records `ESR=0x02000000` (`EC=0`, unknown instruction),
`ELR=0xffffffff800036f4` (the UDF instruction), and `SPSR=0x600003c5`
(EL1h), then advances ELR and resumes to halt. The bounded QEMU run printed
`vector=0x4` and the success marker without a secondary exception.

## Device tree and serial output

The AArch64 image requests a DTB from Limine. The protocol returns its pointer
as an HHDM virtual pointer in bootloader-reclaimable memory, or a null response
when no DTB is available. Before reading its header, the kernel verifies that
the full 40-byte header is within a bootloader-reclaimable memory-map range;
after reading `totalsize`, it checks the entire blob range before parsing.
`fdt.cpp` checks header fields and block bounds, token and string bounds, node
depth, cell tuple lengths, overflow, and ambiguity. It translates `reg` through
non-empty `ranges` at every parent bus; empty `ranges` means identity, while
missing or unsupported translations fail closed. Synthetic fixtures cover
direct and translated addresses, nested buses, empty/missing/malformed ranges,
overflow, ambiguous matches, unsupported cell counts, and missing or malformed
`reg`. The DTB is for hardware discovery; use Limine's memory map for allocator
regions. Do not rely on `/chosen` properties or DTB `memory@` nodes (Limine may
remove them).

The default QEMU `virt` UEFI/Limine boot has no firmware DTB response, as
allowed by the protocol. A real handoff was separately verified with the
Limine v12.9.4 `dtb_path` local option, using a QEMU-generated `virt` DTB
(`qemu-system-aarch64 -machine virt,dumpdtb=...`). Limine returned the DTB
response, and the kernel verified its HHDM pointer and bootloader-reclaimable
buffer before parsing. The runtime result was PL011 `reg base=0x09000000`,
`size=0x1000`; QEMU's node did not provide a direct `clock-frequency`
property. This only validates discovery and address translation, not UART I/O.

The minimal QEMU kernel currently uses QEMU semihosting `SYS_WRITE0` for early
debug output rather than accessing the PL011 UART. This avoids assuming that a
physical MMIO address is identity-mapped or mapped with Device attributes; the
Limine AArch64 HHDM contract specifies Normal Write-Back attributes, which are
not a safe generic MMIO mapping. The root makefile enables semihosting for the
AArch64 QEMU run target. Native semihosting can expose host file operations, so
only run trusted test kernels with it enabled. This debug channel is not a
serial backend. No PL011 MMIO access has been attempted. A future UART backend
must establish/verify a Device-memory mapping without disturbing bootloader
tables.

## Build, boot, and test strategy

Preserve and continue to validate the x86-64 path with:

```sh
make TOOLCHAIN=llvm
timeout 20s make TOOLCHAIN=llvm run QEMUFLAGS="-m 2G -serial stdio"
```

The independent AArch64 build and boot commands are:

```sh
make TOOLCHAIN=llvm ARCH=aarch64
make TOOLCHAIN=llvm ARCH=aarch64 run QEMUFLAGS="-m 2G -serial stdio -display none"
```

The existing root makefile uses QEMU's `virt` machine with `cortex-a72`, UEFI
OVMF, a Limine AArch64 EFI executable, and a CD ISO. For debugging, add QEMU's
`-S -s` options and attach an AArch64-capable GDB to the QEMU stub; keep serial
output on a separate channel when needed. The test run must distinguish a
kernel that reaches its expected halt loop from a timeout before initialization.

Keep x86-64 boot tests unchanged. Add host tests for pure address arithmetic,
FDT parsing, and allocator policy, plus synthetic AArch64 table-walker tests
with controlled reads. Then add QEMU tests for Limine handoff, console output,
boot memory-map handling, and read-only translation. Never test page-table
mutation by replacing the live root. Only consider mapping tests in an isolated
root whose page-table allocations and cleanup are tracked.

Clang 21 and LLD 21 produce and link the AArch64 kernel ELF and ISO; the ELF is
a static AArch64 executable with `_start` as its entry and no undefined
symbols. The Limine AArch64 EFI image and AArch64 OVMF firmware are present.
QEMU 10.2.1 boots the image using `virt` and `cortex-a72`. The following
bounded checks were run:

```sh
make TOOLCHAIN=llvm
timeout 25s make TOOLCHAIN=llvm run QEMUFLAGS="-m 2G -serial stdio -display none -no-reboot -no-shutdown"

make TOOLCHAIN=llvm ARCH=aarch64
timeout 25s make TOOLCHAIN=llvm ARCH=aarch64 run QEMUFLAGS="-m 2G -serial stdio -display none -no-reboot -no-shutdown"

make -C kernel TOOLCHAIN=llvm ARCH=aarch64 clean
timeout 25s make TOOLCHAIN=llvm ARCH=aarch64 AARCH64_EXCEPTION_TEST=1 run QEMUFLAGS="-m 2G -serial stdio -display none -no-reboot -no-shutdown"

git diff --check
```

The x86-64 boot printed PMM and VMM validation success, including:

```text
[INFO] PMM validation tests passed
[INFO] VMM walker, mapping, and address-space tests passed
[INFO] Boot screen rendered
```

The normal AArch64 boot printed:

```text
MilOS AArch64 kernel entered via Limine
Exception vectors installed at EL0x1
FDT parser tests passed
AArch64 synthetic descriptor tests passed
CurrentEL=0x4
SCTLR_EL1=0x30D0199D
TCR_EL1=0x4B5103510
TTBR0_EL1=0xBBF68000
TTBR1_EL1=0xBBF67000
MAIR_EL1=0xFFFF
ID_AA64MMFR0_EL1=0x1124
MilOS AArch64: Limine DTB response unavailable
```

The separate exception build printed the expected UDF syndrome and resumed:

```text
AArch64 exception test captured ESR/ELR/SPSR
vector=0x4
ESR=0x2000000
ELR=0xFFFFFFFF800036F4
SPSR=0x600003C5
AArch64 controlled exception test passed
FDT parser tests passed
MilOS AArch64: Limine DTB response unavailable
```

The AArch64 read-only paging diagnostic is isolated in
`src/arch/aarch64/aarch64_paging.cpp`; it is not linked into x86-64. It reads
the active EL1-view registers and supports only the verified Limine 4-KiB,
four-level, 48-bit-VA regime with physical addresses up to 48 bits. Other
regimes fail closed. Table pages are dereferenced only through the Limine
HHDM after confirming that the complete page is in a bootloader-reclaimable
memory-map entry and that the resulting HHDM range is valid for TTBR1. The
scanner bounds traversal to 4096 table pages per root and reports inaccessible
or malformed tables as inconclusive rather than treating them as unmapped.
Synthetic descriptor fixtures cover invalid/unmapped entries, table, block and
page descriptors, offset preservation, AP/APTable permissions, PXN/UXN,
shareability, MAIR decoding, reserved address bits, reserved table-descriptor
bits, reserved shareability values, unsupported configuration, overflow, and
inaccessible table memory. A root-scan fixture also verifies a TTBR1 mapping
at root index 0; each AArch64 TTBR root covers its own full 512-entry range,
so the scanner must not apply an x86-style half-root split. Reserved encodings
are rejected rather than reported as valid mappings. These fixtures do not
establish live mapping behavior.

With the Limine v12.9.4 `dtb_path` override and a QEMU `virt` DTB generated by
`qemu-system-aarch64 -machine virt,dumpdtb=/tmp/milos-a64-walker-20261025/virt.dtb -cpu cortex-a72 -m 2G`,
a bounded runtime boot observed:

```text
CurrentEL=0x4
SCTLR_EL1=0x30D0199D
TCR_EL1=0x4B5103510
TTBR0_EL1=0xBBF68000
TTBR1_EL1=0xBBF67000
MAIR_EL1=0xFFFF
ID_AA64MMFR0_EL1=0x1124
Translation regime: 4KiB granule, 4 levels, VA=48 bits, PA=44 bits; SCTLR_EL1.M=1
HHDM offset=0xFFFF000000000000
TTBR0 root PA=0xBBF68000 HHDM VA=0xFFFF0000BBF68000 memmap type=0x5 region=0xBBF18000-0xBBFFE000
TTBR1 root PA=0xBBF67000 HHDM VA=0xFFFF0000BBF67000 memmap type=0x5 region=0xBBF18000-0xBBFFE000
DTB PL011 reg base=0x9000000 size=0x1000
PL011 active-table audit: unmapped
TTBR0 scan: complete; TTBR1 scan: complete; table pages scanned=0x206
unmapped physical gap base=0x9000000 size=0x1000
PL011 registers were not accessed; semihosting remains active
MilOS AArch64 diagnostics passed; halting
```

`CurrentEL=0x4` is EL1. `TCR_EL1` has `T0SZ=T1SZ=16`, `TG0=00` and `TG1=10`
(both 4-KiB), and `IPS=4`; `ID_AA64MMFR0_EL1.PARange=4` confirms 44-bit
physical addresses. The 48-bit virtual regime has four translation levels.
`SCTLR_EL1.M` is set. Both sampled TTBR roots resolved to type-5
bootloader-reclaimable pages, and both full active-root scans completed.
`MAIR_EL1=0xFFFF` means every AttrIndx byte in this run is `0xFF` (Normal
memory); no Device attribute was available in the observed MAIR value.

Thus the entire PL011 physical range was absent from the mappings found in
both active roots during this QEMU run; it was not merely absent from a guessed
HHDM or identity alias. No translation descriptor or system register was
written, and no PL011 address was dereferenced. This result is specific to the
tested Limine/QEMU boot, not a guarantee for other firmware or hardware.
Correct Device attributes alone would not establish that a peripheral is safe
to access; clock, reset, power, security state, and device-specific setup are
also outside this audit.

The normal AArch64 boot without `dtb_path` still reported that Limine did not
provide a DTB response, after printing the register snapshot and passing the
synthetic tests. The override boot confirmed runtime discovery and active-table
inspection independently of those fixtures. QEMU semihosting remains the
diagnostic output path, not PL011 serial. All bounded QEMU runs ended with
expected timeout exit 124 after the intentional `wfi` halt; success markers
were printed before the halt. This validates only QEMU `virt`, not a
smartphone.

## Isolated Device-map proposal experiment

There is no AArch64 physical-page allocator in the selected source set. The
existing PMM and VMM implementations are x86-64 implementations, are excluded
from the AArch64 image, and are not reused here. Although the active table pages
were readable through a validated Limine HHDM path for the prior audit, that is
not an allocation or ownership mechanism for new translation tables. Therefore
this experiment intentionally stops at synthetic tables backed by ordinary
kernel RAM. It does not create an address space with physical table pages,
publish a root, access any experimental virtual address, or support allocator
cleanup.

The synthetic proposal is limited to the observed regime: four-level,
4-KiB translation, 48-bit VA, with the runtime-observed 44-bit PA width. It
rejects other level/granule/address configurations, non-page-aligned ranges,
overflow/out-of-range PA values, non-page-size requests, and invalid or
non-canonical virtual addresses. The runtime FDT-provided PL011 base and size
are passed to the experiment; they are not the source of a hardcoded address.
The currently verified `0x1000` range is represented by exactly one 4-KiB
synthetic page descriptor, with adjacent virtual pages verified unmapped.

The proposed descriptor uses AttrIndx 0 and a *proposed, not active* MAIR
value whose byte 0 is `0x00` (Device-nGnRnE); byte 1 is `0xFF` (Normal
memory). It requests AP=`00` (EL1 read/write, EL0 no access), AF=1,
outer-shareable SH=`10`, PXN=1, and UXN=1. The builder decodes its own
descriptor and validates the device attribute, permissions, shareability,
granularity, alignment, and bounds. Fixture checks translate the range's
first, middle, and last bytes, reject malformed reserved descriptor bits,
and verify unsupported regimes, invalid/missing-sized ranges, misalignment,
overflow, wrong MAIR type, and adjacent-page non-coverage. This remains a
synthetic descriptor/table test, not proof of an active or usable mapping.

In the bounded runtime-DTB QEMU run, the kernel again discovered PL011 at
`0x09000000` with size `0x1000`; the active-root audit remained `unmapped`.
The new synthetic proposal then passed and printed that the mapping was not
installed. The proposal reported its Device-nGnRnE AttrIndx/MAIR byte,
AP=`00`, execute-never bits, and outer-shareable setting. The initial live
snapshot was `CurrentEL=0x4`, `TCR_EL1=0x4B5103510`,
`TTBR0_EL1=0xBBF68000`, `TTBR1_EL1=0xBBF67000`, and `MAIR_EL1=0xFFFF`.
The immediately post-test register snapshot matched the pre-test snapshot.
The QEMU command used `timeout 15s qemu-system-aarch64 -M virt -cpu
cortex-a72 ... -cdrom <temporary-dtb-iso> -m 2G -serial stdio -display none
-no-reboot -no-shutdown`; it printed the success and halt markers before
expected timeout exit 124. The temporary ISO used the verified Limine
`dtb_path` override and a QEMU-generated `virt` DTB. No PL011 MMIO was done.

The experiment's only table reader resolves addresses in its own four-page
synthetic buffer. It receives neither TTBR roots nor the live-table reader,
and no store to a system register, active table, UART, or TLB operation is in
the code path. Live translation-register snapshots are compared immediately
before and after the experiment. Active table descriptor bytes are not
independently hashed; they are not accessed by this isolated test. The
register comparison and source/buffer isolation are the evidence available
for this milestone.

### Ordering plan for future live work (not executed)

1. Build a separately owned table hierarchy while it is unreachable by active
   translations; validate descriptors and table-page accessibility.
2. Before publication, complete descriptor stores and use the architecture-
   required DSB/ISB sequence. A future root switch must separately validate
   TCR/MAIR compatibility, address-space ownership, and all mappings needed
   to continue execution.
3. A future TTBR change requires the prescribed context synchronization and
   correct ASID/TLBI policy. Changes to an already active translation require
   break-before-make where applicable and appropriate local/remote TLB
   invalidation; none is issued here.
4. Only after a Device mapping is active and independently audited may a
   separate device milestone consider peripheral-specific clock, reset,
   power, security, and access-ordering prerequisites. Barriers alone do not
   make an unsafe peripheral access safe.

This plan is single-CPU only. SMP publication, remote TLB shootdown,
concurrent address-space lifetime, and AArch64 allocator integration remain
out of scope. No PL011 registers were accessed and no UART output was
attempted.

### Regression checks for this experiment

`make TOOLCHAIN=llvm` and `make TOOLCHAIN=llvm ARCH=aarch64` both built and
linked successfully. Bounded normal QEMU boots were run as
`timeout 18s make TOOLCHAIN=llvm run QEMUFLAGS="-m 2G -serial stdio -display
none -no-reboot -no-shutdown"` and
`timeout 12s make TOOLCHAIN=llvm ARCH=aarch64 run QEMUFLAGS="-m 2G -serial
stdio -display none -no-reboot -no-shutdown"`. The x86-64 run printed PMM,
VMM, and framebuffer success; the AArch64 default run printed FDT and
descriptor fixture success and the documented unavailable-DTB message. Both
QEMU runs ended with expected timeout exit 124 at the kernel halt path. The
separate runtime-DTB boot printed the successful isolated proposal marker,
unchanged register snapshot marker, and no-MMIO marker before its expected
timeout 124. `git diff --check` also passed after these changes.

## Next smallest milestone

Provide an AArch64-owned physical-page allocator and safe table-page
access/ownership contract, then exercise allocation and destruction of an
isolated root without installing it or changing Limine's active mappings.
Keep the synthetic proposal tests as descriptor-level validation. Do not
switch roots or access PL011 until allocator ownership, table attributes,
barriers/TLB semantics, and device preconditions are separately verified.
This does not claim smartphone compatibility.
