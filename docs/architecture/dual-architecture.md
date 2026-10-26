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

There is no interrupt-controller setup, scheduler, or AArch64 VMM in the
current sources. An AArch64-specific physical-page allocator now initializes
from Limine's memory map; it is not a page-table manager. The exception-vector
implementation and bounded FDT parser have controlled QEMU tests; the runtime
DTB path has been verified using Limine's documented DTB override. PMM and VMM
checks are kernel boot diagnostics (the VMM also contains synthetic tests),
not a separate host-test suite.

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
3. Initialize a platform console, common memory helpers, and the
   architecture-specific PMM metadata. The AArch64 PMM uses only validated
   Limine `USABLE` entries; do not infer usable RAM from device-tree `memory@`
   nodes.
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

The existing PMM and VMM implementations remain x86-64 implementations and
are excluded from the AArch64 image. A separate AArch64 physical-page
allocator is now present, but it is not a page-table manager. The earlier
Device-map proposal still intentionally stops at synthetic tables backed by
ordinary kernel RAM: it does not create an address space with physical table
pages, publish a root, or access any experimental virtual address.

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
concurrent address-space lifetime, and integration of allocation with a live
AArch64 VMM remain out of scope. No PL011 registers were accessed and no UART
output was attempted.

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

## AArch64 physical-page allocator

`src/arch/aarch64/aarch64_pmm.cpp` is a separate, single-CPU allocator; the
x86-64 PMM/VMM sources remain unchanged and are not linked for AArch64. It
accepts only the verified 4-level, 4-KiB, 48-bit-VA regime with a physical
address width from 32 through 48 bits. Initialization checks the memory-map
response shape and bounded entry count (128), known Limine memory types,
nonzero/non-overflowing ranges, physical-address width, sorted bases, the
documented alignment of usable and bootloader-reclaimable entries, and overlap
involving any usable entry. Overlap among non-usable entries is tolerated;
unknown memory types and any ambiguous usable overlap fail closed.

Only `LIMINE_MEMMAP_USABLE` pages enter the allocator. Bootloader-reclaimable,
reserved, ACPI, framebuffer, and all other entries remain unavailable. The
Limine base-revision-6 HHDM contract maps usable memory; initialization also
checks that every usable range has a non-overflowing, canonical HHDM
translation. Two bitmaps are placed in enough contiguous pages at the start
of the first suitable usable region, and those metadata pages (and physical
page zero if present) are excluded before allocation begins. Allocation tracks
ownership in a second bitmap; release rejects unaligned, non-usable,
metadata, never-allocated, and already-released pages. The public writable
pointer conversion is limited to an allocated page and validates its complete
HHDM range. There is no general arbitrary-physical-address conversion API.

Metadata is intentionally allocator-owned and has no teardown operation;
it remains reserved for the lifetime of this boot. A bounded memory map over
128 entries, unsupported PA widths/regimes, a bad HHDM range, or insufficient
contiguous usable memory for metadata causes initialization to fail. The
allocator is single-CPU and has no locking, page clearing, allocation
alignment beyond one 4-KiB page, zones, or reclaim policy.
Teardown is not exposed because this kernel has no shutdown/lifetime protocol
that can prove all allocated-page references have ended; returning metadata
pages early would invalidate allocator state.

Synthetic tests use a page-aligned ordinary-RAM backing array and fake
physical addresses; the mapper translates those addresses only into that
backing array. They cover allocation uniqueness/alignment/exhaustion,
metadata reservation, release ownership and double-free rejection, rejected
memory maps (null/empty/oversized, null entries, unknown types, zero length,
overflow, PA-width excess, misalignment, unsorted bases, usable overlaps, and
no usable RAM), unsupported regimes, metadata-mapper failure, HHDM overflow
and canonicality, and refusal to obtain a pointer for an unowned/reserved
page. These tests are not evidence of the live map. The boot diagnostic then
initializes a separate live allocator from Limine's actual memory-map and
HHDM responses, allocates two pages, validates their HHDM pointers with
ordinary-memory write/read checks, releases them, and confirms the free-page
count returns to its starting value. It does not use page-table pages or
access MMIO.

In a bounded default QEMU `virt` boot with 256 MiB RAM, the kernel observed
`CurrentEL=0x4` (EL1), `SCTLR_EL1.M=1`, `TCR_EL1=0x4B5103510`,
`MAIR_EL1=0xFFFF`, `ID_AA64MMFR0_EL1=0x1124` (44-bit PA), and
`HHDM offset=0xFFFF000000000000`. The TTBR0/TTBR1 root pages were both
reported in Limine bootloader-reclaimable memory, not usable memory. The
allocator reported 0xE2EA total usable pages, 0xE2E6 free pages, four metadata
pages, and seven usable regions; its live allocate/access/release check passed.
The before/after snapshot of CurrentEL, SCTLR, TCR, both TTBRs, MAIR, and
ID_AA64MMFR0 was unchanged. Active table descriptor bytes were not hashed;
the protection evidence is that the allocator writes only to its reserved
usable-RAM metadata and allocations, while Limine's memory-map policy excludes
bootloader-reclaimable page-table storage. The normal boot had no DTB response,
so it halted at the existing optional-DTB diagnostic after the PMM tests.

The same 256-MiB kernel was also booted with a QEMU-generated `virt` DTB passed
through Limine's `dtb_path` option. That run reported 0xE1E9 usable pages,
0xE1E5 free pages, four metadata pages, and eight usable regions; the live PMM
allocation/access/release checks passed before the DTB was parsed. PL011 was
then discovered at physical `0x09000000`, size `0x1000`; the read-only active
table audit reported it unmapped, both root scans completed, and the synthetic
Device-map proposal passed without installing a mapping. The translation
register snapshot remained unchanged, and the output explicitly confirmed that
PL011 registers were not accessed. This demonstrates that reserving PMM state
in usable memory did not overwrite the DTB or interfere with its
bootloader-reclaimable handoff in this QEMU run.

The bounded boot ended with expected timeout exit 124 after the success
markers and intentional `wfi` halt. This confirms the allocator on the tested
Limine/QEMU configuration only, not on smartphone hardware.

## Isolated AArch64 page-table hierarchy

`src/arch/aarch64/aarch64_address_space.cpp` adds an AArch64-only, isolated
4-KiB hierarchy backed by the AArch64 PMM. The root and every intermediate
table page are separately allocated and recorded as owned; mapped leaf pages
are borrowed allocated pages and remain the caller's responsibility. All
table pointers are obtained through the PMM's ownership-checked HHDM pointer
API. The hierarchy is never installed in TTBR0/TTBR1, and its virtual
addresses are only translated by the software walker; none are dereferenced.
Leaf pages are borrowed rather than pinned or transferred: the caller must
keep each page allocated while its leaf descriptor remains mapped. Releasing
a leaf while it remains mapped violates this contract; the hierarchy does not
prevent an external PMM client from doing so. If pruning an empty table fails
to release a table page, `unmap_page` returns `rollback_failed` after clearing
the leaf descriptor. In that case the mapping is removed and the caller may
release the leaf; the empty table branch remains linked and owned so `destroy`
can retry its release. A successful unmap likewise leaves leaf-page release
to the caller.
`destroy` releases table pages post-order and removes each parent link before
releasing its child. If a release fails, that link is restored; any earlier
successfully released branches remain detached, so the still-created
hierarchy remains valid and `destroy` can be retried.

The implementation accepts only the detected four-level, 48-bit VA regime
with 32- through 48-bit PA widths. It requires MAIR AttrIndx 1 to be `0xFF`
(Normal memory) and constructs EL1 read/write, EL0-inaccessible, access-flag
set, inner-shareable, PXN+UXN 4-KiB page descriptors. This is intentionally
not the Device-memory proposal: no PL011 address or Device descriptor is used
by this hierarchy. `unmap_page` prunes empty intermediate tables, and
`destroy` validates the complete owned-table graph before releasing pages.
Failed intermediate allocations roll back linked descriptors and allocated
tables. Destruction is explicit; callers must not abandon a created hierarchy.
The code is single-CPU and does not provide synchronization or an automatic
destructor.

Synthetic tests use ordinary aligned RAM and a fake page provider. They cover
empty create/destroy; descriptor attribute bits; lowest/highest canonical
pages and offset preservation; physical-width, virtual-canonicality, and
alignment rejection; duplicate mappings; unmapped unmap; unmap/remap and
per-level pruning; allocation failure at each intermediate-table depth;
rollback after partial allocations and on an existing branch; and the fixed
64-table ownership limit. Tests compare the allocator's per-page allocation
bitmap before and after rollback/destruction and verify unrelated/borrowed
pages remain allocated. Injected release failure makes `map_page` return
`rollback_failed` while retaining the still-owned partial hierarchy for a
subsequent successful `destroy`. Malformed table/leaf descriptors and invalid
ownership records are rejected without freeing pages; duplicate table
allocations and duplicate child references are also tested. The API maps one
page per call, so range-size alignment/overflow inputs do not exist; it checks
the complete fixed 4-KiB virtual page and physical-address width instead.
These fixtures validate the software hierarchy only and are not evidence
about live mappings.

The live QEMU diagnostic allocates independent PMM leaf pages and a distinct
root, maps four pages across two root branches, verifies translations and
unmapped/collision behavior, unmaps/remaps, destroys the hierarchy, checks
that borrowed leaves remain owned until the caller releases them, and
confirms the PMM free-page count returns to its starting value. The hierarchy
never accesses the mapped memory through its proposed virtual addresses.

On the latest bounded 2-GiB QEMU `virt` boot, synthetic AArch64 hierarchy
tests and the PMM-backed hierarchy test passed. The observed live setup was
EL1 with `SCTLR_EL1.M=1`, 4-KiB granules, four levels, 48-bit VA, 44-bit PA,
`TCR_EL1=0x4B5103510`, and `MAIR_EL1=0xFFFF` (Normal memory in AttrIndx 1).
The active-table checksum traversed 0x206 pages across the two roots and was
unchanged before and after the live hierarchy exercise; this run reported
`0x089DAF32A02BE502`. All sampled active translation registers were also
unchanged. The boot printed the hierarchy success markers, then halted at the
pre-existing no-DTB diagnostic; the bounded QEMU process ended with expected
timeout exit 124. No active descriptor, TTBR, TCR, MAIR, SCTLR, or TLB
operation was performed.

This run used the default QEMU configuration without a DTB response, so it
does not reproduce PL011 discovery in the same boot as the new hierarchy
test. The previously documented runtime-DTB/PL011 audit remains separate
evidence and does not imply PL011 was mapped or accessed here. A passing
synthetic or isolated hierarchy test does not prove that a proposed address
space is safe to publish or use for device access.

## Next smallest milestone

Re-run the runtime-DTB/PL011 discovery and read-only mapping audit with the
hierarchy-enabled image, preserving the no-MMIO constraint. Any subsequent
live address-space or device work still requires separate validation of
publication, ASID/TLB policy, break-before-make requirements, mapping
attributes, and peripheral-specific preconditions. SMP-safe ownership and
remote TLB shootdown remain out of scope.

## AArch64 address-space activation readiness audit

This audit does not activate the isolated hierarchy. It distinguishes linked
virtual addresses from runtime physical addresses: the AArch64 ELF currently
has identical high-half `p_vaddr` and `p_paddr` values, but the kernel does
not request Limine's executable-address response, so those ELF fields are not
used as evidence of the runtime physical load base. The Limine protocol
header provides `limine_executable_address_request`; the kernel currently
requests only HHDM, memory-map, and DTB responses.

The linked ELF program headers establish the following virtual ranges
(end-exclusive):

| Content | Linked VA range | ELF permissions | Physical range / current runtime translation |
| --- | --- | --- | --- |
| Limine requests | `0xffffffff80000000-0xffffffff800000d8` | RW | Runtime PA not obtained; active mapping not enumerated by VA |
| Kernel text, entry and vectors | `0xffffffff80001000-0xffffffff8000f328` | RX | Runtime PA not obtained; current execution and installed VBAR_EL1 establish these VAs are live, not their full PA ranges |
| Read-only data and build ID | `0xffffffff80010000-0xffffffff8001120c` | R | Runtime PA not obtained; active mapping not enumerated by VA |
| Init arrays, data and BSS | `0xffffffff80012000-0xffffffff8008d019` | RW | Runtime PA not obtained; globals and PMM state are live, but exact physical backing is not recorded |

The linker places `.text.entry`, `.text.vectors`, exception handlers, and
ordinary text in the executable segment; `.rodata` is read-only; and
`.init_array`, `.ctors`, `.data`, and `.bss` are writable. The exception
vector table is at `0xffffffff80001800` in the linked image and is installed
in `VBAR_EL1`. `_start` aligns Limine's incoming SP, sets SP_EL1 to that
aligned value and SP_EL0 to 32 KiB below it; the assembly comments rely on
Limine providing 64 KiB below the original SP. Neither stack's runtime
virtual/physical bounds are logged or translated by the current diagnostic.
Both stack ranges, the code continuing after any root switch, the active
exception vector, its handler code, and any state touched by exception
dispatch must remain mapped at compatible virtual addresses. An exception
after a switch must not rely on a missing stack or vector mapping.

The kernel uses the HHDM offset and Limine memory map to initialize the PMM
and to access allocated usable-RAM pages, PMM bitmap metadata, and table
pages. For a new root, these HHDM virtual addresses and physical pages must
remain mapped with writable Normal-memory permissions where the allocator or
page-table writer needs writes. The current code retains and uses
HHDM-backed Limine response pointers for the memory map and DTB after PMM
initialization; those bootloader-reclaimable pages are not owned by the PMM
and must either remain mapped or be copied into owned kernel memory before
their original mappings are removed. The runtime-DTB path continues to parse
and inspect the DTB until it halts. No AArch64 framebuffer, module, or UART
request is made, and debug output uses QEMU semihosting; no MMIO region is
accessed.

The latest bounded QEMU `virt` run observed EL1, `SCTLR_EL1.M=1`,
`TCR_EL1=0x4B5103510`, 4-KiB granules, four levels, VA=48 bits, PA=44 bits,
and `MAIR_EL1=0xFFFF`. These are observations of this QEMU boot, not a
platform-wide promise. The address-space implementation accepts 32- to
48-bit PA regimes, but a future activation must restrict itself to a freshly
validated runtime regime and preserve MAIR/TCR/SCTLR compatibility. Current
leaf descriptors use AttrIndx 1 (`0xFF` Normal), AP=`00` (EL1 RW, EL0
inaccessible), AF=1, inner-shareable, PXN=1, and UXN=1. This is suitable only
for normal writable data, not executable kernel text or read-only sections.

The current API cannot construct a complete kernel root safely:

- `map_page` accepts only a physical page which the PMM reports as allocated;
  it cannot borrow kernel-image pages, Limine stack pages, or bootloader-owned
  response pages outside the PMM allocation bitmap.
- Leaf permissions and memory attributes are fixed to the single RW,
  EL0-inaccessible, execute-never Normal descriptor. The API cannot express
  RX text, RO data, or per-region policy.
- There is no API to retrieve validated runtime physical ranges for the
  kernel segments or incoming stacks, and no active VA-to-PA query for
  enumerating the current kernel mappings. `limine_executable_address_request`
  is available in the checked protocol header but is not issued.
- The root constructor creates an empty root and the single-page mapper has no
  kernel-layout or boot-data policy. It does not prove that instruction
  fetch, stack access, global state, HHDM metadata, or exception handling will
  remain valid after changing roots.

Accordingly, no proposed root mapping was synthesized: the required physical
backing for the kernel segments and stacks cannot be established from the
current API and recorded boot output. Existing PMM-backed hierarchy tests
prove isolated table ownership, descriptor encoding, and software translation
for allocated test leaves only; they do not prove root-switch readiness. The
smallest prerequisite is a read-only boot-time inventory that obtains the
executable physical/virtual base, resolves the live stack ranges and required
VA-to-PA mappings, and validates physical spans against the Limine memory map.
Then a separate non-activating builder/test should check kernel segment
permissions, stack and exception coverage, response lifetime, HHDM/metadata
access, and table-page ownership before any activation work.

The current AArch64 hierarchy borrows leaf pages. The caller must keep each
leaf allocated while mapped and unmap it before release; the API has no pin
count, token, or notification that prevents another PMM client from releasing
a borrowed page prematurely. This documented caller-managed lifetime is the
smallest contract compatible with the existing allocator and API. No
reference-counting subsystem is justified until concurrent clients or
independent ownership transfer are introduced.

The activation readiness audit and its QEMU tests made no TTBR, TCR, MAIR,
SCTLR, active descriptor, or TLB changes, and accessed no MMIO. The bounded
active-tree checksums cover only readable, reachable pages successfully
walked from both active roots; a matching checksum is not proof for pages
outside that scan. In the latest runtime-DTB run, both root scans completed
over `0x206` pages and the checksum was `0xBD132E7547FF0731` before and after
the isolated test. This establishes non-interference only for the measured
registers and scanned table pages.

### Read-only active VA-to-PA inventory follow-up

The AArch64 paging module now exposes a read-only `translate_active_address`
query. It selects the active TTBR root for a canonical address and uses the
existing descriptor walker and HHDM-backed table reader; it does not write
translation registers, descriptors, or TLB state. Invalid/noncanonical
addresses and unsupported regimes are rejected before a table walk. The
boot diagnostic inventories linked image sections, captured stack ranges,
PMM metadata, the active roots' HHDM aliases, and Limine response objects.
It reports page translations, permissions, descriptor attributes, memory-map
classification, and physical contiguity without dereferencing inventory
target addresses. Limine's executable-address response is cross-checked
against each section's active translation rather than treating linker
addresses as physical addresses.

The entry shim records the aligned stack top and normal-stack initial top in
writable kernel BSS before entering C++. This avoids probing the banked
`SP_EL0` system register, which raised an undefined-instruction exception in
the initial QEMU implementation. The inventory checks that the current `sp`
falls in the normal stack half and then inspects the normal and exception
halves separately, each according to the Limine 64-KiB stack guarantee and
the entry shim's 32-KiB split.

The latest bounded default AArch64 QEMU boot observed EL1, 4-KiB granules,
four levels, 48-bit VA, 44-bit PA, `TCR_EL1=0x4B5103510`,
`MAIR_EL1=0xFFFF`, `TTBR0_EL1=0xBBF68000`, and
`TTBR1_EL1=0xBBF67000`. Limine reported executable physical base
`0xBBE62000` and virtual base `0xFFFFFFFF80000000`; the active walker
matched the image request, text, read-only data, and writable-data/BSS
physical ranges. It reported the kernel sections as EL1 RO+executable text,
EL1 RO+PXN/UXN read-only data, and EL1 RW+PXN/UXN writable data/BSS, all with
AttrIndx 0, MAIR byte `0xFF`, AF=1, and SH=`0x3`. The 64-KiB Limine stack
was mapped through HHDM as two 32-KiB bootloader-reclaimable ranges; PMM
metadata was mapped at physical `0x40000000` for `0x20000` bytes as usable
memory. HHDM, memory-map, and executable-address response objects were also
reported mapped in bootloader-reclaimable memory. Runtime output included
passing invalid-address and unsupported-regime query checks.

The pre-test active-root audit covered `0x206` reachable table pages with
checksum `0x1B2DD8FDC8F5F667`. After the inventory and isolated hierarchy test,
the kernel reported the active translation-register snapshot and read-only
active-table checksums unchanged, followed by the hierarchy success marker.
This is evidence only for the sampled registers and the readable table pages
covered by that bounded scan; it does not prove state outside those bounds.
No isolated root was installed, no active descriptor was written, no TLB
operation was issued, and no MMIO was accessed.

This latest boot had no Limine DTB response and printed the unavailable-DTB
diagnostic. Therefore runtime FDT parsing and PL011 discovery were not
revalidated by this boot; the earlier documented runtime-DTB run remains
separate historical evidence. QEMU exited with timeout status 124 after the
isolated hierarchy success and intentional halt. The first inventory attempt
did not reach those markers because of the `SP_EL0` probe described above;
the captured-entry-value implementation was rebuilt and the subsequent boot
completed the inventory and regression checks.

A follow-up runtime-DTB boot used a QEMU-generated `virt` DTB and a temporary
ISO with Limine's `dtb_path` override; neither the checked-in default boot
configuration nor the kernel's default path was changed. The same inventory
reported the Limine DTB-response object and the complete 1-MiB DTB blob as
mapped bootloader-reclaimable HHDM ranges. FDT parsing discovered PL011 at
physical `0x09000000`, size `0x1000`; the active-root audit reported that
range unmapped, and the isolated synthetic Device-map proposal passed without
installing a mapping. The diagnostic explicitly reported that PL011 registers
were not accessed and semihosting remained active. On this run, the pre-test
active-table scan covered `0x206` pages with checksum
`0xFA76F18F28431B7D`; the post-test equality marker passed. The bounded QEMU
process exited 124 after the success markers and intentional halt.

## AArch64 non-activating kernel mapping builder

The isolated address-space module now has a policy-aware
`build_kernel_mappings` path. It remains limited to a 4-KiB granule, four
levels, and 48-bit virtual addresses. Physical-address widths from 32 through
48 bits are accepted only when the regime's address mask agrees; the tested
QEMU runtime reported 44-bit PAs, `TCR_EL1=0x4B5103510`, and
`MAIR_EL1=0xFFFF`. The builder checks the selected MAIR byte at every
region's AttrIndx and accepts only `0xFF` (Normal memory). It does not create
Device mappings.

Supported 4-KiB leaf policies are:

| Policy | AP / EL0 | PXN | UXN | Other attributes |
| --- | --- | --- | --- | --- |
| Kernel text | `10` (EL1 read-only, EL0 denied) | 0 | 1 | AF=1, inner-shareable, AttrIndx selects validated `0xFF` |
| Read-only data | `10` (EL1 read-only, EL0 denied) | 1 | 1 | AF=1, inner-shareable, validated Normal memory |
| Writable data and stacks | `00` (EL1 read/write, EL0 denied) | 1 | 1 | AF=1, inner-shareable, validated Normal memory |

The W+X policy is explicitly unsupported. Descriptors are accepted only if
they match one of these exact encodings, including the runtime PA mask;
unknown encodings are rejected. Table pages are page-aligned, PMM-allocated,
tracked by the hierarchy, and validated as owned before dereference. Leaf
pages remain borrowed and are never freed by hierarchy destruction. Callers
must keep borrowed pages allocated until they are unmapped. A failed unmap
does not release a leaf; if pruning a table fails, the empty branch remains
owned for retry during destruction.

Each declared region supplies page-aligned, end-exclusive VA/PA ranges, a
Limine memory-map type, a role mask, policy, and AttrIndx. The builder checks
the supported regime, range overflow, canonical VA boundaries, PA width,
memory-map response and range classification, alignment, attribute index,
required roles, virtual overlap, and unsafe physical aliases. It rejects
unsupported map types and accepts only validated usable, bootloader-
reclaimable, or executable/module backing. The memory-map response is checked
for bounded entry count, aligned response and entries, known types, sorted
bases, range overflow and PA limits, plus usable-memory overlap. Missing
required mapping roles, including the table-page role, fail explicitly.
Owned table pages receive validated HHDM aliases inside the isolated root.
Partial builds attempt destruction; a PMM release failure is returned as
`rollback_failed`, leaving retryable ownership metadata rather than reporting
successful cleanup.

The runtime validation path uses the active read-only translator and Limine
responses to establish physical backing before proposing mappings for linked
text, read-only and writable image sections; captured normal and exception
stack halves; PMM metadata; Limine request/response objects and memory-map
entry objects; and the DTB response and full blob when supplied and validated.
The linker defines the virtual section bounds; active translations establish
runtime PA backing and contiguity, and the executable-address response
cross-checks the image. The original builder milestone mapped only a selected runtime subset and had
a 64-table-page limit. The expanded usable-RAM HHDM coverage and measured
capacity are documented in the following milestone section. The hierarchy
remains inactive.

Synthetic tests exercise all supported policies and descriptor AP/PXN/UXN
bits; invalid MAIR indices and regimes; noncanonical VA, alignment, overflow
and PA-width failures; unmapped backing; overlaps and conflicting aliases;
W+X rejection; required-role coverage; absent neighboring mappings; borrowed
leaf lifetime; owned-table validation; allocation-failure rollback; and
retry after injected PMM release failures. Synthetic addresses are not
presented as kernel mappings.

The preceding builder-milestone default AArch64 QEMU boot printed the
synthetic builder pass marker
and `Runtime kernel mapping builder coverage, cleanup, and active-state
checks passed (DTB unavailable)`. Its pre/post active-tree scans each covered
`0x206` reachable table pages and both reported checksum
`0xC1CC2347488FCE1F`. The final runtime-DTB boot used a temporary ISO and a
QEMU-generated `virt` DTB; it printed the builder pass marker with DTB
included, discovered PL011 at `0x09000000` with size `0x1000`, and reported
the range unmapped in the active tree. Its pre/post scans each covered
`0x206` pages and both reported checksum `0x15501399A7DEFD3`. Both boots
reported that PL011 registers were not accessed; the isolated Device proposal
was not installed. Both bounded runs exited 124 after the success markers
and intentional halt.

The preceding x86-64 build and bounded QEMU regression also passed the existing PMM,
VMM, mapping, and framebuffer markers. No x86-64 source or behavior was
changed for this builder. These tests do not activate a new root, alter active
descriptors or translation registers, perform TLB maintenance, or access
MMIO. The read-only checksums cover only the successfully traversed active
table pages; equal checksums do not establish non-interference for unreadable
or unvisited memory. Full-HHDM coverage and any further activation readiness
work remain separate blockers.

## AArch64 usable-HHDM expansion and activation-readiness audit

This milestone expands only the validated Normal-memory HHDM subset required
for the PMM. The builder maps every page in every non-overlapping,
page-aligned Limine `USABLE` memory-map entry at `HHDM offset + physical
address`. Before accepting an entry, it walks every active HHDM page and
checks the expected physical translation, Normal MAIR byte `0xFF`,
inner-shareability, AF, EL1 write access, and EL0 denial. The proposed leaf
policy remains EL1 RW, EL0 inaccessible, AF=1, inner-shareable, AttrIndx=1,
PXN=1 and UXN=1. Thus the proposed HHDM is non-executable even though the
observed active Limine HHDM descriptors were PXN=0 and UXN=0. The builder
validates the HHDM offset and every range's end-exclusive VA arithmetic and
canonicality before constructing entries.

The QEMU memory-map policy is deliberately selective:

| Limine class | Proposed mapping |
| --- | --- |
| `USABLE` | All validated entries are mapped as Normal writable, EL0-denied, NX HHDM pages. This is the PMM-accessible RAM guarantee. |
| `BOOTLOADER_RECLAIMABLE` | Only validated live objects are mapped: current and exception stacks, Limine HHDM/memory-map/executable-address/DTB responses, memory-map pointer array and entry objects, DTB blob when present, and active TTBR root pages. The object ranges use the narrowest current builder policy; no entire reclaimable range is implicitly mapped. |
| `EXECUTABLE_AND_MODULES` | Linked kernel request, text, rodata, data and BSS ranges are mapped at their linker VAs with their per-section policies. No module request or module use was found. This class is not added wholesale to the HHDM, avoiding writable HHDM aliases of executable text. |
| Reserved, ACPI reclaimable/NVS, bad memory, framebuffer, reserved-mapped, and gaps | Not mapped by this Normal-memory builder. In particular, no MMIO or framebuffer range is installed as Normal memory. |

The memory-map validator rejects overlapping entries of any type, unsorted or
misaligned usable/reclaimable entries, unknown types, physical overflow, and
physical ranges beyond the validated PA width. Mapping regions independently
reject unaligned ranges, VA/PA overflow, noncanonical ranges, unsupported
memory-map classes, MAIR indices not selecting `0xFF`, virtual overlap,
conflicting physical aliases and W+X. The synthetic reserved-gap test checks
that an address in a reserved interval and an address just before a declared
range remain unmapped. Device attributes remain unsupported here; PL011 is
only discovered and inspected read-only when the DTB is supplied.

### Table capacity and measured requirement

The original 64-entry ownership array was the immediate capacity limit. The
mapper also emits only 4-KiB leaves; it does not use block descriptors. A
contiguous 2-GiB range therefore needs 1,024 L3 tables for 2-MiB spans, two
L2 tables for 1-GiB spans, one L1 table, and a root (1,028 table pages before
additional distinct kernel-VA branches). The expanded QEMU map is sparse and
contains seven usable regions, so its measured count differs from that
simple contiguous estimate.

The implementation uses on-demand PMM table allocation with a validated
bounded capacity of 2,048 owned table pages per hierarchy. This is not a
preallocated PMM table pool. Two statically reserved ownership registries
allow at most two simultaneously created hierarchies; their combined
`OwnedTablePage` metadata is 64 KiB of kernel BSS. The bound is twice the
measured QEMU requirement and accommodates the observed 2-GiB physical
window plus separate kernel sections. Exhaustion returns
`ownership_capacity_exceeded`; map/build rollback is attempted and a release
failure remains retryable as `rollback_failed`. A synthetic test constrains a
hierarchy to four pages and verifies that a mapping requiring another branch
fails without exposing it as mapped. Each allocated table page is tracked;
borrowed leaf pages are never released by destruction.

The final default AArch64 QEMU builder run validated `0x7E031000` bytes across
seven usable HHDM regions (`0x7E031` usable 4-KiB pages). The complete
inactive layout walked `0x7E0ED` mapped pages, including kernel and live
boot-object ranges, and allocated `0x3FC` (1,020) table pages against the
`0x800` (2,048) page capacity. The runtime-DTB layout mapped the same
`0x7E0ED` pages and allocated the same 1,020 table pages. Every declared
region page was translated and checked before destruction; every owned table
page's HHDM alias was also checked. PMM free-page statistics were restored
after destruction. These are runtime results for the tested QEMU boot, not a
promise about other Limine memory maps or devices.

### Runtime-object inventory (observed QEMU run)

The following representative virtual and physical addresses were observed
through the read-only active translator. They are evidence for that boot,
not constants. All displayed ranges are end-exclusive.

| Object | Active VA / physical backing | Lifetime and required permission | Inactive proposed mapping |
| --- | --- | --- | --- |
| Limine request section | VA `0xFFFFFFFF80000000-0xFFFFFFFF80000108`; PA starts `0xBBE39000`; `EXECUTABLE_AND_MODULES` | Request/response storage is read after handoff; EL1 read-only is sufficient for current code | Yes, read-only, EL0 denied, NX |
| Kernel text, entry, vectors, exception handlers | VA `0xFFFFFFFF80001000-0xFFFFFFFF80014A58`; PA starts `0xBBE3A000`; `EXECUTABLE_AND_MODULES` | Must remain executable for current code, return address and VBAR target | Yes, EL1 RO, privileged executable, UXN |
| Kernel rodata | VA `0xFFFFFFFF80015000-0xFFFFFFFF80017384`; PA starts `0xBBE4E000` | Read-only image lifetime | Yes, EL1 RO, PXN/UXN |
| Kernel data and BSS, including PMM/global state and ownership registry | VA `0xFFFFFFFF80018000-0xFFFFFFFF800A4030`; PA starts `0xBBE51000` | Writable for kernel lifetime | Yes, EL1 RW, PXN/UXN |
| Normal and exception stacks | VA `0xFFFF0000BBF8D000-0xFFFF0000BBF9D000`; PA `0xBBF8D000-0xBBF9D000`; `BOOTLOADER_RECLAIMABLE` | Both 32-KiB halves remain live; must be writable | Yes, EL1 RW, PXN/UXN |
| PMM metadata | HHDM VA `0xFFFF000040000000-0xFFFF000040020000`; PA `0x40000000-0x40020000`; `USABLE` | Used by allocation/release for kernel lifetime | Yes, through full usable HHDM, EL1 RW, PXN/UXN |
| Usable RAM | Seven non-contiguous `USABLE` ranges; HHDM VA is `0xFFFF000000000000 + PA` | PMM allocation domain; ranges were individually active-walked and classified | Yes, every validated page, EL1 RW, PXN/UXN |
| Limine response structures and memory-map data | HHDM aliases; representative response PAs `0xBBF4E000` (HHDM response), `0xBBC1B000` (memory-map response), `0xBBC13000` (entry-pointer array); `BOOTLOADER_RECLAIMABLE` | Retained and read during kernel diagnostics; caller does not reclaim them in this boot | Yes, validated object pages with read-only policy |
| Active TTBR roots and reachable descendants | Roots and descendant PAs are discovered from read-only TTBR snapshots and descriptor traversal; the scanner accepts only table pages validated as `BOOTLOADER_RECLAIMABLE` | Bootloader-provided pages are retained because PMM allocates only from `USABLE` | Yes; reclaimable table pages are added read-only at their HHDM aliases, and usable table pages are covered by the broad usable HHDM |
| Newly allocated inactive table pages | PMM-allocated `USABLE` pages; HHDM VA is offset plus PA | Owned until hierarchy destruction; writable while building/walking | Yes, through full usable HHDM; returned to PMM after this test |
| DTB when supplied | Blob PA `0xBBD30000-0xBBE30000`, HHDM VA `0xFFFF0000BBD30000-0xFFFF0000BBE30000`; `BOOTLOADER_RECLAIMABLE` | Parsed through FDT validation during this boot | Yes, read-only, EL0 denied, NX |
| PL011 and other MMIO | PL011 physical `0x09000000`, size `0x1000`, reported unmapped in both active roots | No register access or runtime dependence | No; Device mapping is unsupported |
| Framebuffer | No AArch64 framebuffer request or use found | No runtime dependency identified | No |
| Active page-table descendants and future transition/recovery storage | Full page list, lifetime and post-switch access requirements were not established | Needed only if a future transition/diagnostic/recovery path dereferences them | Not proven mapped |

The inventory confirms that current image code, sections, captured stack
halves, PMM metadata and the listed live boot objects can be represented in
the inactive root with the tested permissions. The executable-address
response cross-check matched each linker image section. The runtime-DTB boot
mapped the validated 1-MiB DTB blob without dereferencing PL011. No proposed
VA was loaded or stored through; all tests used the software walker.

### Activation-readiness checklist

| Item | Status | Evidence / limitation |
| --- | --- | --- |
| Required kernel section mappings | PASS | Linker bounds and active PA translations cross-checked against Limine executable-address response; every proposed page walked. |
| Full HHDM coverage for PMM-usable RAM | PASS (QEMU scope) | Seven `USABLE` ranges, `0x7E031000` bytes; active HHDM walked page-by-page and inactive translations verified. Other firmware maps require fresh validation. |
| HHDM policy and MAIR compatibility | PASS (tested regime) | Runtime `MAIR_EL1=0xFFFF`; AttrIndx=1 is `0xFF`; proposed HHDM is RW/NX, AF, inner-shareable, EL0-denied. Device/FB ranges excluded. |
| Declared-region aliases and W^X | PASS (builder scope) | Physical/virtual conflict checks, exact descriptor validation and W+X rejection; kernel executable/modules are not included in writable HHDM ranges. |
| Bootloader-object lifetime | UNKNOWN | Current response/DTB objects remain live and mapped for the diagnostic. No durable copy/reclaim policy or protocol-wide post-handoff lifetime contract has been established. |
| Current and exception stack continuity | PASS (observed layout) | Captured entry values and Limine 64-KiB guarantee resolve both stack halves; both map writable at unchanged VAs. Future SP selection is not implemented. |
| Exception vectors and handler code | PASS (mapping) | VBAR points into linker `.text`; vector and handler sections are in the validated executable text range. No post-switch exception was induced. |
| Reachable active page-table backing | PASS (tested mapping) | The runtime scan inventories all reachable table pages and the proposed root is walked for every HHDM alias. This does not establish that restoring the old root is a safe recovery path. |
| PMM continuity for usable RAM | PASS (mapping) | Every usable HHDM page and PMM metadata is mapped RW/NX; current allocator uses only validated usable ranges. No allocator was run after a root switch. |
| Transition code, temporary state and return path | UNKNOWN | No root-switch sequence exists. The current kernel code and stack are mapped, but no transition-specific code/state contract was implemented or executed. |
| Console/diagnostic continuity | UNKNOWN | QEMU diagnostics use semihosting; there is no AArch64 framebuffer or UART backend. Hardware console availability after a transition is not established. |
| Table ownership, capacity and rollback | PASS (tested bounds) | 1,020 pages measured under the 2,048 bound; constrained-capacity, allocation failure, cleanup and release-failure retry tests pass. No claim beyond the configured bound. |
| TLB maintenance and ordering | UNKNOWN | No activation, ASID policy, break-before-make sequence or TLB invalidation design is implemented or tested. |
| Translation-register configuration | UNKNOWN | Runtime EL1/TCR/MAIR/PA width were observed and the root matches the tested regime; future TTBR/TCR/MAIR/SCTLR update order and compatibility are undefined. |
| Safe recovery or halt on transition failure | UNKNOWN | No transition routine or recovery path exists; no failure is injected after changing roots. |

**Overall: NOT READY.** The inactive QEMU root covers the tested usable HHDM
and known runtime layout, but boot-object lifetime, old-root recovery,
transition behavior, TLB ordering and console continuity
remain unresolved. This milestone intentionally performs no TTBR, TCR, MAIR,
SCTLR, active descriptor, or TLB writes and accesses no MMIO.

The follow-up transition design and detailed object-lifetime audit are in
[aarch64-transition-readiness.md](./aarch64-transition-readiness.md). It
clarifies that old-root recoverability remains unproven even though scanned
active table pages are now addressable through the inactive root.

### Final validation evidence

The final default AArch64 run reported the exact pre/post register snapshot:
`CurrentEL=0x4`, `SCTLR_EL1=0x30D0199D`,
`TCR_EL1=0x4B5103510`, `TTBR0_EL1=0xBBF68000`,
`TTBR1_EL1=0xBBF67000`, `MAIR_EL1=0xFFFF`, and
`ID_AA64MMFR0_EL1=0x1124`. The active-table scans each completed over
`0x206` pages and matched checksum `0x33A8E93D4290B024`. In the final
runtime-DTB run the register snapshots were identical; both `0x206`-page
scans matched checksum `0x9DCEC27BB4E0AF15`. Checksums cover only readable,
reachable pages successfully traversed from both active roots; they do not
prove stability for unreadable or unvisited pages. Both runs printed their
builder success markers before the expected timeout exit 124 after the halt.

The exact requested x86-64 and AArch64 build commands succeeded. The bounded
x86-64 QEMU run passed the existing PMM, VMM and framebuffer markers and
exited 124 at the intentional timeout. The bounded default and runtime-DTB
AArch64 QEMU runs passed their mapping markers and exited 124 after halt.

The subsequent exception-dependency/capacity follow-up is recorded in
[aarch64-transition-readiness.md](./aarch64-transition-readiness.md). Its
current-kernel default AArch64 run passed the pre-activation verifier and
scoped capacity check, but a clean runtime-DTB rerun faulted before candidate
verification and remains unresolved. That later failure supersedes the
runtime-DTB pass above for the current kernel; overall readiness remains
**NOT READY**.
`git diff --check` is recorded separately in the final repository check.
No x86-64 sources were changed, and no commit or push was made.
