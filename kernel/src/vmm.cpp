#include <cstdint>
#include <pmm.hpp>
#include <vmm.hpp>

namespace MilOS::Memory {

struct AddressSpaceAccess {
    static AddressSpace::State& get(AddressSpace& address_space) {
        return address_space.state_;
    }

    static const AddressSpace::State& get(
        const AddressSpace& address_space
    ) {
        return address_space.state_;
    }
};

namespace {

constexpr std::uint64_t PAGE_SIZE = 4096;
constexpr std::uint64_t TWO_MIB_PAGE_SIZE = 1ull << 21;
constexpr std::uint64_t ONE_GIB_PAGE_SIZE = 1ull << 30;
constexpr std::uint64_t PRESENT = 1;
constexpr std::uint64_t WRITABLE = 1ull << 1;
constexpr std::uint64_t USER_ACCESS = 1ull << 2;
constexpr std::uint64_t LARGE_PAGE = 1ull << 7;
constexpr std::uint64_t MAX_ADDRESS_MASK = 0x000ffffffffff000ull;
constexpr std::uint64_t INVALID_ADDRESS = ~std::uint64_t{0};
constexpr std::uint64_t ADDRESS_SPACE_STATE =
    0x4d694f5341444452ull;

struct WalkOptions {
    std::uint64_t physical_address_mask;
    std::uint8_t levels;
    bool two_mib_pages_supported;
    bool one_gib_pages_supported;
    bool page_attribute_table_supported;
};

using EntryReader = bool (*)(
    void* context,
    std::uint64_t table_physical_address,
    std::uint16_t index,
    std::uint64_t& entry
);

std::uint64_t root_physical_address = 0;
std::uint8_t level_count = 0;
std::uint64_t owned_page_tables[
    ADDRESS_SPACE_MAX_OWNED_PAGE_TABLES
] = {};
std::uint64_t owned_page_table_count = 0;

using OwnedMapping = AddressSpaceMapping;

OwnedMapping owned_mappings[
    ADDRESS_SPACE_MAX_OWNED_MAPPINGS
] = {};
std::uint64_t owned_mapping_count = 0;
std::uint64_t active_cr3_value = 0;

#if defined(__x86_64__)
std::uint64_t hhdm_offset = 0;
std::uint64_t physical_address_mask = 0;
limine_memmap_response* memory_map = nullptr;
bool two_mib_pages_supported = false;
bool one_gib_pages_supported = false;
bool page_attribute_table_supported = false;
#endif

bool is_canonical(std::uint64_t address, std::uint8_t levels) {
    if (levels != 4 && levels != 5)
        return false;

    const std::uint8_t sign_bit = levels == 5 ? 56 : 47;
    const std::uint64_t upper = address >> (sign_bit + 1);
    const std::uint64_t upper_mask =
        INVALID_ADDRESS >> (sign_bit + 1);
    const bool sign = ((address >> sign_bit) & 1) != 0;

    return upper == (sign ? upper_mask : 0);
}

std::uint64_t page_index(std::uint64_t address, std::uint8_t level) {
    const std::uint8_t shift =
        static_cast<std::uint8_t>(12 + 9 * (level - 1));
    return (address >> shift) & 0x1ff;
}

bool walk(
    std::uint64_t virtual_address,
    std::uint64_t root,
    const WalkOptions& options,
    EntryReader read_entry,
    void* reader_context,
    std::uint64_t& physical_address,
    std::uint64_t* page_size
) {
    if (read_entry == nullptr || options.levels < 4 || options.levels > 5 ||
        !is_canonical(virtual_address, options.levels))
        return false;

    std::uint64_t table_address = root;

    for (std::uint8_t level = options.levels; level > 0; --level) {
        std::uint64_t entry;
        if (!read_entry(
                reader_context,
                table_address,
                static_cast<std::uint16_t>(
                    page_index(virtual_address, level)),
                entry) ||
            (entry & PRESENT) == 0 ||
            (entry & MAX_ADDRESS_MASK &
             ~options.physical_address_mask) != 0)
            return false;

        if (level == 3 && (entry & LARGE_PAGE) != 0) {
            constexpr std::uint64_t reserved =
                (ONE_GIB_PAGE_SIZE - 1) & ~((1ull << 13) - 1);
            if (!options.one_gib_pages_supported ||
                (entry & reserved) != 0 ||
                (!options.page_attribute_table_supported &&
                 (entry & (1ull << 12)) != 0))
                return false;

            const std::uint64_t result =
                (entry & options.physical_address_mask &
                 ~(ONE_GIB_PAGE_SIZE - 1)) |
                (virtual_address & (ONE_GIB_PAGE_SIZE - 1));
            physical_address = result;
            if (page_size != nullptr)
                *page_size = ONE_GIB_PAGE_SIZE;
            return true;
        }

        if (level == 2 && (entry & LARGE_PAGE) != 0) {
            constexpr std::uint64_t reserved =
                (TWO_MIB_PAGE_SIZE - 1) & ~((1ull << 13) - 1);
            if (!options.two_mib_pages_supported ||
                (entry & reserved) != 0 ||
                (!options.page_attribute_table_supported &&
                 (entry & (1ull << 12)) != 0))
                return false;

            const std::uint64_t result =
                (entry & options.physical_address_mask &
                 ~(TWO_MIB_PAGE_SIZE - 1)) |
                (virtual_address & (TWO_MIB_PAGE_SIZE - 1));
            physical_address = result;
            if (page_size != nullptr)
                *page_size = TWO_MIB_PAGE_SIZE;
            return true;
        }

        if ((level == 5 || level == 4) &&
            (entry & LARGE_PAGE) != 0)
            return false;

        if (level == 3 && (entry & LARGE_PAGE) != 0)
            return false;

        if (level == 1) {
            if (!options.page_attribute_table_supported &&
                (entry & LARGE_PAGE) != 0)
                return false;

            const std::uint64_t result =
                (entry & options.physical_address_mask) |
                (virtual_address & (PAGE_SIZE - 1));
            physical_address = result;
            if (page_size != nullptr)
                *page_size = PAGE_SIZE;
            return true;
        }

        table_address = entry & options.physical_address_mask;
    }

    return false;
}

bool hhdm_region_contains_page(
    const limine_memmap_response* memory_map,
    std::uint64_t physical_address
) {
    if (memory_map == nullptr || memory_map->entries == nullptr)
        return false;

    for (std::uint64_t i = 0; i < memory_map->entry_count; ++i) {
        const auto* entry = memory_map->entries[i];
        if (entry == nullptr || entry->base > INVALID_ADDRESS - entry->length)
            continue;

        switch (entry->type) {
            case LIMINE_MEMMAP_USABLE:
            case LIMINE_MEMMAP_ACPI_RECLAIMABLE:
            case LIMINE_MEMMAP_ACPI_NVS:
            case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE:
            case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES:
            case LIMINE_MEMMAP_FRAMEBUFFER:
            case LIMINE_MEMMAP_RESERVED_MAPPED:
                break;
            default:
                continue;
        }

        if (physical_address < entry->base)
            continue;

        const std::uint64_t offset = physical_address - entry->base;
        if (offset <= entry->length &&
            PAGE_SIZE <= entry->length - offset)
            return true;
    }

    return false;
}

bool hhdm_page_virtual_address(
    std::uint64_t offset,
    std::uint64_t physical_address,
    std::uint8_t levels,
    std::uint64_t& virtual_address
) {
    if ((physical_address & (PAGE_SIZE - 1)) != 0 ||
        physical_address > INVALID_ADDRESS - offset)
        return false;

    const std::uint64_t candidate = offset + physical_address;
    if (candidate > INVALID_ADDRESS - (PAGE_SIZE - 1) ||
        !is_canonical(candidate, levels) ||
        !is_canonical(candidate + PAGE_SIZE - 1, levels))
        return false;

    virtual_address = candidate;
    return true;
}

#if defined(__x86_64__)
std::uint32_t read_cpuid_edx(std::uint32_t leaf, std::uint32_t subleaf) {
    std::uint32_t unused_a;
    std::uint32_t unused_b;
    std::uint32_t unused_c;
    std::uint32_t result_d;

    asm volatile(
        "cpuid"
        : "=a"(unused_a), "=b"(unused_b),
          "=c"(unused_c), "=d"(result_d)
        : "a"(leaf), "c"(subleaf)
    );

    return result_d;
}

bool supports_two_mib_pages() {
    return (read_cpuid_edx(1, 0) & (1u << 3)) != 0;
}

bool supports_page_attribute_table() {
    return (read_cpuid_edx(1, 0) & (1u << 16)) != 0;
}

bool supports_one_gib_pages() {
    std::uint32_t max_extended_leaf;
    std::uint32_t unused_b;
    std::uint32_t unused_c;
    std::uint32_t unused_d;

    asm volatile(
        "cpuid"
        : "=a"(max_extended_leaf), "=b"(unused_b),
          "=c"(unused_c), "=d"(unused_d)
        : "a"(0x80000000u), "c"(0)
    );

    return max_extended_leaf >= 0x80000001u &&
           supports_two_mib_pages() &&
           (read_cpuid_edx(0x80000001u, 0) & (1u << 26)) != 0;
}

bool physical_address_bits(std::uint8_t& bits) {
    constexpr std::uint8_t X86_64_MIN_PHYSICAL_BITS = 36;
    constexpr std::uint8_t X86_64_MAX_PHYSICAL_BITS = 52;

    std::uint32_t max_extended_leaf;
    std::uint32_t unused_b;
    std::uint32_t unused_c;
    std::uint32_t unused_d;

    asm volatile(
        "cpuid"
        : "=a"(max_extended_leaf), "=b"(unused_b),
          "=c"(unused_c), "=d"(unused_d)
        : "a"(0x80000000u), "c"(0)
    );

    if (max_extended_leaf < 0x80000008u) {
        bits = X86_64_MIN_PHYSICAL_BITS;
        return true;
    }

    std::uint32_t physical_bits;
    asm volatile(
        "cpuid"
        : "=a"(physical_bits), "=b"(unused_b),
          "=c"(unused_c), "=d"(unused_d)
        : "a"(0x80000008u), "c"(0)
    );
    physical_bits &= 0xff;

    if (physical_bits < X86_64_MIN_PHYSICAL_BITS ||
        physical_bits > X86_64_MAX_PHYSICAL_BITS)
        return false;

    bits = static_cast<std::uint8_t>(physical_bits);
    return true;
}

struct HhdmReaderContext {
    std::uint64_t offset;
    std::uint8_t levels;
    std::uint64_t physical_mask;
    const limine_memmap_response* memory_map;
};

bool hhdm_table_pointer(
    const HhdmReaderContext& reader,
    std::uint64_t table_physical_address,
    volatile std::uint64_t*& table
) {
    if ((table_physical_address & (PAGE_SIZE - 1)) != 0 ||
        (table_physical_address & MAX_ADDRESS_MASK &
         ~reader.physical_mask) != 0 ||
        !hhdm_region_contains_page(
            reader.memory_map, table_physical_address))
        return false;

    std::uint64_t table_virtual_address;
    if (!hhdm_page_virtual_address(
            reader.offset,
            table_physical_address,
            reader.levels,
            table_virtual_address))
        return false;

    table = reinterpret_cast<volatile std::uint64_t*>(
        table_virtual_address);
    return true;
}

bool read_hhdm_entry(
    void* context,
    std::uint64_t table_physical_address,
    std::uint16_t index,
    std::uint64_t& entry
) {
    auto* reader = static_cast<HhdmReaderContext*>(context);
    if (reader == nullptr)
        return false;

    volatile std::uint64_t* table;
    if (!hhdm_table_pointer(
            *reader, table_physical_address, table))
        return false;

    entry = table[index];
    return true;
}

using AllocatePage = std::uint64_t (*)(void* context);
using FreePage = bool (*)(void* context, std::uint64_t physical_address);

struct MutationContext {
    HhdmReaderContext reader;
    std::uint64_t root;
    std::uint64_t* owned_tables;
    std::uint64_t* owned_table_count;
    std::uint64_t owned_table_capacity;
    OwnedMapping* owned_mappings;
    std::uint64_t* owned_mapping_count;
    std::uint64_t owned_mapping_capacity;
    AllocatePage allocate_page;
    FreePage free_page;
    void* allocator_context;
    bool invalidate_tlb;
};

std::uint64_t allocate_pmm_page(void*) {
    return MilOS::Memory::allocate_page();
}

bool free_pmm_page(void*, std::uint64_t physical_address) {
    return MilOS::Memory::free_page(physical_address);
}

bool page_is_owned(
    const MutationContext& context,
    std::uint64_t physical_address
) {
    for (std::uint64_t i = 0; i < *context.owned_table_count; ++i) {
        if (context.owned_tables[i] == physical_address)
            return true;
    }
    return false;
}

void remove_owned_page(
    MutationContext& context,
    std::uint64_t physical_address
) {
    for (std::uint64_t i = 0; i < *context.owned_table_count; ++i) {
        if (context.owned_tables[i] == physical_address) {
            context.owned_tables[i] =
                context.owned_tables[*context.owned_table_count - 1];
            --*context.owned_table_count;
            return;
        }
    }
}

std::uint64_t find_owned_mapping(
    const MutationContext& context,
    std::uint64_t virtual_address,
    std::uint64_t physical_address
) {
    for (std::uint64_t i = 0; i < *context.owned_mapping_count; ++i) {
        if (context.owned_mappings[i].virtual_address == virtual_address &&
            context.owned_mappings[i].physical_address == physical_address)
            return i;
    }
    return INVALID_ADDRESS;
}

void remove_owned_mapping(
    MutationContext& context,
    std::uint64_t mapping_index
) {
    context.owned_mappings[mapping_index] =
        context.owned_mappings[*context.owned_mapping_count - 1];
    --*context.owned_mapping_count;
}

bool allocate_zeroed_table(
    MutationContext& context,
    std::uint64_t& physical_address
) {
    physical_address = context.allocate_page(context.allocator_context);
    if (physical_address == 0)
        return false;
    if ((physical_address & (PAGE_SIZE - 1)) != 0 ||
        (physical_address & ~context.reader.physical_mask) != 0) {
        context.free_page(
            context.allocator_context, physical_address);
        physical_address = 0;
        return false;
    }

    volatile std::uint64_t* table;
    if (!hhdm_table_pointer(
            context.reader, physical_address, table)) {
        context.free_page(
            context.allocator_context, physical_address);
        physical_address = 0;
        return false;
    }

    for (std::uint16_t i = 0; i < 512; ++i)
        table[i] = 0;
    return true;
}

bool release_uncommitted_tables(
    MutationContext& context,
    const std::uint64_t* pages,
    std::uint8_t page_count
) {
    bool released = true;
    while (page_count > 0) {
        --page_count;
        if (!context.free_page(
                context.allocator_context, pages[page_count]))
            released = false;
    }
    return released;
}

bool map_page_in_context(
    MutationContext& context,
    std::uint64_t virtual_address,
    std::uint64_t physical_address,
    bool writable
) {
    if (context.reader.levels < 4 ||
        context.reader.levels > 5 ||
        context.owned_tables == nullptr ||
        context.owned_table_count == nullptr ||
        context.owned_mappings == nullptr ||
        context.owned_mapping_count == nullptr ||
        context.allocate_page == nullptr ||
        context.free_page == nullptr ||
        (virtual_address & (PAGE_SIZE - 1)) != 0 ||
        (physical_address & (PAGE_SIZE - 1)) != 0 ||
        (physical_address & ~context.reader.physical_mask) != 0 ||
        !is_canonical(virtual_address, context.reader.levels) ||
        *context.owned_mapping_count >=
            context.owned_mapping_capacity)
        return false;
    if (*context.owned_table_count > context.owned_table_capacity)
        return false;

    volatile std::uint64_t* slots[6] = {};
    std::uint64_t table_addresses[6] = {};
    table_addresses[context.reader.levels] = context.root;
    std::uint64_t table_address = context.root;
    std::uint8_t missing_level = 0;

    for (std::uint8_t level = context.reader.levels;
         level > 1;
         --level) {
        volatile std::uint64_t* table;
        if (!hhdm_table_pointer(
                context.reader, table_address, table))
            return false;

        auto* slot = &table[page_index(virtual_address, level)];
        slots[level] = slot;
        const std::uint64_t entry = *slot;

        if ((entry & PRESENT) == 0) {
            if (entry != 0)
                return false;
            missing_level = level;
            break;
        }

        if ((entry & MAX_ADDRESS_MASK &
             ~context.reader.physical_mask) != 0 ||
            (entry & LARGE_PAGE) != 0 ||
            (writable && (entry & WRITABLE) == 0))
            return false;

        table_address = entry & context.reader.physical_mask;
        table_addresses[level - 1] = table_address;
    }

    if (missing_level == 0) {
        volatile std::uint64_t* table;
        if (!hhdm_table_pointer(
                context.reader, table_address, table))
            return false;
        slots[1] = &table[page_index(virtual_address, 1)];
        if (*slots[1] != 0)
            return false;
    }

    const std::uint8_t new_table_count =
        missing_level == 0
            ? 0
            : static_cast<std::uint8_t>(missing_level - 1);
    if (new_table_count >
        context.owned_table_capacity - *context.owned_table_count)
        return false;

    std::uint64_t new_tables[4] = {};
    std::uint8_t allocated_count = 0;
    for (; allocated_count < new_table_count; ++allocated_count) {
        if (!allocate_zeroed_table(
                context, new_tables[allocated_count])) {
            const std::uint8_t release_count =
                new_tables[allocated_count] == 0
                    ? allocated_count
                    : static_cast<std::uint8_t>(allocated_count + 1);
            release_uncommitted_tables(
                context, new_tables, release_count);
            return false;
        }

        const std::uint8_t level = static_cast<std::uint8_t>(
            missing_level - allocated_count - 1);
        volatile std::uint64_t* table;
        if (!hhdm_table_pointer(
                context.reader, new_tables[allocated_count], table)) {
            ++allocated_count;
            release_uncommitted_tables(
                context, new_tables, allocated_count);
            return false;
        }
        slots[level] = &table[page_index(virtual_address, level)];
        table_addresses[level] = new_tables[allocated_count];
    }

    for (std::uint8_t i = 0; i < allocated_count; ++i)
        context.owned_tables[*context.owned_table_count + i] =
            new_tables[i];
    *context.owned_table_count += allocated_count;

    for (std::uint8_t i = 0; i < allocated_count; ++i) {
        const std::uint8_t parent_level =
            static_cast<std::uint8_t>(missing_level - i);
        *slots[parent_level] =
            new_tables[i] | PRESENT | WRITABLE;
    }

    *slots[1] = physical_address | PRESENT |
        (writable ? WRITABLE : 0);
    context.owned_mappings[*context.owned_mapping_count] = {
        virtual_address,
        physical_address
    };
    ++*context.owned_mapping_count;

    if (context.invalidate_tlb)
        asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
    return true;
}

bool unmap_page_in_context(
    MutationContext& context,
    std::uint64_t virtual_address
) {
    if (context.reader.levels < 4 ||
        context.reader.levels > 5 ||
        context.owned_tables == nullptr ||
        context.owned_table_count == nullptr ||
        context.owned_mappings == nullptr ||
        context.owned_mapping_count == nullptr ||
        !is_canonical(virtual_address, context.reader.levels) ||
        (virtual_address & (PAGE_SIZE - 1)) != 0)
        return false;

    volatile std::uint64_t* slots[6] = {};
    std::uint64_t table_addresses[6] = {};
    table_addresses[context.reader.levels] = context.root;
    std::uint64_t table_address = context.root;

    for (std::uint8_t level = context.reader.levels;
         level > 1;
         --level) {
        volatile std::uint64_t* table;
        if (!hhdm_table_pointer(
                context.reader, table_address, table))
            return false;

        auto* slot = &table[page_index(virtual_address, level)];
        slots[level] = slot;
        const std::uint64_t entry = *slot;
        if ((entry & PRESENT) == 0 ||
            (entry & MAX_ADDRESS_MASK &
             ~context.reader.physical_mask) != 0 ||
            (entry & LARGE_PAGE) != 0)
            return false;

        table_address = entry & context.reader.physical_mask;
        table_addresses[level - 1] = table_address;
    }

    volatile std::uint64_t* leaf_table;
    if (!hhdm_table_pointer(
            context.reader, table_address, leaf_table))
        return false;
    slots[1] = &leaf_table[page_index(virtual_address, 1)];
    const std::uint64_t leaf = *slots[1];
    if ((leaf & PRESENT) == 0 ||
        (leaf & MAX_ADDRESS_MASK &
         ~context.reader.physical_mask) != 0 ||
        find_owned_mapping(
            context,
            virtual_address,
            leaf & context.reader.physical_mask) == INVALID_ADDRESS)
        return false;

    *slots[1] = 0;
    if (context.invalidate_tlb)
        asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
    remove_owned_mapping(
        context,
        find_owned_mapping(
            context,
            virtual_address,
            leaf & context.reader.physical_mask));

    for (std::uint8_t level = 1;
         level < context.reader.levels;
         ++level) {
        const std::uint64_t child_address = table_addresses[level];
        if (!page_is_owned(context, child_address))
            break;

        volatile std::uint64_t* child_table;
        if (!hhdm_table_pointer(
                context.reader, child_address, child_table))
            break;

        bool empty = true;
        for (std::uint16_t i = 0; i < 512; ++i) {
            if (child_table[i] != 0) {
                empty = false;
                break;
            }
        }
        if (!empty)
            break;

        volatile std::uint64_t* parent_slot = slots[level + 1];
        const std::uint64_t parent_entry = *parent_slot;
        *parent_slot = 0;
        if (context.invalidate_tlb)
            asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");

        if (!context.free_page(
                context.allocator_context, child_address)) {
            *parent_slot = parent_entry;
            if (context.invalidate_tlb)
                asm volatile(
                    "invlpg (%0)" : : "r"(virtual_address) : "memory");
            break;
        }
        remove_owned_page(context, child_address);
    }

    return true;
}

bool read_4k_leaf_entry(
    const HhdmReaderContext& reader,
    std::uint64_t root,
    std::uint64_t virtual_address,
    std::uint64_t& leaf_entry
) {
    if (!is_canonical(virtual_address, reader.levels))
        return false;

    HhdmReaderContext reader_copy = reader;
    std::uint64_t table_address = root;
    for (std::uint8_t level = reader.levels; level > 0; --level) {
        std::uint64_t entry;
        if (!read_hhdm_entry(
                &reader_copy,
                table_address,
                static_cast<std::uint16_t>(
                    page_index(virtual_address, level)),
                entry) ||
            (entry & PRESENT) == 0)
            return false;

        if (level == 1) {
            leaf_entry = entry;
            return true;
        }
        if ((entry & LARGE_PAGE) != 0)
            return false;
        table_address = entry & reader.physical_mask;
    }
    return false;
}

MutationContext make_address_space_context(
    AddressSpace& address_space,
    AllocatePage allocate_page,
    FreePage free_page,
    void* allocator_context,
    bool invalidate_tlb
) {
    auto& state = AddressSpaceAccess::get(address_space);
    return {
        {
            hhdm_offset,
            state.levels,
            physical_address_mask,
            memory_map
        },
        state.root_physical_address,
        state.owned_table_pages,
        &state.owned_table_count,
        ADDRESS_SPACE_MAX_OWNED_PAGE_TABLES,
        state.owned_mappings,
        &state.owned_mapping_count,
        ADDRESS_SPACE_MAX_OWNED_MAPPINGS,
        allocate_page,
        free_page,
        allocator_context,
        invalidate_tlb
    };
}

bool address_space_is_empty(const AddressSpace& address_space) {
    const auto& state = AddressSpaceAccess::get(address_space);
    if (state.state != 0 ||
        state.root_physical_address != 0 ||
        state.owned_table_count != 0 ||
        state.owned_mapping_count != 0 ||
        state.levels != 0)
        return false;

    for (std::uint8_t i = 0; i < sizeof(state.reserved); ++i) {
        if (state.reserved[i] != 0)
            return false;
    }
    return true;
}

bool address_space_is_valid(const AddressSpace& address_space) {
    const auto& state = AddressSpaceAccess::get(address_space);
    if (state.state != ADDRESS_SPACE_STATE ||
        state.root_physical_address == 0 ||
        (state.root_physical_address & (PAGE_SIZE - 1)) != 0 ||
        (state.root_physical_address &
         ~physical_address_mask) != 0 ||
        state.levels != level_count ||
        state.owned_table_count >
            ADDRESS_SPACE_MAX_OWNED_PAGE_TABLES ||
        state.owned_mapping_count >
            ADDRESS_SPACE_MAX_OWNED_MAPPINGS)
        return false;

    for (std::uint8_t i = 0; i < sizeof(state.reserved); ++i) {
        if (state.reserved[i] != 0)
            return false;
    }
    return true;
}

void clear_address_space(AddressSpace& address_space) {
    auto& state = AddressSpaceAccess::get(address_space);
    state.root_physical_address = 0;
    state.owned_table_count = 0;
    state.owned_mapping_count = 0;
    state.levels = 0;
    for (std::uint8_t i = 0; i < sizeof(state.reserved); ++i)
        state.reserved[i] = 0;
    state.state = 0;

    for (std::uint16_t i = 0; i < 512; ++i)
        state.shared_root_entries[i] = 0;
    for (std::uint64_t i = 0;
         i < ADDRESS_SPACE_MAX_OWNED_PAGE_TABLES;
         ++i)
        state.owned_table_pages[i] = 0;
    for (std::uint64_t i = 0;
         i < ADDRESS_SPACE_MAX_OWNED_MAPPINGS;
         ++i) {
        state.owned_mappings[i].virtual_address = 0;
        state.owned_mappings[i].physical_address = 0;
    }
}

bool create_address_space_with_allocator(
    AddressSpace& address_space,
    AllocatePage allocate_page,
    FreePage free_page,
    void* allocator_context
) {
    if (!address_space_is_empty(address_space) ||
        level_count < 4 || level_count > 5 ||
        allocate_page == nullptr || free_page == nullptr)
        return false;

    MutationContext context = make_address_space_context(
        address_space,
        allocate_page,
        free_page,
        allocator_context,
        false);
    context.reader.levels = level_count;

    std::uint64_t new_root = 0;
    if (!allocate_zeroed_table(context, new_root))
        return false;

    volatile std::uint64_t* source_root;
    volatile std::uint64_t* destination_root;
    if (!hhdm_table_pointer(
            context.reader, root_physical_address, source_root) ||
        !hhdm_table_pointer(
            context.reader, new_root, destination_root)) {
        free_page(allocator_context, new_root);
        return false;
    }

    std::uint64_t shared_entries[512] = {};
    for (std::uint16_t index = 256; index < 512; ++index) {
        const std::uint64_t entry = source_root[index];
        if (((entry & PRESENT) == 0 && entry != 0) ||
            (entry & MAX_ADDRESS_MASK &
             ~physical_address_mask) != 0 ||
            ((entry & PRESENT) != 0 && (entry & LARGE_PAGE) != 0)) {
            free_page(allocator_context, new_root);
            return false;
        }
        shared_entries[index] = entry;
        destination_root[index] = entry;
    }

    clear_address_space(address_space);
    auto& state = AddressSpaceAccess::get(address_space);
    state.root_physical_address = new_root;
    state.levels = level_count;
    for (std::uint16_t index = 256; index < 512; ++index)
        state.shared_root_entries[index] =
            shared_entries[index];
    state.state = ADDRESS_SPACE_STATE;
    return true;
}

bool address_space_root_entries_are_consistent(
    const AddressSpace& address_space,
    const volatile std::uint64_t* root
) {
    const auto& state = AddressSpaceAccess::get(address_space);
    for (std::uint16_t index = 0; index < 256; ++index) {
        if (root[index] != 0)
            return false;
    }
    for (std::uint16_t index = 256; index < 512; ++index) {
        if (root[index] != state.shared_root_entries[index])
            return false;
    }
    return true;
}

bool address_space_private_mapping(
    const AddressSpace& address_space,
    std::uint64_t virtual_address
) {
    const auto& state = AddressSpaceAccess::get(address_space);
    return page_index(virtual_address, state.levels) < 256;
}

AddressSpace test_address_spaces[2] = {};
#endif

struct SyntheticTables {
    std::uint64_t entries[5][512];
};

struct SyntheticReaderContext {
    SyntheticTables* tables;
};

bool read_synthetic_entry(
    void* context,
    std::uint64_t table_physical_address,
    std::uint16_t index,
    std::uint64_t& entry
) {
    auto* reader = static_cast<SyntheticReaderContext*>(context);
    if (reader == nullptr || reader->tables == nullptr ||
        index >= 512 || table_physical_address < PAGE_SIZE ||
        table_physical_address > 5 * PAGE_SIZE ||
        table_physical_address % PAGE_SIZE != 0)
        return false;

    const std::uint64_t level = table_physical_address / PAGE_SIZE;
    entry = reader->tables->entries[level - 1][index];
    return true;
}

void clear_synthetic_tables(SyntheticTables& tables) {
    for (std::uint8_t table = 0; table < 5; ++table) {
        for (std::uint16_t index = 0; index < 512; ++index)
            tables.entries[table][index] = 0;
    }
}

void build_synthetic_path(
    SyntheticTables& tables,
    std::uint64_t virtual_address,
    std::uint8_t levels,
    std::uint8_t leaf_level,
    std::uint64_t leaf_entry
) {
    for (std::uint8_t level = levels; level > leaf_level; --level) {
        const std::uint64_t child_table =
            static_cast<std::uint64_t>(level - 1) * PAGE_SIZE;
        tables.entries[level - 1][page_index(virtual_address, level)] =
            child_table | PRESENT;
    }

    tables.entries[leaf_level - 1][
        page_index(virtual_address, leaf_level)] = leaf_entry;
}

bool expect_failed_walk(
    std::uint64_t virtual_address,
    std::uint64_t root,
    const WalkOptions& options,
    SyntheticReaderContext& context
) {
    constexpr std::uint64_t OUTPUT_SENTINEL = 0xfeedfacecafebeefull;
    std::uint64_t physical_address = OUTPUT_SENTINEL;
    std::uint64_t page_size = OUTPUT_SENTINEL;

    return !walk(
               virtual_address,
               root,
               options,
               read_synthetic_entry,
               &context,
               physical_address,
               &page_size) &&
           physical_address == OUTPUT_SENTINEL &&
           page_size == OUTPUT_SENTINEL;
}

std::uint8_t synthetic_tests(
    std::uint8_t physical_bits,
    const WalkOptions& hardware_options
) {
    constexpr std::uint64_t VIRTUAL_ADDRESS = 0x0000123456789abcull;
    constexpr std::uint64_t PHYSICAL_FRAME = 0x0000000123400000ull;
    constexpr std::uint64_t OUTPUT_SENTINEL = 0xfeedfacecafebeefull;
    const std::uint64_t physical_mask =
        ((1ull << physical_bits) - 1) & MAX_ADDRESS_MASK;

    SyntheticTables tables;
    SyntheticReaderContext reader = {&tables};
    WalkOptions options = hardware_options;
    options.physical_address_mask = physical_mask;

    clear_synthetic_tables(tables);
    build_synthetic_path(
        tables, VIRTUAL_ADDRESS, 4, 1, PHYSICAL_FRAME | PRESENT);
    std::uint64_t translated = OUTPUT_SENTINEL;
    std::uint64_t page_size = OUTPUT_SENTINEL;
    if (!walk(
            VIRTUAL_ADDRESS,
            4 * PAGE_SIZE,
            options,
            read_synthetic_entry,
            &reader,
            translated,
            &page_size) ||
        translated !=
            (PHYSICAL_FRAME | (VIRTUAL_ADDRESS & (PAGE_SIZE - 1))) ||
        page_size != PAGE_SIZE)
        return 1;

    clear_synthetic_tables(tables);
    build_synthetic_path(
        tables,
        VIRTUAL_ADDRESS,
        4,
        2,
        (PHYSICAL_FRAME & ~(TWO_MIB_PAGE_SIZE - 1)) |
            LARGE_PAGE | PRESENT);
    translated = OUTPUT_SENTINEL;
    page_size = OUTPUT_SENTINEL;
    const bool two_mib_walked = walk(
        VIRTUAL_ADDRESS,
        4 * PAGE_SIZE,
        options,
        read_synthetic_entry,
        &reader,
        translated,
        &page_size);
    if (options.two_mib_pages_supported) {
        if (!two_mib_walked || page_size != TWO_MIB_PAGE_SIZE ||
            translated !=
                ((PHYSICAL_FRAME & ~(TWO_MIB_PAGE_SIZE - 1)) |
                 (VIRTUAL_ADDRESS & (TWO_MIB_PAGE_SIZE - 1))))
            return 2;
    } else if (two_mib_walked ||
               translated != OUTPUT_SENTINEL ||
               page_size != OUTPUT_SENTINEL) {
        return 2;
    }

    clear_synthetic_tables(tables);
    build_synthetic_path(
        tables,
        VIRTUAL_ADDRESS,
        4,
        3,
        (PHYSICAL_FRAME & ~(ONE_GIB_PAGE_SIZE - 1)) |
            LARGE_PAGE | PRESENT);
    translated = OUTPUT_SENTINEL;
    page_size = OUTPUT_SENTINEL;
    const bool one_gib_walked = walk(
        VIRTUAL_ADDRESS,
        4 * PAGE_SIZE,
        options,
        read_synthetic_entry,
        &reader,
        translated,
        &page_size);
    if (options.one_gib_pages_supported) {
        if (!one_gib_walked || page_size != ONE_GIB_PAGE_SIZE ||
            translated !=
                ((PHYSICAL_FRAME & ~(ONE_GIB_PAGE_SIZE - 1)) |
                 (VIRTUAL_ADDRESS & (ONE_GIB_PAGE_SIZE - 1))))
            return 3;
    } else if (one_gib_walked ||
               translated != OUTPUT_SENTINEL ||
               page_size != OUTPUT_SENTINEL) {
        return 3;
    }

    options.levels = 5;
    clear_synthetic_tables(tables);
    build_synthetic_path(
        tables, VIRTUAL_ADDRESS, 5, 1, PHYSICAL_FRAME | PRESENT);
    translated = OUTPUT_SENTINEL;
    page_size = OUTPUT_SENTINEL;
    if (!walk(
            VIRTUAL_ADDRESS,
            5 * PAGE_SIZE,
            options,
            read_synthetic_entry,
            &reader,
            translated,
            &page_size) ||
        translated !=
            (PHYSICAL_FRAME | (VIRTUAL_ADDRESS & (PAGE_SIZE - 1))) ||
        page_size != PAGE_SIZE)
        return 4;

    for (std::uint8_t levels = 4; levels <= 5; ++levels) {
        options.levels = levels;
        for (std::uint8_t missing_level = 1;
             missing_level <= levels;
             ++missing_level) {
            clear_synthetic_tables(tables);
            build_synthetic_path(
                tables, VIRTUAL_ADDRESS, levels, 1,
                PHYSICAL_FRAME | PRESENT);
            tables.entries[missing_level - 1][
                page_index(VIRTUAL_ADDRESS, missing_level)] &= ~PRESENT;
            if (!expect_failed_walk(
                    VIRTUAL_ADDRESS,
                    static_cast<std::uint64_t>(levels) * PAGE_SIZE,
                    options,
                    reader))
                return 5;
        }
    }

    options.levels = 4;
    for (std::uint8_t large_level = 2; large_level <= 3; ++large_level) {
        clear_synthetic_tables(tables);
        const std::uint64_t size =
            large_level == 2 ? TWO_MIB_PAGE_SIZE : ONE_GIB_PAGE_SIZE;
        build_synthetic_path(
            tables,
            VIRTUAL_ADDRESS,
            4,
            large_level,
            (PHYSICAL_FRAME & ~(size - 1)) | LARGE_PAGE | PRESENT |
                (1ull << 13));
        if (!expect_failed_walk(
                VIRTUAL_ADDRESS, 4 * PAGE_SIZE, options, reader))
            return 6;
    }

    if (physical_bits < 52) {
        clear_synthetic_tables(tables);
        build_synthetic_path(
            tables,
            VIRTUAL_ADDRESS,
            4,
            1,
            PHYSICAL_FRAME | PRESENT | (1ull << physical_bits));
        if (!expect_failed_walk(
                VIRTUAL_ADDRESS, 4 * PAGE_SIZE, options, reader))
            return 7;
    }

    options.levels = 4;
    if (!expect_failed_walk(
            0x0000800000000000ull,
            4 * PAGE_SIZE,
            options,
            reader))
        return 8;

    options.levels = 5;
    if (!expect_failed_walk(
            0x0100000000000000ull,
            5 * PAGE_SIZE,
            options,
            reader))
        return 8;

    if (!expect_failed_walk(
            VIRTUAL_ADDRESS,
            6 * PAGE_SIZE,
            options,
            reader))
        return 9;

    return 0;
}

bool synthetic_hhdm_tests(std::uint8_t levels) {
    constexpr std::uint64_t PHYSICAL_PAGE = 0x2000;
    limine_memmap_entry entry = {
        .base = PHYSICAL_PAGE,
        .length = PAGE_SIZE,
        .type = LIMINE_MEMMAP_USABLE
    };
    limine_memmap_entry* entries[] = {&entry};
    limine_memmap_response memory_map = {
        .revision = 0,
        .entry_count = 1,
        .entries = entries
    };

    std::uint64_t result = 0xfeedfacecafebeefull;
    if (!hhdm_region_contains_page(&memory_map, PHYSICAL_PAGE) ||
        !hhdm_page_virtual_address(
            0xffff800000000000ull,
            PHYSICAL_PAGE,
            levels,
            result) ||
        result != 0xffff800000002000ull)
        return false;

    entry.type = LIMINE_MEMMAP_RESERVED;
    if (hhdm_region_contains_page(&memory_map, PHYSICAL_PAGE))
        return false;
    entry.type = LIMINE_MEMMAP_USABLE;

    entry.length = PAGE_SIZE - 1;
    if (hhdm_region_contains_page(&memory_map, PHYSICAL_PAGE))
        return false;
    entry.length = PAGE_SIZE;

    entry.base = INVALID_ADDRESS - PAGE_SIZE + 1;
    entry.length = PAGE_SIZE;
    if (hhdm_region_contains_page(&memory_map, PHYSICAL_PAGE))
        return false;
    entry.base = PHYSICAL_PAGE;

    result = 0xfeedfacecafebeefull;
    if (hhdm_page_virtual_address(
            INVALID_ADDRESS - PAGE_SIZE + 1,
            PHYSICAL_PAGE,
            levels,
            result) ||
        result != 0xfeedfacecafebeefull)
        return false;

    result = 0xfeedfacecafebeefull;
    if (hhdm_page_virtual_address(
            0x0000800000000000ull,
            PHYSICAL_PAGE,
            4,
            result) ||
        result != 0xfeedfacecafebeefull)
        return false;

    result = 0xfeedfacecafebeefull;
    if (hhdm_page_virtual_address(
            0xffff800000000000ull,
            PHYSICAL_PAGE + 1,
            levels,
            result) ||
        result != 0xfeedfacecafebeefull)
        return false;

    return true;
}

struct TestAllocator {
    std::uint64_t allocation_calls;
    std::uint64_t fail_on_call;
    std::uint64_t free_calls;
    std::uint64_t fail_on_free_call;
};

std::uint64_t allocate_test_page(void* context) {
    auto* allocator = static_cast<TestAllocator*>(context);
    if (allocator->allocation_calls == allocator->fail_on_call) {
        ++allocator->allocation_calls;
        return 0;
    }
    ++allocator->allocation_calls;
    return MilOS::Memory::allocate_page();
}

bool free_test_page(void* context, std::uint64_t physical_address) {
    auto* allocator = static_cast<TestAllocator*>(context);
    if (allocator->free_calls++ == allocator->fail_on_free_call)
        return false;
    return MilOS::Memory::free_page(physical_address);
}

std::uint8_t mapping_tests(std::uint8_t physical_bits) {
    constexpr std::uint64_t VIRTUAL_ADDRESS = 0x0000400000000000ull;
    constexpr std::uint64_t SECOND_VIRTUAL_ADDRESS =
        VIRTUAL_ADDRESS + PAGE_SIZE;
    constexpr std::uint64_t MAX_TEST_TABLES = 8;
    constexpr std::uint64_t MAX_TEST_MAPPINGS = 2;
    constexpr std::uint64_t MAX_FOREIGN_TABLES = 4;
    constexpr std::uint64_t NEVER_FAIL = INVALID_ADDRESS;

    const std::uint64_t free_before = MilOS::Memory::free_pages();
    const std::uint64_t data_page = MilOS::Memory::allocate_page();
    const std::uint64_t test_root = MilOS::Memory::allocate_page();
    if (data_page == 0 || test_root == 0) {
        if (data_page != 0)
            MilOS::Memory::free_page(data_page);
        if (test_root != 0)
            MilOS::Memory::free_page(test_root);
        return 12;
    }

    std::uint64_t owned_tables[MAX_TEST_TABLES] = {};
    std::uint64_t owned_table_count = 0;
    OwnedMapping owned_mappings[MAX_TEST_MAPPINGS] = {};
    std::uint64_t owned_mapping_count = 0;
    std::uint64_t foreign_tables[MAX_FOREIGN_TABLES] = {};
    std::uint64_t foreign_table_count = 0;
    TestAllocator allocator = {
        0,
        NEVER_FAIL,
        0,
        NEVER_FAIL
    };
    MutationContext context = {
        {
            hhdm_offset,
            level_count,
            physical_address_mask,
            memory_map
        },
        test_root,
        owned_tables,
        &owned_table_count,
        MAX_TEST_TABLES,
        owned_mappings,
        &owned_mapping_count,
        MAX_TEST_MAPPINGS,
        allocate_test_page,
        free_test_page,
        &allocator,
        false
    };

    auto cleanup = [&]() {
        bool success = true;
        for (std::uint64_t i = 0; i < owned_table_count; ++i) {
            if (!MilOS::Memory::free_page(owned_tables[i]))
                success = false;
        }
        for (std::uint64_t i = 0; i < foreign_table_count; ++i) {
            if (!MilOS::Memory::free_page(foreign_tables[i]))
                success = false;
        }
        if (!MilOS::Memory::free_page(test_root))
            success = false;
        if (!MilOS::Memory::free_page(data_page))
            success = false;
        return success;
    };

    volatile std::uint64_t* root_table;
    if (!hhdm_table_pointer(
            context.reader, test_root, root_table)) {
        cleanup();
        return 13;
    }
    for (std::uint16_t i = 0; i < 512; ++i)
        root_table[i] = 0;

    const std::uint8_t invalid_sign_bit =
        level_count == 5 ? 56 : 47;
    const std::uint64_t noncanonical_address =
        1ull << (invalid_sign_bit + 1);
    if (map_page_in_context(
            context, VIRTUAL_ADDRESS + 1, data_page, true) ||
        map_page_in_context(
            context, VIRTUAL_ADDRESS, data_page + 1, true) ||
        map_page_in_context(
            context,
            noncanonical_address,
            data_page,
            true) ||
        map_page_in_context(
            context,
            VIRTUAL_ADDRESS,
            1ull << physical_bits,
            true)) {
        cleanup();
        return 14;
    }

    const std::uint64_t root_index =
        page_index(VIRTUAL_ADDRESS, level_count);
    allocator.fail_on_call = 1;
    if (map_page_in_context(
            context, VIRTUAL_ADDRESS, data_page, true) ||
        owned_table_count != 0 ||
        owned_mapping_count != 0 ||
        root_table[root_index] != 0 ||
        MilOS::Memory::free_pages() != free_before - 2) {
        cleanup();
        return 15;
    }

    allocator.allocation_calls = 0;
    allocator.fail_on_call = NEVER_FAIL;
    if (!map_page_in_context(
            context, VIRTUAL_ADDRESS, data_page, true) ||
        !map_page_in_context(
            context, SECOND_VIRTUAL_ADDRESS, data_page, false) ||
        owned_mapping_count != 2 ||
        owned_table_count != level_count - 1)
    {
        cleanup();
        return 16;
    }

    WalkOptions options = {
        physical_address_mask,
        level_count,
        two_mib_pages_supported,
        one_gib_pages_supported,
        page_attribute_table_supported
    };
    std::uint64_t translated = INVALID_ADDRESS;
    std::uint64_t page_size = 0;
    std::uint64_t writable_leaf;
    std::uint64_t readonly_leaf;
    if (!walk(
            VIRTUAL_ADDRESS,
            test_root,
            options,
            read_hhdm_entry,
            &context.reader,
            translated,
            &page_size) ||
        translated != data_page ||
        page_size != PAGE_SIZE ||
        map_page_in_context(
            context, VIRTUAL_ADDRESS, data_page, true) ||
        owned_mapping_count != 2)
    {
        cleanup();
        return 17;
    }
    if (!read_4k_leaf_entry(
            context.reader,
            test_root,
            VIRTUAL_ADDRESS,
            writable_leaf) ||
        !read_4k_leaf_entry(
            context.reader,
            test_root,
            SECOND_VIRTUAL_ADDRESS,
            readonly_leaf) ||
        (writable_leaf & (WRITABLE | USER_ACCESS)) != WRITABLE ||
        (readonly_leaf & (WRITABLE | USER_ACCESS)) != 0)
    {
        cleanup();
        return 17;
    }

    const std::uint64_t free_before_unmap =
        MilOS::Memory::free_pages();
    if (!unmap_page_in_context(context, VIRTUAL_ADDRESS) ||
        owned_mapping_count != 1 ||
        owned_table_count != level_count - 1 ||
        MilOS::Memory::free_pages() != free_before_unmap ||
        walk(
            VIRTUAL_ADDRESS,
            test_root,
            options,
            read_hhdm_entry,
            &context.reader,
            translated,
            &page_size) ||
        !walk(
            SECOND_VIRTUAL_ADDRESS,
            test_root,
            options,
            read_hhdm_entry,
            &context.reader,
            translated,
            &page_size) ||
        translated != data_page) {
        cleanup();
        return 18;
    }

    allocator.free_calls = 0;
    allocator.fail_on_free_call = 0;
    const std::uint64_t free_before_deferred_reclaim =
        MilOS::Memory::free_pages();
    if (!unmap_page_in_context(context, SECOND_VIRTUAL_ADDRESS) ||
        owned_mapping_count != 0 ||
        owned_table_count != level_count - 1 ||
        MilOS::Memory::free_pages() != free_before_deferred_reclaim ||
        walk(
            SECOND_VIRTUAL_ADDRESS,
            test_root,
            options,
            read_hhdm_entry,
            &context.reader,
            translated,
            &page_size)) {
        cleanup();
        return 22;
    }
    allocator.free_calls = 0;
    allocator.fail_on_free_call = NEVER_FAIL;
    if (!map_page_in_context(
            context, SECOND_VIRTUAL_ADDRESS, data_page, false) ||
        !unmap_page_in_context(context, SECOND_VIRTUAL_ADDRESS) ||
        owned_table_count != 0) {
        cleanup();
        return 22;
    }

    const std::uint64_t foreign_virtual_address =
        VIRTUAL_ADDRESS + (1ull << 30);
    std::uint64_t foreign_table_address = test_root;
    for (std::uint8_t level = level_count; level > 1; --level) {
        const std::uint64_t child_table =
            MilOS::Memory::allocate_page();
        if (child_table == 0 ||
            foreign_table_count >= MAX_FOREIGN_TABLES) {
            if (child_table != 0)
                MilOS::Memory::free_page(child_table);
            cleanup();
            return 20;
        }
        foreign_tables[foreign_table_count++] = child_table;

        volatile std::uint64_t* child;
        volatile std::uint64_t* parent;
        if (!hhdm_table_pointer(
                context.reader, child_table, child) ||
            !hhdm_table_pointer(
                context.reader, foreign_table_address, parent)) {
            cleanup();
            return 20;
        }
        for (std::uint16_t i = 0; i < 512; ++i)
            child[i] = 0;
        parent[page_index(foreign_virtual_address, level)] =
            child_table | PRESENT | WRITABLE;
        foreign_table_address = child_table;
    }
    volatile std::uint64_t* foreign_leaf_table;
    if (!hhdm_table_pointer(
            context.reader, foreign_table_address, foreign_leaf_table)) {
        cleanup();
        return 20;
    }
    auto* foreign_leaf = &foreign_leaf_table[
        page_index(foreign_virtual_address, 1)];
    *foreign_leaf = data_page | PRESENT;
    const std::uint64_t free_before_foreign_unmap =
        MilOS::Memory::free_pages();
    if (unmap_page_in_context(context, foreign_virtual_address) ||
        *foreign_leaf != (data_page | PRESENT) ||
        MilOS::Memory::free_pages() != free_before_foreign_unmap) {
        cleanup();
        return 21;
    }

    if (!cleanup() ||
        MilOS::Memory::free_pages() != free_before)
        return 19;
    return 0;
}

std::uint8_t address_space_tests(std::uint8_t physical_bits) {
    constexpr std::uint64_t NEVER_FAIL = INVALID_ADDRESS;
    AddressSpace& first = test_address_spaces[0];
    AddressSpace& second = test_address_spaces[1];
    if (!address_space_is_empty(first) ||
        !address_space_is_empty(second))
        return 23;

    const std::uint64_t free_before = MilOS::Memory::free_pages();
    TestAllocator failing_allocator = {
        0,
        0,
        0,
        NEVER_FAIL
    };
    if (create_address_space_with_allocator(
            first,
            allocate_test_page,
            free_test_page,
            &failing_allocator) ||
        !address_space_is_empty(first) ||
        MilOS::Memory::free_pages() != free_before)
        return 24;

    if (!address_space_create(first) ||
        !address_space_create(second)) {
        if (AddressSpaceAccess::get(first).state ==
            ADDRESS_SPACE_STATE)
            address_space_destroy(first);
        if (AddressSpaceAccess::get(second).state ==
            ADDRESS_SPACE_STATE)
            address_space_destroy(second);
        return 25;
    }

    auto& first_state = AddressSpaceAccess::get(first);
    auto& second_state = AddressSpaceAccess::get(second);
    std::uint64_t data_page = 0;
    bool data_page_owned = false;
    auto cleanup = [&]() {
        bool success = true;
        auto release_space = [&](AddressSpace& address_space) {
            auto& state = AddressSpaceAccess::get(address_space);
            while (state.owned_mapping_count <=
                       ADDRESS_SPACE_MAX_OWNED_MAPPINGS &&
                   state.owned_mapping_count != 0) {
                const auto virtual_address =
                    state.owned_mappings[
                        state.owned_mapping_count - 1
                    ].virtual_address;
                if (!address_space_unmap_4k(
                        address_space, virtual_address)) {
                    success = false;
                    break;
                }
            }
            if (state.state == ADDRESS_SPACE_STATE &&
                !address_space_destroy(address_space))
                success = false;
        };
        release_space(first);
        release_space(second);
        if (data_page_owned) {
            if (!MilOS::Memory::free_page(data_page))
                success = false;
            data_page_owned = false;
        }
        return success;
    };
    auto fail_test = [&](std::uint8_t failure) {
        cleanup();
        return failure;
    };

    if ((first_state.root_physical_address & (PAGE_SIZE - 1)) != 0 ||
        (second_state.root_physical_address & (PAGE_SIZE - 1)) != 0 ||
        first_state.root_physical_address == second_state.root_physical_address)
        return fail_test(26);

    HhdmReaderContext reader = {
        hhdm_offset,
        level_count,
        physical_address_mask,
        memory_map
    };
    volatile std::uint64_t* active_root;
    volatile std::uint64_t* first_root;
    volatile std::uint64_t* second_root;
    if (!hhdm_table_pointer(
            reader, root_physical_address, active_root) ||
        !hhdm_table_pointer(
            reader, first_state.root_physical_address, first_root) ||
        !hhdm_table_pointer(
            reader, second_state.root_physical_address, second_root))
        return fail_test(27);

    for (std::uint16_t index = 0; index < 256; ++index) {
        if (first_root[index] != 0 || second_root[index] != 0)
            return fail_test(27);
    }
    for (std::uint16_t index = 256; index < 512; ++index) {
        if (first_root[index] != active_root[index] ||
            second_root[index] != active_root[index] ||
            first_state.shared_root_entries[index] != active_root[index] ||
            second_state.shared_root_entries[index] != active_root[index])
            return fail_test(27);
    }

    std::uint64_t hhdm_test_address;
    if (!hhdm_page_virtual_address(
            hhdm_offset,
            root_physical_address,
            level_count,
            hhdm_test_address))
        return fail_test(28);
    std::uint64_t active_translation = INVALID_ADDRESS;
    std::uint64_t space_translation = INVALID_ADDRESS;
    if (!translate(hhdm_test_address, active_translation) ||
        active_translation != root_physical_address ||
        !address_space_translate(
            first, hhdm_test_address, space_translation) ||
        space_translation != active_translation ||
        !address_space_translate(
            second, hhdm_test_address, space_translation) ||
        space_translation != active_translation)
        return fail_test(28);

    const auto kernel_symbol =
        reinterpret_cast<std::uint64_t>(&vmm_init);
    if (!translate(kernel_symbol, active_translation) ||
        !address_space_translate(
            first, kernel_symbol, space_translation) ||
        space_translation != active_translation ||
        !address_space_translate(
            second, kernel_symbol, space_translation) ||
        space_translation != active_translation)
        return fail_test(28);

    std::uint64_t candidate = 0x0000400000000000ull;
    bool candidate_found = false;
    for (std::uint16_t attempt = 0; attempt < 2048; ++attempt) {
        std::uint64_t ignored;
        if (!translate(candidate, ignored) &&
            !translate(candidate + PAGE_SIZE, ignored) &&
            !address_space_translate(first, candidate, ignored) &&
            !address_space_translate(second, candidate, ignored)) {
            candidate_found = true;
            break;
        }
        candidate += 2 * PAGE_SIZE;
    }
    if (!candidate_found)
        return fail_test(29);

    data_page = MilOS::Memory::allocate_page();
    if (data_page == 0)
        return fail_test(30);
    data_page_owned = true;

    const std::uint64_t free_before_mappings =
        MilOS::Memory::free_pages();
    const std::uint64_t table_count =
        static_cast<std::uint64_t>(level_count - 1);

    for (std::uint64_t fail_at = 0;
         fail_at < table_count;
         ++fail_at) {
        TestAllocator allocator = {
            0,
            fail_at,
            0,
            NEVER_FAIL
        };
        auto context = make_address_space_context(
            first,
            allocate_test_page,
            free_test_page,
            &allocator,
            false);
        if (map_page_in_context(
                context, candidate, data_page, true) ||
            first_state.owned_table_count != 0 ||
            first_state.owned_mapping_count != 0 ||
            first_root[page_index(candidate, level_count)] != 0 ||
            MilOS::Memory::free_pages() != free_before_mappings)
            return fail_test(31);
    }

    if (!address_space_map_4k(
            first, candidate, data_page, true) ||
        !address_space_map_4k(
            first, candidate + PAGE_SIZE, data_page, false))
        return fail_test(32);

    std::uint64_t translated = INVALID_ADDRESS;
    std::uint64_t page_size = 0;
    std::uint64_t writable_leaf;
    std::uint64_t readonly_leaf;
    if (!address_space_translate(
            first, candidate, translated, &page_size) ||
        translated != data_page ||
        page_size != PAGE_SIZE ||
        address_space_translate(second, candidate, translated) ||
        translate(candidate, translated) ||
        !read_4k_leaf_entry(
            reader,
            first_state.root_physical_address,
            candidate,
            writable_leaf) ||
        !read_4k_leaf_entry(
            reader,
            first_state.root_physical_address,
            candidate + PAGE_SIZE,
            readonly_leaf) ||
        (writable_leaf & (WRITABLE | USER_ACCESS)) != WRITABLE ||
        (readonly_leaf & (WRITABLE | USER_ACCESS)) != 0)
        return fail_test(33);

    const auto free_before_busy_destroy =
        MilOS::Memory::free_pages();
    if (address_space_destroy(first) ||
        MilOS::Memory::free_pages() != free_before_busy_destroy ||
        first_state.owned_mapping_count != 2)
        return fail_test(33);

    const auto map_count_before_capacity_failure =
        first_state.owned_mapping_count;
    first_state.owned_mapping_count =
        ADDRESS_SPACE_MAX_OWNED_MAPPINGS;
    if (address_space_map_4k(
            first, candidate + 2 * PAGE_SIZE, data_page, true)) {
        first_state.owned_mapping_count = map_count_before_capacity_failure;
        return fail_test(34);
    }
    first_state.owned_mapping_count = map_count_before_capacity_failure;

    if (address_space_map_4k(
            first, candidate + 1, data_page, true) ||
        address_space_map_4k(
            first, candidate + 2 * PAGE_SIZE, 1ull << physical_bits, true) ||
        address_space_map_4k(
            first,
            1ull << ((level_count == 5 ? 56 : 47) + 1),
            data_page,
            true) ||
        address_space_map_4k(
            first, 0xffff800000000000ull, data_page, true) ||
        address_space_unmap_4k(second, candidate) ||
        address_space_unmap_4k(first, candidate + 1))
        return fail_test(35);

    const std::uint64_t free_before_unmapping =
        MilOS::Memory::free_pages();
    if (!address_space_unmap_4k(first, candidate) ||
        address_space_translate(first, candidate, translated) ||
        !address_space_translate(
            first, candidate + PAGE_SIZE, translated) ||
        translated != data_page ||
        MilOS::Memory::free_pages() != free_before_unmapping)
        return fail_test(36);

    if (!address_space_unmap_4k(first, candidate + PAGE_SIZE) ||
        MilOS::Memory::free_pages() != free_before_unmapping + table_count ||
        address_space_unmap_4k(first, candidate + PAGE_SIZE))
        return fail_test(37);

    const auto before_bad_destroy = MilOS::Memory::free_pages();
    first_state.owned_mapping_count =
        ADDRESS_SPACE_MAX_OWNED_MAPPINGS + 1;
    if (address_space_destroy(first) ||
        MilOS::Memory::free_pages() != before_bad_destroy)
        return fail_test(38);
    first_state.owned_mapping_count = 0;

    const auto saved_root = first_state.root_physical_address;
    first_state.root_physical_address = root_physical_address;
    if (address_space_destroy(first)) {
        first_state.root_physical_address = saved_root;
        return fail_test(39);
    }
    first_state.root_physical_address = saved_root;

    const auto invalid_root = INVALID_ADDRESS & ~(PAGE_SIZE - 1);
    first_state.root_physical_address = invalid_root;
    translated = 0xfeedfacecafebeefull;
    page_size = 0xfeedfacecafebeefull;
    if (address_space_translate(
            first, hhdm_test_address, translated, &page_size) ||
        translated != 0xfeedfacecafebeefull ||
        page_size != 0xfeedfacecafebeefull ||
        address_space_destroy(first)) {
        first_state.root_physical_address = saved_root;
        return fail_test(40);
    }
    first_state.root_physical_address = saved_root;

    const auto free_before_destroy = MilOS::Memory::free_pages();
    if (!address_space_destroy(first) ||
        !address_space_destroy(second) ||
        MilOS::Memory::free_pages() != free_before_destroy + 2 ||
        address_space_destroy(first) ||
        !translate(hhdm_test_address, active_translation) ||
        active_translation != root_physical_address ||
        MilOS::Memory::free_pages() != free_before - 1)
        return fail_test(41);
    data_page_owned = false;
    if (!MilOS::Memory::free_page(data_page) ||
        MilOS::Memory::free_page(data_page) ||
        MilOS::Memory::free_pages() != free_before)
        return 42;
    return 0;
}

}

bool vmm_init(
    std::uint64_t requested_hhdm_offset,
    limine_memmap_response* requested_memory_map
) {
#if defined(__x86_64__)
    if (owned_page_table_count != 0 || owned_mapping_count != 0)
        return false;

    hhdm_offset = 0;
    physical_address_mask = 0;
    memory_map = nullptr;
    root_physical_address = 0;
    active_cr3_value = 0;
    level_count = 0;
    two_mib_pages_supported = false;
    one_gib_pages_supported = false;
    page_attribute_table_supported = false;

    if (requested_memory_map == nullptr ||
        requested_memory_map->entries == nullptr ||
        requested_memory_map->entry_count == 0)
        return false;

    std::uint8_t physical_bits;
    if (!physical_address_bits(physical_bits))
        return false;

    std::uint64_t cr3;
    std::uint64_t cr4;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %%cr4, %0" : "=r"(cr4));

    const std::uint8_t requested_level_count =
        (cr4 & (1ull << 12)) != 0 ? 5 : 4;
    physical_address_mask =
        ((1ull << physical_bits) - 1) & MAX_ADDRESS_MASK;
    if ((cr3 & MAX_ADDRESS_MASK & ~physical_address_mask) != 0)
        return false;

    hhdm_offset = requested_hhdm_offset;
    memory_map = requested_memory_map;
    level_count = requested_level_count;

    const std::uint64_t requested_root =
        cr3 & physical_address_mask;
    active_cr3_value = requested_root;
    HhdmReaderContext reader = {
        hhdm_offset,
        level_count,
        physical_address_mask,
        memory_map
    };
    std::uint64_t root_entry;
    if (!read_hhdm_entry(&reader, requested_root, 0, root_entry)) {
        hhdm_offset = 0;
        physical_address_mask = 0;
        memory_map = nullptr;
        level_count = 0;
        return false;
    }

    root_physical_address = requested_root;
    const WalkOptions options = {
        physical_address_mask,
        level_count,
        supports_two_mib_pages(),
        supports_one_gib_pages(),
        supports_page_attribute_table()
    };
    two_mib_pages_supported = options.two_mib_pages_supported;
    one_gib_pages_supported = options.one_gib_pages_supported;
    page_attribute_table_supported = options.page_attribute_table_supported;

    (void)root_entry;
    return true;
#else
    (void)requested_hhdm_offset;
    (void)requested_memory_map;
    return false;
#endif
}

bool address_space_create(AddressSpace& address_space) {
#if defined(__x86_64__)
    if (level_count == 0 || !address_space_is_empty(address_space))
        return false;
    return create_address_space_with_allocator(
        address_space,
        allocate_pmm_page,
        free_pmm_page,
        nullptr);
#else
    (void)address_space;
    return false;
#endif
}

bool address_space_destroy(AddressSpace& address_space) {
#if defined(__x86_64__)
    if (!address_space_is_valid(address_space))
        return false;

    const auto& state = AddressSpaceAccess::get(address_space);
    std::uint64_t current_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if ((current_cr3 & physical_address_mask) ==
            state.root_physical_address ||
        state.root_physical_address == active_cr3_value ||
        state.owned_table_count != 0 ||
        state.owned_mapping_count != 0)
        return false;

    HhdmReaderContext reader = {
        hhdm_offset,
        state.levels,
        physical_address_mask,
        memory_map
    };
    volatile std::uint64_t* root;
    if (!hhdm_table_pointer(
            reader, state.root_physical_address, root) ||
        !address_space_root_entries_are_consistent(
            address_space, root))
        return false;

    if (!MilOS::Memory::free_page(state.root_physical_address))
        return false;
    clear_address_space(address_space);
    return true;
#else
    (void)address_space;
    return false;
#endif
}

bool address_space_translate(
    const AddressSpace& address_space,
    std::uint64_t virtual_address,
    std::uint64_t& physical_address,
    std::uint64_t* page_size
) {
#if defined(__x86_64__)
    if (!address_space_is_valid(address_space))
        return false;

    const auto& state = AddressSpaceAccess::get(address_space);
    HhdmReaderContext reader = {
        hhdm_offset,
        state.levels,
        physical_address_mask,
        memory_map
    };
    const WalkOptions options = {
        physical_address_mask,
        state.levels,
        two_mib_pages_supported,
        one_gib_pages_supported,
        page_attribute_table_supported
    };
    return walk(
        virtual_address,
        state.root_physical_address,
        options,
        read_hhdm_entry,
        &reader,
        physical_address,
        page_size);
#else
    (void)address_space;
    (void)virtual_address;
    (void)physical_address;
    (void)page_size;
    return false;
#endif
}

bool address_space_map_4k(
    AddressSpace& address_space,
    std::uint64_t virtual_address,
    std::uint64_t physical_address,
    bool writable
) {
#if defined(__x86_64__)
    if (!address_space_is_valid(address_space) ||
        !address_space_private_mapping(address_space, virtual_address))
        return false;

    const auto& state = AddressSpaceAccess::get(address_space);
    std::uint64_t current_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if ((current_cr3 & physical_address_mask) ==
        state.root_physical_address)
        return false;

    auto context = make_address_space_context(
        address_space,
        allocate_pmm_page,
        free_pmm_page,
        nullptr,
        false);
    return map_page_in_context(
        context, virtual_address, physical_address, writable);
#else
    (void)address_space;
    (void)virtual_address;
    (void)physical_address;
    (void)writable;
    return false;
#endif
}

bool address_space_unmap_4k(
    AddressSpace& address_space,
    std::uint64_t virtual_address
) {
#if defined(__x86_64__)
    if (!address_space_is_valid(address_space) ||
        !address_space_private_mapping(address_space, virtual_address))
        return false;

    const auto& state = AddressSpaceAccess::get(address_space);
    std::uint64_t current_cr3;
    asm volatile("mov %%cr3, %0" : "=r"(current_cr3));
    if ((current_cr3 & physical_address_mask) ==
        state.root_physical_address)
        return false;

    auto context = make_address_space_context(
        address_space,
        allocate_pmm_page,
        free_pmm_page,
        nullptr,
        false);
    return unmap_page_in_context(context, virtual_address);
#else
    (void)address_space;
    (void)virtual_address;
    return false;
#endif
}

bool translate(
    std::uint64_t virtual_address,
    std::uint64_t& physical_address,
    std::uint64_t* page_size
) {
#if defined(__x86_64__)
    if (level_count == 0)
        return false;

    HhdmReaderContext reader = {
        hhdm_offset,
        level_count,
        physical_address_mask,
        memory_map
    };
    const WalkOptions options = {
        physical_address_mask,
        level_count,
        two_mib_pages_supported,
        one_gib_pages_supported,
        page_attribute_table_supported
    };
    return walk(
        virtual_address,
        root_physical_address,
        options,
        read_hhdm_entry,
        &reader,
        physical_address,
        page_size);
#else
    (void)virtual_address;
    (void)physical_address;
    (void)page_size;
    return false;
#endif
}

bool map_4k(
    std::uint64_t virtual_address,
    std::uint64_t physical_address,
    bool writable
) {
#if defined(__x86_64__)
    if (level_count == 0)
        return false;

    MutationContext context = {
        {
            hhdm_offset,
            level_count,
            physical_address_mask,
            memory_map
        },
        root_physical_address,
        owned_page_tables,
        &owned_page_table_count,
        ADDRESS_SPACE_MAX_OWNED_PAGE_TABLES,
        owned_mappings,
        &owned_mapping_count,
        ADDRESS_SPACE_MAX_OWNED_MAPPINGS,
        allocate_pmm_page,
        free_pmm_page,
        nullptr,
        true
    };
    return map_page_in_context(
        context, virtual_address, physical_address, writable);
#else
    (void)virtual_address;
    (void)physical_address;
    (void)writable;
    return false;
#endif
}

bool unmap_4k(std::uint64_t virtual_address) {
#if defined(__x86_64__)
    if (level_count == 0)
        return false;

    MutationContext context = {
        {
            hhdm_offset,
            level_count,
            physical_address_mask,
            memory_map
        },
        root_physical_address,
        owned_page_tables,
        &owned_page_table_count,
        ADDRESS_SPACE_MAX_OWNED_PAGE_TABLES,
        owned_mappings,
        &owned_mapping_count,
        ADDRESS_SPACE_MAX_OWNED_MAPPINGS,
        allocate_pmm_page,
        free_pmm_page,
        nullptr,
        true
    };
    return unmap_page_in_context(context, virtual_address);
#else
    (void)virtual_address;
    return false;
#endif
}

std::uint8_t vmm_run_synthetic_tests() {
#if defined(__x86_64__)
    std::uint8_t physical_bits;
    if (!physical_address_bits(physical_bits))
        return 10;

    const WalkOptions options = {
        ((1ull << physical_bits) - 1) & MAX_ADDRESS_MASK,
        4,
        supports_two_mib_pages(),
        supports_one_gib_pages(),
        supports_page_attribute_table()
    };
    const std::uint8_t walk_failure =
        synthetic_tests(physical_bits, options);
    if (walk_failure != 0)
        return walk_failure;
    if (!synthetic_hhdm_tests(options.levels))
        return 11;
    const std::uint8_t mapping_failure =
        mapping_tests(physical_bits);
    if (mapping_failure != 0)
        return mapping_failure;
    return address_space_tests(physical_bits);
#else
    return 10;
#endif
}

std::uint8_t paging_levels() {
    return level_count;
}

std::uint64_t active_root_physical_address() {
    return root_physical_address;
}

}
