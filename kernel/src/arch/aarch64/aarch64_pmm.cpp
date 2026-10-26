#include <cstddef>
#include <cstdint>
#include <limine.h>
#include "aarch64_pmm.hpp"

namespace MilOS::AArch64::Memory {
namespace {

constexpr std::uint64_t INVALID_VALUE = ~std::uint64_t{0};
constexpr std::uint64_t PAGE_MASK = PAGE_SIZE - 1;
constexpr std::uint64_t BITS_PER_BYTE = 8;
constexpr std::uint64_t MAX_MEMORY_MAP_ENTRIES = 128;
constexpr std::uint64_t MAX_PHYSICAL_ADDRESS_BITS = 48;

struct Region {
    std::uint64_t base;
    std::uint64_t page_count;
    std::uint64_t bitmap_offset;
};

struct Engine {
    Region regions[MAX_MEMORY_MAP_ENTRIES];
    std::uint64_t region_count;
    std::uint64_t total_pages;
    std::uint64_t free_pages;
    std::uint64_t metadata_pages;
    std::uint64_t metadata_base;
    std::uint64_t metadata_size;
    std::uint64_t hhdm_offset;
    std::uint64_t physical_address_bits;
    std::uint64_t virtual_address_bits;
    std::uint64_t allocation_cursor;
    std::uint8_t* free_bitmap;
    std::uint8_t* allocated_bitmap;
    bool initialized;
};

struct MapEntry {
    std::uint64_t base;
    std::uint64_t end;
    std::uint64_t type;
};

struct SyntheticBacking {
    alignas(PAGE_SIZE) std::uint8_t bytes[7 * PAGE_SIZE];
    bool fail;
};

using MapPhysicalRange = void* (*)(
    void* context,
    std::uint64_t physical_address,
    std::uint64_t size
);

Engine active_engine{};
SyntheticBacking test_backing{};

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

bool multiply_without_overflow(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result
) {
    if (right != 0 && left > INVALID_VALUE / right)
        return false;
    result = left * right;
    return true;
}

bool align_up_page(std::uint64_t value, std::uint64_t& result) {
    if (value > INVALID_VALUE - PAGE_MASK)
        return false;
    result = (value + PAGE_MASK) & ~PAGE_MASK;
    return true;
}

bool is_canonical_upper(
    std::uint64_t address,
    std::uint64_t address_bits
) {
    if (address_bits == 0 || address_bits >= 64)
        return false;
    const std::uint64_t low_mask =
        (std::uint64_t{1} << address_bits) - 1;
    return address >= INVALID_VALUE - low_mask;
}

bool hhdm_range_is_valid(
    std::uint64_t hhdm_offset,
    std::uint64_t physical_address,
    std::uint64_t size,
    std::uint64_t virtual_address_bits,
    std::uint64_t& virtual_address
) {
    if (size == 0 ||
        (hhdm_offset & PAGE_MASK) != 0 ||
        !add_without_overflow(
            physical_address,
            size - 1,
            virtual_address) ||
        physical_address > INVALID_VALUE - hhdm_offset)
        return false;

    virtual_address = hhdm_offset + physical_address;
    std::uint64_t virtual_end;
    if (!add_without_overflow(
            virtual_address,
            size - 1,
            virtual_end))
        return false;
    return is_canonical_upper(virtual_address, virtual_address_bits) &&
        is_canonical_upper(virtual_end, virtual_address_bits);
}

bool supported_regime(const Paging::Regime& regime) {
    if (regime.levels != 4 ||
        regime.virtual_address_bits != 48 ||
        regime.physical_address_bits < 32 ||
        regime.physical_address_bits > MAX_PHYSICAL_ADDRESS_BITS)
        return false;

    const std::uint64_t expected_mask =
        ((std::uint64_t{1} << regime.physical_address_bits) - 1) &
        ~PAGE_MASK;
    return regime.physical_address_mask == expected_mask;
}

bool known_memory_type(std::uint64_t type) {
    return type <= LIMINE_MEMMAP_RESERVED_MAPPED;
}

bool is_usable_type(std::uint64_t type) {
    return type == LIMINE_MEMMAP_USABLE;
}

bool is_page_aligned_type(std::uint64_t type) {
    return type == LIMINE_MEMMAP_USABLE ||
        type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE;
}

bool collect_map(
    const limine_memmap_response* memory_map,
    std::uint64_t physical_address_bits,
    Engine& candidate,
    MapEntry (&entries)[MAX_MEMORY_MAP_ENTRIES]
) {
    if (memory_map == nullptr ||
        (reinterpret_cast<std::uint64_t>(memory_map) &
         (alignof(limine_memmap_response) - 1)) != 0 ||
        memory_map->entries == nullptr ||
        memory_map->entry_count == 0 ||
        memory_map->entry_count > MAX_MEMORY_MAP_ENTRIES)
        return false;

    const std::uint64_t entries_address =
        reinterpret_cast<std::uint64_t>(memory_map->entries);
    std::uint64_t entries_bytes;
    if ((entries_address & (alignof(limine_memmap_entry*) - 1)) != 0 ||
        !multiply_without_overflow(
            memory_map->entry_count,
            sizeof(limine_memmap_entry*),
            entries_bytes) ||
        entries_address > INVALID_VALUE - entries_bytes)
        return false;

    const std::uint64_t physical_limit =
        std::uint64_t{1} << physical_address_bits;
    std::uint64_t previous_base = 0;

    for (std::uint64_t i = 0; i < memory_map->entry_count; ++i) {
        const limine_memmap_entry* entry = memory_map->entries[i];
        if (entry == nullptr)
            return false;
        const std::uint64_t entry_address =
            reinterpret_cast<std::uint64_t>(entry);
        if ((entry_address & (alignof(limine_memmap_entry) - 1)) != 0)
            return false;

        std::uint64_t end;
        if (!known_memory_type(entry->type) ||
            entry->length == 0 ||
            !add_without_overflow(entry->base, entry->length, end) ||
            end > physical_limit ||
            (i != 0 && entry->base < previous_base) ||
            (is_page_aligned_type(entry->type) &&
             ((entry->base & PAGE_MASK) != 0 ||
              (entry->length & PAGE_MASK) != 0)))
            return false;

        entries[i] = {entry->base, end, entry->type};
        previous_base = entry->base;
    }

    for (std::uint64_t i = 0; i < memory_map->entry_count; ++i) {
        for (std::uint64_t j = i + 1;
             j < memory_map->entry_count;
             ++j) {
            const bool overlaps =
                entries[i].base < entries[j].end &&
                entries[j].base < entries[i].end;
            if (overlaps &&
                (is_usable_type(entries[i].type) ||
                 is_usable_type(entries[j].type)))
                return false;
        }
    }

    candidate.region_count = 0;
    candidate.total_pages = 0;
    for (std::uint64_t i = 0; i < memory_map->entry_count; ++i) {
        if (!is_usable_type(entries[i].type))
            continue;
        if (candidate.region_count >= MAX_MEMORY_MAP_ENTRIES)
            return false;
        const std::uint64_t page_count =
            (entries[i].end - entries[i].base) / PAGE_SIZE;
        if (page_count == 0 ||
            candidate.total_pages > INVALID_VALUE - page_count)
            return false;
        candidate.regions[candidate.region_count++] = {
            entries[i].base,
            page_count,
            candidate.total_pages
        };
        candidate.total_pages += page_count;
    }
    return true;
}

bool physical_range_in_usable_region(
    const Engine& engine,
    std::uint64_t physical_address,
    std::uint64_t size
) {
    if (size == 0 ||
        physical_address > INVALID_VALUE - size)
        return false;
    const std::uint64_t end = physical_address + size;
    for (std::uint64_t i = 0; i < engine.region_count; ++i) {
        const Region& region = engine.regions[i];
        const std::uint64_t region_end =
            region.base + region.page_count * PAGE_SIZE;
        if (physical_address >= region.base && end <= region_end)
            return true;
    }
    return false;
}

void* hhdm_mapper(
    void* context,
    std::uint64_t physical_address,
    std::uint64_t size
) {
    const auto& engine = *static_cast<const Engine*>(context);
    std::uint64_t virtual_address;
    if (!physical_range_in_usable_region(
            engine,
            physical_address,
            size) ||
        !hhdm_range_is_valid(
            engine.hhdm_offset,
            physical_address,
            size,
            engine.virtual_address_bits,
            virtual_address))
        return nullptr;
    return reinterpret_cast<void*>(virtual_address);
}

void set_bit(std::uint8_t* bitmap, std::uint64_t index) {
    bitmap[index / BITS_PER_BYTE] |=
        static_cast<std::uint8_t>(1u << (index % BITS_PER_BYTE));
}

void clear_bit(std::uint8_t* bitmap, std::uint64_t index) {
    bitmap[index / BITS_PER_BYTE] &=
        static_cast<std::uint8_t>(~(1u << (index % BITS_PER_BYTE)));
}

bool bit_is_set(const std::uint8_t* bitmap, std::uint64_t index) {
    return (bitmap[index / BITS_PER_BYTE] &
            (1u << (index % BITS_PER_BYTE))) != 0;
}

InitializationStatus initialize_state(
    Engine& target,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    const Paging::Regime& regime,
    MapPhysicalRange mapper,
    void* mapper_context
) {
    if (target.initialized)
        return InitializationStatus::already_initialized;
    if (!supported_regime(regime))
        return InitializationStatus::unsupported_regime;
    if (mapper == nullptr)
        return InitializationStatus::metadata_unavailable;

    Engine candidate{};
    candidate.hhdm_offset = hhdm_offset;
    candidate.physical_address_bits = regime.physical_address_bits;
    candidate.virtual_address_bits = regime.virtual_address_bits;
    MapEntry map_entries[MAX_MEMORY_MAP_ENTRIES]{};
    if (memory_map == nullptr ||
        memory_map->entries == nullptr ||
        memory_map->entry_count == 0)
        return InitializationStatus::invalid_memory_map;
    if (memory_map->entry_count > MAX_MEMORY_MAP_ENTRIES)
        return InitializationStatus::too_many_memory_map_entries;
    if (!collect_map(
            memory_map,
            regime.physical_address_bits,
            candidate,
            map_entries))
        return InitializationStatus::invalid_memory_map;
    if (candidate.region_count == 0 || candidate.total_pages == 0)
        return InitializationStatus::no_usable_memory;
    for (std::uint64_t i = 0; i < candidate.region_count; ++i) {
        const Region& region = candidate.regions[i];
        std::uint64_t virtual_address;
        if (!hhdm_range_is_valid(
                hhdm_offset,
                region.base,
                region.page_count * PAGE_SIZE,
                regime.virtual_address_bits,
                virtual_address))
            return InitializationStatus::metadata_unavailable;
    }

    std::uint64_t bitmap_bytes;
    if (candidate.total_pages > INVALID_VALUE - (BITS_PER_BYTE - 1))
        return InitializationStatus::metadata_too_large;
    bitmap_bytes =
        (candidate.total_pages + BITS_PER_BYTE - 1) / BITS_PER_BYTE;
    std::uint64_t both_bitmaps_size;
    std::uint64_t storage_size;
    if (!multiply_without_overflow(
            bitmap_bytes,
            2,
            both_bitmaps_size) ||
        !align_up_page(both_bitmaps_size, storage_size))
        return InitializationStatus::metadata_too_large;
    candidate.metadata_pages = storage_size / PAGE_SIZE;

    std::uint64_t metadata_region = INVALID_VALUE;
    for (std::uint64_t i = 0; i < candidate.region_count; ++i) {
        if (candidate.regions[i].page_count >= candidate.metadata_pages) {
            metadata_region = i;
            break;
        }
    }
    if (metadata_region == INVALID_VALUE)
        return InitializationStatus::metadata_unavailable;

    candidate.metadata_base = candidate.regions[metadata_region].base;
    candidate.metadata_size = storage_size;
    candidate.free_pages =
        candidate.total_pages - candidate.metadata_pages;
    candidate.allocation_cursor = 0;

    void* mapping_context =
        mapper == hhdm_mapper ? &candidate : mapper_context;
    void* metadata = mapper(
        mapping_context,
        candidate.metadata_base,
        candidate.metadata_size
    );
    if (metadata == nullptr ||
        (reinterpret_cast<std::uint64_t>(metadata) & PAGE_MASK) != 0)
        return InitializationStatus::metadata_unavailable;

    auto* free_bitmap = static_cast<std::uint8_t*>(metadata);
    auto* allocated_bitmap = free_bitmap + bitmap_bytes;
    for (std::uint64_t i = 0; i < bitmap_bytes; ++i) {
        free_bitmap[i] = 0;
        allocated_bitmap[i] = 0;
    }
    for (std::uint64_t i = 0; i < candidate.total_pages; ++i)
        set_bit(free_bitmap, i);

    const Region& metadata_region_info =
        candidate.regions[metadata_region];
    const std::uint64_t metadata_first_page =
        metadata_region_info.bitmap_offset;
    for (std::uint64_t i = 0; i < candidate.metadata_pages; ++i)
        clear_bit(free_bitmap, metadata_first_page + i);
    for (std::uint64_t i = 0; i < candidate.region_count; ++i) {
        if (candidate.regions[i].base == 0) {
            const std::uint64_t zero_page_index =
                candidate.regions[i].bitmap_offset;
            if (bit_is_set(free_bitmap, zero_page_index)) {
                clear_bit(free_bitmap, zero_page_index);
                --candidate.free_pages;
            }
            break;
        }
    }

    candidate.free_bitmap = free_bitmap;
    candidate.allocated_bitmap = allocated_bitmap;
    candidate.initialized = true;
    target = candidate;
    return InitializationStatus::success;
}

bool locate_page(
    const Engine& engine,
    std::uint64_t physical_address,
    std::uint64_t& bitmap_index
) {
    if ((physical_address & PAGE_MASK) != 0)
        return false;
    for (std::uint64_t i = 0; i < engine.region_count; ++i) {
        const Region& region = engine.regions[i];
        const std::uint64_t region_size =
            region.page_count * PAGE_SIZE;
        if (physical_address < region.base ||
            physical_address - region.base >= region_size)
            continue;
        bitmap_index =
            region.bitmap_offset +
            (physical_address - region.base) / PAGE_SIZE;
        return true;
    }
    return false;
}

bool release_page_from(
    Engine& engine,
    std::uint64_t physical_address
) {
    if (!engine.initialized)
        return false;
    std::uint64_t index;
    if (!locate_page(engine, physical_address, index) ||
        !bit_is_set(engine.allocated_bitmap, index))
        return false;
    clear_bit(engine.allocated_bitmap, index);
    set_bit(engine.free_bitmap, index);
    ++engine.free_pages;
    return true;
}

std::uint64_t physical_address_for_index(
    const Engine& engine,
    std::uint64_t bitmap_index
);

std::uint64_t allocate_page_from(Engine& engine) {
    if (!engine.initialized || engine.free_pages == 0)
        return 0;

    for (std::uint64_t scanned = 0;
         scanned < engine.total_pages;
         ++scanned) {
        const std::uint64_t index =
            (engine.allocation_cursor + scanned) % engine.total_pages;
        if (!bit_is_set(engine.free_bitmap, index))
            continue;
        const std::uint64_t physical =
            physical_address_for_index(engine, index);
        if (physical == 0)
            continue;
        clear_bit(engine.free_bitmap, index);
        set_bit(engine.allocated_bitmap, index);
        --engine.free_pages;
        engine.allocation_cursor = (index + 1) % engine.total_pages;
        return physical;
    }
    return 0;
}

bool page_pointer_from(
    Engine& engine,
    std::uint64_t physical_address,
    MapPhysicalRange mapper,
    void* mapper_context,
    void*& pointer
) {
    pointer = nullptr;
    if (!engine.initialized || mapper == nullptr)
        return false;
    std::uint64_t index;
    if (!locate_page(engine, physical_address, index) ||
        !bit_is_set(engine.allocated_bitmap, index))
        return false;
    void* mapping_context =
        mapper == hhdm_mapper ? &engine : mapper_context;
    pointer = mapper(mapping_context, physical_address, PAGE_SIZE);
    return pointer != nullptr;
}

std::uint64_t physical_address_for_index(
    const Engine& engine,
    std::uint64_t bitmap_index
) {
    for (std::uint64_t i = 0; i < engine.region_count; ++i) {
        const Region& region = engine.regions[i];
        if (bitmap_index >= region.bitmap_offset &&
            bitmap_index - region.bitmap_offset < region.page_count)
            return region.base +
                (bitmap_index - region.bitmap_offset) * PAGE_SIZE;
    }
    return 0;
}

bool is_managed_usable_range(
    const Engine& engine,
    std::uint64_t physical_address,
    std::uint64_t size
) {
    return physical_range_in_usable_region(
        engine,
        physical_address,
        size
    );
}

void* synthetic_mapper(
    void* opaque_context,
    std::uint64_t physical_address,
    std::uint64_t size
) {
    auto& backing = *static_cast<SyntheticBacking*>(opaque_context);
    if (backing.fail || size == 0 ||
        physical_address > INVALID_VALUE - size)
        return nullptr;
    const std::uint64_t end = physical_address + size;
    if (physical_address >= 0x100000 && end <= 0x104000)
        return backing.bytes + (physical_address - 0x100000);
    if (physical_address >= 0x106000 && end <= 0x109000)
        return backing.bytes + 4 * PAGE_SIZE +
            (physical_address - 0x106000);
    return nullptr;
}

void synthetic_map(
    limine_memmap_entry* (&entries)[4],
    limine_memmap_response& response
) {
    static limine_memmap_entry usable_first = {
        0x100000, 4 * PAGE_SIZE, LIMINE_MEMMAP_USABLE
    };
    static limine_memmap_entry reserved = {
        0x104000, 2 * PAGE_SIZE, LIMINE_MEMMAP_RESERVED
    };
    static limine_memmap_entry usable_second = {
        0x106000, 3 * PAGE_SIZE, LIMINE_MEMMAP_USABLE
    };
    static limine_memmap_entry bootloader = {
        0x109000, 2 * PAGE_SIZE, LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
    };
    entries[0] = &usable_first;
    entries[1] = &reserved;
    entries[2] = &usable_second;
    entries[3] = &bootloader;
    response.revision = 0;
    response.entry_count = 4;
    response.entries = entries;
}

bool expect(bool condition, std::uint32_t code, std::uint32_t& failure) {
    if (!condition) {
        failure = code;
        return false;
    }
    return true;
}

}

InitializationStatus initialize(
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    const Paging::Regime& regime
) {
    return initialize_state(
        active_engine,
        hhdm_offset,
        memory_map,
        regime,
        hhdm_mapper,
        &active_engine
    );
}

bool is_initialized() {
    return active_engine.initialized;
}

std::uint64_t allocate_page() {
    return allocate_page_from(active_engine);
}

bool release_page(std::uint64_t physical_address) {
    return release_page_from(active_engine, physical_address);
}

bool writable_pointer_for_allocated_page(
    std::uint64_t physical_address,
    void*& pointer
) {
    return page_pointer_from(
        active_engine,
        physical_address,
        hhdm_mapper,
        &active_engine,
        pointer
    );
}

Statistics statistics() {
    return {
        active_engine.total_pages,
        active_engine.free_pages,
        active_engine.metadata_pages,
        active_engine.region_count,
        active_engine.metadata_base,
        active_engine.metadata_size
    };
}

const char* initialization_status_name(InitializationStatus status) {
    switch (status) {
        case InitializationStatus::success: return "success";
        case InitializationStatus::already_initialized:
            return "already initialized";
        case InitializationStatus::unsupported_regime:
            return "unsupported translation regime";
        case InitializationStatus::invalid_memory_map:
            return "invalid memory map";
        case InitializationStatus::too_many_memory_map_entries:
            return "memory map entry limit exceeded";
        case InitializationStatus::no_usable_memory:
            return "no usable memory";
        case InitializationStatus::metadata_too_large:
            return "allocator metadata size overflow";
        case InitializationStatus::metadata_unavailable:
            return "usable memory or HHDM metadata range unavailable";
    }
    return "unknown initialization status";
}

std::uint32_t run_synthetic_tests() {
    const Paging::Regime regime{
        ((std::uint64_t{1} << 44) - 1) & ~PAGE_MASK,
        44,
        48,
        4
    };
    std::uint32_t failure = 0;
    limine_memmap_entry* entries[4]{};
    limine_memmap_response response{};
    synthetic_map(entries, response);
    SyntheticBacking& backing = test_backing;
    backing.fail = false;
    Engine engine{};

    if (!expect(
            initialize_state(
                engine,
                0xffff000000000000ull,
                &response,
                regime,
                synthetic_mapper,
                &backing) == InitializationStatus::success,
            1,
            failure))
        return failure;

    const Statistics initial{
        engine.total_pages,
        engine.free_pages,
        engine.metadata_pages,
        engine.region_count,
        engine.metadata_base,
        engine.metadata_size
    };
    if (!expect(
            initial.total_pages == 7 &&
            initial.metadata_pages == 1 &&
            initial.metadata_base == 0x100000 &&
            initial.metadata_size == PAGE_SIZE &&
            initial.free_pages == 6 &&
            initial.usable_region_count == 2,
            2,
            failure))
        return failure;
    if (!expect(
            initialize_state(
                engine,
                0xffff000000000000ull,
                &response,
                regime,
                synthetic_mapper,
                &backing) ==
                InitializationStatus::already_initialized,
            3,
            failure))
        return failure;

    std::uint64_t allocated[6]{};
    for (std::uint64_t i = 0; i < 6; ++i) {
        allocated[i] = allocate_page_from(engine);
        if (allocated[i] == 0 ||
            (allocated[i] & PAGE_MASK) != 0 ||
            allocated[i] == engine.metadata_base ||
            !is_managed_usable_range(engine, allocated[i], PAGE_SIZE) ||
            (allocated[i] >= 0x104000 && allocated[i] < 0x106000) ||
            (allocated[i] >= 0x109000 && allocated[i] < 0x10b000))
            return 6;
        for (std::uint64_t j = 0; j < i; ++j) {
            if (allocated[j] == allocated[i])
                return 4;
        }
    }
    if (!expect(
            engine.free_pages == 0 &&
            allocate_page_from(engine) == 0 &&
            !bit_is_set(engine.free_bitmap, 0),
            7,
            failure))
        return failure;

    std::uint64_t allocated_index = 0;
    if (!expect(
            locate_page(engine, allocated[0], allocated_index) &&
            bit_is_set(engine.allocated_bitmap, allocated_index),
            8,
            failure))
        return failure;

    if (!expect(
            !locate_page(engine, allocated[0] + 1, allocated_index) &&
            !locate_page(engine, 0x104000, allocated_index) &&
            !locate_page(engine, engine.metadata_base - PAGE_SIZE,
                         allocated_index),
            9,
            failure))
        return failure;

    void* valid_pointer = nullptr;
    void* expected_pointer =
        allocated[0] < 0x104000
            ? static_cast<void*>(
                backing.bytes + (allocated[0] - 0x100000))
            : static_cast<void*>(
                backing.bytes + 4 * PAGE_SIZE +
                (allocated[0] - 0x106000));
    if (!expect(
            page_pointer_from(
                engine,
                allocated[0],
                synthetic_mapper,
                &backing,
                valid_pointer) &&
            valid_pointer == expected_pointer &&
            !page_pointer_from(
                engine,
                0x104000,
                synthetic_mapper,
                &backing,
                valid_pointer) &&
            !page_pointer_from(
                engine,
                engine.metadata_base,
                synthetic_mapper,
                &backing,
                valid_pointer) &&
            valid_pointer == nullptr &&
            hhdm_range_is_valid(
                engine.hhdm_offset,
                allocated[0],
                PAGE_SIZE,
                engine.virtual_address_bits,
                allocated_index),
            10,
            failure))
        return failure;

    std::uint64_t ignored_virtual = 0;
    std::uint64_t expected_virtual = 0;
    if (!expect(
            hhdm_range_is_valid(
                engine.hhdm_offset,
                0x100123,
                1,
                engine.virtual_address_bits,
                expected_virtual) &&
            expected_virtual == engine.hhdm_offset + 0x100123 &&
            !hhdm_range_is_valid(
                0xfffffffffffff000ull,
                PAGE_SIZE,
                PAGE_SIZE,
                48,
                ignored_virtual) &&
            !hhdm_range_is_valid(
                0xffff000000000001ull,
                0x100000,
                PAGE_SIZE,
                48,
                ignored_virtual) &&
            !hhdm_range_is_valid(
                0,
                0x100000,
                PAGE_SIZE,
                48,
                ignored_virtual),
            11,
            failure))
        return failure;

    backing.fail = true;
    Engine failed_engine{};
    if (!expect(
            initialize_state(
                failed_engine,
                0xffff000000000000ull,
                &response,
                regime,
                synthetic_mapper,
                &backing) ==
                InitializationStatus::metadata_unavailable &&
            !failed_engine.initialized,
            12,
            failure))
        return failure;
    backing.fail = false;

    Engine invalid_hhdm_engine{};
    if (!expect(
            initialize_state(
                invalid_hhdm_engine,
                0,
                &response,
                regime,
                hhdm_mapper,
                nullptr) ==
                InitializationStatus::metadata_unavailable,
            32,
            failure))
        return failure;

    auto invalid_regime = regime;
    invalid_regime.levels = 3;
    Engine invalid_engine{};
    if (!expect(
            initialize_state(
                invalid_engine,
                0xffff000000000000ull,
                &response,
                invalid_regime,
                synthetic_mapper,
                &backing) ==
                InitializationStatus::unsupported_regime,
            13,
            failure))
        return failure;

    auto invalid_response = response;
    invalid_response.entries = nullptr;
    Engine malformed_engine{};
    if (!expect(
            initialize_state(
                malformed_engine,
                0xffff000000000000ull,
                &invalid_response,
                regime,
                synthetic_mapper,
                &backing) ==
                InitializationStatus::invalid_memory_map,
            14,
            failure))
        return failure;

    invalid_response = response;
    invalid_response.entry_count = 0;
    if (!expect(
            initialize_state(
                malformed_engine,
                0xffff000000000000ull,
                &invalid_response,
                regime,
                synthetic_mapper,
                &backing) ==
                InitializationStatus::invalid_memory_map,
            15,
            failure))
        return failure;

    if (!expect(
            initialize_state(
                malformed_engine,
                0xffff000000000000ull,
                nullptr,
                regime,
                synthetic_mapper,
                &backing) ==
                InitializationStatus::invalid_memory_map,
            33,
            failure))
        return failure;

    limine_memmap_entry* saved_pointer = entries[1];
    entries[1] = nullptr;
    const auto null_entry_status = initialize_state(
        malformed_engine,
        0xffff000000000000ull,
        &response,
        regime,
        synthetic_mapper,
        &backing
    );
    entries[1] = saved_pointer;
    if (!expect(
            null_entry_status ==
                InitializationStatus::invalid_memory_map,
            34,
            failure))
        return failure;

    limine_memmap_entry* oversized_entries[
        MAX_MEMORY_MAP_ENTRIES + 1
    ]{};
    for (std::uint64_t i = 0;
         i < MAX_MEMORY_MAP_ENTRIES + 1;
         ++i)
        oversized_entries[i] = entries[0];
    limine_memmap_response oversized_response{
        0,
        MAX_MEMORY_MAP_ENTRIES + 1,
        oversized_entries
    };
    if (!expect(
            initialize_state(
                malformed_engine,
                0xffff000000000000ull,
                &oversized_response,
                regime,
                synthetic_mapper,
                &backing) ==
                InitializationStatus::too_many_memory_map_entries,
            30,
            failure))
        return failure;

    limine_memmap_entry saved = *entries[0];
    *entries[0] = {0x100000, 0, LIMINE_MEMMAP_USABLE};
    const auto zero_length_status = initialize_state(
        malformed_engine,
        0xffff000000000000ull,
        &response,
        regime,
        synthetic_mapper,
        &backing
    );
    *entries[0] = saved;
    if (!expect(
            zero_length_status ==
                InitializationStatus::invalid_memory_map,
            16,
            failure))
        return failure;

    saved = *entries[0];
    *entries[0] = {
        INVALID_VALUE - PAGE_MASK,
        PAGE_SIZE * 2,
        LIMINE_MEMMAP_USABLE
    };
    const auto overflow_status = initialize_state(
        malformed_engine,
        0xffff000000000000ull,
        &response,
        regime,
        synthetic_mapper,
        &backing
    );
    *entries[0] = saved;
    if (!expect(
            overflow_status ==
                InitializationStatus::invalid_memory_map,
            17,
            failure))
        return failure;

    saved = *entries[0];
    *entries[0] = {
        std::uint64_t{1} << 44,
        PAGE_SIZE,
        LIMINE_MEMMAP_USABLE
    };
    const auto address_width_status = initialize_state(
        malformed_engine,
        0xffff000000000000ull,
        &response,
        regime,
        synthetic_mapper,
        &backing
    );
    *entries[0] = saved;
    if (!expect(
            address_width_status ==
                InitializationStatus::invalid_memory_map,
            31,
            failure))
        return failure;

    saved = *entries[0];
    *entries[0] = {0x100001, PAGE_SIZE, LIMINE_MEMMAP_USABLE};
    const auto alignment_status = initialize_state(
        malformed_engine,
        0xffff000000000000ull,
        &response,
        regime,
        synthetic_mapper,
        &backing
    );
    *entries[0] = saved;
    if (!expect(
            alignment_status ==
                InitializationStatus::invalid_memory_map,
            18,
            failure))
        return failure;

    saved = *entries[1];
    *entries[1] = {
        0x102000,
        2 * PAGE_SIZE,
        LIMINE_MEMMAP_RESERVED
    };
    const auto overlap_status = initialize_state(
        malformed_engine,
        0xffff000000000000ull,
        &response,
        regime,
        synthetic_mapper,
        &backing
    );
    *entries[1] = saved;
    if (!expect(
            overlap_status ==
                InitializationStatus::invalid_memory_map,
            19,
            failure))
        return failure;

    saved = *entries[3];
    *entries[3] = {0x106000, PAGE_SIZE, LIMINE_MEMMAP_RESERVED};
    const auto unknown_memory_status = initialize_state(
        malformed_engine,
        0xffff000000000000ull,
        &response,
        regime,
        synthetic_mapper,
        &backing
    );
    *entries[3] = saved;
    if (!expect(
            unknown_memory_status ==
                InitializationStatus::invalid_memory_map,
            20,
            failure))
        return failure;

    saved = *entries[3];
    *entries[3] = {0x105000, PAGE_SIZE, LIMINE_MEMMAP_RESERVED};
    const auto unsorted_status = initialize_state(
        malformed_engine,
        0xffff000000000000ull,
        &response,
        regime,
        synthetic_mapper,
        &backing
    );
    *entries[3] = saved;
    if (!expect(
            unsorted_status ==
                InitializationStatus::invalid_memory_map,
            21,
            failure))
        return failure;

    saved = *entries[0];
    *entries[0] = {0x100000, 4 * PAGE_SIZE, 99};
    const auto unknown_type_status = initialize_state(
        malformed_engine,
        0xffff000000000000ull,
        &response,
        regime,
        synthetic_mapper,
        &backing
    );
    *entries[0] = saved;
    if (!expect(
            unknown_type_status ==
                InitializationStatus::invalid_memory_map,
            22,
            failure))
        return failure;

    saved = *entries[1];
    *entries[1] = {
        0x103000,
        2 * PAGE_SIZE,
        LIMINE_MEMMAP_USABLE
    };
    const auto usable_overlap_status = initialize_state(
        malformed_engine,
        0xffff000000000000ull,
        &response,
        regime,
        synthetic_mapper,
        &backing
    );
    *entries[1] = saved;
    if (!expect(
            usable_overlap_status ==
                InitializationStatus::invalid_memory_map,
            23,
            failure))
        return failure;

    saved = *entries[0];
    const limine_memmap_entry saved_second_usable = *entries[2];
    *entries[0] = {
        0x100000,
        4 * PAGE_SIZE,
        LIMINE_MEMMAP_RESERVED
    };
    *entries[2] = {
        0x106000,
        3 * PAGE_SIZE,
        LIMINE_MEMMAP_RESERVED
    };
    const auto no_usable_status = initialize_state(
        malformed_engine,
        0xffff000000000000ull,
        &response,
        regime,
        synthetic_mapper,
        &backing
    );
    *entries[0] = saved;
    *entries[2] = saved_second_usable;
    if (!expect(
            no_usable_status ==
                InitializationStatus::no_usable_memory,
            29,
            failure))
        return failure;

    for (std::uint64_t i = 0; i < 6; ++i) {
        std::uint64_t index = 0;
        if (!locate_page(engine, allocated[i], index) ||
            !bit_is_set(engine.allocated_bitmap, index))
            return 24;
        if (!release_page_from(engine, allocated[i]))
            return 25;
    }
    if (!expect(
            engine.free_pages == initial.free_pages,
            26,
            failure))
        return failure;
    if (!expect(
            !release_page_from(engine, allocated[0]),
            27,
            failure))
        return failure;
    if (!expect(
            !release_page_from(engine, allocated[0] + 1) &&
            !release_page_from(engine, 0x104000) &&
            !release_page_from(engine, engine.metadata_base),
            28,
            failure))
        return failure;

    return 0;
}

}
