# AArch64 Address-Space Transition Readiness Audit

## Scope and decision

This is a non-activating audit. The proposed hierarchy is never installed in
TTBR1 or TTBR0. No translation-control register, active descriptor, or TLB
state is changed. The result remains **NOT READY**.

The supported runtime regime is the one detected from the running system, not
a build-time assumption: the existing QEMU observations report EL1, 4-KiB
granules, four levels, 48-bit VAs, 44-bit PAs, and `MAIR_EL1=0xFFFF`.
`MAIR_EL1.Attr0=0xFF` is the Limine AArch64 Normal WB attribute for base
revision 4 and later; the builder uses only the verified Normal attribute.
The builder is a high-half root and is a candidate for TTBR1 only. TTBR0 is
not rebuilt or replaced by this work.

## Findings, ranked

| Severity | Finding | Evidence and disposition |
|---|---|---|
| High | Prior readiness checks mapped the two active TTBR root pages but did not establish that all reachable active table-page backing would be accessible through the candidate HHDM. | Fixed for the tested runtime: the read-only scanner can now return the unique reachable table-page addresses; the builder adds bootloader-reclaimable pages as read-only HHDM ranges and checks every scanned page by walking the inactive root. Usable table pages are covered by the validated usable-RAM HHDM. Synthetic tests cover complete inventory, duplicate roots, capacity exhaustion, inaccessible children, and a table page reached at inconsistent levels. |
| High | No safe post-switch recovery path is established. | No switch sequence, recovery trampoline, fault-injection test after a switch, or proof that old-root recovery would be valid exists. Keep UNKNOWN; do not attempt a switch. |
| Medium | Limine-owned response objects require an explicit retain-until-copied policy. | The current PMM imports only `USABLE` ranges; it does not allocate from or reclaim `BOOTLOADER_RECLAIMABLE`, and the builder maps individually validated live objects. This establishes lifetime under the current allocator policy. Any future allocator that reclaims this class must first copy retained metadata or pin the referenced pages. |
| Medium | Active-tree checksum is not a proof of every physical memory location remaining unchanged. | It covers descriptors from readable table pages reached from both roots. It does not cover unmapped/unreadable pages, leaf contents, or pages not reached by the scanner. |
| Medium | Console continuity after a transition is unproven. | AArch64 diagnostics use semihosting (`debug.hpp`); no hardware UART or framebuffer backend is established. The runtime DTB reports PL011, but the driver does not access it and the builder excludes Device mappings. |
| Medium | TLB maintenance, ordering, and post-switch fault handling have not been implemented or exercised. | These are mandatory design inputs for a separate activation milestone, not something the inactive builder can prove. |

## Boot-object lifetime and mapping inventory

Addresses below are runtime-derived. Physical backing is established by
walking the current VA through the read-only active translator and checking
the Limine memory-map class; it must not be inferred from equal-looking VA
and PA values. Linker ranges come from `aarch64.lds`, and the executable
address response cross-checks the loaded image.

| Object | Physical backing and current VA | Lifetime / owner | Candidate-root mapping and permissions | Needed after transition? |
|---|---|---|---|---|
| Kernel request structures and markers | Linker range `__limine_requests_start..__limine_requests_end`; physical backing resolved page-by-page from the active VA. | Kernel image, `EXECUTABLE_AND_MODULES`; loader fills response pointers before handoff. Kernel owns the image pages. | Mapped at original high VA, EL1 read-only, EL0 denied, NX. | Request storage is not required after response data has been copied or otherwise retained. Current diagnostics still read request response pointers. |
| HHDM response and offset | Response object's active VA and PA are validated separately; the offset is copied to a scalar before building. | `BOOTLOADER_RECLAIMABLE`; no current PMM allocation from this class. | Response page mapped read-only; all required usable ranges mapped at `HHDM + PA`. | Offset remains required for allocator metadata and table access; preserve a kernel-owned scalar. The response object itself can be dropped after use. |
| Memory-map response, pointer array, and entry objects | Each current VA is translated and checked to reside in `BOOTLOADER_RECLAIMABLE`. | Limine-provided, retained by the current kernel because the PMM accepts only `USABLE`. | Each object is mapped read-only; usable RAM is mapped RW/NX. | Needed only by current diagnostics/builder and any future memory-type validation. Copy normalized entries into kernel-owned storage before allowing reclaim of the original pages. |
| Executable-address response | Response VA is actively translated and memory-map classified. | `BOOTLOADER_RECLAIMABLE`, retained by current allocator policy. | Individual response pages are read-only. | Needed only for image-backing cross-checks; copy the scalar VA/PA pair if retained as a runtime invariant. |
| Kernel `.text`, entry, vectors, handlers | Linker VAs and active-translated PA ranges; executable-address response cross-check. | Kernel-owned image pages; image remains live for kernel lifetime. | Original high VA, EL1 read-only, privileged executable, UXN; no writable alias is introduced. | Yes: current PC/return path and exception handlers execute here. |
| Kernel `.rodata` | Linker VA range; active-translated PA range. | Kernel-owned image. | Original VA, EL1 read-only, PXN/UXN. | Yes where referenced by live code. |
| Kernel `.data`, `.bss`, PMM state and ownership registry | Linker VA range; active-translated PA range. | Kernel-owned image and state. | Original VA, EL1 RW, PXN/UXN. | Yes. |
| Normal and exception stacks | Entry assembly captures Limine's initial SP. It sets SP_EL1 to the aligned initial top and SP_EL0 to 32 KiB below it; both halves are verified in `BOOTLOADER_RECLAIMABLE`. | Limine-provided backing, retained because PMM does not reclaim this class. Kernel owns continued use after handoff. | Both 32-KiB halves at their current HHDM VAs, EL1 RW, PXN/UXN. | Yes: current execution uses SP_EL0; same-EL exceptions use SP_EL1. A future sequence must preserve both values and their translations. |
| PMM metadata and allocator state | Metadata PA/size from `Memory::statistics()`; validated as `USABLE`. Metadata VA is HHDM offset plus PA. | Allocator-owned pages from the usable PMM; kernel-lifetime state. | HHDM RW/NX, EL0 denied; kernel globals remain RW/NX at image VA. | Yes if the PMM continues after the transition. |
| Active TTBR0/TTBR1 roots and reachable table pages | Root PAs come from read-only register snapshots; descendants come from checked descriptor traversal. The active scanner accepts table pages only when their PA is in `BOOTLOADER_RECLAIMABLE`. | Limine-provided boot tables. The current PMM does not reclaim these pages. | Every scanned bootloader-reclaimable page is mapped read-only at its HHDM VA; usable pages use the broad usable HHDM mapping. Every listed page is walked in the inactive hierarchy. | Read access is useful for diagnostics and any explicitly designed recovery. It does not prove a recovery sequence. |
| Inactive hierarchy tables | PMM-allocated physical pages in `USABLE`; accessed while building through the PMM's HHDM provider. | Hierarchy-owned until destruction; leaves remain borrowed. Current test destroys the hierarchy and returns pages. | HHDM RW/NX for table management. | Yes only if the hierarchy is retained after a future transition; that lifetime and destruction policy must be specified. |
| DTB response and blob, when present | Response VA and entire blob are validated against active translations and `BOOTLOADER_RECLAIMABLE`. The runtime DTB size comes from the validated FDT header. | Limine-provided, retained by current PMM policy. | Response and blob mapped read-only, EL0 denied, NX. | Current FDT discovery needs it during boot. Copy parsed properties into kernel-owned data before any future reclamation if the blob is to be released. |
| Modules, kernel file metadata, framebuffer | No module/file/framebuffer request or corresponding runtime dependency was found in the AArch64 boot code. No framebuffer request is made. | Not requested or consumed by this kernel path. | Not mapped as part of this builder. | Not currently required. Re-audit if requests/drivers are added. |
| MMIO / PL011 | Runtime DTB may describe PL011 physical range; discovery is metadata-only. | Device-owned; not ordinary RAM. | Not mapped; Device-memory policy is unsupported. | No current dependency. Any future driver needs a separately validated Device mapping policy. |

Limine's [protocol specification](https://github.com/limine-bootloader/limine-protocol/blob/trunk/PROTOCOL.md)
defines the HHDM and memory-map classes, documents bootloader page tables as
bootloader-reclaimable, and specifies the entry stack in bootloader-reclaimable
memory. For base revision 4 or later it defines AArch64 `MAIR_EL1.Attr0` as
Normal WB `0xFF`. The kernel's current policy is not to reclaim that class:
`aarch64_pmm.cpp` constructs its PMM ranges only from `LIMINE_MEMMAP_USABLE`.
This policy is a kernel lifetime choice and must remain true until all needed
Limine objects have been copied or released deliberately.

## Active page-table dependencies and limits

`snapshot_active_tables` traverses the descriptors reachable from both
snapshotted TTBR roots. A table read fails closed if its page is not classified
as bootloader-reclaimable, if a descriptor is malformed, or if the bounded
page budget is exhausted. The inventory is de-duplicated across roots.
Repeated references at the same level are allowed; a single physical page
referenced at different levels is rejected as malformed.

The runtime builder now uses this inventory before constructing the candidate
root. It coalesces contiguous bootloader-reclaimable table pages into
read-only Normal HHDM regions and relies on the broad RW/NX HHDM only for pages
classified as usable. After construction it walks every inventoried table-page
alias and requires exact PA equality. This does not copy or reuse old
descriptors, and does not imply that retaining old tables is a safe recovery
strategy.

The scanner's deliberate memory-map restriction means an active table page
outside `BOOTLOADER_RECLAIMABLE` makes the scan incomplete; this must stop
readiness validation rather than be treated as proof of a complete tree.
`MAX_SNAPSHOT_TABLE_PAGES` bounds each root scan at 4096 pages and the caller's
inventory holds at most 8192 unique pages. Any bound failure is an explicit
incomplete result.

## Candidate-root contract and completeness

The inactive root must satisfy all of the following before any future
transition:

- Match the observed translation regime (EL1 stage-1 high VA, 4-KiB granule,
  four levels, detected PA width and VA split); it is only a TTBR1 candidate.
- Keep every instruction, global datum, current stack, exception vector,
  exception handler, PMM metadata page, and retained boot object accessible at
  its current VA and backed by the validated PA.
- Use the same validated Normal-memory attribute and compatible shareability
  for ordinary RAM. Do not map MMIO or framebuffer memory as Normal.
- Keep text privileged-executable and read-only; rodata read-only/NX; data,
  BSS, stacks, PMM metadata, and managed page tables RW/NX; deny EL0 access.
- Keep both current stack banks and the VBAR target valid. Preserve the old
  stack pointer values across the root replacement.
- Map all usable memory that the PMM may allocate from, plus the PMM metadata
  itself. Keep each non-usable boot object pinned and map only its validated
  pages.
- Reject noncanonical addresses, overflow, unaligned backing, overlaps,
  incompatible aliases, unsupported attributes, and W+X. Keep all new table
  pages hierarchy-owned and borrowed leaf pages caller-owned.
- Validate every requested page by descriptor/software walk, and destroy
  incomplete construction before returning success. PMM release failure must
  remain retryable.

The current builder enforces these conditions within its supported mappings.
It does not provide dynamic region-array growth beyond its fixed bound, does
not establish a durable boot-object copying/reclamation subsystem, and does
not execute the PMM under the candidate root.

## Future transition design (not implemented)

### Architectural requirements and observed regime

The architectural reference for implementation is the Arm Architecture
Reference Manual for A-profile architecture, DDI 0487, specifically its
AArch64 system-register descriptions for `CurrentEL`, `SCTLR_EL1`,
`TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, and `MAIR_EL1`; the Virtual Memory
System Architecture (VMSA) chapters on address translation, translation
table descriptors, and TLB maintenance; and the A64 instruction descriptions
for `DSB`, `ISB`, and `TLBI`:
[Arm DDI 0487](https://developer.arm.com/documentation/ddi0487/latest).
The implementation review must pin the applicable manual revision and
feature set before choosing exact instructions. The table below decodes the
snapshots already captured from QEMU; it is evidence about that run, not a
platform guarantee.

| Register/evidence | Observed value and decoded fields | Consequence |
|---|---|---|
| `CurrentEL` | `0x4`; `EL[3:2]=01`, hence EL1 | EL1 system-register bank and EL1 translation regime are active in the observed run. |
| `TCR_EL1` | `0x4B5103510`; `T0SZ=16`, `T1SZ=16`, `TG0=00` (4 KiB), `TG1=10` (4 KiB), `SH0=SH1=11` (Inner Shareable), `IRGN0/1=ORGN0/1=01` (WB/WA), `IPS=100` (44-bit PA), `A1=0`, `AS=0` (8-bit ASID), `TBI0=TBI1=0`, `DS=0`, `EPD0=EPD1=0` | 48-bit lower and upper input-address ranges, using four 4-KiB translation levels in the supported builder. `A1=0` selects the ASID from TTBR0. The observed regime uses no top-byte-ignore or 52-bit descriptor format. |
| `SCTLR_EL1` | `0x30D0199D`; `M=C=I=1`, `SA=SA0=1`, `A=0`, `WXN=0`, `EE=E0E=0` | MMU and data/instruction caches are enabled; data descriptors, rather than `WXN`, must enforce W^X. The observed exception endianness controls are little-endian. |
| `MAIR_EL1` | `0xFFFF`; `Attr0=Attr1=0xFF` | Both observed slots encode Normal, Inner/Outer Write-Back cacheable memory. The code uses the verified Normal slot; this observation does not authorize Device mappings. |
| `ID_AA64MMFR0_EL1` | `0x1124`; `PARange=0100` | Reports 44-bit physical-address support in this run, consistent with `TCR_EL1.IPS=100`. |
| `TTBR0_EL1`, `TTBR1_EL1` | `0xBBF68000`, `0xBBF67000` in the recorded run; ASID and CnP fields are zero | TTBR0 is the lower-range root and TTBR1 the upper-range root. The candidate is built for TTBR1; retaining TTBR0 is a design choice, not proof that all dependencies are high-half. |

For this configuration, addresses in the lower 48-bit input range select
TTBR0 and addresses in the upper canonical range select TTBR1. With both
`T0SZ` and `T1SZ` equal to 16, those ranges do not overlap. A future
TTBR1-only experiment must preserve TTBR0 and keep its lower-address
dependencies valid; it must not assume TTBR1 controls those addresses.
Each TTBR base must identify a 4-KiB-aligned root table in the active
44-bit physical-address range, and every table/page descriptor address must
satisfy the 44-bit output-address-size and configured 4-KiB format
constraints. The current roots have `CnP=0`; the first experiment should
retain that setting unless common-table requirements across all participating
PEs are explicitly established. With `A1=0`, the selected ASID comes from
TTBR0; `AS=0` selects the observed 8-bit ASID size. The actual
`ID_AA64MMFR0_EL1` and `TCR_EL1` must be re-read and checked at runtime,
not copied from these QEMU values.

The candidate leaf encoding in `kernel/src/arch/aarch64/aarch64_address_space.cpp`
uses `AttrIndx=1`, `AF=1`, `SH=Inner Shareable`, EL1-only AP permissions,
`PXN`/`UXN` according to the region policy, and `nG=0` (global). For a
4-KiB level-3 page descriptor, `AP[2:1]=00` is EL1 read/write and
`AP[2:1]=10` is EL1 read-only; EL0 access is denied. Text is read-only,
privileged-executable and unprivileged-execute-never; read-only data is
PXN/UXN; writable data and stacks are also PXN/UXN. Writable-executable
leaves are rejected. The table descriptors point to the next 4-KiB table;
all output addresses must fit the detected PA width. The runtime regime
decoder and active-table checks are in `kernel/src/arch/aarch64/aarch64_paging.cpp`.
Consequently, changing only the TTBR does not make stale global TLB entries
safe: a previous translation can outlive the root selection and can retain
different permissions or attributes. Any eventual sequence must invalidate
the relevant EL1 stage-1 entries including global entries, with scope and
shareability correct for the executing PE(s). ASID-only invalidation is
insufficient for these `nG=0` leaves. The exact TLBI opcode and ordering must
be selected against the pinned DDI 0487 revision and active feature set;
this document intentionally does not prescribe unverified assembly.

Architecture-mandated semantics include: valid, aligned, correctly formatted
table walks for the configured regime; correct memory attributes and
permissions; ordering page-table writes before hardware table walks;
appropriate TLB invalidation when cached translations could conflict; and
context synchronization where required after system-register or translation
context changes. The exact choice to retain TTBR0, use a single PE, mask
interrupts for a first experiment, keep all mappings global, or fail-stop
instead of attempting recovery is an implementation choice and requires
explicit review. QEMU success alone proves none of these architectural
requirements.

### Proposed protocol (design only; not executable)

1. **Establish preconditions.** Read and record `CurrentEL`, `SCTLR_EL1`,
   `TCR_EL1`, both TTBRs, `MAIR_EL1`, `ID_AA64MMFR0_EL1`, `VBAR_EL1`, and
   relevant feature/interrupt state. Require EL1, the supported 4-KiB/48-bit
   regime, compatible 44-bit PA width, the expected MAIR encoding, enabled
   translation, and an explicitly reviewed cache/shareability configuration.
   Refuse to proceed on any mismatch. TTBR0 remains unchanged in this design.
2. **Validate the complete candidate while inactive.** Walk all candidate
   descriptors and all required code, data, stack, vector, PMM, boot-object,
   and page-table ranges. Confirm leaf permissions, `AF`, `SH`, AttrIndx,
   address-width constraints, no W+X, canonical VAs, no arithmetic overflow,
   no incompatible physical aliases, and no unsupported Device mapping.
   Confirm every table page is owned or borrowed according to policy and
   readable at the HHDM address. Reject incomplete active-table scans and
   registry/region-capacity exhaustion. Current tests provide software walks;
   they do not establish that hardware will use the candidate.
3. **Remove bootloader lifetime dependencies.** Preserve the present policy
   that `BOOTLOADER_RECLAIMABLE` is not PMM-allocatable, or first copy the
   normalized memory map, HHDM offset, executable bounds, needed Limine
   response values, and parsed DTB facts into kernel-owned storage. Pin any
   response/blob pages still referenced. Do not reclaim Limine page tables,
   stacks, or metadata while diagnostics or recovery might use them.
4. **Prove address continuity.** Put a small transition stub and its literal
   data in regions mapped identically by both roots. Verify its PC, return
   address, active SP_EL0 and SP_EL1 ranges, VBAR target, vector code, handlers,
   handler globals, PMM state, and candidate table backing at identical VAs
   and physical pages. The stub must not call code or load data whose mapping
   has not been checked. Preserve stack contents and alignment.
5. **Publish candidate tables.** Complete table writes and perform the
   architecture-required ordering/cache maintenance for the observed
   cacheability and shareability regime before hardware can walk them. This
   design has not yet proved the exact barrier/cache-maintenance sequence.
6. **Change root and synchronize translation state.** Write only the intended
   TTBR1 root field, with correct BADDR, ASID selection, and CnP semantics;
   retain TTBR0. Perform the required context synchronization. Invalidate
   stale EL1 stage-1 translations at a scope that includes the candidate's
   global leaves and every participating PE, with architecturally correct
   barriers before/after. The precise order/opcode is a blocker until checked
   against the pinned manual and tested separately.
7. **Probe services before reclamation.** Use a candidate-only test VA whose
   absence from the old root has been verified, then verify code/data access,
   stack use, exception return, PMM allocate/access/release, and table walks.
   Retain old roots and Limine backing throughout. A guest readback of TTBR1
   alone is not proof of a successful hardware translation through the new
   root.
8. **Failure policy.** A pre-write validation failure can leave the active
   regime untouched; destroy the candidate only if allocator rollback is
   verified, otherwise retain ownership records and stop. After a root write,
   there is no proven recovery routine. Do not write the old TTBR speculatively.
   A recovery claim requires demonstrated reachability of recovery code,
   vectors, stacks, and data under the failing regime plus a proven register,
   synchronization, and TLB sequence. Until then, post-write failures are
   `UNKNOWN` for recoverability and the test must be treated as potentially
   unrecoverable.

### Dependency continuity

The before state is the observed Limine/QEMU root pair; the during/after
columns describe the proposed TTBR1-only protocol, not executed behavior.
`Mapped` means the inactive hierarchy has an applicable mapping; it is not
hardware evidence of continuity.

| Dependency | Address/source | Before | During root update | After | Evidence / unresolved point |
|---|---|---|---|---|---|
| Transition PC, instructions, return path | Current high-half kernel `.text`; exact runtime PC/return addresses were not captured | Active | Candidate `.text` mapping exists at original VA; actual transition stub and its literal references are not yet audited | Expected mapped, not exercised | Linker sections and required kernel pages are covered by the builder; exact stub footprint remains `UNKNOWN`. |
| Normal execution stack | Limine initial stack split in `kernel/src/arch/aarch64/entry.S`; SP_EL0 is 32 KiB below aligned top, both halves retained as bootloader-reclaimable | Active | Candidate maps both 32-KiB halves at the current HHDM VA | Expected RW/NX, not exercised | Page coverage is checked, but exact live SP values and stack canaries are not recorded. |
| Exception stack | SP_EL1 set by `entry.S` to aligned initial top | Active | Candidate maps its 32-KiB backing RW/NX | Expected mapped, not exercised | No exception has been raised with the candidate active. |
| VBAR and vectors | Installed by `aarch64_install_exception_vectors` in `entry.S`; vector symbol resides in kernel `.text` | Active | Text mapping exists; VBAR_EL1 is not part of the existing register snapshot | Unknown in executed transition | Need readback and per-vector validation under the candidate regime. |
| Exception handlers and globals | `kernel/src/arch/aarch64/entry.S`, `exceptions.cpp`, `.text`/`.bss` | Active | Candidate has executable text and writable data mappings | Expected mapped; no exception tested after root change | Semihosting/fatal paths and every accessed global need a dependency closure audit. |
| Candidate root and subordinate tables | PMM pages allocated from validated `USABLE` ranges; builder accesses through HHDM | Active regime does not use candidate | Tables are physically present; candidate HHDM aliases are mapped RW/NX | Expected accessible, but hardware walks not tested | The walk checks table structure/software translation; it does not test hardware table walks. |
| Active roots and reachable descendants | TTBR0/TTBR1 snapshot and read-only scanner inventory | Active | Old TTBR0 remains selected for lower VA; old TTBR1 retained but unused after update | Mapped read-only at HHDM for scanned pages | Scanner only covers readable reachable table pages; old-root rollback and inaccessible/unvisited backing remain unproven. |
| PMM state and allocator metadata | Kernel globals and usable-RAM HHDM metadata | Active | Candidate maps globals and usable HHDM RW/NX | Expected usable | PMM has not allocated or released while candidate root is active. |
| Limine responses and DTB | Bootloader-reclaimable response structures/blob | Active and currently consumed | Individually validated pages are mapped read-only | Accessible only while retained/pinned | Current PMM exclusion avoids reuse; durable copying and reclamation are not implemented. |
| Diagnostic path | `kernel/src/arch/aarch64/debug.hpp` `HLT #0xF000` semihosting; `GNUmakefile` enables QEMU semihosting | Active in configured QEMU | Text/data pointers may be mapped; host semihost handling is environment-dependent | Unknown outside same QEMU setup | No UART or framebuffer driver; diagnostics are not a hardware-console guarantee. |
| Temporary status and test data | Future activation test's kernel-owned record | Not present | Must be mapped by both roots or only written after candidate proof | Unknown until implemented | Use a fixed, kernel-owned status record; external observation strategy is described below. |

### Post-switch failure matrix

“Old-root restore” below means writing the saved TTBR1 value; it is not a
recovery guarantee. No restore stub has been implemented or proven reachable.

| Failure immediately after root change | Likely cause | Can current handler be reached? | Old-root restore possible? | Classification and prerequisite |
|---|---|---|---|---|
| Instruction abort fetching transition/return code | Missing or wrong candidate text mapping, stale/invalid descriptor, wrong VA regime | Only if VBAR, exception stack, vector, handler text and data all translate; that closure is not proved after activation | Unknown; the fault may prevent execution of the restore code itself | Unknown / potentially unrecoverable. Prove dual-root identity mapping and test a candidate-only fetch before extending the experiment. |
| Data abort on stack, global, PMM, or table access | Missing mapping, AP/XN fault, AF fault, bad PA, or stale permission | Same-EL exception uses EL1 vector/stack; candidate mappings exist for the identified stack/text/data but no hardware exception test under candidate | Unknown; requires stable recovery state and a mapped restore stub | Unknown. Validate exact access and induce only a controlled synchronous fault in a later test. |
| Exception-entry failure | VBAR invalid/unmapped, SP_EL1 backing absent, vector/stack permission or translation fault | No reliable handler reachability; a fault while taking the original exception can escalate or halt | Not credible without another independently mapped recovery path | Unknown / potentially unrecoverable. Must validate VBAR and SP_EL1 before activation; no fallback vector path exists. |
| Translation-table walk fault / external abort | Invalid descriptor, inaccessible physical table page, PA-size violation, memory-system error | Depends on successful exception entry and mapped handler dependencies | Unknown | Unknown. Software descriptor walks cannot rule out physical bus/interconnect errors. |
| Stale global TLB entry | Candidate and active descriptors differ while candidate leaves have `nG=0` | Handler may run with stale permissions/attributes, so it is not necessarily a trustworthy observer | Restoring TTBR alone cannot guarantee removal of stale entries | Unresolved blocker. Correctly scoped all-global invalidation and barrier sequence must be reviewed and tested. |
| Synchronous exception in the controlled test | Deliberate undefined instruction or other chosen synchronous exception | The current exception diagnostic proves handler operation before transition only | A successful `ERET` is the desired result; not old-root recovery | Future-testable, but current result UNKNOWN. Validate vector, SP_EL1, handler data and return state under candidate first. |
| Semihosting call fails or is not intercepted | QEMU option/semihosting implementation differs, HLT trapped as guest exception | Only if exception path is valid; current output may disappear or become fatal | Unknown | Diagnostic failure is not proof of translation failure. Use persistent status and QEMU-side observation. |
| Asynchronous IRQ/FIQ/SError during transition | Interrupt state, device/system event, or external error | Current interrupt masking/state is not established as a transition invariant | Unknown | Avoid asynchronous stimuli in the first test only after DAIF is read and explicitly controlled; SError cannot be assumed suppressible. |

These cases are not safely classified as recoverable from the existence of
`exceptions.cpp` or an installed VBAR. QEMU cannot establish recovery from all
real hardware external aborts, interconnect errors, or implementation-specific
faults.

### Console and diagnostics

The AArch64 debug interface emits QEMU semihosting requests (`HLT #0xF000`,
`SYS_WRITE0`); the established QEMU command enables semihosting with
`target=native`. This provides host-visible diagnostic text only when the
emulator recognizes and services those requests. It does not provide a guest
UART, guarantee output after a translation change, prove that a guest
instruction executed using the new root, or guarantee useful diagnostics if
exception entry fails. The runtime DTB can describe PL011, but current code
only parses/audits the range; it does not initialize a driver or access the
MMIO registers. No hardware console continuity is established.

For a future activation experiment, use multiple evidence channels: write
monotonic phase/result codes to a kernel-owned, mapped status record; emit
semihost markers as supplementary output; and have the harness record QEMU
exit/timeout status and, where available, QEMU trace/debug observations. The
status record and its backing must be checked under both roots. Do not use a
PL011 or other MMIO path as a diagnostic shortcut.

### Minimal future activation experiment (not run)

The first activation experiment should be a dedicated, single-PE QEMU test
build, not a production boot:

1. Require all readiness checks below that are marked essential to pass,
   pin the exact Arm manual revision and QEMU configuration, and verify the
   live register fields and all relevant backing ranges. Read DAIF and define
   the interrupt policy. Keep candidate and old-root pages allocated.
2. Build and validate the complete candidate, including a dedicated
   candidate-only test VA backed by an allocated, initialized page. Prove via
   active-root software walk that this VA is unmapped before the switch and
   via candidate walk that it maps the expected PA and sentinel. Check kernel
   text RO/X, rodata RO/NX, writable globals/stacks RW/NX, HHDM coverage,
   table-page backing, boot objects, AP/AF/SH/AttrIndx/PXN/UXN/nG, aliases,
   and W^X.
3. Record a pre-switch status code and register snapshot. Execute only the
   separately reviewed root-update/TLBI/barrier stub. Immediately read back
   TTBR1 and verify the selected BADDR while TTBR0 and the unchanged regime
   registers match the precondition snapshot.
4. Prove the new root is actually used by loading the known sentinel through
   the candidate-only VA (not merely by reading TTBR1), then continue through
   an ordinary kernel function and write a post-switch status code. A failure
   to emit output alone is inconclusive; the harness must also inspect its
   status/trace evidence.
5. Exercise a PMM allocate, write/read through the candidate HHDM, and release;
   check allocator counts and the table-owner registry before/after.
6. Trigger one controlled synchronous exception with a deliberately chosen,
   recoverable test instruction; require the existing handler's expected
   syndrome, exception-stack canary, return marker, and continued execution.
   Do not trigger translation faults or table-walk corruption in the initial
   activation test.
7. Emit a final success code only after all checks, then halt. A pass requires
   the candidate-only sentinel read, register check, PMM round trip, exception
   return, and final status record. A timeout before the final record, wrong
   TTBR value, failed sentinel/access check, unexpected exception, allocator
   mismatch, or missing return marker is FAIL or INCONCLUSIVE depending on
   which independent evidence survived; a missing semihost line by itself is
   inconclusive, not a pass or proof of a fault.

This experiment would provide evidence only for the tested QEMU build,
configuration, and path. It cannot safely test every physical bus error,
SError, real-hardware cache-coherency fault, SMP shootdown race, or failure
that destroys exception-vector/stack reachability. Do not intentionally
corrupt the live root or induce a fault whose handler reachability is the
property under test.

### Readiness checklist

Statuses are limited to `PASS`, `FAIL`, and `UNKNOWN`; `PASS` is scoped to the
evidence described and is not permission to activate.

| Requirement | Status | Evidence |
|---|---|---|
| Required mapping completeness | UNKNOWN | Tested layout walks declared kernel, usable HHDM, boot-object, and active-table mappings, but no transition stub or candidate-only hardware probe exists and its exact dependency closure is not yet defined. |
| HHDM coverage | PASS | All validated usable ranges in the recorded QEMU runs are checked/mapped; other firmware layouts require fresh runtime checks. |
| Memory attributes and permissions | PASS | Synthetic/runtime descriptor checks enforce tested MAIR, AP, AF, SH, XN, and W^X policies; no post-switch hardware permission test. |
| Alias safety | PASS | Builder rejects conflicting alias policies in its supported scope; no claim about unrepresented hardware aliases. |
| Active page-table dependencies | UNKNOWN | Reachable scanner inventory is mapped, but only readable reachable pages are covered; old-root recovery/access is not demonstrated. |
| Boot-object lifetime | PASS | Current PMM excludes bootloader-reclaimable memory; lifetime depends on preserving that policy or copying/pinning before reclaim. |
| Stack continuity | UNKNOWN | Both stack halves are mapped, but live SP values/canaries and post-switch operation are not tested. |
| Exception handling | UNKNOWN | Vectors and handler mappings are present, but VBAR readback and exception/return under candidate are not tested. |
| PMM continuity | UNKNOWN | Candidate maps metadata and usable HHDM, but allocation/access/release has not run after activation. |
| TLB and register requirements | UNKNOWN | Register decode is recorded, but exact TTBR/TLBI/barrier sequence and global-entry invalidation remain unimplemented and unreviewed at instruction level. |
| Console continuity | UNKNOWN | Semihosting is QEMU-only and no hardware console driver exists. |
| Pre-switch validation | PASS | Non-activating runtime builder, descriptor walks, ownership/rollback checks and register snapshots pass for recorded QEMU layouts. |
| Post-switch failure handling | UNKNOWN | No post-switch fault test or reachable recovery stub; several failures may prevent handler entry. |
| Recovery strategy | UNKNOWN | Old-root restore, vectors/stack reachability after faults, and post-write fail-stop behavior are not proven. |

**Overall: NOT READY.** Essential continuity, global TLB sequencing,
post-switch exception behavior, and recovery remain unresolved. Retain the
bootloader-memory exclusion policy or copy/pin data before any future reclaim
change. Passing the inactive builder tests is not authorization to activate
the root.

### Blockers and closure evidence

| Blocker | Evidence needed before reconsidering readiness |
|---|---|
| Exact architecture sequence | Pin the A-profile manual revision and target feature set; independently review the TTBR write, descriptor publication, global TLBI scope, DSB/ISB ordering and CnP/ASID behavior against that text. |
| Transition code/data closure | Disassemble the final linked transition stub; enumerate its PC/return, literal, stack, global and call dependencies; software-walk every VA in both roots. |
| Exception continuity/recovery | Read back VBAR and validate vector, SP_EL1, handler code/data; demonstrate a controlled exception/return after activation. Do not claim old-root recovery until an independently mapped restore path and its TLB sequence are proved. |
| Boot-data lifetime | Either preserve permanent exclusion of bootloader-reclaimable pages or implement copying/pinning and demonstrate no post-switch references to reclaimed objects. |
| PMM/table continuity | Run allocation, mapped access, release, counts, and table ownership checks under the candidate root without reclaiming old tables. |
| Console/observability | Establish a tested result-record/QEMU-observation path independent of guest semihost output; no MMIO console assumption. |
| Platform scope | State the supported PE count and configuration. SMP requires reviewed broadcast invalidation/shootdown and synchronization; QEMU uniprocessor results do not close it. |

No transition was attempted. The original transition-protocol review was
documentation-only. The later pre-activation verifier adds read-only candidate
walks and synthetic tests, but introduces no TTBR/TCR/MAIR/SCTLR writes,
active descriptor writes, TLB maintenance, or MMIO accesses.

## Validation performed

For this review, the x86-64 QEMU run printed PMM and VMM success markers,
including `VMM walker, mapping, and address-space tests passed`, before the
intentional timeout (exit 124). The AArch64 build passed; its default QEMU
boot printed the synthetic descriptor, isolated hierarchy, mapping-builder,
and PMM test markers, then reported `Runtime kernel mapping builder coverage,
cleanup, and active-state checks passed (DTB unavailable)` before its
intentional halt/timeout (exit 124). It validated seven usable HHDM regions
totaling `0x7E01D000` bytes. The active-tree scan covered `0x206` reachable
pages; the de-duplicated inventory also contained `0x206` table pages. The
inactive root walked `0x7E2EE` mapped pages and used `0x3FC` (1,020) owned
table pages from its `0x800` (2,048) limit. Builder cleanup restored PMM
counts.

For the runtime-DTB revalidation, the existing temporary Limine `dtb_path`
fixture and QEMU-generated `virt` DTB were copied into a fresh temporary ISO
containing the current AArch64 kernel; the checked-in boot configuration was
not changed. It passed the synthetic and runtime builder markers, validated
seven usable ranges totaling `0x7DF1C000` bytes, enumerated `0x206` unique
active table pages, and produced the same `0x7E2EE` mapped-page and `0x3FC`
owned-table counts. The before/after active-tree checksum was
`0x3BED26446797339D`. FDT parsing discovered PL011 at PA `0x09000000`, size
`0x1000`; the active-table audit found it unmapped, and the isolated Device
proposal remained synthetic. Output explicitly confirms that no PL011
registers were accessed. `MilOS AArch64 diagnostics passed; halting` appeared
before the intentional timeout (exit 124).

Both current-kernel AArch64 runs reported identical pre/post snapshots:
`CurrentEL=0x4`, `SCTLR_EL1=0x30D0199D`,
`TCR_EL1=0x4B5103510`, `TTBR0_EL1=0xBBF68000`,
`TTBR1_EL1=0xBBF67000`, `MAIR_EL1=0xFFFF`, and
`ID_AA64MMFR0_EL1=0x1124`. In the default boot both active-tree scans covered
`0x206` pages with checksum `0xE9805D9FD805A66F`; the DTB boot scans covered
the same count with checksum `0x3BED26446797339D`. These values match within
each boot; they differ between boots because the runtime memory layout and
table descriptors differ.

The default and DTB output both explicitly report that the candidate remains
inactive and that no active descriptor write or TLB maintenance occurred.
Source inspection found system-register reads in `aarch64_paging.cpp`, but no
TTBR/TCR/MAIR/SCTLR writes or `TLBI` instruction in the AArch64 source. The
`MSR` operations found in entry/exception assembly are for stack selection,
exception return state, and VBAR installation; they are outside this
transition experiment. The DTB run explicitly confirms no PL011 register
access.

`git -c core.whitespace=cr-at-eol diff --check` is the whitespace check for
this worktree because earlier architecture documentation uses CRLF. Exact
final Git state is reported with the task completion summary.

## Validation limits

Active register snapshots compare the captured EL, SCTLR, TCR, TTBR0, TTBR1,
MAIR, and MMFR0 values before and after the inactive test. Active-tree
checksums cover descriptors from readable reachable table pages only. Neither
measurement proves that unmapped/unreadable memory was unchanged, nor do they
prove that an address-space transition is safe. The tests do not write
TTBR/TCR/MAIR/SCTLR, modify active descriptors, perform TLB maintenance, or
access MMIO.

## Pre-activation verifier implementation and latest validation

`Hierarchy::verify_candidate` now produces per-check `PASS`, `FAIL`, or
`UNKNOWN` states for the inactive hierarchy. It checks candidate-root
separation, owned-table structure and HHDM aliases, required mapping roles,
per-page translations, physical memory-map backing, exact leaf descriptors,
alias conflicts, full usable-HHDM coverage when the inventory is declared
complete, active-table backing, boot-object lifetime evidence, both stack
ranges, exception-vector mapping, PMM metadata, remaining-capacity evidence,
and unsupported Device mappings. A `PASS` is not inferred from a success
message: the verifier walks candidate translations and compares the encoded
leaf descriptor to the expected policy.

Synthetic fixtures exercise positive checks and negative cases for missing
roles, stack/vector coverage, active-table backing, owned-table corruption,
missing mapped pages, descriptor mutation, conflicting aliases, W+X policy,
invalid MAIR index, non-canonical and overflowing ranges, invalid physical
backing, unknown boot-object lifetime, insufficient known capacity, incomplete
active inventory, active-root collision, and allocation failure. They are
software fixtures, not simulated hardware faults. The verifier's first live
run exposed that the required-role check did not set its `PASS` state; this was
fixed, and the known-good synthetic fixture now asserts each expected
per-check state. Another fixture now removes a leaf descriptor directly and
checks the verifier result, avoiding conflating that test with unmap/pruning
behavior.

Both live runs reported `Pre-activation verifier overall=UNKNOWN`. Candidate
root, ownership, required roles, mapped regions, physical backing, descriptor
policy, alias safety, HHDM coverage, active-table backing, boot-object
lifetime, stack continuity, exception vectors, PMM accessibility, and
unsupported-device checks were `PASS`. Exception dependency closure remains
`UNKNOWN` because the verifier does not establish the full transitive closure
of vector/handler code, stack, globals, and diagnostic accesses. Remaining
capacity is `UNKNOWN`: 1,020 of 2,048 owned-table slots were consumed in this
layout, but future transition/recovery demand is not established. These
unknowns intentionally keep the result `NOT READY`.

The latest default QEMU `virt` boot passed the descriptor, isolated hierarchy,
mapping-builder, pre-activation verifier, synthetic PMM, and live PMM tests.
The inactive builder checked seven usable HHDM ranges totaling
`0x7E00D000` bytes, `0x7E2E1` mapped pages, and `0x3FC` owned page-table pages
out of `0x800` capacity. All `0x206` unique reachable active table pages were
included and mapped through the candidate. The active-table checksum was
`0x421752ED8265ECB9` both before and after the builder; the captured
`CurrentEL`, `SCTLR_EL1`, `TCR_EL1`, `TTBR0_EL1`, `TTBR1_EL1`, `MAIR_EL1`, and
`ID_AA64MMFR0_EL1` snapshots also matched before and after. The builder's
coverage, cleanup, and active-state success marker appeared before the
expected timeout (exit 124) and the intentional no-DTB halt.

The default runtime verifier values were:

```text
candidate-root=PASS
owned-table-pages=PASS
required-roles=PASS
mapped-regions=PASS
physical-backing=PASS
descriptor-policy=PASS
alias-safety=PASS
hhdm-coverage=PASS
active-table-backing=PASS
boot-object-lifetime=PASS
stack-continuity=PASS
exception-vectors=PASS
exception-dependencies=UNKNOWN
pmm-accessibility=PASS
remaining-capacity=UNKNOWN
unsupported-device-mappings=PASS
```

A second current-kernel run used a QEMU-generated `virt` DTB via a temporary
Limine `dtb_path` ISO; the checked-in boot configuration was unchanged. It
passed the same verifier checks and retained the same `0x206` active table
pages, `0x7E2E1` candidate pages, and `0x3FC` owned pages. Its HHDM coverage
was seven usable ranges totaling `0x7DF0C000` bytes. The active-table checksum
was `0x80072467BB06A720` before and after, and all captured translation
registers matched across the builder. The DTB run also passed the isolated
synthetic Device proposal without installing it, discovered PL011 at PA
`0x09000000` with size `0x1000`, and confirmed the active-table audit found it
unmapped. It explicitly printed `PL011 registers were not accessed;
semihosting remains active` and `MilOS AArch64 diagnostics passed; halting`
before the expected timeout (exit 124).

The x86-64 build and bounded QEMU regression also passed its PMM validation,
VMM translation, mapping, address-space, unmapped-address, and large-page
markers before timeout (exit 124). The make-run wrapper did not preserve the
multiword `QEMUFLAGS` argument in this Windows/WSL invocation (`qemu-system`
received `-m` without a value), so the x86-64 QEMU image was run directly with
the repository's UEFI/QEMU configuration for this regression.

During verifier integration, a direct `MRS SP_EL0` diagnostic caused an
undefined-instruction exception on the tested QEMU `cortex-a72` boot at
`ELR_EL1=0xFFFFFFFF80015F8C` (`ESR_EL1=0x02000000`). The check now obtains the
currently selected SP with `MOV ..., SP` (the kernel keeps `SPSel=0` after
entry) and uses the SP_EL1 top recorded by entry assembly. The subsequent
default and runtime-DTB boots passed stack-continuity verification. This is
runtime evidence for this QEMU configuration only; it is not an architectural
claim that general-purpose software may omit verifying the active stack
selection and bounds.

Overall status remains **NOT READY**. No TTBR switch, translation-register
write, active-descriptor modification, TLB maintenance, or MMIO access was
performed. The verifier and capacity results below supersede the earlier
`UNKNOWN` results for those two checks. Post-switch exception recovery,
transition ordering/invalidation, console continuity, and the runtime-DTB
failure recorded below remain blockers.

## Exception dependency and capacity follow-up

This section records the latest source changes and current-kernel evidence;
earlier verifier output above is historical and its `UNKNOWN` exception and
capacity states are superseded here.

### Exception dependency closure

The verifier no longer accepts a caller-supplied boolean as proof of
exception-code closure. `main.cpp` derives three dependency ranges from the
linker-defined text, rodata, and writable-image boundaries, intersects them
with the builder's corresponding regions, and supplies explicit dependency
metadata. `Hierarchy::verify_candidate` requires the expected roles, complete
page-aligned coverage, valid source ranges and physical backing, successful
candidate walks, and exact leaf policies/descriptors. It therefore checks
the full statically linked image's executable code, constants, and writable
state, rather than only checking that VBAR points into an executable range.
The verifier separately checks the complete vector extent and both stack
halves, including their permissions and the vector's containing descriptor
page.

The reviewed control path is: vector slot in `entry.S` -> per-vector save
stub -> `aarch64_exception_dispatch` in `exceptions.cpp` -> diagnostic
helpers in `debug.hpp` on the fatal path. Stack frame storage is on the
exception stack; exception globals are in BSS; referenced format strings and
constants are in rodata. These dependencies are covered conservatively by
the complete linker image ranges. The AArch64 ELF has no undefined symbols
and no relocations, which bounds linked external/runtime dependencies for
this image. This is a mapping-closure result, not a proof of dynamic
exception recovery or safe operation after changing roots.

Synthetic verifier cases now cover absent vector/handler mappings, missing
handler pages, inaccessible writable handler data, incorrect dependency
permissions, invalid dependency ranges, and missing/incomplete dependency
metadata. These cases prove the verifier rejects the represented failures;
they do not emulate hardware exceptions or a post-switch fault.

### Page-table capacity evidence

For the tested four-level, 4-KiB regime, one additional 4-KiB leaf mapping
whose VA falls under entirely fresh intermediate branches needs at most
three new owned table pages: one each at levels 1, 2, and 3; the existing
root is not counted as an additional page. `main.cpp` declares this as a
three-page reserve and the live verifier checks that the hierarchy still has
at least that many registry slots. This is a narrowly scoped reserve for one
future fresh-VA probe, not a reserve for unspecified transition features.

The current default QEMU layout measured `0x3FC` (1,020) owned pages from a
`0x800` (2,048) page registry. The candidate walked `0x7E2DD` virtual pages;
seven validated usable ranges contained `0x7E008` physical pages. After
reserving three pages, `0x401` (1,025) registry slots remain. The measured
table count comes from the actual built tree, including shared branches and
sparse ranges; it is not extrapolated from a fixed multiplier. Synthetic
capacity tests exercise exhaustion, rollback, unmapping/pruning, and retry.
The existing allocation-failure paths continue to return explicit errors
and preserve ownership accounting.

No finite capacity for arbitrary future features can be established from the
current design. As a deliberately theoretical bound, fully populating every
4-KiB leaf in a 48-bit four-level TTBR1 aperture would require
`1 + 512 + 262,144 + 134,217,728 = 134,480,385` table pages. This is not the
declared supported layout; it demonstrates that the 2,048-page registry is
not an arbitrary-aperture guarantee. Any additional retained mappings,
recovery trampoline, transition data, or device mapping needs a specified
layout, a derived table count, and an exhaustion/rollback test before its
capacity can be marked supported.

### Latest default QEMU verification

The current AArch64 kernel was booted on QEMU `virt` / `cortex-a72` without a
runtime DTB. Synthetic descriptor, isolated hierarchy, builder, verifier,
and PMM tests passed. Every reported pre-activation verifier check passed:

```text
candidate-root=PASS
owned-table-pages=PASS
required-roles=PASS
mapped-regions=PASS
physical-backing=PASS
descriptor-policy=PASS
alias-safety=PASS
hhdm-coverage=PASS
active-table-backing=PASS
boot-object-lifetime=PASS
stack-continuity=PASS
exception-vectors=PASS
exception-dependencies=PASS
pmm-accessibility=PASS
remaining-capacity=PASS
unsupported-device-mappings=PASS
```

The candidate walked `0x7E2DD` pages and used `0x3FC` of `0x800` owned table
pages; the explicit three-page probe reserve left `0x401`. Candidate
destruction returned the owned pages to PMM. Active-tree scans each covered
`0x206` reachable table pages and matched checksum
`0xDDBB701D058F74EB`. Before/after snapshots matched:
`CurrentEL=0x4`, `SCTLR_EL1=0x30D0199D`,
`TCR_EL1=0x4B5103510`, `TTBR0_EL1=0xBBF68000`,
`TTBR1_EL1=0xBBF67000`, `MAIR_EL1=0xFFFF`, and
`ID_AA64MMFR0_EL1=0x1124`. The candidate remained inactive. The expected
timeout exit 124 occurred after the builder cleanup and active-state markers;
the no-DTB halt marker appeared.

### Runtime-DTB result and exception caveat

The current-kernel runtime-DTB revalidation did **not** pass. Two temporary
ISO attempts supplied the previously generated QEMU `virt` DTB; both emitted
`FDT parser tests passed` and then stalled before the paging synthetic-test
success marker. QEMU `-d int` records an EL1 prefetch abort at
`0xFFFFFFFF8000CC00` (`ESR=0x86000007`, level-3 translation fault), in
`snapshot_table_root`, followed by repeated aborts while fetching
`0xFFFFFFFF800025A8` in the exception handler. Therefore that DTB run has no
candidate verifier, register-snapshot, active-tree checksum, or builder
result. The evidence is consistent with the active Limine root lacking
executable mappings for those code pages in that run, but the cause was not
isolated; classify the DTB runtime validation as **FAIL/UNRESOLVED**, not as a
candidate-verifier failure or a passing test. It also demonstrates why a
handler's existence alone does not prove it can handle a translation fault.
The default run does not exercise a hardware exception during the candidate
test, and no post-switch exception test was attempted.

### Updated readiness checklist

| Requirement | Status | Evidence and remaining limitation |
|---|---|---|
| Required kernel mappings | PASS (default QEMU scope) | Full candidate walks and exact descriptors for declared image and boot-object ranges. |
| HHDM coverage | PASS (default QEMU scope) | All seven validated usable ranges/pages were walked and checked. |
| Permissions and memory attributes | PASS (declared candidate scope) | Exact descriptor policy checks include EL1/EL0 access, AF, AttrIndx, shareability, PXN/UXN, and W^X. Other platforms/layouts require fresh evidence. |
| Alias safety | PASS (declared candidate scope) | Builder and verifier reject conflicting physical aliases and W+X policy. |
| Active page-table dependencies | PASS (default QEMU scope) | All `0x206` inventoried reachable table pages were mapped/read through candidate HHDM; unreadable/unreachable descriptors remain outside checksum coverage. |
| Exception mapping closure | PASS (static linked-image mapping scope) | Derived full text/rodata/writable ranges, vector range, and stack descriptors pass. Post-switch fault delivery/recovery is UNKNOWN; runtime-DTB boot separately hit an active-root translation fault. |
| Boot-object lifetime | PASS (current allocator policy only) | PMM allocates only `USABLE`; bootloader-reclaimable inputs are retained for the tested boot. A future reclaim/copy policy is unspecified. |
| Stack continuity | PASS (default layout only) | Both stack halves are mapped RW/NX at current VAs; no root switch or SP change was performed. |
| PMM continuity | PASS (default QEMU scope) | Runtime allocate/access/release and candidate cleanup restore PMM counts; no operations were performed under the candidate as active root. |
| Capacity | PASS (one fresh-VA probe reserve only) | 1,020 measured + 3 reserved <= 2,048. Unspecified future mapping demand remains UNKNOWN. |
| TLB/register requirements | UNKNOWN | No activation or TLBI; a source-backed transition sequence and target-specific validation remain required. |
| Console continuity | UNKNOWN | Semihosting only; no hardware console path is established. |
| Pre-switch validation | PASS (default QEMU scope) | Inactive software walks, verifier, synthetic tests, and unchanged register snapshots pass. |
| Post-switch failure handling | UNKNOWN | No handler/stack/data continuity proof after changing roots, and runtime-DTB active exception handling failed as recorded. |
| Recovery strategy | UNKNOWN | No recovery path has been proven; no assumption that restoring the old root is safe. |
| Runtime-DTB boot | FAIL/UNRESOLVED | Current-kernel attempt faults before candidate verification; root cause not isolated. |

**Overall readiness remains NOT READY.** The default candidate verifier now
passes its statically described mapping closure and narrowly scoped capacity
reserve, but the runtime-DTB regression is unresolved, arbitrary near-term
capacity is unspecified, and register/TLB sequencing, post-switch exception
handling, console continuity, and recovery remain unproven. No transition was
implemented or attempted: no active root or translation-control register was
written, no active descriptor was modified, no TLBI was issued, and no MMIO
register was accessed.
