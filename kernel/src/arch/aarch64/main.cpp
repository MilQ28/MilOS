#include <cstdint>
#include <limine.h>
#include "aarch64_address_space.hpp"
#include "aarch64_paging.hpp"
#include "aarch64_pmm.hpp"
#include "debug.hpp"
#include "exceptions.hpp"
#include "fdt.hpp"

namespace {

namespace AddressSpace = MilOS::AArch64::AddressSpace;
namespace Memory = MilOS::AArch64::Memory;

constexpr std::uint64_t FDT_HEADER_SIZE = 40;
constexpr std::uint64_t PAGE_SIZE = 4096;
constexpr std::uint64_t MAX_INVENTORY_PAGES = 16384;
constexpr std::uint64_t MAX_ACTIVE_TABLE_PAGES = 8192;
std::uint64_t active_table_pages[MAX_ACTIVE_TABLE_PAGES]{};

extern "C" char __kernel_image_start[];
extern "C" char __kernel_image_end[];
extern "C" char __limine_requests_start[];
extern "C" char __limine_requests_end[];
extern "C" char __text_start[];
extern "C" char __text_end[];
extern "C" char __rodata_start[];
extern "C" char __rodata_end[];
extern "C" char __writable_start[];
extern "C" char __writable_end[];
extern "C" std::uint64_t aarch64_boot_stack_top;
extern "C" std::uint64_t aarch64_boot_normal_stack_top;
extern "C" char aarch64_exception_vectors[];

std::uint64_t read_vbar_el1() {
    std::uint64_t value;
    asm volatile("mrs %0, VBAR_EL1" : "=r"(value));
    return value;
}

std::uint64_t read_current_sp() {
    std::uint64_t value;
    asm volatile("mov %0, sp" : "=r"(value));
    return value;
}

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

__attribute__((used, section(".limine_requests")))
volatile limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

struct RangeInventory {
    bool complete;
    bool physically_contiguous;
    bool uniform_attributes;
    bool uniform_memory_type;
    std::uint64_t first_page_physical;
    std::uint64_t last_page_physical;
    std::uint64_t physical_start;
    std::uint64_t physical_end;
    std::uint64_t page_count;
    std::uint64_t memory_map_type;
    MilOS::AArch64::Paging::Mapping first_mapping;
};

const char* active_translation_status_name(
    MilOS::AArch64::Paging::ActiveTranslationStatus status
) {
    using MilOS::AArch64::Paging::ActiveTranslationStatus;
    switch (status) {
        case ActiveTranslationStatus::mapped: return "mapped";
        case ActiveTranslationStatus::unmapped: return "unmapped";
        case ActiveTranslationStatus::invalid_address: return "invalid address";
        case ActiveTranslationStatus::unsupported_regime:
            return "unsupported regime";
        case ActiveTranslationStatus::inaccessible_table:
            return "inaccessible table";
        case ActiveTranslationStatus::malformed_descriptor:
            return "malformed descriptor";
    }
    return "unknown";
}

const char* memory_map_type_name(std::uint64_t type) {
    switch (type) {
        case LIMINE_MEMMAP_USABLE: return "usable";
        case LIMINE_MEMMAP_RESERVED: return "reserved";
        case LIMINE_MEMMAP_ACPI_RECLAIMABLE: return "ACPI-reclaimable";
        case LIMINE_MEMMAP_ACPI_NVS: return "ACPI-NVS";
        case LIMINE_MEMMAP_BAD_MEMORY: return "bad-memory";
        case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            return "bootloader-reclaimable";
        case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            return "executable/modules";
        case LIMINE_MEMMAP_FRAMEBUFFER: return "framebuffer";
        case LIMINE_MEMMAP_RESERVED_MAPPED: return "reserved-mapped";
        default: return "unknown";
    }
}

bool classify_physical_page(
    const limine_memmap_response* memory_map,
    std::uint64_t physical_address,
    std::uint64_t& type
) {
    if (memory_map == nullptr ||
        memory_map->entries == nullptr ||
        (physical_address & (PAGE_SIZE - 1)) != 0)
        return false;
    bool found = false;
    for (std::uint64_t i = 0; i < memory_map->entry_count; ++i) {
        const limine_memmap_entry* entry = memory_map->entries[i];
        if (entry == nullptr || entry->length < PAGE_SIZE ||
            entry->base > ~std::uint64_t{0} - entry->length)
            continue;
        const std::uint64_t end = entry->base + entry->length;
        if (physical_address < entry->base ||
            physical_address > end - PAGE_SIZE)
            continue;
        if (found)
            return false;
        type = entry->type;
        found = true;
    }
    return found;
}

bool same_mapping_attributes(
    const MilOS::AArch64::Paging::Mapping& left,
    const MilOS::AArch64::Paging::Mapping& right
) {
    return left.mair_value == right.mair_value &&
        left.attribute_index == right.attribute_index &&
        left.shareability == right.shareability &&
        left.privileged_writable == right.privileged_writable &&
        left.unprivileged_access == right.unprivileged_access &&
        left.unprivileged_writable == right.unprivileged_writable &&
        left.privileged_execute_never ==
            right.privileged_execute_never &&
        left.unprivileged_execute_never ==
            right.unprivileged_execute_never &&
        left.access_flag == right.access_flag &&
        left.memory_type == right.memory_type;
}

RangeInventory inspect_active_range(
    const char* name,
    std::uint64_t virtual_start,
    std::uint64_t virtual_end,
    const MilOS::AArch64::Paging::RegisterSnapshot& registers,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map
) {
    using MilOS::AArch64::Paging::ActiveTranslationStatus;
    using MilOS::AArch64::Paging::Mapping;

    RangeInventory inventory{};
    inventory.physically_contiguous = true;
    inventory.uniform_attributes = true;
    inventory.uniform_memory_type = true;
    if (virtual_start >= virtual_end) {
        MilOS::AArch64::Debug::write("[VA inventory] ");
        MilOS::AArch64::Debug::write(name);
        MilOS::AArch64::Debug::write(": invalid or empty range\n");
        return inventory;
    }

    const std::uint64_t first_virtual_page =
        virtual_start & ~(PAGE_SIZE - 1);
    const std::uint64_t last_virtual_page =
        (virtual_end - 1) & ~(PAGE_SIZE - 1);
    inventory.page_count =
        (last_virtual_page - first_virtual_page) / PAGE_SIZE + 1;
    if (inventory.page_count > MAX_INVENTORY_PAGES) {
        MilOS::AArch64::Debug::write("[VA inventory] ");
        MilOS::AArch64::Debug::write(name);
        MilOS::AArch64::Debug::write(": range exceeds diagnostic bound\n");
        return inventory;
    }

    bool have_type = false;
    std::uint64_t current_virtual_page = first_virtual_page;
    while (true) {
        const auto translation =
            MilOS::AArch64::Paging::translate_active_address(
                registers,
                hhdm_offset,
                memory_map,
                current_virtual_page
            );
        if (translation.status != ActiveTranslationStatus::mapped) {
            MilOS::AArch64::Debug::write("[VA inventory] ");
            MilOS::AArch64::Debug::write(name);
            MilOS::AArch64::Debug::write(": ");
            MilOS::AArch64::Debug::write(
                active_translation_status_name(translation.status)
            );
            MilOS::AArch64::Debug::write(" at VA=");
            MilOS::AArch64::Debug::write_hex(current_virtual_page);
            MilOS::AArch64::Debug::write("\n");
            return inventory;
        }

        const Mapping& mapping = translation.mapping;
        const std::uint64_t page_index =
            (current_virtual_page - first_virtual_page) / PAGE_SIZE;
        if (page_index == 0) {
            inventory.first_page_physical =
                mapping.translated_physical_address;
            inventory.first_mapping = mapping;
        } else {
            const std::uint64_t physical_delta =
                    current_virtual_page - first_virtual_page;
            if (inventory.first_page_physical >
                        ~std::uint64_t{0} - physical_delta ||
                    mapping.translated_physical_address !=
                        inventory.first_page_physical +
                            physical_delta)
                    inventory.physically_contiguous = false;
            inventory.uniform_attributes =
                    inventory.uniform_attributes &&
                    same_mapping_attributes(inventory.first_mapping, mapping);
        }

        std::uint64_t memory_type = 0;
        if (!classify_physical_page(
                memory_map,
                mapping.translated_physical_address,
                memory_type)) {
            inventory.uniform_memory_type = false;
        } else if (!have_type) {
            inventory.memory_map_type = memory_type;
            have_type = true;
        } else if (inventory.memory_map_type != memory_type) {
            inventory.uniform_memory_type = false;
        }

        inventory.last_page_physical =
            mapping.translated_physical_address;
        if (current_virtual_page == last_virtual_page)
            break;
        current_virtual_page += PAGE_SIZE;
    }

    const std::uint64_t first_offset =
        virtual_start - first_virtual_page;
    const std::uint64_t last_offset =
        virtual_end - last_virtual_page;
    if (inventory.first_page_physical >
            ~std::uint64_t{0} - first_offset ||
        inventory.last_page_physical >
            ~std::uint64_t{0} - last_offset)
        return inventory;
    inventory.physical_start =
        inventory.first_page_physical + first_offset;
    inventory.physical_end =
        inventory.last_page_physical + last_offset;
    inventory.complete = true;

    MilOS::AArch64::Debug::write("[VA inventory] ");
    MilOS::AArch64::Debug::write(name);
    MilOS::AArch64::Debug::write(" VA=[");
    MilOS::AArch64::Debug::write_hex(virtual_start);
    MilOS::AArch64::Debug::write(",");
    MilOS::AArch64::Debug::write_hex(virtual_end);
    MilOS::AArch64::Debug::write(") pages=");
    MilOS::AArch64::Debug::write_hex(inventory.page_count);
    if (inventory.physically_contiguous) {
        MilOS::AArch64::Debug::write(" PA=[");
        MilOS::AArch64::Debug::write_hex(inventory.physical_start);
        MilOS::AArch64::Debug::write(",");
        MilOS::AArch64::Debug::write_hex(inventory.physical_end);
        MilOS::AArch64::Debug::write(")");
    } else {
        MilOS::AArch64::Debug::write(" PA-pages(first,last)=");
        MilOS::AArch64::Debug::write_hex(
            inventory.first_page_physical);
        MilOS::AArch64::Debug::write(",");
        MilOS::AArch64::Debug::write_hex(
            inventory.last_page_physical);
        MilOS::AArch64::Debug::write(" non-contiguous");
    }
    MilOS::AArch64::Debug::write(" memmap=");
    MilOS::AArch64::Debug::write(
        inventory.uniform_memory_type && have_type
            ? memory_map_type_name(inventory.memory_map_type)
            : "mixed/unknown"
    );
    MilOS::AArch64::Debug::write("\n");
    if (inventory.uniform_attributes) {
        const Mapping& mapping = inventory.first_mapping;
        MilOS::AArch64::Debug::write("  EL1=");
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
            mapping.privileged_execute_never ? "1" : "0"
        );
        MilOS::AArch64::Debug::write(" UXN=");
        MilOS::AArch64::Debug::write(
            mapping.unprivileged_execute_never ? "1" : "0"
        );
        MilOS::AArch64::Debug::write(" AF=");
        MilOS::AArch64::Debug::write(
            mapping.access_flag ? "1" : "0"
        );
        MilOS::AArch64::Debug::write(" AttrIndx=");
        MilOS::AArch64::Debug::write_hex(mapping.attribute_index);
        MilOS::AArch64::Debug::write(" MAIR=");
        MilOS::AArch64::Debug::write_hex(
            (mapping.mair_value >>
             (mapping.attribute_index * 8)) & 0xff
        );
        MilOS::AArch64::Debug::write(" SH=");
        MilOS::AArch64::Debug::write_hex(mapping.shareability);
        MilOS::AArch64::Debug::write("\n");
    } else {
        MilOS::AArch64::Debug::write(
            "  descriptor attributes vary across this range\n"
        );
    }
    return inventory;
}
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

bool same_active_table_snapshot(
    const MilOS::AArch64::Paging::ActiveTableSnapshot& left,
    const MilOS::AArch64::Paging::ActiveTableSnapshot& right
) {
    return left.ttbr0_status == right.ttbr0_status &&
        left.ttbr1_status == right.ttbr1_status &&
        left.table_pages_scanned == right.table_pages_scanned &&
        left.checksum == right.checksum;
}

bool initialize_and_test_pmm(
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memmap,
    const MilOS::AArch64::Paging::Regime& regime
) {
    using MilOS::AArch64::Debug::write;
    using MilOS::AArch64::Debug::write_hex;
    using MilOS::AArch64::Memory::InitializationStatus;
    using MilOS::AArch64::Memory::PAGE_SIZE;

    const InitializationStatus status =
        MilOS::AArch64::Memory::initialize(
            hhdm_offset,
            memmap,
            regime
        );
    if (status != InitializationStatus::success) {
        write("AArch64 PMM initialization failed: ");
        write(
            MilOS::AArch64::Memory::initialization_status_name(status)
        );
        write("\n");
        return false;
    }

    const auto before =
        MilOS::AArch64::Memory::statistics();
    write("AArch64 PMM initialized: usable pages=");
    write_hex(before.total_pages);
    write(" free pages=");
    write_hex(before.free_pages);
    write(" metadata pages=");
    write_hex(before.metadata_pages);
    write(" usable regions=");
    write_hex(before.usable_region_count);
    write("\n");
    if (before.free_pages < 2) {
        write("AArch64 PMM runtime check failed: fewer than two free pages\n");
        return false;
    }

    const std::uint64_t first =
        MilOS::AArch64::Memory::allocate_page();
    const std::uint64_t second =
        MilOS::AArch64::Memory::allocate_page();
    void* first_pointer = nullptr;
    void* second_pointer = nullptr;
    const bool first_pointer_valid =
        first != 0 &&
        MilOS::AArch64::Memory::writable_pointer_for_allocated_page(
            first,
            first_pointer
        );
    const bool second_pointer_valid =
        second != 0 &&
        MilOS::AArch64::Memory::writable_pointer_for_allocated_page(
            second,
            second_pointer
        );
    const std::uint64_t max_address = ~std::uint64_t{0};
    const bool hhdm_additions_valid =
        first <= max_address - hhdm_offset &&
        second <= max_address - hhdm_offset;

    bool valid = hhdm_additions_valid &&
        first != 0 &&
        second != 0 &&
        first != second &&
        (first & (PAGE_SIZE - 1)) == 0 &&
        (second & (PAGE_SIZE - 1)) == 0 &&
        first_pointer_valid &&
        second_pointer_valid &&
        reinterpret_cast<std::uint64_t>(first_pointer) ==
            hhdm_offset + first &&
        reinterpret_cast<std::uint64_t>(second_pointer) ==
            hhdm_offset + second;
    if (valid) {
        auto* first_bytes = static_cast<volatile std::uint8_t*>(
            first_pointer
        );
        auto* second_bytes = static_cast<volatile std::uint8_t*>(
            second_pointer
        );
        first_bytes[0] = 0xA5;
        first_bytes[PAGE_SIZE - 1] = 0x5A;
        second_bytes[0] = 0x3C;
        valid = first_bytes[0] == 0xA5 &&
            first_bytes[PAGE_SIZE - 1] == 0x5A &&
            second_bytes[0] == 0x3C;
    }

    const auto while_allocated =
        MilOS::AArch64::Memory::statistics();
    valid = valid &&
        while_allocated.free_pages + 2 == before.free_pages;

    const bool first_released =
        first != 0 &&
        MilOS::AArch64::Memory::release_page(first);
    const bool second_released =
        second != 0 &&
        MilOS::AArch64::Memory::release_page(second);
    const auto after =
        MilOS::AArch64::Memory::statistics();
    valid = valid &&
        first_released &&
        second_released &&
        after.free_pages == before.free_pages &&
        !MilOS::AArch64::Memory::release_page(first);

    if (!valid) {
        write("AArch64 PMM runtime allocation/translation check failed\n");
        return false;
    }
    write(
        "AArch64 PMM runtime allocation, HHDM access, and release passed\n"
    );
    return true;
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
        case RootScanStatus::unsupported_regime:
            return "unsupported translation regime";
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

void compare_executable_address_response(
    const char* name,
    std::uint64_t virtual_start,
    std::uint64_t response_virtual_base,
    std::uint64_t response_physical_base,
    const RangeInventory& inventory
) {
    MilOS::AArch64::Debug::write("  ");
    MilOS::AArch64::Debug::write(name);
    MilOS::AArch64::Debug::write(" executable-response cross-check=");
    if (!inventory.complete || !inventory.physically_contiguous ||
        virtual_start < response_virtual_base ||
        response_physical_base >
            ~std::uint64_t{0} -
                (virtual_start - response_virtual_base)) {
        MilOS::AArch64::Debug::write("unavailable\n");
        return;
    }
    const std::uint64_t expected =
        response_physical_base +
        (virtual_start - response_virtual_base);
    MilOS::AArch64::Debug::write(
        expected == inventory.physical_start ? "match PA=" : "mismatch expected PA="
    );
    MilOS::AArch64::Debug::write_hex(expected);
    MilOS::AArch64::Debug::write("\n");
}

void inventory_runtime_mappings(
    const MilOS::AArch64::Paging::RegisterSnapshot& registers,
    const MilOS::AArch64::Paging::Regime& regime,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    const limine_executable_address_response* executable_address
) {
    using MilOS::AArch64::Paging::ActiveTranslationStatus;
    using MilOS::AArch64::Paging::translate_active_address;

    MilOS::AArch64::Debug::write(
        "Read-only active VA-to-PA runtime inventory\n"
    );
    if (executable_address == nullptr) {
        MilOS::AArch64::Debug::write(
            "Executable-address response unavailable; image PA is not derived\n"
        );
    } else {
        write_named_hex(
            "Limine executable physical base=",
            executable_address->physical_base
        );
        write_named_hex(
            "Limine executable virtual base=",
            executable_address->virtual_base
        );
    }

    struct ImageRange {
        const char* name;
        std::uint64_t start;
        std::uint64_t end;
    };
    const ImageRange image_ranges[] = {
        {
            "Limine request structures",
            reinterpret_cast<std::uint64_t>(__limine_requests_start),
            reinterpret_cast<std::uint64_t>(__limine_requests_end)
        },
        {
            "kernel text, entry, vectors, handlers",
            reinterpret_cast<std::uint64_t>(__text_start),
            reinterpret_cast<std::uint64_t>(__text_end)
        },
        {
            "kernel read-only data",
            reinterpret_cast<std::uint64_t>(__rodata_start),
            reinterpret_cast<std::uint64_t>(__rodata_end)
        },
        {
            "kernel writable data and BSS",
            reinterpret_cast<std::uint64_t>(__writable_start),
            reinterpret_cast<std::uint64_t>(__writable_end)
        }
    };
    const std::uint64_t linked_image_start =
        reinterpret_cast<std::uint64_t>(__kernel_image_start);
    const std::uint64_t linked_image_end =
        reinterpret_cast<std::uint64_t>(__kernel_image_end);
    write_named_hex("Linker image start=", linked_image_start);
    write_named_hex("Linker image end=", linked_image_end);
    for (const ImageRange& range : image_ranges) {
        const RangeInventory inventory = inspect_active_range(
            range.name,
            range.start,
            range.end,
            registers,
            hhdm_offset,
            memory_map
        );
        if (executable_address != nullptr)
            compare_executable_address_response(
                range.name,
                range.start,
                executable_address->virtual_base,
                executable_address->physical_base,
                inventory
            );
    }

    std::uint64_t current_sp = 0;
    asm volatile("mov %0, sp" : "=r"(current_sp));
    const std::uint64_t stack_top = aarch64_boot_stack_top;
    const std::uint64_t normal_stack_top =
        aarch64_boot_normal_stack_top;
    write_named_hex("Observed current SP=", current_sp);
    write_named_hex("Entry stack top=", stack_top);
    write_named_hex("Normal stack top=", normal_stack_top);
    if (stack_top >= 0x10000 &&
        normal_stack_top == stack_top - 0x8000 &&
        current_sp >= stack_top - 0x10000 &&
        current_sp <= normal_stack_top) {
        MilOS::AArch64::Debug::write(
            "Stack bounds verified from captured entry values and Limine's 64KiB stack contract\n"
        );
        inspect_active_range(
            "Limine normal stack backing",
            stack_top - 0x10000,
            normal_stack_top,
            registers,
            hhdm_offset,
            memory_map
        );
        inspect_active_range(
            "Limine exception stack backing",
            normal_stack_top,
            stack_top,
            registers,
            hhdm_offset,
            memory_map
        );
    } else {
        MilOS::AArch64::Debug::write(
            "Stack bounds unresolved: captured entry values do not match the stack split\n"
        );
    }

    if (hhdm_offset <= ~std::uint64_t{0} - PAGE_SIZE) {
        const std::uint64_t ttbr0_root =
            registers.ttbr0_el1 & regime.physical_address_mask;
        const std::uint64_t ttbr1_root =
            registers.ttbr1_el1 & regime.physical_address_mask;
        const std::uint64_t roots[] = {ttbr0_root, ttbr1_root};
        for (const std::uint64_t root : roots) {
            if (root > ~std::uint64_t{0} - hhdm_offset)
                continue;
            const std::uint64_t virtual_address = hhdm_offset + root;
            MilOS::AArch64::Debug::write("Active table HHDM page PA=");
            MilOS::AArch64::Debug::write_hex(root);
            const auto translation = translate_active_address(
                registers,
                hhdm_offset,
                memory_map,
                virtual_address
            );
            MilOS::AArch64::Debug::write(" status=");
            MilOS::AArch64::Debug::write(
                ActiveTranslationStatus::mapped == translation.status
                    ? "mapped"
                    : active_translation_status_name(translation.status)
            );
            if (translation.status == ActiveTranslationStatus::mapped) {
                MilOS::AArch64::Debug::write(" => PA=");
                MilOS::AArch64::Debug::write_hex(
                    translation.mapping.translated_physical_address
                );
            }
            std::uint64_t type = 0;
            if (classify_physical_page(memory_map, root, type)) {
                MilOS::AArch64::Debug::write(" memmap=");
                MilOS::AArch64::Debug::write(memory_map_type_name(type));
            }
            MilOS::AArch64::Debug::write("\n");
        }
    }

    const auto metadata = MilOS::AArch64::Memory::statistics();
    write_named_hex("PMM metadata physical base=", metadata.metadata_base);
    write_named_hex("PMM metadata byte size=", metadata.metadata_size);
    if (metadata.metadata_size != 0 &&
        metadata.metadata_base <=
            ~std::uint64_t{0} - hhdm_offset &&
        metadata.metadata_size <=
            ~std::uint64_t{0} -
                (hhdm_offset + metadata.metadata_base)) {
        const RangeInventory metadata_range = inspect_active_range(
            "PMM metadata HHDM mapping",
            hhdm_offset + metadata.metadata_base,
            hhdm_offset + metadata.metadata_base + metadata.metadata_size,
            registers,
            hhdm_offset,
            memory_map
        );
        (void)metadata_range;
    }

    if (hhdm_request.response != nullptr) {
        const std::uint64_t response =
            reinterpret_cast<std::uint64_t>(hhdm_request.response);
        if (response <=
            ~std::uint64_t{0} - sizeof(limine_hhdm_response)) {
            const RangeInventory hhdm_response_range =
                inspect_active_range(
                    "Limine HHDM response",
                    response,
                    response + sizeof(limine_hhdm_response),
                    registers,
                    hhdm_offset,
                    memory_map
                );
            (void)hhdm_response_range;
        }
    }
    if (memory_map != nullptr) {
        const std::uint64_t response =
            reinterpret_cast<std::uint64_t>(memory_map);
        if (response <= ~std::uint64_t{0} - sizeof(*memory_map)) {
            const RangeInventory memory_map_response_range =
                inspect_active_range(
                    "Limine memory-map response",
                    response,
                    response + sizeof(*memory_map),
                    registers,
                    hhdm_offset,
                    memory_map
                );
            (void)memory_map_response_range;
        }
        if (memory_map->entries != nullptr &&
            memory_map->entry_count <= 128 &&
            memory_map->entry_count <=
                ~std::uint64_t{0} /
                    sizeof(limine_memmap_entry*)) {
            const std::uint64_t entries =
                reinterpret_cast<std::uint64_t>(memory_map->entries);
            const std::uint64_t bytes =
                memory_map->entry_count * sizeof(limine_memmap_entry*);
            if (entries <= ~std::uint64_t{0} - bytes) {
                const RangeInventory entries_range =
                    inspect_active_range(
                        "Limine memory-map entry pointer array",
                        entries,
                        entries + bytes,
                        registers,
                        hhdm_offset,
                        memory_map
                    );
                (void)entries_range;
            }
        }
        for (std::uint64_t i = 0;
             i < memory_map->entry_count && i < 128;
             ++i) {
            const limine_memmap_entry* entry =
                memory_map->entries[i];
            if (entry == nullptr)
                continue;
            const std::uint64_t entry_address =
                reinterpret_cast<std::uint64_t>(entry);
            if (entry_address >
                ~std::uint64_t{0} - sizeof(*entry))
                continue;
            MilOS::AArch64::Debug::write(
                "Memory-map entry object #");
            MilOS::AArch64::Debug::write_hex(i);
            MilOS::AArch64::Debug::write(" ");
            inspect_active_range(
                "entry object",
                entry_address,
                entry_address + sizeof(*entry),
                registers,
                hhdm_offset,
                memory_map
            );
        }
    }
    if (executable_address != nullptr) {
        const std::uint64_t response =
            reinterpret_cast<std::uint64_t>(executable_address);
        if (response <=
            ~std::uint64_t{0} -
                sizeof(limine_executable_address_response)) {
            inspect_active_range(
                "Limine executable-address response",
                response,
                response +
                    sizeof(limine_executable_address_response),
                registers,
                hhdm_offset,
                memory_map
            );
        }
    }

    if (dtb_request.response != nullptr) {
        const std::uint64_t response =
            reinterpret_cast<std::uint64_t>(dtb_request.response);
        if (response <=
            ~std::uint64_t{0} - sizeof(limine_dtb_response)) {
            inspect_active_range(
                "Limine DTB response",
                response,
                response + sizeof(limine_dtb_response),
                registers,
                hhdm_offset,
                memory_map
            );
        }
    }

    const auto invalid_query = translate_active_address(
        registers,
        hhdm_offset,
        memory_map,
        0x0001000000000000ull
    );
    auto unsupported_registers = registers;
    unsupported_registers.tcr_el1 ^= std::uint64_t{1} << 30;
    const auto unsupported_query = translate_active_address(
        unsupported_registers,
        hhdm_offset,
        memory_map,
        reinterpret_cast<std::uint64_t>(__text_start)
    );
    if (invalid_query.status ==
            ActiveTranslationStatus::invalid_address &&
        unsupported_query.status ==
            ActiveTranslationStatus::unsupported_regime) {
        MilOS::AArch64::Debug::write(
            "Active VA-to-PA query invalid-address/unsupported-regime checks passed\n"
        );
    } else {
        MilOS::AArch64::Debug::write(
            "Active VA-to-PA query negative checks inconclusive\n"
        );
    }
}

bool append_validated_runtime_region(
    AddressSpace::MappingRegion* regions,
    std::uint32_t& region_count,
    std::uint64_t virtual_start,
    std::uint64_t virtual_end,
    AddressSpace::LeafPolicy policy,
    std::uint64_t expected_memory_type,
    std::uint32_t role_mask,
    const MilOS::AArch64::Paging::RegisterSnapshot& registers,
    const MilOS::AArch64::Paging::Regime& regime,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    const limine_executable_address_response* executable_address,
    bool verify_executable_response
) {
    using MilOS::AArch64::Paging::ActiveTranslationStatus;
    if (regions == nullptr ||
        region_count >= AddressSpace::MAX_KERNEL_MAPPING_REGIONS ||
        virtual_end <= virtual_start)
        return false;

    const std::uint64_t page_start = virtual_start & ~(PAGE_SIZE - 1);
    if (virtual_end > ~std::uint64_t{0} - (PAGE_SIZE - 1))
        return false;
    const std::uint64_t page_end =
        (virtual_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (page_end <= page_start ||
        (page_end - page_start) / PAGE_SIZE > MAX_INVENTORY_PAGES)
        return false;

    std::uint64_t physical_base = 0;
    MilOS::AArch64::Paging::Mapping first_mapping{};
    std::uint64_t virtual_page = page_start;
    while (virtual_page < page_end) {
        const auto translation =
            MilOS::AArch64::Paging::translate_active_address(
                registers,
                hhdm_offset,
                memory_map,
                virtual_page
            );
        if (translation.status != ActiveTranslationStatus::mapped ||
            (translation.mapping.translated_physical_address &
             (PAGE_SIZE - 1)) != 0 ||
            translation.mapping.memory_type !=
                MilOS::AArch64::Paging::MemoryType::normal)
            return false;

        const std::uint64_t offset = virtual_page - page_start;
        if (offset == 0) {
            physical_base =
                translation.mapping.translated_physical_address;
            first_mapping = translation.mapping;
        } else {
            if (physical_base > ~std::uint64_t{0} - offset ||
                translation.mapping.translated_physical_address !=
                    physical_base + offset ||
                !same_mapping_attributes(
                    first_mapping,
                    translation.mapping))
                return false;
        }
        std::uint64_t actual_type = 0;
        if (!classify_physical_page(
                memory_map,
                translation.mapping.translated_physical_address,
                actual_type) ||
            actual_type != expected_memory_type)
            return false;

        if (policy == AddressSpace::LeafPolicy::kernel_text &&
            (translation.mapping.privileged_writable ||
             translation.mapping.unprivileged_access ||
             translation.mapping.privileged_execute_never))
            return false;
        if (policy == AddressSpace::LeafPolicy::read_only &&
            translation.mapping.unprivileged_access)
            return false;
        if (policy == AddressSpace::LeafPolicy::writable &&
            !translation.mapping.privileged_writable)
            return false;
        virtual_page += PAGE_SIZE;
    }

    const std::uint64_t region_size = page_end - page_start;
    const std::uint64_t physical_limit =
        std::uint64_t{1} << regime.physical_address_bits;
    if (physical_base > physical_limit ||
        region_size > physical_limit - physical_base)
        return false;

    if (verify_executable_response) {
        if (executable_address == nullptr ||
            page_start < executable_address->virtual_base ||
            executable_address->physical_base >
                ~std::uint64_t{0} -
                    (page_start - executable_address->virtual_base) ||
            executable_address->physical_base +
                    (page_start - executable_address->virtual_base) !=
                physical_base)
            return false;
    }

    regions[region_count++] = {
        page_start,
        physical_base,
        region_size,
        expected_memory_type,
        role_mask,
        policy,
        1
    };
    return true;
}

bool append_validated_usable_hhdm_region(
    AddressSpace::MappingRegion* regions,
    std::uint32_t& region_count,
    const limine_memmap_entry& entry,
    const MilOS::AArch64::Paging::RegisterSnapshot& registers,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map
) {
    using MilOS::AArch64::Paging::ActiveTranslationStatus;
    using MilOS::AArch64::Paging::MemoryType;
    using MilOS::AArch64::Paging::translate_active_address;

    if (regions == nullptr ||
        region_count >= AddressSpace::MAX_KERNEL_MAPPING_REGIONS ||
        entry.type != LIMINE_MEMMAP_USABLE ||
        entry.length == 0 ||
        (entry.base & (PAGE_SIZE - 1)) != 0 ||
        (entry.length & (PAGE_SIZE - 1)) != 0 ||
        entry.base > ~std::uint64_t{0} - entry.length ||
        hhdm_offset > ~std::uint64_t{0} - entry.base) {
        MilOS::AArch64::Debug::write(
            "HHDM range rejected before audit: PA=");
        MilOS::AArch64::Debug::write_hex(entry.base);
        MilOS::AArch64::Debug::write(" size=");
        MilOS::AArch64::Debug::write_hex(entry.length);
        MilOS::AArch64::Debug::write("\n");
        return false;
    }

    const std::uint64_t virtual_base = hhdm_offset + entry.base;
    std::uint64_t physical_end = entry.base + entry.length;
    if (hhdm_offset > ~std::uint64_t{0} - physical_end ||
        virtual_base > ~std::uint64_t{0} - (entry.length - 1))
        return false;
    const std::uint64_t virtual_end =
        hhdm_offset + physical_end;
    const std::uint64_t virtual_last = virtual_end - 1;
    const auto is_hhdm_canonical = [](std::uint64_t address) {
        constexpr std::uint64_t LOWER_LIMIT =
            (std::uint64_t{1} << 48) - 1;
        constexpr std::uint64_t UPPER_BASE =
            ~std::uint64_t{0} - LOWER_LIMIT;
        return address <= LOWER_LIMIT || address >= UPPER_BASE;
    };
    if (!is_hhdm_canonical(virtual_base) ||
        !is_hhdm_canonical(virtual_last) ||
        ((virtual_base <= 0x0000ffffffffffffull) !=
         (virtual_last <= 0x0000ffffffffffffull))) {
        MilOS::AArch64::Debug::write(
            "HHDM range rejected as noncanonical: VA=");
        MilOS::AArch64::Debug::write_hex(virtual_base);
        MilOS::AArch64::Debug::write("..");
        MilOS::AArch64::Debug::write_hex(virtual_last);
        MilOS::AArch64::Debug::write("\n");
        return false;
    }

    for (std::uint64_t offset = 0;
         offset < entry.length;
         offset += PAGE_SIZE) {
        const std::uint64_t virtual_address =
            virtual_base + offset;
        const std::uint64_t expected_physical =
            entry.base + offset;
        const auto translation = translate_active_address(
            registers,
            hhdm_offset,
            memory_map,
            virtual_address
        );
        if (translation.status != ActiveTranslationStatus::mapped) {
            MilOS::AArch64::Debug::write(
                "Active HHDM page is not mapped: VA=");
            MilOS::AArch64::Debug::write_hex(virtual_address);
            MilOS::AArch64::Debug::write(" PA=");
            MilOS::AArch64::Debug::write_hex(expected_physical);
            MilOS::AArch64::Debug::write(" status=");
            MilOS::AArch64::Debug::write_hex(
                static_cast<std::uint64_t>(translation.status));
            MilOS::AArch64::Debug::write("\n");
            return false;
        }
        const auto& mapping = translation.mapping;
        const std::uint64_t attribute =
            (mapping.mair_value >>
             (mapping.attribute_index * 8)) & 0xff;
        if (mapping.translated_physical_address != expected_physical ||
            mapping.memory_type != MemoryType::normal ||
            mapping.physical_base >
                ~std::uint64_t{0} - hhdm_offset ||
            mapping.virtual_base !=
                hhdm_offset + mapping.physical_base ||
            attribute != 0xff ||
            mapping.shareability != 3 ||
            !mapping.privileged_writable ||
            mapping.unprivileged_access ||
            !mapping.access_flag) {
            MilOS::AArch64::Debug::write(
                "Active HHDM descriptor policy mismatch PA=");
            MilOS::AArch64::Debug::write_hex(mapping.physical_base);
            MilOS::AArch64::Debug::write(" VA=");
            MilOS::AArch64::Debug::write_hex(mapping.virtual_base);
            MilOS::AArch64::Debug::write(" AttrIndx=");
            MilOS::AArch64::Debug::write_hex(mapping.attribute_index);
            MilOS::AArch64::Debug::write(" MAIR byte=");
            MilOS::AArch64::Debug::write_hex(attribute);
            MilOS::AArch64::Debug::write(" Normal=");
            MilOS::AArch64::Debug::write(
                mapping.memory_type == MemoryType::normal ? "yes" : "no");
            MilOS::AArch64::Debug::write(" RW=");
            MilOS::AArch64::Debug::write(
                mapping.privileged_writable ? "yes" : "no");
            MilOS::AArch64::Debug::write(" EL0=");
            MilOS::AArch64::Debug::write(
                mapping.unprivileged_access ? "yes" : "no");
            MilOS::AArch64::Debug::write(" SH=");
            MilOS::AArch64::Debug::write_hex(mapping.shareability);
            MilOS::AArch64::Debug::write("\n");
            return false;
        }
    }

    regions[region_count++] = {
        virtual_base,
        entry.base,
        entry.length,
        LIMINE_MEMMAP_USABLE,
        AddressSpace::ROLE_HHDM_USABLE,
        AddressSpace::LeafPolicy::writable,
        1
    };
    return true;
}

bool append_active_table_mapping_regions(
    AddressSpace::MappingRegion* regions,
    std::uint32_t& region_count,
    std::uint64_t* table_pages,
    std::uint64_t table_page_count,
    std::uint64_t hhdm_offset,
    const MilOS::AArch64::Paging::RegisterSnapshot& registers,
    const MilOS::AArch64::Paging::Regime& regime,
    const limine_memmap_response* memory_map
) {
    using AddressSpace::LeafPolicy;
    using AddressSpace::ROLE_ACTIVE_TABLES;

    if (regions == nullptr || table_pages == nullptr ||
        table_page_count == 0 ||
        table_page_count > MAX_ACTIVE_TABLE_PAGES)
        return false;

    for (std::uint64_t i = 1; i < table_page_count; ++i) {
        const std::uint64_t value = table_pages[i];
        std::uint64_t position = i;
        while (position != 0 && table_pages[position - 1] > value) {
            table_pages[position] = table_pages[position - 1];
            --position;
        }
        table_pages[position] = value;
    }

    std::uint64_t run_base = 0;
    std::uint64_t run_end = 0;
    bool have_run = false;
    const auto flush_run = [&]() {
        if (!have_run)
            return true;
        if (hhdm_offset > ~std::uint64_t{0} - run_base ||
            hhdm_offset > ~std::uint64_t{0} - run_end)
            return false;
        const bool appended = append_validated_runtime_region(
            regions,
            region_count,
            hhdm_offset + run_base,
            hhdm_offset + run_end,
            LeafPolicy::read_only,
            LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE,
            ROLE_ACTIVE_TABLES,
            registers,
            regime,
            hhdm_offset,
            memory_map,
            nullptr,
            false
        );
        have_run = false;
        return appended;
    };

    for (std::uint64_t i = 0; i < table_page_count; ++i) {
        const std::uint64_t page = table_pages[i];
        if ((page & (PAGE_SIZE - 1)) != 0)
            return false;
        if (i != 0 && page == table_pages[i - 1])
            continue;

        std::uint64_t type = 0;
        if (!classify_physical_page(memory_map, page, type))
            return false;
        if (type == LIMINE_MEMMAP_USABLE) {
            if (!flush_run())
                return false;
            continue;
        }
        if (type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
            return false;

        if (have_run && page == run_end) {
            if (run_end > ~std::uint64_t{0} - PAGE_SIZE)
                return false;
            run_end += PAGE_SIZE;
            continue;
        }
        if (!flush_run() || page > ~std::uint64_t{0} - PAGE_SIZE)
            return false;
        run_base = page;
        run_end = page + PAGE_SIZE;
        have_run = true;
    }
    if (!flush_run())
        return false;
    return true;
}

bool normalize_runtime_regions(
    AddressSpace::MappingRegion* regions,
    std::uint32_t& region_count
) {
    for (std::uint32_t i = 1; i < region_count; ++i) {
        AddressSpace::MappingRegion value = regions[i];
        std::uint32_t position = i;
        while (position != 0 &&
               regions[position - 1].virtual_base > value.virtual_base) {
            regions[position] = regions[position - 1];
            --position;
        }
        regions[position] = value;
    }

    std::uint32_t output = 0;
    for (std::uint32_t i = 0; i < region_count; ++i) {
        AddressSpace::MappingRegion current = regions[i];
        if (output == 0) {
            regions[output++] = current;
            continue;
        }

        AddressSpace::MappingRegion& previous = regions[output - 1];
        const std::uint64_t previous_end =
            previous.virtual_base + previous.size;
        const std::uint64_t current_end =
            current.virtual_base + current.size;
        const bool overlaps = current.virtual_base < previous_end;
        const bool adjacent = current.virtual_base == previous_end;
        const bool same_policy =
            previous.policy == current.policy &&
            previous.attribute_index == current.attribute_index &&
            previous.memory_map_type == current.memory_map_type;
        if (overlaps) {
            if (!same_policy ||
                previous.physical_base >
                    ~std::uint64_t{0} -
                        (current.virtual_base - previous.virtual_base) ||
                current.physical_base !=
                    previous.physical_base +
                        (current.virtual_base - previous.virtual_base))
                return false;
            previous.role_mask |= current.role_mask;
            if (current_end > previous_end)
                previous.size = current_end - previous.virtual_base;
            continue;
        }
        if (adjacent && same_policy &&
            previous.physical_base <=
                ~std::uint64_t{0} - previous.size &&
            previous.physical_base + previous.size ==
                current.physical_base) {
            previous.size += current.size;
            previous.role_mask |= current.role_mask;
            continue;
        }
        regions[output++] = current;
    }
    region_count = output;
    return true;
}

bool run_runtime_kernel_mapping_builder(
    const MilOS::AArch64::Paging::RegisterSnapshot& expected_registers,
    const MilOS::AArch64::Paging::Regime& regime,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    const limine_executable_address_response* executable_address,
    const limine_dtb_response* dtb_response,
    const std::uint8_t* dtb,
    std::uint64_t dtb_size
) {
    using MilOS::AArch64::Paging::RootScanStatus;
    using namespace MilOS::AArch64::AddressSpace;
    MilOS::AArch64::Debug::write(
        "Building non-activating runtime kernel mapping hierarchy\n"
    );

    MappingRegion regions[MAX_KERNEL_MAPPING_REGIONS]{};
    std::uint32_t region_count = 0;
    std::uint64_t hhdm_usable_bytes = 0;
    std::uint32_t hhdm_usable_regions = 0;
    auto add = [&](std::uint64_t start,
                   std::uint64_t end,
                   LeafPolicy policy,
                   std::uint64_t memory_type,
                   std::uint32_t role,
                   bool image_range = false) {
        return append_validated_runtime_region(
            regions,
            region_count,
            start,
            end,
            policy,
            memory_type,
            role,
            expected_registers,
            regime,
            hhdm_offset,
            memory_map,
            executable_address,
            image_range
        );
    };

    bool inputs_valid =
        add(
            reinterpret_cast<std::uint64_t>(__limine_requests_start),
            reinterpret_cast<std::uint64_t>(__limine_requests_end),
            LeafPolicy::read_only,
            LIMINE_MEMMAP_EXECUTABLE_AND_MODULES,
            ROLE_KERNEL_REQUESTS,
            true
        ) &&
        add(
            reinterpret_cast<std::uint64_t>(__text_start),
            reinterpret_cast<std::uint64_t>(__text_end),
            LeafPolicy::kernel_text,
            LIMINE_MEMMAP_EXECUTABLE_AND_MODULES,
            ROLE_KERNEL_TEXT | ROLE_EXCEPTION_VECTORS,
            true
        ) &&
        add(
            reinterpret_cast<std::uint64_t>(__rodata_start),
            reinterpret_cast<std::uint64_t>(__rodata_end),
            LeafPolicy::read_only,
            LIMINE_MEMMAP_EXECUTABLE_AND_MODULES,
            ROLE_KERNEL_RODATA,
            true
        ) &&
        add(
            reinterpret_cast<std::uint64_t>(__writable_start),
            reinterpret_cast<std::uint64_t>(__writable_end),
            LeafPolicy::writable,
            LIMINE_MEMMAP_EXECUTABLE_AND_MODULES,
            ROLE_KERNEL_WRITABLE | ROLE_EXCEPTION_DATA,
            true
        );

    if (memory_map == nullptr ||
        memory_map->entries == nullptr ||
        memory_map->entry_count == 0 ||
        memory_map->entry_count > 128) {
        inputs_valid = false;
    } else {
        for (std::uint64_t i = 0;
             i < memory_map->entry_count && inputs_valid;
             ++i) {
            const limine_memmap_entry* entry =
                memory_map->entries[i];
            if (entry == nullptr)
                inputs_valid = false;
            else if (entry->type == LIMINE_MEMMAP_USABLE) {
                inputs_valid = append_validated_usable_hhdm_region(
                    regions,
                    region_count,
                    *entry,
                    expected_registers,
                    hhdm_offset,
                    memory_map
                );
                if (inputs_valid) {
                    if (hhdm_usable_bytes >
                        ~std::uint64_t{0} - entry->length)
                        inputs_valid = false;
                    else {
                        hhdm_usable_bytes += entry->length;
                        ++hhdm_usable_regions;
                    }
                }
            }
        }
    }
    if (!inputs_valid) {
        MilOS::AArch64::Debug::write(
            "Runtime mapping builder stopped: active HHDM does not safely cover every usable range\n"
        );
        return false;
    }

    const std::uint64_t stack_top = aarch64_boot_stack_top;
    const std::uint64_t normal_stack_top =
        aarch64_boot_normal_stack_top;
    if (stack_top < 0x10000 ||
        normal_stack_top != stack_top - 0x8000 ||
        reinterpret_cast<std::uint64_t>(__builtin_frame_address(0)) <
            stack_top - 0x10000 ||
        reinterpret_cast<std::uint64_t>(__builtin_frame_address(0)) >
            normal_stack_top)
        inputs_valid = false;
    else
        inputs_valid = inputs_valid &&
            add(
                stack_top - 0x10000,
                normal_stack_top,
                LeafPolicy::writable,
                LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE,
                ROLE_STACK
            ) &&
            add(
                normal_stack_top,
                stack_top,
                LeafPolicy::writable,
                LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE,
                ROLE_STACK
            );

    const auto pmm = Memory::statistics();
    if (pmm.metadata_size == 0 ||
        pmm.metadata_base > ~std::uint64_t{0} - hhdm_offset ||
        pmm.metadata_size >
            ~std::uint64_t{0} -
                (hhdm_offset + pmm.metadata_base)) {
        inputs_valid = false;
    } else {
        inputs_valid = inputs_valid &&
            add(
                hhdm_offset + pmm.metadata_base,
                hhdm_offset + pmm.metadata_base + pmm.metadata_size,
                LeafPolicy::writable,
                LIMINE_MEMMAP_USABLE,
                ROLE_PMM_METADATA
            );
    }

    auto add_object = [&](const void* pointer,
                         std::uint64_t size,
                         std::uint64_t type,
                         std::uint32_t role = ROLE_BOOT_DATA) {
        const std::uint64_t start =
            reinterpret_cast<std::uint64_t>(pointer);
        if (pointer == nullptr || size == 0 ||
            start > ~std::uint64_t{0} - size)
            return false;
        return add(
            start,
            start + size,
            LeafPolicy::read_only,
            type,
            role
        );
    };

    inputs_valid = inputs_valid &&
        add_object(hhdm_request.response,
                   sizeof(limine_hhdm_response),
                   LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) &&
        add_object(memory_map,
                   sizeof(limine_memmap_response),
                   LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) &&
        add_object(executable_address,
                   sizeof(limine_executable_address_response),
                   LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE);

    std::uint64_t active_table_page_count = 0;
    const auto active_before =
        MilOS::AArch64::Paging::snapshot_active_tables(
            expected_registers,
            hhdm_offset,
            memory_map,
            active_table_pages,
            MAX_ACTIVE_TABLE_PAGES,
            &active_table_page_count
        );
    if (active_before.ttbr0_status != RootScanStatus::complete ||
        active_before.ttbr1_status != RootScanStatus::complete ||
        active_table_page_count == 0) {
        inputs_valid = false;
    } else {
        inputs_valid = inputs_valid &&
            append_active_table_mapping_regions(
                regions,
                region_count,
                active_table_pages,
                active_table_page_count,
                hhdm_offset,
                expected_registers,
                regime,
                memory_map
            );
    }

    if (memory_map == nullptr ||
        memory_map->entries == nullptr ||
        memory_map->entry_count == 0 ||
        memory_map->entry_count > 128 ||
        memory_map->entry_count >
            ~std::uint64_t{0} / sizeof(limine_memmap_entry*)) {
        inputs_valid = false;
    } else {
        inputs_valid = inputs_valid &&
            add_object(
                memory_map->entries,
                memory_map->entry_count *
                    sizeof(limine_memmap_entry*),
                LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
            );
        for (std::uint64_t i = 0;
             i < memory_map->entry_count;
             ++i) {
            inputs_valid = inputs_valid &&
                add_object(
                    memory_map->entries[i],
                    sizeof(limine_memmap_entry),
                    LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
                );
        }
    }

    const bool include_dtb =
        dtb_response != nullptr && dtb != nullptr && dtb_size >= 40;
    if (include_dtb) {
        inputs_valid = inputs_valid &&
            add_object(
                dtb_response,
                sizeof(limine_dtb_response),
                LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
            ) &&
            add_object(
                dtb,
                dtb_size,
                LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE,
                ROLE_DTB
            );
    }

    if (!inputs_valid || !normalize_runtime_regions(
            regions,
            region_count)) {
        MilOS::AArch64::Debug::write(
            "Runtime mapping builder stopped: inventory is incomplete or inconsistent\n"
        );
        return false;
    }

    MilOS::AArch64::Debug::write(
        "Validated usable HHDM coverage: regions=");
    MilOS::AArch64::Debug::write_hex(hhdm_usable_regions);
    MilOS::AArch64::Debug::write(" bytes=");
    MilOS::AArch64::Debug::write_hex(hhdm_usable_bytes);
    MilOS::AArch64::Debug::write("\n");

    std::uint32_t required_roles =
        ROLE_KERNEL_REQUESTS |
        ROLE_KERNEL_TEXT |
        ROLE_KERNEL_RODATA |
        ROLE_KERNEL_WRITABLE |
        ROLE_STACK |
        ROLE_BOOT_DATA |
        ROLE_PMM_METADATA |
        ROLE_TABLE_PAGES |
        ROLE_HHDM_USABLE |
        ROLE_EXCEPTION_VECTORS |
        ROLE_EXCEPTION_DATA;
    if (include_dtb)
        required_roles |= ROLE_DTB;

    const auto registers_before =
        MilOS::AArch64::Paging::read_registers();
    if (!same_register_snapshot(expected_registers, registers_before) ||
        active_before.ttbr0_status != RootScanStatus::complete ||
        active_before.ttbr1_status != RootScanStatus::complete) {
        MilOS::AArch64::Debug::write(
            "Runtime mapping builder stopped: active-state baseline is inconclusive\n"
        );
        return false;
    }
    MilOS::AArch64::Debug::write(
        "Builder active-register snapshot before mapping\n");
    report_paging_registers(registers_before);
    MilOS::AArch64::Debug::write(
        "Builder active-tree baseline: pages=");
    MilOS::AArch64::Debug::write_hex(
        active_before.table_pages_scanned);
    MilOS::AArch64::Debug::write(" checksum=");
    MilOS::AArch64::Debug::write_hex(active_before.checksum);
    MilOS::AArch64::Debug::write("\n");
    MilOS::AArch64::Debug::write(
        "Active table-page inventory unique=");
    MilOS::AArch64::Debug::write_hex(active_table_page_count);
    MilOS::AArch64::Debug::write("\n");
    const auto pmm_before = Memory::statistics();
    Hierarchy hierarchy;
    Status result = hierarchy.create(regime, registers_before.mair_el1);
    if (result == Status::success) {
        result = build_kernel_mappings(
            hierarchy,
            regime,
            registers_before.mair_el1,
            hhdm_offset,
            memory_map,
            regions,
            region_count,
            required_roles
        );
    }

    bool coverage_valid = result == Status::success;
    std::uint64_t expected_mapped_pages = 0;
    if (coverage_valid) {
        for (std::uint32_t i = 0; i < region_count; ++i) {
            const MappingRegion& region = regions[i];
            expected_mapped_pages += region.size / PAGE_SIZE;
            for (std::uint64_t offset = 0; offset < region.size;
                 offset += PAGE_SIZE) {
                const Translation translation =
                    hierarchy.translate(region.virtual_base + offset);
                const bool page_valid =
                    translation.status == Status::success &&
                    translation.physical_address ==
                        region.physical_base + offset;
                if (!page_valid) {
                    MilOS::AArch64::Debug::write(
                        "Builder region coverage mismatch VA=");
                    MilOS::AArch64::Debug::write_hex(
                        region.virtual_base + offset);
                    MilOS::AArch64::Debug::write(" status=");
                    MilOS::AArch64::Debug::write(
                        status_name(translation.status));
                    MilOS::AArch64::Debug::write(" expected PA=");
                    MilOS::AArch64::Debug::write_hex(
                        region.physical_base + offset);
                    MilOS::AArch64::Debug::write(" actual PA=");
                    MilOS::AArch64::Debug::write_hex(
                        translation.physical_address);
                    MilOS::AArch64::Debug::write("\n");
                }
                coverage_valid = coverage_valid && page_valid;
            }
        }
        for (std::uint64_t i = 0;
             i < active_table_page_count;
             ++i) {
            const std::uint64_t table_page = active_table_pages[i];
            if (hhdm_offset >
                ~std::uint64_t{0} - table_page) {
                coverage_valid = false;
                continue;
            }
            const Translation translation =
                hierarchy.translate(hhdm_offset + table_page);
            const bool page_valid =
                translation.status == Status::success &&
                translation.physical_address == table_page;
            if (!page_valid) {
                MilOS::AArch64::Debug::write(
                    "Builder active-table backing is not mapped PA=");
                MilOS::AArch64::Debug::write_hex(table_page);
                MilOS::AArch64::Debug::write(" status=");
                MilOS::AArch64::Debug::write(status_name(translation.status));
                MilOS::AArch64::Debug::write("\n");
            }
            coverage_valid = coverage_valid && page_valid;
        }
        for (std::uint32_t i = 0;
             i < hierarchy.owned_table_page_count();
             ++i) {
            const std::uint64_t table_page =
                 hierarchy.owned_table_page_physical_address(i);
            const Translation translation =
                hierarchy.translate(hhdm_offset + table_page);
            const bool alias_valid =
                translation.status == Status::success &&
                translation.physical_address == table_page &&
                table_page !=
                    (registers_before.ttbr0_el1 &
                     regime.physical_address_mask) &&
                table_page !=
                    (registers_before.ttbr1_el1 &
                     regime.physical_address_mask);
            if (!alias_valid) {
                MilOS::AArch64::Debug::write(
                    "Builder table-page alias mismatch PA=");
                MilOS::AArch64::Debug::write_hex(table_page);
                MilOS::AArch64::Debug::write(" status=");
                MilOS::AArch64::Debug::write(status_name(translation.status));
                MilOS::AArch64::Debug::write(" translated=");
                MilOS::AArch64::Debug::write_hex(
                    translation.physical_address);
                MilOS::AArch64::Debug::write("\n");
            }
            coverage_valid = coverage_valid && alias_valid;
        }
        const std::uint64_t image_end =
            reinterpret_cast<std::uint64_t>(__kernel_image_end);
        const std::uint64_t after_image_page =
            (image_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        const Translation unexpected =
            hierarchy.translate(after_image_page);
        if (unexpected.status != Status::not_mapped) {
            MilOS::AArch64::Debug::write(
                "Builder found unexpected post-image mapping VA=");
            MilOS::AArch64::Debug::write_hex(after_image_page);
            MilOS::AArch64::Debug::write(" status=");
            MilOS::AArch64::Debug::write(status_name(unexpected.status));
            MilOS::AArch64::Debug::write("\n");
        }
        coverage_valid = coverage_valid &&
            unexpected.status == Status::not_mapped;
    }

    if (hierarchy.is_created()) {
        const auto dependency_for = [&](
            std::uint64_t virtual_start,
            std::uint64_t virtual_end,
            std::uint32_t role_mask,
            LeafPolicy policy
        ) {
            ExceptionDependency dependency{
                0, 0, 0, role_mask, policy
            };
            if (virtual_start >= virtual_end ||
                virtual_end >
                    ~std::uint64_t{0} - (PAGE_SIZE - 1))
                return dependency;
            const std::uint64_t page_start =
                virtual_start & ~(PAGE_SIZE - 1);
            const std::uint64_t page_end =
                (virtual_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
            if (page_end <= page_start)
                return dependency;
            dependency.virtual_base = page_start;
            dependency.size = page_end - page_start;
            for (std::uint32_t i = 0; i < region_count; ++i) {
                const MappingRegion& region = regions[i];
                if ((region.role_mask & role_mask) == 0 ||
                    region.policy != policy ||
                    region.virtual_base > page_start ||
                    region.size >
                        ~std::uint64_t{0} - region.virtual_base ||
                    region.virtual_base + region.size < page_end)
                    continue;
                const std::uint64_t offset =
                    page_start - region.virtual_base;
                if (region.physical_base >
                    ~std::uint64_t{0} - offset)
                    return dependency;
                dependency.physical_base =
                    region.physical_base + offset;
                return dependency;
            }
            return dependency;
        };
        const ExceptionDependency exception_dependencies[] = {
            dependency_for(
                reinterpret_cast<std::uint64_t>(__text_start),
                reinterpret_cast<std::uint64_t>(__text_end),
                ROLE_KERNEL_TEXT,
                LeafPolicy::kernel_text
            ),
            dependency_for(
                reinterpret_cast<std::uint64_t>(__rodata_start),
                reinterpret_cast<std::uint64_t>(__rodata_end),
                ROLE_KERNEL_RODATA,
                LeafPolicy::read_only
            ),
            dependency_for(
                reinterpret_cast<std::uint64_t>(__writable_start),
                reinterpret_cast<std::uint64_t>(__writable_end),
                ROLE_KERNEL_WRITABLE,
                LeafPolicy::writable
            )
        };
        VerificationEvidence evidence{};
        evidence.hhdm_offset = hhdm_offset;
        evidence.active_registers = registers_before;
        evidence.active_table_pages = active_table_pages;
        evidence.active_table_page_count = active_table_page_count;
        evidence.active_inventory_complete =
            active_before.ttbr0_status == RootScanStatus::complete &&
            active_before.ttbr1_status == RootScanStatus::complete;
        evidence.hhdm_inventory_complete = true;
        evidence.boot_object_lifetime_established = true;
        evidence.exception_dependencies = exception_dependencies;
        evidence.exception_dependency_count =
            sizeof(exception_dependencies) /
            sizeof(exception_dependencies[0]);
        evidence.vector_base = read_vbar_el1();
        evidence.expected_vector_base =
            reinterpret_cast<std::uint64_t>(
                aarch64_exception_vectors);
        evidence.vector_size = 2048;
        const std::uint64_t stack_top = aarch64_boot_stack_top;
        const std::uint64_t normal_stack_top =
            aarch64_boot_normal_stack_top;
        evidence.normal_stack_base = stack_top - 0x10000;
        evidence.normal_stack_size = 0x8000;
        evidence.exception_stack_base = normal_stack_top;
        evidence.exception_stack_size = 0x8000;
        evidence.active_sp_el0 = read_current_sp();
        evidence.active_sp_el1 = stack_top;
        const auto pmm = Memory::statistics();
        evidence.pmm_metadata_base = pmm.metadata_base;
        evidence.pmm_metadata_size = pmm.metadata_size;
        evidence.required_additional_table_pages =
            regime.levels - 1;
        evidence.additional_capacity_requirement_known = true;

        const VerificationReport verification =
            hierarchy.verify_candidate(
                memory_map,
                regions,
                region_count,
                required_roles,
                evidence
            );
        MilOS::AArch64::Debug::write(
            "Pre-activation verifier overall=");
        MilOS::AArch64::Debug::write(
            verification_state_name(verification.overall));
        MilOS::AArch64::Debug::write("\n");
        for (std::uint32_t i = 0;
             i < static_cast<std::uint32_t>(
                     VerificationCheck::count);
             ++i) {
            const auto check =
                static_cast<VerificationCheck>(i);
            MilOS::AArch64::Debug::write(
                verification_check_name(check));
            MilOS::AArch64::Debug::write("=");
            MilOS::AArch64::Debug::write(
                verification_state_name(
                    verification.checks[i]));
            if (check == verification.first_non_pass) {
                MilOS::AArch64::Debug::write(" at ");
                MilOS::AArch64::Debug::write_hex(
                    verification.failing_address);
            }
            MilOS::AArch64::Debug::write("\n");
        }
        coverage_valid = coverage_valid &&
            verification.overall != VerificationState::fail;
    }

    const std::uint32_t built_table_page_count =
        hierarchy.owned_table_page_count();
    const Status destroy_status = hierarchy.is_created()
        ? hierarchy.destroy()
        : Status::success;
    const auto registers_after =
        MilOS::AArch64::Paging::read_registers();
    const auto active_after =
        MilOS::AArch64::Paging::snapshot_active_tables(
            registers_after,
            hhdm_offset,
            memory_map
        );
    MilOS::AArch64::Debug::write(
        "Builder active-register snapshot after mapping and cleanup\n");
    report_paging_registers(registers_after);
    const auto pmm_after = Memory::statistics();
    const bool active_unchanged =
        same_register_snapshot(registers_before, registers_after) &&
        active_before.ttbr0_status == RootScanStatus::complete &&
        active_before.ttbr1_status == RootScanStatus::complete &&
        active_after.ttbr0_status == RootScanStatus::complete &&
        active_after.ttbr1_status == RootScanStatus::complete &&
        same_active_table_snapshot(active_before, active_after);
    const bool allocator_restored =
        pmm_before.free_pages == pmm_after.free_pages &&
        pmm_before.total_pages == pmm_after.total_pages;
    MilOS::AArch64::Debug::write(
        "Builder active-tree final: pages=");
    MilOS::AArch64::Debug::write_hex(
        active_after.table_pages_scanned);
    MilOS::AArch64::Debug::write(" checksum=");
    MilOS::AArch64::Debug::write_hex(active_after.checksum);
    MilOS::AArch64::Debug::write("\n");
    if (result != Status::success ||
        !coverage_valid ||
        destroy_status != Status::success ||
        !active_unchanged ||
        !allocator_restored) {
        MilOS::AArch64::Debug::write(
            "Runtime mapping builder validation failed: status=");
        MilOS::AArch64::Debug::write(status_name(result));
        MilOS::AArch64::Debug::write(" coverage=");
        MilOS::AArch64::Debug::write(coverage_valid ? "ok" : "failed");
        MilOS::AArch64::Debug::write(" cleanup=");
        MilOS::AArch64::Debug::write(status_name(destroy_status));
        MilOS::AArch64::Debug::write(" active-state=");
        MilOS::AArch64::Debug::write(
            active_unchanged ? "unchanged" : "changed/inconclusive");
        MilOS::AArch64::Debug::write("\n");
        return false;
    }

    MilOS::AArch64::Debug::write(
        "Inactive root validated mapped pages=");
    MilOS::AArch64::Debug::write_hex(expected_mapped_pages);
    MilOS::AArch64::Debug::write(" owned table pages=");
    MilOS::AArch64::Debug::write_hex(built_table_page_count);
    MilOS::AArch64::Debug::write(" capacity=");
    MilOS::AArch64::Debug::write_hex(MAX_OWNED_TABLE_PAGES);
    MilOS::AArch64::Debug::write(
        " fresh-VA probe reserve=");
    const std::uint32_t probe_table_page_reserve =
        regime.levels - 1;
    MilOS::AArch64::Debug::write_hex(
        probe_table_page_reserve);
    MilOS::AArch64::Debug::write(
        " remaining after reserve=");
    MilOS::AArch64::Debug::write_hex(
        built_table_page_count <= MAX_OWNED_TABLE_PAGES -
                probe_table_page_reserve
            ? MAX_OWNED_TABLE_PAGES -
                built_table_page_count -
                probe_table_page_reserve
            : 0);
    MilOS::AArch64::Debug::write("\n");
    MilOS::AArch64::Debug::write(
        include_dtb
            ? "Runtime kernel mapping builder coverage, cleanup, and active-state checks passed (DTB included)\n"
            : "Runtime kernel mapping builder coverage, cleanup, and active-state checks passed (DTB unavailable)\n"
    );
    MilOS::AArch64::Debug::write(
        "Isolated builder root remained inactive; table pages were HHDM-aliased and returned to PMM\n"
    );
    return true;
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

    const std::uint32_t address_space_test_result =
        MilOS::AArch64::AddressSpace::run_synthetic_tests();
    if (address_space_test_result != 0) {
        MilOS::AArch64::Debug::write(
            "AArch64 synthetic address-space test failed: ");
        MilOS::AArch64::Debug::write_hex(address_space_test_result);
        MilOS::AArch64::Debug::write("\n");
        halt();
    }
    MilOS::AArch64::Debug::write(
        "AArch64 synthetic isolated hierarchy tests passed\n"
    );

    const std::uint32_t builder_test_result =
        MilOS::AArch64::AddressSpace::run_builder_synthetic_tests();
    if (builder_test_result != 0) {
        MilOS::AArch64::Debug::write(
            "AArch64 synthetic kernel mapping builder tests failed: ");
        MilOS::AArch64::Debug::write_hex(builder_test_result);
        MilOS::AArch64::Debug::write("\n");
        halt();
    }
    MilOS::AArch64::Debug::write(
        "AArch64 synthetic kernel mapping builder tests passed\n"
    );

    const std::uint32_t pre_activation_test_result =
        MilOS::AArch64::AddressSpace::
            run_pre_activation_verifier_tests();
    if (pre_activation_test_result != 0) {
        MilOS::AArch64::Debug::write(
            "AArch64 pre-activation verifier tests failed: ");
        MilOS::AArch64::Debug::write_hex(
            pre_activation_test_result);
        MilOS::AArch64::Debug::write("\n");
        halt();
    }
    MilOS::AArch64::Debug::write(
        "AArch64 pre-activation verifier PASS/FAIL/UNKNOWN tests passed\n"
    );

    const std::uint32_t pmm_test_result =
        MilOS::AArch64::Memory::run_synthetic_tests();
    if (pmm_test_result != 0) {
        MilOS::AArch64::Debug::write(
            "AArch64 synthetic PMM test failed: ");
        MilOS::AArch64::Debug::write_hex(pmm_test_result);
        MilOS::AArch64::Debug::write("\n");
        halt();
    }
    MilOS::AArch64::Debug::write(
        "AArch64 synthetic physical allocator tests passed\n"
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

    MilOS::AArch64::Paging::Regime pmm_regime{};
    if (MilOS::AArch64::Paging::detect_regime(
            paging_registers,
            pmm_regime) !=
        MilOS::AArch64::Paging::RegimeStatus::supported) {
        MilOS::AArch64::Debug::write(
            "AArch64 PMM skipped: active translation regime unsupported\n"
        );
        halt();
    }
    const auto registers_before_pmm =
        MilOS::AArch64::Paging::read_registers();
    if (!initialize_and_test_pmm(hhdm_offset, memmap, pmm_regime))
        halt();
    const auto registers_after_pmm =
        MilOS::AArch64::Paging::read_registers();
    if (!same_register_snapshot(
            registers_before_pmm,
            registers_after_pmm)) {
        MilOS::AArch64::Debug::write(
            "AArch64 PMM changed an active translation register\n"
        );
        halt();
    }
    MilOS::AArch64::Debug::write(
        "AArch64 PMM left active translation-register snapshot unchanged; metadata is confined to validated usable RAM\n"
    );

    const auto active_tables_before =
        MilOS::AArch64::Paging::snapshot_active_tables(
            registers_after_pmm,
            hhdm_offset,
            memmap
        );
    if (active_tables_before.ttbr0_status ==
            MilOS::AArch64::Paging::RootScanStatus::complete &&
        active_tables_before.ttbr1_status ==
            MilOS::AArch64::Paging::RootScanStatus::complete) {
        MilOS::AArch64::Debug::write(
            "AArch64 active-table checksum before isolated test: pages=");
        MilOS::AArch64::Debug::write_hex(
            active_tables_before.table_pages_scanned);
        MilOS::AArch64::Debug::write(" checksum=");
        MilOS::AArch64::Debug::write_hex(active_tables_before.checksum);
        MilOS::AArch64::Debug::write("\n");
    } else {
        MilOS::AArch64::Debug::write(
            "AArch64 active-table checksum incomplete before isolated test: TTBR0=");
        MilOS::AArch64::Debug::write(
            root_scan_status_name(active_tables_before.ttbr0_status));
        MilOS::AArch64::Debug::write(" TTBR1=");
        MilOS::AArch64::Debug::write(
            root_scan_status_name(active_tables_before.ttbr1_status));
        MilOS::AArch64::Debug::write(" pages=");
        MilOS::AArch64::Debug::write_hex(
            active_tables_before.table_pages_scanned);
        MilOS::AArch64::Debug::write("\n");
    }

    inventory_runtime_mappings(
        registers_after_pmm,
        pmm_regime,
        hhdm_offset,
        memmap,
        executable_address_request.response
    );

    const std::uint32_t live_address_space_test_result =
        MilOS::AArch64::AddressSpace::run_live_tests(
            pmm_regime,
            hhdm_offset,
            memmap,
            registers_after_pmm
        );
    if (live_address_space_test_result != 0) {
        MilOS::AArch64::Debug::write(
            "AArch64 live isolated hierarchy test failed: ");
        MilOS::AArch64::Debug::write_hex(
            live_address_space_test_result);
        MilOS::AArch64::Debug::write("\n");
        halt();
    }
    const auto registers_after_address_space =
        MilOS::AArch64::Paging::read_registers();
    const auto active_tables_after =
        MilOS::AArch64::Paging::snapshot_active_tables(
            registers_after_address_space,
            hhdm_offset,
            memmap
        );
    if (!same_register_snapshot(
            registers_after_pmm,
            registers_after_address_space)) {
        MilOS::AArch64::Debug::write(
            "AArch64 isolated hierarchy changed active translation registers\n"
        );
        halt();
    }
    if (active_tables_before.ttbr0_status ==
            MilOS::AArch64::Paging::RootScanStatus::complete &&
        active_tables_before.ttbr1_status ==
            MilOS::AArch64::Paging::RootScanStatus::complete &&
        active_tables_after.ttbr0_status ==
            MilOS::AArch64::Paging::RootScanStatus::complete &&
        active_tables_after.ttbr1_status ==
            MilOS::AArch64::Paging::RootScanStatus::complete) {
        if (!same_active_table_snapshot(
                active_tables_before,
                active_tables_after)) {
            MilOS::AArch64::Debug::write(
                "AArch64 active-table checksum changed during isolated test\n"
            );
            halt();
        }
        MilOS::AArch64::Debug::write(
            "Active translation-register snapshot and read-only active-table checksums unchanged\n"
        );
    } else {
        MilOS::AArch64::Debug::write(
            "Active translation-register snapshot unchanged; active-table checksum comparison inconclusive\n"
        );
    }
    MilOS::AArch64::Debug::write(
        "AArch64 isolated hierarchy allocation/map/walk/unmap/destroy passed\n"
        "No TTBR switch, active descriptor write, or TLB maintenance performed\n"
    );

    if (dtb_request.response == nullptr) {
        if (!run_runtime_kernel_mapping_builder(
                registers_after_address_space,
                pmm_regime,
                hhdm_offset,
                memmap,
                executable_address_request.response,
                nullptr,
                nullptr,
                0)) {
            MilOS::AArch64::Debug::write(
                "Runtime kernel mapping builder did not validate the available layout\n"
            );
            halt();
        }
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
    if (!dtb_range_is_in_bootloader_memory(
            dtb, FDT_HEADER_SIZE, hhdm_offset, memmap)) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: DTB header is outside bootloader-reclaimable memory\n");
        halt();
    }
    const std::uint64_t dtb_virtual =
        reinterpret_cast<std::uint64_t>(dtb);
    if (dtb_virtual > ~std::uint64_t{0} - FDT_HEADER_SIZE ||
        !inspect_active_range(
            "runtime DTB header",
            dtb_virtual,
            dtb_virtual + FDT_HEADER_SIZE,
            paging_registers,
            hhdm_offset,
            memmap
        ).complete) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: DTB header active translation unavailable\n");
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
    if (dtb_virtual > ~std::uint64_t{0} - dtb_size ||
        !inspect_active_range(
            "runtime DTB data",
            dtb_virtual,
            dtb_virtual + dtb_size,
            paging_registers,
            hhdm_offset,
            memmap
        ).complete) {
        MilOS::AArch64::Debug::write(
            "MilOS AArch64: DTB active translation incomplete\n");
        halt();
    }
    if (!run_runtime_kernel_mapping_builder(
            registers_after_address_space,
            pmm_regime,
            hhdm_offset,
            memmap,
            executable_address_request.response,
            dtb_request.response,
            dtb,
            dtb_size)) {
        MilOS::AArch64::Debug::write(
            "Runtime kernel mapping builder did not validate the DTB layout\n"
        );
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
