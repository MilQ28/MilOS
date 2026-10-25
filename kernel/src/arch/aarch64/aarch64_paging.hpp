#pragma once

#include <cstdint>
#include <limine.h>

namespace MilOS::AArch64::Paging {

struct RegisterSnapshot {
    std::uint64_t current_el;
    std::uint64_t sctlr_el1;
    std::uint64_t tcr_el1;
    std::uint64_t ttbr0_el1;
    std::uint64_t ttbr1_el1;
    std::uint64_t mair_el1;
    std::uint64_t id_aa64mmfr0_el1;
};

enum class RegimeStatus : std::uint8_t {
    supported,
    unsupported_exception_level,
    mmu_disabled,
    unsupported_configuration,
    inconsistent_registers
};

enum class MemoryType : std::uint8_t {
    unknown,
    device,
    normal
};

enum class RootScanStatus : std::uint8_t {
    complete,
    inaccessible_table,
    malformed_descriptor,
    budget_exhausted
};

enum class AuditStatus : std::uint8_t {
    mapped,
    unmapped,
    inconclusive,
    unsupported_regime,
    invalid_range
};

struct Regime {
    std::uint64_t physical_address_mask;
    std::uint8_t physical_address_bits;
    std::uint8_t virtual_address_bits;
    std::uint8_t levels;
};

struct Mapping {
    std::uint64_t virtual_base;
    std::uint64_t physical_base;
    std::uint64_t virtual_address;
    std::uint64_t translated_physical_address;
    std::uint64_t size;
    std::uint64_t mair_value;
    std::uint8_t level;
    std::uint8_t attribute_index;
    std::uint8_t shareability;
    bool privileged_writable;
    bool unprivileged_access;
    bool unprivileged_writable;
    bool privileged_execute_never;
    bool unprivileged_execute_never;
    bool access_flag;
    MemoryType memory_type;
};

constexpr std::uint32_t MAX_AUDIT_MAPPINGS = 32;
constexpr std::uint32_t MAX_AUDIT_GAPS = 32;

struct PhysicalRange {
    std::uint64_t base;
    std::uint64_t size;
};

struct AuditReport {
    AuditStatus status;
    RootScanStatus ttbr0_status;
    RootScanStatus ttbr1_status;
    std::uint32_t mapping_count;
    std::uint32_t gap_count;
    std::uint64_t table_pages_scanned;
    bool fully_covered;
    Mapping mappings[MAX_AUDIT_MAPPINGS];
    PhysicalRange gaps[MAX_AUDIT_GAPS];
};

using EntryReader = bool (*)(
    void* context,
    std::uint64_t table_physical_address,
    std::uint16_t index,
    std::uint64_t& descriptor
);

struct SyntheticWalkConfig {
    std::uint64_t root_physical_address;
    std::uint64_t mair_value;
    std::uint8_t physical_address_bits;
    std::uint8_t virtual_address_bits;
    std::uint8_t levels;
    bool upper_address_range;
};

RegisterSnapshot read_registers();
RegimeStatus detect_regime(
    const RegisterSnapshot& registers,
    Regime& regime
);
AuditReport inspect_physical_range(
    const RegisterSnapshot& registers,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    std::uint64_t physical_base,
    std::uint64_t physical_size
);
std::uint32_t run_synthetic_tests();
std::uint32_t run_isolated_device_mapping_tests(
    const Regime& regime,
    PhysicalRange device_range
);

}
