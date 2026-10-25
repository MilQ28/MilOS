#include <cstdint>
#include <limine.h>
#include "aarch64_paging.hpp"
#include "debug.hpp"
#include "exceptions.hpp"
#include "fdt.hpp"

namespace {

constexpr std::uint64_t FDT_HEADER_SIZE = 40;

__attribute__((used, section(".limine_requests")))
volatile std::uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

__attribute__((used, section(".limine_requests_start")))
volatile std::uint64_t limine_requests_start_marker[] =
    LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
volatile std::uint64_t limine_requests_end_marker[] =
    LIMINE_REQUESTS_END_MARKER;

__attribute__((used, section(".limine_requests")))
volatile limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".limine_requests")))
volatile limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".limine_requests")))
volatile limine_dtb_request dtb_request = {
    .id = LIMINE_DTB_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

std::uint32_t read_be32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

bool dtb_range_is_in_bootloader_memory(
    const std::uint8_t* dtb,
    std::uint64_t total_size,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memmap
) {
    const auto virtual_address =
        reinterpret_cast<std::uint64_t>(dtb);
    if (virtual_address < hhdm_offset ||
        total_size == 0 ||
        virtual_address - hhdm_offset >
            ~std::uint64_t{0} - total_size ||
        memmap == nullptr ||
        memmap->entries == nullptr ||
        memmap->entry_count == 0)
        return false;

    const std::uint64_t physical_address =
        virtual_address - hhdm_offset;
    const std::uint64_t physical_end =
        physical_address + total_size;

    for (std::uint64_t i = 0; i < memmap->entry_count; ++i) {
        const limine_memmap_entry* entry = memmap->entries[i];
        if (entry == nullptr ||
            entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            entry->length == 0 ||
            entry->base > ~std::uint64_t{0} - entry->length)
            continue;

        const std::uint64_t region_end = entry->base + entry->length;
        if (physical_address >= entry->base &&
            physical_end <= region_end)
            return true;
    }
    return false;
}

[[noreturn]] void halt() {
    for (;;) {
        asm volatile("wfi");
    }
}

void write_named_hex(const char* name, std::uint64_t value) {
    MilOS::AArch64::Debug::write(name);
    MilOS::AArch64::Debug::write_hex(value);
    MilOS::AArch64::Debug::write("\n");
}

bool same_register_snapshot(
    const MilOS::AArch64::Paging::RegisterSnapshot& left,
    const MilOS::AArch64::Paging::RegisterSnapshot& right
) {
    return left.current_el == right.current_el &&
        left.sctlr_el1 == right.sctlr_el1 &&
        left.tcr_el1 == right.tcr_el1 &&
        left.ttbr0_el1 == right.ttbr0_el1 &&
        left.ttbr1_el1 == right.ttbr1_el1 &&
        left.mair_el1 == right.mair_el1 &&
        left.id_aa64mmfr0_el1 == right.id_aa64mmfr0_el1;
}

void report_paging_registers(
    const MilOS::AArch64::Paging::RegisterSnapshot& registers
) {
    write_named_hex("CurrentEL=", registers.current_el);
    write_named_hex("SCTLR_EL1=", registers.sctlr_el1);
    write_named_hex("TCR_EL1=", registers.tcr_el1);
    write_named_hex("TTBR0_EL1=", registers.ttbr0_el1);
    write_named_hex("TTBR1_EL1=", registers.ttbr1_el1);
    write_named_hex("MAIR_EL1=", registers.mair_el1);
    write_named_hex(
        "ID_AA64MMFR0_EL1=",
        registers.id_aa64mmfr0_el1
    );

    MilOS::AArch64::Paging::Regime regime{};
    const auto status =
        MilOS::AArch64::Paging::detect_regime(registers, regime);
    if (status ==
        MilOS::AArch64::Paging::RegimeStatus::supported) {
        MilOS::AArch64::Debug::write(
            "Translation regime: 4KiB granule, 4 levels, VA=48 bits, PA="
        );
        MilOS::AArch64::Debug::write_hex(
            regime.physical_address_bits
        );
        MilOS::AArch64::Debug::write(" bits; SCTLR_EL1.M=1\n");
    } else {
        MilOS::AArch64::Debug::write(
            "Translation regime unsupported or inconsistent; raw registers above\n"
        );
    }
}

const char* root_scan_status_name(
    MilOS::AArch64::Paging::RootScanStatus status
) {
    using MilOS::AArch64::Paging::RootScanStatus;
    switch (status) {
        case RootScanStatus::complete: return "complete";
        case RootScanStatus::inaccessible_table: return "inaccessible table";
        case RootScanStatus::malformed_descriptor:
            return "malformed descriptor";
        case RootScanStatus::budget_exhausted:
            return "scan bound exhausted";
    }
    return "unknown";
}

const char* memory_type_name(
    MilOS::AArch64::Paging::MemoryType type
) {
    using MilOS::AArch64::Paging::MemoryType;
    switch (type) {
        case MemoryType::device: return "Device";
        case MemoryType::normal: return "Normal";
        case MemoryType::unknown: return "unknown";
    }
    return "unknown";
}

void report_mapping(
    const MilOS::AArch64::Paging::Mapping& mapping
) {
    using MilOS::AArch64::Paging::MemoryType;
    MilOS::AArch64::Debug::write("mapping VA=");
    MilOS::AArch64::Debug::write_hex(mapping.virtual_base);
    MilOS::AArch64::Debug::write(" PA=");
    MilOS::AArch64::Debug::write_hex(mapping.physical_base);
    MilOS::AArch64::Debug::write(" size=");
    MilOS::AArch64::Debug::write_hex(mapping.size);
    MilOS::AArch64::Debug::write(" level=");
    MilOS::AArch64::Debug::write_hex(mapping.level);
    MilOS::AArch64::Debug::write("\n  PL011-range VA=");
    MilOS::AArch64::Debug::write_hex(mapping.virtual_address);
    MilOS::AArch64::Debug::write(" => PA=");
    MilOS::AArch64::Debug::write_hex(
        mapping.translated_physical_address
    );
    MilOS::AArch64::Debug::write("\n  permissions: EL1=");
    MilOS::AArch64::Debug::write(
        mapping.privileged_writable ? "RW" : "RO"
    );
    MilOS::AArch64::Debug::write(" EL0=");
    MilOS::AArch64::Debug::write(
        !mapping.unprivileged_access
            ? "none"
            : (mapping.unprivileged_writable ? "RW" : "RO")
    );
    MilOS::AArch64::Debug::write(" PXN=");
    MilOS::AArch64::Debug::write(
        mapping.privileged_execute_never ? "yes" : "no"
    );
    MilOS::AArch64::Debug::write(" UXN=");
    MilOS::AArch64::Debug::write(
        mapping.unprivileged_execute_never ? "yes" : "no"
    );
    MilOS::AArch64::Debug::write(" AF=");
    MilOS::AArch64::Debug::write(
        mapping.access_flag ? "1" : "0"
    );
    MilOS::AArch64::Debug::write("\n  SH=");
    switch (mapping.shareability) {
        case 0:
            MilOS::AArch64::Debug::write("non-shareable");
            break;
        case 1:
            MilOS::AArch64::Debug::write("reserved");
            break;
        case 2:
            MilOS::AArch64::Debug::write("outer-shareable");
            break;
        case 3:
            MilOS::AArch64::Debug::write("inner-shareable");
            break;
    }
    MilOS::AArch64::Debug::write(" AttrIndx=");
    MilOS::AArch64::Debug::write_hex(mapping.attribute_index);
    MilOS::AArch64::Debug::write(" MAIR=");
    MilOS::AArch64::Debug::write_hex(
        (mapping.mair_value >> (mapping.attribute_index * 8)) & 0xff
    );
    MilOS::AArch64::Debug::write(" (");
    MilOS::AArch64::Debug::write(memory_type_name(mapping.memory_type));
    MilOS::AArch64::Debug::write(")\n");
}

void report_pl011_audit(
    const MilOS::AArch64::Paging::AuditReport& report
) {
    using MilOS::AArch64::Paging::AuditStatus;
    MilOS::AArch64::Debug::write("PL011 active-table audit: ");
    switch (report.status) {
        case AuditStatus::mapped:
            MilOS::AArch64::Debug::write("mapped");
            break;
        case AuditStatus::unmapped:
            MilOS::AArch64::Debug::write("unmapped");
            break;
        case AuditStatus::inconclusive:
            MilOS::AArch64::Debug::write("inconclusive");
            break;
        case AuditStatus::unsupported_regime:
            MilOS::AArch64::Debug::write("unsupported regime");
            break;
        case AuditStatus::invalid_range:
            MilOS::AArch64::Debug::write("invalid range");
            break;
    }
    MilOS::AArch64::Debug::write("\nTTBR0 scan: ");
    MilOS::AArch64::Debug::write(
        root_scan_status_name(report.ttbr0_status)
    );
    MilOS::AArch64::Debug::write("; TTBR1 scan: ");
    MilOS::AArch64::Debug::write(
        root_scan_status_name(report.ttbr1_status)
    );
    MilOS::AArch64::Debug::write("; table pages scanned=");
    MilOS::AArch64::Debug::write_hex(report.table_pages_scanned);
    MilOS::AArch64::Debug::write("\n");
    for (std::uint32_t i = 0; i < report.mapping_count; ++i)
        report_mapping(report.mappings[i]);
    for (std::uint32_t i = 0; i < report.gap_count; ++i) {
        MilOS::AArch64::Debug::write("unmapped physical gap base=");
        MilOS::AArch64::Debug::write_hex(report.gaps[i].base);
        MilOS::AArch64::Debug::write(" size=");
        MilOS::AArch64::Debug::write_hex(report.gaps[i].size);
        MilOS::AArch64::Debug::write("\n");
    }
    if (report.status == AuditStatus::mapped) {
        MilOS::AArch64::Debug::write(
            report.fully_covered
                ? "PL011 physical range fully covered by observed mappings\n"
                : "PL011 physical range has mapping gaps\n"
        );
        MilOS::AArch64::Debug::write(
            "A mapping and Device attributes alone do not establish peripheral safety\n"
        );
    }
}

void report_table_page_location(
    const char* name,
    std::uint64_t ttbr,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map
) {
    constexpr std::uint64_t address_mask = 0x0000FFFFFFFFF000ull;
    const std::uint64_t physical_address = ttbr & address_mask;
    MilOS::AArch64::Debug::write(name);
    MilOS::AArch64::Debug::write(" root PA=");
    MilOS::AArch64::Debug::write_hex(physical_address);
    MilOS::AArch64::Debug::write(" HHDM VA=");
    if (physical_address <= ~std::uint64_t{0} - hhdm_offset)
        MilOS::AArch64::Debug::write_hex(
            hhdm_offset + physical_address
        );
    else
        MilOS::AArch64::Debug::write("overflow");

    bool found = false;
    if (memory_map != nullptr && memory_map->entries != nullptr) {
        for (std::uint64_t i = 0; i < memory_map->entry_count; ++i) {
            const limine_memmap_entry* entry = memory_map->entries[i];
            if (entry == nullptr ||
                entry->base > ~std::uint64_t{0} - entry->length)
                continue;
            const std::uint64_t end = entry->base + entry->length;
            if (physical_address < entry->base ||
                physical_address > end ||
                end - physical_address < 4096)
                continue;
            MilOS::AArch64::Debug::write(" memmap type=");
            MilOS::AArch64::Debug::write_hex(entry->type);
            MilOS::AArch64::Debug::write(" region=");
            MilOS::AArch64::Debug::write_hex(entry->base);
            MilOS::AArch64::Debug::write("-");
            MilOS::AArch64::Debug::write_hex(end);
            found = true;
            break;
        }
    }
    if (!found)
        MilOS::AArch64::Debug::write(" memmap region=none");
    MilOS::AArch64::Debug::write("\n");
}

}

extern "C" [[noreturn]] void aarch64_kernel_main() {
    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision)) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: unsupported Limine base revision\n");
        halt();
    }

    MilOS::AArch64::Debug::write(
        "MilOS AArch64 kernel entered via Limine\n");

    const std::uint64_t exception_level =
        MilOS::AArch64::install_exception_vectors();
    if (exception_level == 0) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: unsupported exception level\n");
        halt();
    }
    MilOS::AArch64::Debug::write("Exception vectors installed at EL");
    MilOS::AArch64::Debug::write_hex(exception_level);
    MilOS::AArch64::Debug::write("\n");

#if defined(MILOS_AARCH64_EXCEPTION_TEST)
    if (!MilOS::AArch64::run_exception_diagnostic_test()) {
        MilOS::AArch64::Debug::write(
            "AArch64 controlled exception test failed\n");
        halt();
    }
    MilOS::AArch64::Debug::write(
        "AArch64 controlled exception test passed\n");
#endif

    if (!MilOS::AArch64::run_fdt_parser_tests()) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: FDT parser tests failed\n");
        halt();
    }
    MilOS::AArch64::Debug::write("FDT parser tests passed\n");

    const std::uint32_t paging_test_result =
        MilOS::AArch64::Paging::run_synthetic_tests();
    if (paging_test_result != 0) {
        MilOS::AArch64::Debug::write("AArch64 synthetic test failed: ");
        MilOS::AArch64::Debug::write_hex(paging_test_result);
        MilOS::AArch64::Debug::write("\n");
        halt();
    }
    MilOS::AArch64::Debug::write(
        "AArch64 synthetic descriptor tests passed\n"
    );

    const auto paging_registers =
        MilOS::AArch64::Paging::read_registers();
    report_paging_registers(paging_registers);

    if (hhdm_request.response == nullptr) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: Limine HHDM unavailable\n");
        halt();
    }
    if (memmap_request.response == nullptr) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: Limine memory map unavailable\n");
        halt();
    }
    if (dtb_request.response == nullptr) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: Limine DTB response unavailable\n");
        halt();
    }
    if (dtb_request.response->dtb_ptr == nullptr) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: Limine DTB pointer unavailable\n");
        halt();
    }

    const auto* dtb = static_cast<const std::uint8_t*>(
        dtb_request.response->dtb_ptr);
    const std::uint64_t hhdm_offset = hhdm_request.response->offset;
    const auto* memmap = memmap_request.response;
    write_named_hex("HHDM offset=", hhdm_offset);
    report_table_page_location(
        "TTBR0",
        paging_registers.ttbr0_el1,
        hhdm_offset,
        memmap
    );
    report_table_page_location(
        "TTBR1",
        paging_registers.ttbr1_el1,
        hhdm_offset,
        memmap
    );
    if (!dtb_range_is_in_bootloader_memory(
            dtb, FDT_HEADER_SIZE, hhdm_offset, memmap)) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: DTB header is outside bootloader-reclaimable memory\n");
        halt();
    }
    if (read_be32(dtb) != 0xD00DFEED) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: invalid DTB magic\n");
        halt();
    }

    const std::uint64_t dtb_size = read_be32(dtb + 4);
    if (dtb_size < 40 ||
        !dtb_range_is_in_bootloader_memory(
            dtb, dtb_size, hhdm_offset, memmap)) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: DTB is outside bootloader-reclaimable memory\n");
        halt();
    }

    MilOS::AArch64::Pl011Info uart{};
    if (!MilOS::AArch64::find_enabled_pl011(dtb, dtb_size, uart)) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: no valid enabled PL011 node in DTB\n");
        halt();
    }

    MilOS::AArch64::Debug::write("DTB PL011 reg base=");
    MilOS::AArch64::Debug::write_hex(uart.register_base);
    MilOS::AArch64::Debug::write(" size=");
    MilOS::AArch64::Debug::write_hex(uart.register_size);
    if (uart.has_clock_frequency) {
        MilOS::AArch64::Debug::write(" clock-frequency=");
        MilOS::AArch64::Debug::write_hex(uart.clock_frequency);
    }
    MilOS::AArch64::Debug::write("\n");
    const auto audit = MilOS::AArch64::Paging::inspect_physical_range(
        paging_registers,
        hhdm_offset,
        memmap,
        uart.register_base,
        uart.register_size
    );
    report_pl011_audit(audit);

    MilOS::AArch64::Paging::Regime experiment_regime{};
    if (MilOS::AArch64::Paging::detect_regime(
            paging_registers,
            experiment_regime) !=
        MilOS::AArch64::Paging::RegimeStatus::supported) {
        MilOS::AArch64::Debug::write(
            "AArch64 synthetic Device-map proposal skipped: unsupported live regime\n");
        halt();
    }

    MilOS::AArch64::Debug::write(
        "Testing isolated synthetic PL011 Device-map proposal only\n"
        "Proposed MAIR AttrIndx=0 value=0x00 (Device-nGnRnE), not active\n"
        "Proposed mapping: 4KiB page, AP=00 EL1 RW/EL0 none, PXN=1, UXN=1, SH=Outer\n"
    );
    const auto registers_before_experiment =
        MilOS::AArch64::Paging::read_registers();
    const std::uint32_t device_mapping_test_result =
        MilOS::AArch64::Paging::run_isolated_device_mapping_tests(
            experiment_regime,
            {uart.register_base, uart.register_size}
        );
    const auto registers_after_experiment =
        MilOS::AArch64::Paging::read_registers();
    if (device_mapping_test_result != 0) {
        MilOS::AArch64::Debug::write(
            "AArch64 isolated Device-map proposal test failed: ");
        MilOS::AArch64::Debug::write_hex(
            device_mapping_test_result);
        MilOS::AArch64::Debug::write("\n");
        halt();
    }
    if (!same_register_snapshot(
            registers_before_experiment,
            registers_after_experiment)) {
        MilOS::AArch64::Debug::write(
            "AArch64 translation-register snapshot changed during synthetic test\n");
        halt();
    }
    MilOS::AArch64::Debug::write(
        "AArch64 isolated synthetic Device-map proposal tests passed; mapping not installed\n"
        "Active translation-register snapshot unchanged; experiment used only synthetic RAM tables\n"
        "No active translation descriptor was written\n"
    );

    MilOS::AArch64::Debug::write(
        "PL011 registers were not accessed; semihosting remains active\n"
    );
    MilOS::AArch64::Debug::write(
        "MilOS AArch64 diagnostics passed; halting\n");
    halt();
}
