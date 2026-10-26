#include <cstddef>
#include <cstdint>
#include "aarch64_paging.hpp"

namespace MilOS::AArch64::Paging {
namespace {

constexpr std::uint64_t PAGE_SIZE = 4096;
constexpr std::uint64_t PAGE_MASK = PAGE_SIZE - 1;
constexpr std::uint64_t DESCRIPTOR_VALID = 1;
constexpr std::uint64_t DESCRIPTOR_TYPE = 2;
constexpr std::uint64_t ADDRESS_FIELD_MASK = 0x0000FFFFFFFFF000ull;
constexpr std::uint64_t RESERVED_ADDRESS_HIGH_MASK =
    0x0003000000000000ull;
constexpr std::uint64_t TABLE_DESCRIPTOR_RESERVED_LOW_MASK = 0xffc;
constexpr std::uint64_t INVALID_VALUE = ~std::uint64_t{0};
constexpr std::uint32_t MAX_TABLE_PAGES = 4096;
constexpr std::uint32_t MAX_SNAPSHOT_TABLE_PAGES = 4096;
constexpr std::uint8_t REQUIRED_VA_BITS = 48;
constexpr std::uint8_t REQUIRED_LEVELS = 4;
constexpr std::uint8_t DEVICE_ATTRIBUTE_INDEX = 0;
constexpr std::uint8_t DEVICE_NGNRNE_ATTRIBUTE = 0x00;
constexpr std::uint64_t PROPOSED_MAIR_VALUE = 0x000000000000ff00ull;
constexpr std::uint64_t EXPERIMENT_VIRTUAL_BASE =
    0xffff800000100000ull;

enum class WalkStatus : std::uint8_t {
    mapped,
    unmapped,
    invalid_input,
    unsupported_configuration,
    inaccessible_table,
    malformed_descriptor
};

struct WalkResult {
    WalkStatus status;
    Mapping mapping;
};

struct TableRestrictions {
    bool force_privileged_read_only;
    bool force_privileged_only;
    bool privileged_execute_never;
    bool unprivileged_execute_never;
};

struct LiveReaderContext {
    std::uint64_t hhdm_offset;
    const limine_memmap_response* memory_map;
    std::uint64_t cached_page;
    bool cached_page_valid;
};

struct ScanContext {
    const SyntheticWalkConfig* config;
    EntryReader read_entry;
    void* reader_context;
    std::uint64_t physical_base;
    std::uint64_t physical_end;
    std::uint64_t table_pages_scanned;
    RootScanStatus status;
    std::uint32_t mapping_count;
    Mapping* mappings;
};

struct SyntheticPage {
    std::uint64_t physical_address;
    std::uint64_t entries[512];
};

struct SyntheticMemory {
    SyntheticPage pages[4];
    std::uint32_t page_count;
    bool accessible;
};

struct SnapshotTablePage {
    std::uint64_t physical_address;
    std::uint8_t level;
};

// Shared scratch keeps the bounded traversal off the boot stack.
SnapshotTablePage snapshot_table_pages[MAX_SNAPSHOT_TABLE_PAGES]{};

bool add_without_overflow(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result
) {
    if (left > INVALID_VALUE - right)
        return false;
    result = left + right;
    return true;
}

bool canonical_address(
    std::uint64_t address,
    std::uint8_t address_bits,
    bool upper
) {
    if (address_bits == 0 || address_bits >= 64)
        return false;
    const std::uint64_t low_region_limit =
        (std::uint64_t{1} << address_bits) - 1;
    const std::uint64_t high_region_base =
        INVALID_VALUE - low_region_limit;
    return upper
        ? address >= high_region_base
        : address <= low_region_limit;
}

std::uint64_t level_shift(std::uint8_t level) {
    return 12 + 9 * (3 - level);
}

std::uint16_t level_index(std::uint64_t address, std::uint8_t level) {
    return static_cast<std::uint16_t>(
        (address >> level_shift(level)) & 0x1ff
    );
}

std::uint8_t physical_address_width(std::uint8_t encoding) {
    switch (encoding) {
        case 0: return 32;
        case 1: return 36;
        case 2: return 40;
        case 3: return 42;
        case 4: return 44;
        case 5: return 48;
        default: return 0;
    }
}

MemoryType classify_mair(std::uint8_t value) {
    if ((value & 0xf0) != 0)
        return MemoryType::normal;
    switch (value) {
        case 0x00:
        case 0x04:
        case 0x08:
        case 0x0c:
            return MemoryType::device;
        default:
            return MemoryType::unknown;
    }
}

bool decode_leaf(
    const SyntheticWalkConfig& config,
    std::uint64_t descriptor,
    std::uint8_t level,
    std::uint64_t virtual_base,
    const TableRestrictions& restrictions,
    Mapping& mapping
) {
    const std::uint64_t size = std::uint64_t{1} << level_shift(level);
    const std::uint64_t physical_mask =
        ((std::uint64_t{1} << config.physical_address_bits) - 1) & ~PAGE_MASK;
    std::uint64_t physical_end;
    if ((descriptor & RESERVED_ADDRESS_HIGH_MASK) != 0 ||
        (descriptor & ADDRESS_FIELD_MASK & ~physical_mask) != 0 ||
        (descriptor & ADDRESS_FIELD_MASK & (size - 1)) != 0 ||
        ((descriptor >> 8) & 0x3) == 1 ||
        !add_without_overflow(
            descriptor & physical_mask & ~(size - 1),
            size,
            physical_end) ||
        physical_end >
            (std::uint64_t{1} << config.physical_address_bits))
        return false;

    const std::uint8_t ap =
        static_cast<std::uint8_t>((descriptor >> 6) & 0x3);
    const bool writable = ((ap & 0x2) == 0) &&
        !restrictions.force_privileged_read_only;
    const bool user_access = (ap & 0x1) != 0 &&
        !restrictions.force_privileged_only;
    const std::uint8_t attribute_index =
        static_cast<std::uint8_t>((descriptor >> 2) & 0x7);
    const std::uint8_t mair_attribute = static_cast<std::uint8_t>(
        (config.mair_value >> (attribute_index * 8)) & 0xff
    );

    mapping = {
        virtual_base,
        descriptor & physical_mask & ~(size - 1),
        virtual_base,
        descriptor & physical_mask & ~(size - 1),
        size,
        config.mair_value,
        level,
        attribute_index,
        static_cast<std::uint8_t>((descriptor >> 8) & 0x3),
        writable,
        user_access,
        writable && user_access,
        ((descriptor >> 53) & 1) != 0 ||
            restrictions.privileged_execute_never,
        ((descriptor >> 54) & 1) != 0 ||
            restrictions.unprivileged_execute_never,
        ((descriptor >> 10) & 1) != 0,
        classify_mair(mair_attribute)
    };
    return true;
}

bool supports_synthetic_regime(const Regime& regime) {
    if (regime.levels != REQUIRED_LEVELS ||
        regime.virtual_address_bits != REQUIRED_VA_BITS ||
        regime.physical_address_bits < 32 ||
        regime.physical_address_bits > 48)
        return false;
    const std::uint64_t expected_mask =
        ((std::uint64_t{1} << regime.physical_address_bits) - 1) &
        ~PAGE_MASK;
    return regime.physical_address_mask == expected_mask;
}

bool build_device_page_descriptor(
    const Regime& regime,
    PhysicalRange device_range,
    std::uint64_t virtual_base,
    std::uint64_t mair_value,
    std::uint64_t& descriptor
) {
    if (!supports_synthetic_regime(regime) ||
        device_range.size != PAGE_SIZE ||
        (device_range.base & PAGE_MASK) != 0 ||
        (virtual_base & PAGE_MASK) != 0 ||
        static_cast<std::uint8_t>(
            mair_value >> (DEVICE_ATTRIBUTE_INDEX * 8)
        ) != DEVICE_NGNRNE_ATTRIBUTE ||
        classify_mair(DEVICE_NGNRNE_ATTRIBUTE) != MemoryType::device)
        return false;

    std::uint64_t physical_end;
    std::uint64_t virtual_end;
    if (!add_without_overflow(
            device_range.base,
            device_range.size,
            physical_end) ||
        physical_end >
            (std::uint64_t{1} << regime.physical_address_bits) ||
        !add_without_overflow(
            virtual_base,
            device_range.size - 1,
            virtual_end) ||
        !canonical_address(
            virtual_base,
            regime.virtual_address_bits,
            true) ||
        !canonical_address(
            virtual_end,
            regime.virtual_address_bits,
            true))
        return false;

    constexpr std::uint64_t ACCESS_FLAG = std::uint64_t{1} << 10;
    constexpr std::uint64_t OUTER_SHAREABLE = std::uint64_t{2} << 8;
    constexpr std::uint64_t PRIVILEGED_EXECUTE_NEVER =
        std::uint64_t{1} << 53;
    constexpr std::uint64_t UNPRIVILEGED_EXECUTE_NEVER =
        std::uint64_t{1} << 54;

    descriptor =
        device_range.base |
        DEVICE_ATTRIBUTE_INDEX << 2 |
        OUTER_SHAREABLE |
        ACCESS_FLAG |
        PRIVILEGED_EXECUTE_NEVER |
        UNPRIVILEGED_EXECUTE_NEVER |
        DESCRIPTOR_VALID |
        DESCRIPTOR_TYPE;

    SyntheticWalkConfig config{
        0,
        mair_value,
        regime.physical_address_bits,
        regime.virtual_address_bits,
        regime.levels,
        true
    };
    Mapping decoded{};
    return decode_leaf(
        config,
        descriptor,
        REQUIRED_LEVELS - 1,
        virtual_base,
        {},
        decoded
    ) &&
        decoded.physical_base == device_range.base &&
        decoded.size == device_range.size &&
        decoded.attribute_index == DEVICE_ATTRIBUTE_INDEX &&
        decoded.memory_type == MemoryType::device &&
        decoded.privileged_writable &&
        !decoded.unprivileged_access &&
        decoded.privileged_execute_never &&
        decoded.unprivileged_execute_never &&
        decoded.shareability == 2;
}

WalkResult walk_address(
    const SyntheticWalkConfig& config,
    EntryReader read_entry,
    void* reader_context,
    std::uint64_t virtual_address
) {
    WalkResult result{};
    result.status = WalkStatus::invalid_input;
    if (read_entry == nullptr ||
        !canonical_address(
            virtual_address,
            config.virtual_address_bits,
            config.upper_address_range))
        return result;
    if ((config.levels != 4) ||
        config.physical_address_bits < 32 ||
        config.physical_address_bits > 48 ||
        config.virtual_address_bits != REQUIRED_VA_BITS) {
        result.status = WalkStatus::unsupported_configuration;
        return result;
    }

    const std::uint64_t root = config.root_physical_address;
    if ((root & PAGE_MASK) != 0)
        return result;
    const std::uint64_t physical_mask =
        ((std::uint64_t{1} << config.physical_address_bits) - 1) & ~PAGE_MASK;
    if ((root & ~physical_mask) != 0)
        return result;

    std::uint64_t table = root;
    TableRestrictions restrictions{};
    for (std::uint8_t level = 0; level < REQUIRED_LEVELS; ++level) {
        std::uint64_t descriptor = 0;
        if (!read_entry(
                reader_context,
                table,
                level_index(virtual_address, level),
                descriptor)) {
            result.status = WalkStatus::inaccessible_table;
            return result;
        }
        if ((descriptor & DESCRIPTOR_VALID) == 0) {
            result.status = WalkStatus::unmapped;
            return result;
        }

        const bool table_or_page = (descriptor & DESCRIPTOR_TYPE) != 0;
        const bool last_level = level == REQUIRED_LEVELS - 1;
        if (last_level) {
            if (!table_or_page) {
                result.status = WalkStatus::malformed_descriptor;
                return result;
            }
            const std::uint64_t size = PAGE_SIZE;
            const std::uint64_t base =
                virtual_address & ~(size - 1);
            if (!decode_leaf(
                    config,
                    descriptor,
                    level,
                    base,
                    restrictions,
                    result.mapping)) {
                result.status = WalkStatus::malformed_descriptor;
                return result;
            }
            result.mapping.translated_physical_address =
                result.mapping.physical_base +
                (virtual_address & PAGE_MASK);
            result.mapping.virtual_address = virtual_address;
            result.status = WalkStatus::mapped;
            return result;
        }

        if (!table_or_page) {
            if (level == 0) {
                result.status = WalkStatus::malformed_descriptor;
                return result;
            }
            const std::uint64_t size =
                std::uint64_t{1} << level_shift(level);
            const std::uint64_t base =
                virtual_address & ~(size - 1);
            if (!decode_leaf(
                    config,
                    descriptor,
                    level,
                    base,
                    restrictions,
                    result.mapping)) {
                result.status = WalkStatus::malformed_descriptor;
                return result;
            }
            result.mapping.translated_physical_address =
                result.mapping.physical_base +
                (virtual_address & (size - 1));
            result.mapping.virtual_address = virtual_address;
            result.status = WalkStatus::mapped;
            return result;
        }

        if ((descriptor & RESERVED_ADDRESS_HIGH_MASK) != 0 ||
            (descriptor & TABLE_DESCRIPTOR_RESERVED_LOW_MASK) != 0 ||
            (descriptor & ADDRESS_FIELD_MASK & ~physical_mask) != 0) {
            result.status = WalkStatus::malformed_descriptor;
            return result;
        }
        table = descriptor & physical_mask;
        const std::uint64_t ap_table = (descriptor >> 61) & 0x3;
        restrictions.force_privileged_read_only =
            restrictions.force_privileged_read_only ||
            ((ap_table & 0x1) != 0);
        restrictions.force_privileged_only =
            restrictions.force_privileged_only ||
            ((ap_table & 0x2) != 0);
        restrictions.unprivileged_execute_never =
            restrictions.unprivileged_execute_never ||
            ((descriptor >> 60) & 1) != 0;
        restrictions.privileged_execute_never =
            restrictions.privileged_execute_never ||
            ((descriptor >> 59) & 1) != 0;
    }

    result.status = WalkStatus::malformed_descriptor;
    return result;
}

bool memmap_contains_bootloader_page(
    const limine_memmap_response* memory_map,
    std::uint64_t physical_address
) {
    if (memory_map == nullptr ||
        memory_map->entries == nullptr ||
        (physical_address & PAGE_MASK) != 0)
        return false;
    for (std::uint64_t i = 0; i < memory_map->entry_count; ++i) {
        const limine_memmap_entry* entry = memory_map->entries[i];
        if (entry == nullptr ||
            entry->type != LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            entry->length < PAGE_SIZE ||
            entry->base > INVALID_VALUE - entry->length)
            continue;
        const std::uint64_t end = entry->base + entry->length;
        if (physical_address >= entry->base &&
            physical_address <= end - PAGE_SIZE)
            return true;
    }
    return false;
}

bool live_read_entry(
    void* opaque_context,
    std::uint64_t table_physical_address,
    std::uint16_t index,
    std::uint64_t& descriptor
) {
    auto& context = *static_cast<LiveReaderContext*>(opaque_context);
    if (index >= 512 || (table_physical_address & PAGE_MASK) != 0)
        return false;
    if (!context.cached_page_valid ||
        context.cached_page != table_physical_address) {
        if (!memmap_contains_bootloader_page(
                context.memory_map,
                table_physical_address))
            return false;
        context.cached_page = table_physical_address;
        context.cached_page_valid = true;
    }
    if (table_physical_address >
        INVALID_VALUE - context.hhdm_offset)
        return false;
    const std::uint64_t virtual_address =
        context.hhdm_offset + table_physical_address;
    if (!canonical_address(
            virtual_address,
            REQUIRED_VA_BITS,
            true) ||
        virtual_address > INVALID_VALUE - PAGE_MASK ||
        !canonical_address(
            virtual_address + PAGE_MASK,
            REQUIRED_VA_BITS,
            true))
        return false;

    const auto* table = reinterpret_cast<const volatile std::uint64_t*>(
        virtual_address
    );
    descriptor = table[index];
    return true;
}

std::uint64_t checksum_word(std::uint64_t checksum, std::uint64_t word) {
    for (std::uint32_t byte = 0; byte < sizeof(word); ++byte) {
        checksum ^= static_cast<std::uint8_t>(word >> (byte * 8));
        checksum *= 1099511628211ull;
    }
    return checksum;
}

RootScanStatus snapshot_table_root(
    std::uint64_t root,
    const SyntheticWalkConfig& config,
    EntryReader read_entry,
    void* reader_context,
    std::uint64_t& page_count,
    std::uint64_t& checksum,
    std::uint64_t* output_pages = nullptr,
    std::uint64_t output_capacity = 0,
    std::uint64_t* output_count = nullptr
) {
    if (root == 0 || (root & PAGE_MASK) != 0)
        return RootScanStatus::inaccessible_table;

    snapshot_table_pages[0] = {root, 0};
    page_count = 1;
    checksum = checksum_word(1469598103934665603ull, root);
    const std::uint64_t physical_mask =
        ((std::uint64_t{1} << config.physical_address_bits) - 1) &
        ~PAGE_MASK;

    for (std::uint32_t cursor = 0; cursor < page_count; ++cursor) {
        const SnapshotTablePage current = snapshot_table_pages[cursor];
        checksum = checksum_word(checksum, current.physical_address);
        for (std::uint16_t index = 0; index < 512; ++index) {
            std::uint64_t descriptor = 0;
            if (!read_entry(
                    reader_context,
                    current.physical_address,
                    index,
                    descriptor))
                return RootScanStatus::inaccessible_table;
            checksum = checksum_word(checksum, descriptor);
            if ((descriptor & DESCRIPTOR_VALID) == 0)
                continue;

            const bool is_table =
                (descriptor & DESCRIPTOR_TYPE) != 0;
            const std::uint8_t level = current.level;
            if (level == REQUIRED_LEVELS - 1) {
                if (!is_table)
                    return RootScanStatus::malformed_descriptor;
                continue;
            }
            if (!is_table) {
                if (level == 0)
                    return RootScanStatus::malformed_descriptor;
                continue;
            }

            if ((descriptor & RESERVED_ADDRESS_HIGH_MASK) != 0 ||
                (descriptor & TABLE_DESCRIPTOR_RESERVED_LOW_MASK) != 0 ||
                (descriptor & ADDRESS_FIELD_MASK & ~physical_mask) != 0)
                return RootScanStatus::malformed_descriptor;

            const std::uint64_t child =
                descriptor & physical_mask;
            bool already_queued = false;
            for (std::uint64_t queued = 0;
                 queued < page_count;
                 ++queued) {
                if (snapshot_table_pages[queued].physical_address == child) {
                    if (snapshot_table_pages[queued].level != level + 1)
                        return RootScanStatus::malformed_descriptor;
                    already_queued = true;
                    break;
                }
            }
            if (already_queued)
                continue;
            if (page_count == MAX_SNAPSHOT_TABLE_PAGES)
                return RootScanStatus::budget_exhausted;
            snapshot_table_pages[page_count] = {
                child,
                static_cast<std::uint8_t>(level + 1)
            };
            ++page_count;
        }
    }
    if (output_pages != nullptr || output_count != nullptr ||
        output_capacity != 0) {
        if (output_pages == nullptr || output_count == nullptr)
            return RootScanStatus::budget_exhausted;
        for (std::uint64_t i = 0; i < page_count; ++i) {
            const std::uint64_t physical =
                snapshot_table_pages[i].physical_address;
            bool already_present = false;
            for (std::uint64_t j = 0; j < *output_count; ++j) {
                if (output_pages[j] == physical) {
                    already_present = true;
                    break;
                }
            }
            if (already_present)
                continue;
            if (*output_count >= output_capacity)
                return RootScanStatus::budget_exhausted;
            output_pages[(*output_count)++] = physical;
        }
    }
    return RootScanStatus::complete;
}

void fail_scan(ScanContext& context, RootScanStatus status) {
    if (context.status == RootScanStatus::complete)
        context.status = status;
}

bool scan_table(
    ScanContext& context,
    std::uint64_t table_physical_address,
    std::uint8_t level,
    std::uint64_t virtual_prefix,
    bool upper,
    const TableRestrictions& restrictions
) {
    if (context.status != RootScanStatus::complete)
        return false;
    if (++context.table_pages_scanned > MAX_TABLE_PAGES) {
        fail_scan(context, RootScanStatus::budget_exhausted);
        return false;
    }

    const std::uint64_t physical_mask =
        ((std::uint64_t{1} <<
          context.config->physical_address_bits) - 1) & ~PAGE_MASK;
    for (std::uint16_t index = 0; index < 512; ++index) {
        std::uint64_t descriptor = 0;
        if (!context.read_entry(
                context.reader_context,
                table_physical_address,
                index,
                descriptor)) {
            fail_scan(context, RootScanStatus::inaccessible_table);
            return false;
        }
        if ((descriptor & DESCRIPTOR_VALID) == 0)
            continue;

        const bool is_table = (descriptor & DESCRIPTOR_TYPE) != 0;
        const bool last_level = level == REQUIRED_LEVELS - 1;
        const std::uint64_t shift = level_shift(level);
        const std::uint64_t virtual_part =
            static_cast<std::uint64_t>(index) << shift;
        const std::uint64_t child_prefix =
            virtual_prefix | virtual_part;

        if (last_level || !is_table) {
            if ((last_level && !is_table) || (level == 0 && !is_table)) {
                fail_scan(context, RootScanStatus::malformed_descriptor);
                return false;
            }
            Mapping mapping{};
            std::uint64_t canonical_prefix = child_prefix;
            if (upper)
                canonical_prefix |= 0xffff000000000000ull;
            if (!decode_leaf(
                    *context.config,
                    descriptor,
                    level,
                    canonical_prefix,
                    restrictions,
                    mapping)) {
                fail_scan(context, RootScanStatus::malformed_descriptor);
                return false;
            }

            std::uint64_t mapping_end;
            if (!add_without_overflow(
                    mapping.physical_base,
                    mapping.size,
                    mapping_end)) {
                fail_scan(context, RootScanStatus::malformed_descriptor);
                return false;
            }
            if (mapping.physical_base < context.physical_end &&
                mapping_end > context.physical_base) {
                if (context.mapping_count >= MAX_AUDIT_MAPPINGS) {
                    fail_scan(context, RootScanStatus::budget_exhausted);
                    return false;
                }
                const std::uint64_t overlap_start =
                    mapping.physical_base > context.physical_base
                        ? mapping.physical_base
                        : context.physical_base;
                mapping.virtual_address +=
                    overlap_start - mapping.physical_base;
                mapping.translated_physical_address = overlap_start;
                context.mappings[context.mapping_count++] = mapping;
            }
            continue;
        }

        if ((descriptor & RESERVED_ADDRESS_HIGH_MASK) != 0 ||
            (descriptor & TABLE_DESCRIPTOR_RESERVED_LOW_MASK) != 0 ||
            (descriptor & ADDRESS_FIELD_MASK & ~physical_mask) != 0) {
            fail_scan(context, RootScanStatus::malformed_descriptor);
            return false;
        }
        TableRestrictions child_restrictions = restrictions;
        const std::uint64_t ap_table = (descriptor >> 61) & 0x3;
        child_restrictions.force_privileged_read_only =
            child_restrictions.force_privileged_read_only ||
            ((ap_table & 0x1) != 0);
        child_restrictions.force_privileged_only =
            child_restrictions.force_privileged_only ||
            ((ap_table & 0x2) != 0);
        child_restrictions.unprivileged_execute_never =
            child_restrictions.unprivileged_execute_never ||
            ((descriptor >> 60) & 1) != 0;
        child_restrictions.privileged_execute_never =
            child_restrictions.privileged_execute_never ||
            ((descriptor >> 59) & 1) != 0;
        if (!scan_table(
                context,
                descriptor & physical_mask,
                static_cast<std::uint8_t>(level + 1),
                child_prefix,
                upper,
                child_restrictions))
            return false;
    }
    return true;
}

RootScanStatus scan_root(
    const SyntheticWalkConfig& config,
    EntryReader read_entry,
    void* reader_context,
    std::uint64_t root,
    bool upper,
    std::uint64_t physical_base,
    std::uint64_t physical_end,
    std::uint64_t& table_pages_scanned,
    Mapping* mappings,
    std::uint32_t& mapping_count
) {
    if (root == 0 || (root & PAGE_MASK) != 0)
        return RootScanStatus::inaccessible_table;
    ScanContext context{
        &config,
        read_entry,
        reader_context,
        physical_base,
        physical_end,
        0,
        RootScanStatus::complete,
        mapping_count,
        mappings
    };
    TableRestrictions restrictions{};
    scan_table(context, root, 0, 0, upper, restrictions);
    table_pages_scanned += context.table_pages_scanned;
    mapping_count = context.mapping_count;
    return context.status;
}

bool synthetic_read_entry(
    void* opaque_memory,
    std::uint64_t table_physical_address,
    std::uint16_t index,
    std::uint64_t& descriptor
) {
    auto& memory = *static_cast<SyntheticMemory*>(opaque_memory);
    if (!memory.accessible || index >= 512)
        return false;
    for (std::uint32_t page = 0; page < memory.page_count; ++page) {
        if (memory.pages[page].physical_address == table_physical_address) {
            descriptor = memory.pages[page].entries[index];
            return true;
        }
    }
    return false;
}

void initialize_synthetic_page(
    SyntheticPage& page,
    std::uint64_t physical_address
) {
    page.physical_address = physical_address;
    for (std::uint32_t i = 0; i < 512; ++i)
        page.entries[i] = 0;
}

bool synthetic_test(bool condition) {
    return condition;
}

}

RegisterSnapshot read_registers() {
    RegisterSnapshot registers{};
    asm volatile("mrs %0, CurrentEL" : "=r"(registers.current_el));
    asm volatile("mrs %0, SCTLR_EL1" : "=r"(registers.sctlr_el1));
    asm volatile("mrs %0, TCR_EL1" : "=r"(registers.tcr_el1));
    asm volatile("mrs %0, TTBR0_EL1" : "=r"(registers.ttbr0_el1));
    asm volatile("mrs %0, TTBR1_EL1" : "=r"(registers.ttbr1_el1));
    asm volatile("mrs %0, MAIR_EL1" : "=r"(registers.mair_el1));
    asm volatile(
        "mrs %0, ID_AA64MMFR0_EL1"
        : "=r"(registers.id_aa64mmfr0_el1)
    );
    return registers;
}

RegimeStatus detect_regime(
    const RegisterSnapshot& registers,
    Regime& regime
) {
    if (registers.current_el != 4 && registers.current_el != 8)
        return RegimeStatus::unsupported_exception_level;
    if ((registers.sctlr_el1 & 1) == 0)
        return RegimeStatus::mmu_disabled;

    const std::uint8_t t0sz =
        static_cast<std::uint8_t>((registers.tcr_el1 >> 0) & 0x3f);
    const std::uint8_t tg0 =
        static_cast<std::uint8_t>((registers.tcr_el1 >> 14) & 0x3);
    const std::uint8_t t1sz =
        static_cast<std::uint8_t>((registers.tcr_el1 >> 16) & 0x3f);
    const std::uint8_t tg1 =
        static_cast<std::uint8_t>((registers.tcr_el1 >> 30) & 0x3);
    const std::uint8_t ips =
        static_cast<std::uint8_t>((registers.tcr_el1 >> 32) & 0x7);
    const std::uint8_t pa_encoding =
        static_cast<std::uint8_t>(
            registers.id_aa64mmfr0_el1 & 0xf
        );
    const std::uint8_t pa_bits = physical_address_width(pa_encoding);
    if (t0sz != 16 || t1sz != 16 ||
        tg0 != 0 || tg1 != 2 ||
        ((registers.tcr_el1 >> 59) & 1) != 0 ||
        ((registers.tcr_el1 >> 7) & 1) != 0 ||
        ((registers.tcr_el1 >> 23) & 1) != 0 ||
        ((registers.tcr_el1 >> 37) & 1) != 0 ||
        ((registers.tcr_el1 >> 38) & 1) != 0)
        return RegimeStatus::unsupported_configuration;
    if (pa_bits == 0 || pa_bits > 48)
        return RegimeStatus::unsupported_configuration;
    if (ips != pa_encoding)
        return RegimeStatus::inconsistent_registers;

    const std::uint64_t mask =
        ((std::uint64_t{1} << pa_bits) - 1) & ~PAGE_MASK;
    const std::uint64_t ttbr_mask = ADDRESS_FIELD_MASK;
    const std::uint64_t ttbr0_root = registers.ttbr0_el1 & ttbr_mask;
    const std::uint64_t ttbr1_root = registers.ttbr1_el1 & ttbr_mask;
    if ((registers.ttbr0_el1 & 0xffe) != 0 ||
        (registers.ttbr1_el1 & 0xffe) != 0 ||
        (ttbr0_root & ~mask) != 0 ||
        (ttbr1_root & ~mask) != 0)
        return RegimeStatus::inconsistent_registers;

    regime = {
        mask,
        pa_bits,
        REQUIRED_VA_BITS,
        REQUIRED_LEVELS
    };
    return RegimeStatus::supported;
}

ActiveTableSnapshot snapshot_active_tables(
    const RegisterSnapshot& registers,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    std::uint64_t* table_page_addresses,
    std::uint64_t table_page_capacity,
    std::uint64_t* table_page_count
) {
    if (table_page_count != nullptr)
        *table_page_count = 0;
    ActiveTableSnapshot snapshot{
        RootScanStatus::unsupported_regime,
        RootScanStatus::unsupported_regime,
        0,
        1469598103934665603ull
    };
    Regime regime{};
    if (detect_regime(registers, regime) != RegimeStatus::supported)
        return snapshot;
    const bool inventory_requested =
        table_page_addresses != nullptr ||
        table_page_capacity != 0 ||
        table_page_count != nullptr;
    if (inventory_requested &&
        (table_page_addresses == nullptr ||
         table_page_capacity == 0 ||
         table_page_count == nullptr))
        return snapshot;

    const SyntheticWalkConfig config{
        0,
        registers.mair_el1,
        regime.physical_address_bits,
        regime.virtual_address_bits,
        regime.levels,
        false
    };
    LiveReaderContext reader_context{
        hhdm_offset,
        memory_map,
        0,
        false
    };
    const std::uint64_t ttbr0_root =
        registers.ttbr0_el1 & regime.physical_address_mask;
    const std::uint64_t ttbr1_root =
        registers.ttbr1_el1 & regime.physical_address_mask;
    std::uint64_t ttbr0_pages = 0;
    std::uint64_t ttbr1_pages = 0;
    std::uint64_t ttbr0_checksum = 0;
    std::uint64_t ttbr1_checksum = 0;
    snapshot.ttbr0_status = snapshot_table_root(
        ttbr0_root,
        config,
        live_read_entry,
        &reader_context,
        ttbr0_pages,
        ttbr0_checksum,
        table_page_addresses,
        table_page_capacity,
        table_page_count
    );
    snapshot.ttbr1_status = snapshot_table_root(
        ttbr1_root,
        config,
        live_read_entry,
        &reader_context,
        ttbr1_pages,
        ttbr1_checksum,
        table_page_addresses,
        table_page_capacity,
        table_page_count
    );
    if ((snapshot.ttbr0_status != RootScanStatus::complete ||
         snapshot.ttbr1_status != RootScanStatus::complete) &&
        table_page_count != nullptr)
        *table_page_count = 0;
    snapshot.table_pages_scanned = ttbr0_pages + ttbr1_pages;
    snapshot.checksum = checksum_word(
        checksum_word(snapshot.checksum, ttbr0_checksum),
        ttbr1_checksum
    );
    return snapshot;
}

ActiveTranslation translate_active_address(
    const RegisterSnapshot& registers,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    std::uint64_t virtual_address
) {
    ActiveTranslation result{};
    result.status = ActiveTranslationStatus::unsupported_regime;

    Regime regime{};
    if (detect_regime(registers, regime) != RegimeStatus::supported)
        return result;

    constexpr std::uint64_t LOWER_LIMIT =
        (std::uint64_t{1} << REQUIRED_VA_BITS) - 1;
    constexpr std::uint64_t UPPER_BASE =
        INVALID_VALUE - LOWER_LIMIT;
    const bool upper = virtual_address >= UPPER_BASE;
    if (!upper && virtual_address > LOWER_LIMIT) {
        result.status = ActiveTranslationStatus::invalid_address;
        return result;
    }

    const SyntheticWalkConfig config{
        upper
            ? registers.ttbr1_el1 & regime.physical_address_mask
            : registers.ttbr0_el1 & regime.physical_address_mask,
        registers.mair_el1,
        regime.physical_address_bits,
        regime.virtual_address_bits,
        regime.levels,
        upper
    };
    LiveReaderContext reader_context{
        hhdm_offset,
        memory_map,
        0,
        false
    };
    const WalkResult walked = walk_address(
        config,
        live_read_entry,
        &reader_context,
        virtual_address
    );
    switch (walked.status) {
        case WalkStatus::mapped:
            result.status = ActiveTranslationStatus::mapped;
            result.mapping = walked.mapping;
            break;
        case WalkStatus::unmapped:
            result.status = ActiveTranslationStatus::unmapped;
            break;
        case WalkStatus::invalid_input:
            result.status = ActiveTranslationStatus::invalid_address;
            break;
        case WalkStatus::unsupported_configuration:
            result.status = ActiveTranslationStatus::unsupported_regime;
            break;
        case WalkStatus::inaccessible_table:
            result.status = ActiveTranslationStatus::inaccessible_table;
            break;
        case WalkStatus::malformed_descriptor:
            result.status = ActiveTranslationStatus::malformed_descriptor;
            break;
    }
    return result;
}

AuditReport inspect_physical_range(
    const RegisterSnapshot& registers,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    std::uint64_t physical_base,
    std::uint64_t physical_size
) {
    AuditReport report{};
    report.status = AuditStatus::invalid_range;
    report.ttbr0_status = RootScanStatus::inaccessible_table;
    report.ttbr1_status = RootScanStatus::inaccessible_table;
    if (physical_size == 0 ||
        physical_base > INVALID_VALUE - physical_size)
        return report;

    Regime regime{};
    if (detect_regime(registers, regime) != RegimeStatus::supported) {
        report.status = AuditStatus::unsupported_regime;
        return report;
    }
    if (physical_base + physical_size >
        (std::uint64_t{1} << regime.physical_address_bits))
        return report;
    const SyntheticWalkConfig config{
        0,
        registers.mair_el1,
        regime.physical_address_bits,
        regime.virtual_address_bits,
        regime.levels,
        false
    };
    LiveReaderContext reader_context{
        hhdm_offset,
        memory_map,
        0,
        false
    };
    const std::uint64_t physical_end =
        physical_base + physical_size;
    const std::uint64_t ttbr0_root =
        registers.ttbr0_el1 & ADDRESS_FIELD_MASK;
    const std::uint64_t ttbr1_root =
        registers.ttbr1_el1 & ADDRESS_FIELD_MASK;

    report.ttbr0_status = scan_root(
        config,
        live_read_entry,
        &reader_context,
        ttbr0_root,
        false,
        physical_base,
        physical_end,
        report.table_pages_scanned,
        report.mappings,
        report.mapping_count
    );
    report.ttbr1_status = scan_root(
        config,
        live_read_entry,
        &reader_context,
        ttbr1_root,
        true,
        physical_base,
        physical_end,
        report.table_pages_scanned,
        report.mappings,
        report.mapping_count
    );

    for (std::uint32_t i = 1; i < report.mapping_count; ++i) {
        const Mapping value = report.mappings[i];
        std::uint32_t j = i;
        while (j > 0 &&
               report.mappings[j - 1].physical_base >
                   value.physical_base) {
            report.mappings[j] = report.mappings[j - 1];
            --j;
        }
        report.mappings[j] = value;
    }

    std::uint64_t cursor = physical_base;
    for (std::uint32_t i = 0; i < report.mapping_count; ++i) {
        const Mapping& mapping = report.mappings[i];
        std::uint64_t mapping_end;
        if (!add_without_overflow(
                mapping.physical_base,
                mapping.size,
                mapping_end))
            continue;
        const std::uint64_t overlap_start =
            mapping.physical_base > physical_base
                ? mapping.physical_base
                : physical_base;
        const std::uint64_t overlap_end =
            mapping_end < physical_end ? mapping_end : physical_end;
        if (overlap_start > cursor &&
            report.gap_count < MAX_AUDIT_GAPS) {
            report.gaps[report.gap_count++] = {
                cursor,
                overlap_start - cursor
            };
        }
        if (overlap_end > cursor)
            cursor = overlap_end;
    }
    if (cursor < physical_end &&
        report.gap_count < MAX_AUDIT_GAPS) {
        report.gaps[report.gap_count++] = {
            cursor,
            physical_end - cursor
        };
    }
    report.fully_covered =
        report.mapping_count != 0 && report.gap_count == 0;

    const bool roots_complete =
        report.ttbr0_status == RootScanStatus::complete &&
        report.ttbr1_status == RootScanStatus::complete;
    if (report.mapping_count != 0) {
        report.status = AuditStatus::mapped;
    } else if (roots_complete) {
        report.status = AuditStatus::unmapped;
    } else {
        report.status = AuditStatus::inconclusive;
    }
    return report;
}

std::uint32_t run_synthetic_tests() {
    SyntheticMemory memory{};
    memory.page_count = 4;
    memory.accessible = true;
    initialize_synthetic_page(memory.pages[0], 0x1000);
    initialize_synthetic_page(memory.pages[1], 0x2000);
    initialize_synthetic_page(memory.pages[2], 0x3000);
    initialize_synthetic_page(memory.pages[3], 0x4000);

    constexpr std::uint64_t virtual_address = 0xffff800012345678ull;
    const std::uint16_t root_index = level_index(virtual_address, 0);
    const std::uint16_t level1_index = level_index(virtual_address, 1);
    const std::uint16_t level2_index = level_index(virtual_address, 2);
    const std::uint16_t level3_index = level_index(virtual_address, 3);
    memory.pages[0].entries[root_index] = 0x2000 | 3;
    memory.pages[1].entries[level1_index] = 0x3000 | 3;
    memory.pages[2].entries[level2_index] = 0x4000 | 3;

    const SyntheticWalkConfig config{
        0x1000,
        0x00000000000000ffull |
            (std::uint64_t{0x04} << 8) |
            (std::uint64_t{0x44} << 16),
        48,
        48,
        4,
        true
    };

    WalkResult result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(result.status == WalkStatus::unmapped))
        return 1;

    memory.pages[3].entries[level3_index] =
        0x12345000ull |
        (std::uint64_t{2} << 2) |
        (std::uint64_t{1} << 6) |
        (std::uint64_t{3} << 8) |
        (std::uint64_t{1} << 10) |
        (std::uint64_t{1} << 53) |
        (std::uint64_t{1} << 54) |
        3;
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(
            result.status == WalkStatus::mapped &&
            result.mapping.physical_base == 0x12345000 &&
            result.mapping.translated_physical_address == 0x12345678 &&
            result.mapping.virtual_base ==
                (virtual_address & ~PAGE_MASK) &&
            result.mapping.size == PAGE_SIZE &&
            result.mapping.level == 3 &&
            result.mapping.privileged_writable &&
            result.mapping.unprivileged_access &&
            result.mapping.unprivileged_writable &&
            result.mapping.privileged_execute_never &&
            result.mapping.unprivileged_execute_never &&
            result.mapping.shareability == 3 &&
            result.mapping.attribute_index == 2 &&
            result.mapping.memory_type == MemoryType::normal &&
            result.mapping.access_flag))
        return 2;

    memory.pages[3].entries[level3_index + 1] =
        0x22345000ull |
        (std::uint64_t{2} << 2) |
        (std::uint64_t{1} << 6) |
        (std::uint64_t{3} << 8) |
        (std::uint64_t{1} << 10) |
        (std::uint64_t{1} << 53) |
        (std::uint64_t{1} << 54) |
        3;
    const WalkResult next_page = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        (virtual_address & ~PAGE_MASK) + PAGE_SIZE + 0x45
    );
    if (!synthetic_test(
            next_page.status == WalkStatus::mapped &&
            next_page.mapping.translated_physical_address ==
                0x22345045 &&
            next_page.mapping.size == PAGE_SIZE &&
            next_page.mapping.physical_base !=
                result.mapping.physical_base + PAGE_SIZE))
        return 19;
    memory.pages[3].entries[level3_index + 1] = 0;

    memory.pages[3].entries[level3_index] =
        (memory.pages[3].entries[level3_index] & ~(3ull << 8)) |
        (1ull << 8);
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(
            result.status == WalkStatus::malformed_descriptor))
        return 18;
    memory.pages[3].entries[level3_index] =
        (memory.pages[3].entries[level3_index] & ~(3ull << 8)) |
        (3ull << 8);

    memory.pages[3].entries[level3_index] =
        (memory.pages[3].entries[level3_index] & ~(3ull << 6)) |
        (2ull << 6);
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(
            result.status == WalkStatus::mapped &&
            !result.mapping.privileged_writable &&
            !result.mapping.unprivileged_writable))
        return 3;

    memory.pages[2].entries[level2_index] =
        0x12000000ull | (std::uint64_t{3} << 6) | 1;
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(
            result.status == WalkStatus::mapped &&
            result.mapping.size == (std::uint64_t{1} << 21) &&
            result.mapping.level == 2 &&
            result.mapping.physical_base == 0x12000000 &&
            result.mapping.translated_physical_address ==
                (0x12000000ull |
                 (virtual_address & ((std::uint64_t{1} << 21) - 1))) &&
            !result.mapping.privileged_writable &&
            result.mapping.unprivileged_access &&
            result.mapping.mair_value == config.mair_value))
        return 4;

    memory.pages[1].entries[level1_index] =
        0x40000000ull | (std::uint64_t{3} << 6) | 1;
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(
            result.status == WalkStatus::mapped &&
            result.mapping.size == (std::uint64_t{1} << 30) &&
            result.mapping.level == 1 &&
            result.mapping.attribute_index == 0 &&
            result.mapping.memory_type == MemoryType::normal))
        return 5;

    memory.pages[1].entries[level1_index] = 0x3000 | 3;
    memory.pages[2].entries[level2_index] = 0x4000 | 3;
    memory.pages[3].entries[level3_index] =
        0x12345000ull | (std::uint64_t{1} << 2) | 3;
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (result.status != WalkStatus::mapped)
        return 60;
    if (result.mapping.attribute_index != 1)
        return 61;
    if (result.mapping.memory_type != MemoryType::device)
        return 62;

    memory.pages[0].entries[root_index] =
        0x2000 | 3 |
        (std::uint64_t{1} << 61) |
        (std::uint64_t{1} << 62) |
        (std::uint64_t{1} << 60) |
        (std::uint64_t{1} << 59);
    memory.pages[3].entries[level3_index] =
        0x12345000ull | (std::uint64_t{1} << 6) | 3;
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(
            result.status == WalkStatus::mapped &&
            !result.mapping.privileged_writable &&
            !result.mapping.unprivileged_access &&
            result.mapping.privileged_execute_never &&
            result.mapping.unprivileged_execute_never))
        return 16;

    memory.pages[0].entries[root_index] = 0x2000 | 3;
    memory.pages[1].entries[level1_index] = 0x3000 | 3;
    memory.pages[2].entries[level2_index] = 0x4000 | 3;
    memory.pages[3].entries[level3_index] = 0;
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(result.status == WalkStatus::unmapped))
        return 7;

    memory.pages[2].entries[level2_index] =
        0x12001000ull | 1;
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(
            result.status == WalkStatus::malformed_descriptor))
        return 8;

    memory.pages[2].entries[level2_index] = 0x4000 | 3;
    memory.pages[3].entries[level3_index] =
        0x12345000ull | 3;
    memory.accessible = false;
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(result.status == WalkStatus::inaccessible_table))
        return 9;

    memory.accessible = true;
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        0x0000800012345678ull
    );
    if (!synthetic_test(result.status == WalkStatus::invalid_input))
        return 10;
    if (!synthetic_test(
            canonical_address(0xffff000000000000ull, 48, true) &&
            canonical_address(0x0000ffffffffffffull, 48, false) &&
            !canonical_address(0x0001000000000000ull, 48, false)))
        return 15;

    SyntheticWalkConfig unsupported = config;
    unsupported.levels = 5;
    result = walk_address(
        unsupported,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(
            result.status == WalkStatus::unsupported_configuration))
        return 11;

    if (!synthetic_test(
            classify_mair(0x04) == MemoryType::device &&
            classify_mair(0x44) == MemoryType::normal &&
            classify_mair(0x02) == MemoryType::unknown))
        return 12;

    std::uint64_t sum = 0;
    if (!synthetic_test(
            !add_without_overflow(INVALID_VALUE - 3, 4, sum) &&
            add_without_overflow(0x1000, PAGE_SIZE, sum) &&
            sum == 0x2000))
        return 13;

    const AuditReport overflow_report = inspect_physical_range(
        RegisterSnapshot{},
        0,
        nullptr,
        INVALID_VALUE - 2,
        4
    );
    if (!synthetic_test(
            overflow_report.status == AuditStatus::invalid_range))
        return 14;

    memory.pages[0].entries[root_index] = 0x2000 | 3;
    memory.pages[1].entries[level1_index] = 0x3000 | 3;
    memory.pages[2].entries[level2_index] = 0x4000 | 3;
    memory.pages[3].entries[level3_index] =
        0x12345000ull | (std::uint64_t{1} << 48) | 3;
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(
            result.status == WalkStatus::malformed_descriptor))
        return 17;

    memory.pages[0].entries[root_index] = 0x2000 | 3 | (1ull << 2);
    memory.pages[1].entries[level1_index] = 0x3000 | 3;
    memory.pages[2].entries[level2_index] = 0x4000 | 3;
    memory.pages[3].entries[level3_index] = 0x12345000ull | 3;
    result = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        virtual_address
    );
    if (!synthetic_test(
            result.status == WalkStatus::malformed_descriptor))
        return 19;

    for (SyntheticPage& page : memory.pages)
        initialize_synthetic_page(page, page.physical_address);
    memory.pages[0].entries[0] = 0x2000 | 3;
    memory.pages[1].entries[0] = 1;
    Mapping mappings[MAX_AUDIT_MAPPINGS]{};
    std::uint32_t mapping_count = 0;
    std::uint64_t table_pages_scanned = 0;
    const RootScanStatus scan_status = scan_root(
        config,
        synthetic_read_entry,
        &memory,
        0x1000,
        true,
        0x09000000,
        0x09001000,
        table_pages_scanned,
        mappings,
        mapping_count
    );
    if (!synthetic_test(scan_status == RootScanStatus::complete))
        return 20;
    if (!synthetic_test(mapping_count == 1))
        return 21;
    if (!synthetic_test(
            mappings[0].virtual_address == 0xffff000009000000ull &&
            mappings[0].translated_physical_address == 0x09000000))
        return 22;

    SyntheticMemory snapshot_memory{};
    snapshot_memory.page_count = 4;
    snapshot_memory.accessible = true;
    initialize_synthetic_page(snapshot_memory.pages[0], 0x1000);
    initialize_synthetic_page(snapshot_memory.pages[1], 0x2000);
    initialize_synthetic_page(snapshot_memory.pages[2], 0x3000);
    initialize_synthetic_page(snapshot_memory.pages[3], 0x4000);
    snapshot_memory.pages[0].entries[0] = 0x2000 | 3;
    snapshot_memory.pages[1].entries[0] = 0x3000 | 3;
    snapshot_memory.pages[2].entries[0] = 0x4000 | 3;
    snapshot_memory.pages[3].entries[0] = 0x12345000ull | 3;
    std::uint64_t table_page_addresses[4]{};
    std::uint64_t table_page_count = 0;
    std::uint64_t scanned_pages = 0;
    std::uint64_t scan_checksum = 0;
    RootScanStatus inventory_status = snapshot_table_root(
        0x1000,
        config,
        synthetic_read_entry,
        &snapshot_memory,
        scanned_pages,
        scan_checksum,
        table_page_addresses,
        4,
        &table_page_count
    );
    if (!synthetic_test(
            inventory_status == RootScanStatus::complete &&
            table_page_count == 4 &&
            table_page_addresses[0] == 0x1000 &&
            table_page_addresses[1] == 0x2000 &&
            table_page_addresses[2] == 0x3000 &&
            table_page_addresses[3] == 0x4000))
        return 23;

    inventory_status = snapshot_table_root(
        0x1000,
        config,
        synthetic_read_entry,
        &snapshot_memory,
        scanned_pages,
        scan_checksum,
        table_page_addresses,
        4,
        &table_page_count
    );
    if (!synthetic_test(
            inventory_status == RootScanStatus::complete &&
            table_page_count == 4))
        return 24;

    table_page_count = 0;
    inventory_status = snapshot_table_root(
        0x1000,
        config,
        synthetic_read_entry,
        &snapshot_memory,
        scanned_pages,
        scan_checksum,
        table_page_addresses,
        3,
        &table_page_count
    );
    if (!synthetic_test(
            inventory_status == RootScanStatus::budget_exhausted))
        return 25;

    snapshot_memory.pages[0].entries[0] = 0x5000 | 3;
    table_page_count = 0;
    inventory_status = snapshot_table_root(
        0x1000,
        config,
        synthetic_read_entry,
        &snapshot_memory,
        scanned_pages,
        scan_checksum,
        table_page_addresses,
        4,
        &table_page_count
    );
    if (!synthetic_test(
            inventory_status == RootScanStatus::inaccessible_table &&
            table_page_count == 0))
        return 26;

    initialize_synthetic_page(snapshot_memory.pages[0], 0x1000);
    initialize_synthetic_page(snapshot_memory.pages[1], 0x2000);
    initialize_synthetic_page(snapshot_memory.pages[2], 0x3000);
    initialize_synthetic_page(snapshot_memory.pages[3], 0x4000);
    snapshot_memory.pages[0].entries[0] = 0x2000 | 3;
    snapshot_memory.pages[1].entries[0] = 0x3000 | 3;
    snapshot_memory.pages[2].entries[0] = 0x2000 | 3;
    inventory_status = snapshot_table_root(
        0x1000,
        config,
        synthetic_read_entry,
        &snapshot_memory,
        scanned_pages,
        scan_checksum,
        table_page_addresses,
        4,
        &table_page_count
    );
    if (!synthetic_test(
            inventory_status == RootScanStatus::malformed_descriptor))
        return 27;

    return 0;
}

std::uint32_t run_isolated_device_mapping_tests(
    const Regime& regime,
    PhysicalRange device_range
) {
    if (!supports_synthetic_regime(regime))
        return 1;

    Regime unsupported_regime = regime;
    unsupported_regime.levels = 3;
    std::uint64_t descriptor = 0;
    if (build_device_page_descriptor(
            unsupported_regime,
            device_range,
            EXPERIMENT_VIRTUAL_BASE,
            PROPOSED_MAIR_VALUE,
            descriptor))
        return 2;
    unsupported_regime = regime;
    unsupported_regime.virtual_address_bits = 47;
    if (build_device_page_descriptor(
            unsupported_regime,
            device_range,
            EXPERIMENT_VIRTUAL_BASE,
            PROPOSED_MAIR_VALUE,
            descriptor))
        return 8;
    unsupported_regime = regime;
    unsupported_regime.physical_address_bits = 52;
    if (build_device_page_descriptor(
            unsupported_regime,
            device_range,
            EXPERIMENT_VIRTUAL_BASE,
            PROPOSED_MAIR_VALUE,
            descriptor))
        return 9;
    unsupported_regime = regime;
    unsupported_regime.physical_address_mask ^= PAGE_SIZE;
    if (build_device_page_descriptor(
            unsupported_regime,
            device_range,
            EXPERIMENT_VIRTUAL_BASE,
            PROPOSED_MAIR_VALUE,
            descriptor))
        return 10;

    if (build_device_page_descriptor(
            regime,
            {0, 0},
            EXPERIMENT_VIRTUAL_BASE,
            PROPOSED_MAIR_VALUE,
            descriptor) ||
        build_device_page_descriptor(
            regime,
            {device_range.base, 0},
            EXPERIMENT_VIRTUAL_BASE,
            PROPOSED_MAIR_VALUE,
            descriptor) ||
        build_device_page_descriptor(
            regime,
            {device_range.base, PAGE_SIZE * 512},
            EXPERIMENT_VIRTUAL_BASE,
            PROPOSED_MAIR_VALUE,
            descriptor) ||
        build_device_page_descriptor(
            regime,
            {device_range.base | 1, PAGE_SIZE},
            EXPERIMENT_VIRTUAL_BASE,
            PROPOSED_MAIR_VALUE,
            descriptor) ||
        build_device_page_descriptor(
            regime,
            {INVALID_VALUE & ~PAGE_MASK, PAGE_SIZE},
            EXPERIMENT_VIRTUAL_BASE,
            PROPOSED_MAIR_VALUE,
            descriptor) ||
        build_device_page_descriptor(
            regime,
            {std::uint64_t{1} << regime.physical_address_bits, PAGE_SIZE},
            EXPERIMENT_VIRTUAL_BASE,
            PROPOSED_MAIR_VALUE,
            descriptor) ||
        build_device_page_descriptor(
            regime,
            device_range,
            EXPERIMENT_VIRTUAL_BASE + 1,
            PROPOSED_MAIR_VALUE,
            descriptor) ||
        build_device_page_descriptor(
            regime,
            device_range,
            0x0001000000000000ull,
            PROPOSED_MAIR_VALUE,
            descriptor) ||
        build_device_page_descriptor(
            regime,
            device_range,
            EXPERIMENT_VIRTUAL_BASE,
            0x00000000000000ffull,
            descriptor))
        return 3;

    if (!build_device_page_descriptor(
            regime,
            device_range,
            EXPERIMENT_VIRTUAL_BASE,
            PROPOSED_MAIR_VALUE,
            descriptor))
        return 4;

    SyntheticMemory memory{};
    memory.page_count = 4;
    memory.accessible = true;
    initialize_synthetic_page(memory.pages[0], 0x1000);
    initialize_synthetic_page(memory.pages[1], 0x2000);
    initialize_synthetic_page(memory.pages[2], 0x3000);
    initialize_synthetic_page(memory.pages[3], 0x4000);

    const std::uint16_t root_index =
        level_index(EXPERIMENT_VIRTUAL_BASE, 0);
    const std::uint16_t level1_index =
        level_index(EXPERIMENT_VIRTUAL_BASE, 1);
    const std::uint16_t level2_index =
        level_index(EXPERIMENT_VIRTUAL_BASE, 2);
    const std::uint16_t level3_index =
        level_index(EXPERIMENT_VIRTUAL_BASE, 3);
    memory.pages[0].entries[root_index] = 0x2000 | 3;
    memory.pages[1].entries[level1_index] = 0x3000 | 3;
    memory.pages[2].entries[level2_index] = 0x4000 | 3;
    memory.pages[3].entries[level3_index] = descriptor;

    const SyntheticWalkConfig config{
        0x1000,
        PROPOSED_MAIR_VALUE,
        regime.physical_address_bits,
        regime.virtual_address_bits,
        regime.levels,
        true
    };
    const std::uint64_t offsets[] = {
        0,
        PAGE_SIZE / 2,
        PAGE_SIZE - 1
    };
    for (const std::uint64_t offset : offsets) {
        const WalkResult result = walk_address(
            config,
            synthetic_read_entry,
            &memory,
            EXPERIMENT_VIRTUAL_BASE + offset
        );
        if (result.status != WalkStatus::mapped ||
            result.mapping.translated_physical_address !=
                device_range.base + offset ||
            result.mapping.virtual_base != EXPERIMENT_VIRTUAL_BASE ||
            result.mapping.physical_base != device_range.base ||
            result.mapping.size != device_range.size ||
            result.mapping.attribute_index != DEVICE_ATTRIBUTE_INDEX ||
            result.mapping.mair_value != PROPOSED_MAIR_VALUE ||
            result.mapping.memory_type != MemoryType::device ||
            !result.mapping.privileged_writable ||
            result.mapping.unprivileged_access ||
            result.mapping.unprivileged_writable ||
            !result.mapping.privileged_execute_never ||
            !result.mapping.unprivileged_execute_never ||
            result.mapping.shareability != 2)
            return 5;
    }

    const WalkResult before = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        EXPERIMENT_VIRTUAL_BASE - PAGE_SIZE
    );
    const WalkResult after = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        EXPERIMENT_VIRTUAL_BASE + PAGE_SIZE
    );
    if (before.status != WalkStatus::unmapped ||
        after.status != WalkStatus::unmapped)
        return 6;

    memory.pages[3].entries[level3_index] =
        descriptor | (std::uint64_t{1} << 48);
    const WalkResult malformed = walk_address(
        config,
        synthetic_read_entry,
        &memory,
        EXPERIMENT_VIRTUAL_BASE
    );
    if (malformed.status != WalkStatus::malformed_descriptor)
        return 7;

    return 0;
}

}
