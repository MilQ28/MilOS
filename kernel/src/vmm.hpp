#pragma once

#include <cstdint>
#include <limine.h>

namespace MilOS::Memory {

constexpr std::uint64_t ADDRESS_SPACE_MAX_OWNED_PAGE_TABLES = 1024;
constexpr std::uint64_t ADDRESS_SPACE_MAX_OWNED_MAPPINGS = 1024;

struct AddressSpaceMapping {
    std::uint64_t virtual_address;
    std::uint64_t physical_address;
};

class AddressSpace {
public:
    AddressSpace() = default;
    AddressSpace(const AddressSpace&) = delete;
    AddressSpace& operator=(const AddressSpace&) = delete;

private:
    struct State {
        std::uint64_t root_physical_address = 0;
        std::uint64_t owned_table_count = 0;
        std::uint64_t owned_mapping_count = 0;
        std::uint64_t shared_root_entries[512] = {};
        std::uint64_t owned_table_pages[
            ADDRESS_SPACE_MAX_OWNED_PAGE_TABLES
        ] = {};
        AddressSpaceMapping owned_mappings[
            ADDRESS_SPACE_MAX_OWNED_MAPPINGS
        ] = {};
        std::uint8_t levels = 0;
        std::uint8_t reserved[7] = {};
        std::uint64_t state = 0;
    };

    State state_{};
    friend struct AddressSpaceAccess;
};

bool vmm_init(
    std::uint64_t hhdm_offset,
    limine_memmap_response* memmap
);

bool translate(
    std::uint64_t virtual_address,
    std::uint64_t& physical_address,
    std::uint64_t* page_size = nullptr
);

// VMM mutation and PMM use assume one CPU and no concurrent callers.
// Active-root invlpg affects only the current CPU; SMP needs page-table/PMM
// synchronization and acknowledged TLB shootdowns before reclaiming tables.
// Creates a supervisor mapping; writable controls only the leaf R/W bit.
// Returns false for conflicts, invalid inputs, or exhausted ownership records.
bool map_4k(
    std::uint64_t virtual_address,
    std::uint64_t physical_address,
    bool writable
);

// Removes only a mapping created by map_4k; the data page remains owned by
// its caller.
bool unmap_4k(std::uint64_t virtual_address);

// AddressSpace instances are empty after construction and are non-copyable.
// The new root snapshots and borrows all active upper-half root entries and
// owns no data pages. It is not activated or guaranteed ready for execution;
// later changes to the active root are not propagated.
bool address_space_create(AddressSpace& address_space);
// Destroy only after private mappings have been unmapped; borrowed subtrees
// are never freed, and the currently active root is rejected.
bool address_space_destroy(AddressSpace& address_space);
bool address_space_translate(
    const AddressSpace& address_space,
    std::uint64_t virtual_address,
    std::uint64_t& physical_address,
    std::uint64_t* page_size = nullptr
);
bool address_space_map_4k(
    AddressSpace& address_space,
    std::uint64_t virtual_address,
    std::uint64_t physical_address,
    bool writable
);
bool address_space_unmap_4k(
    AddressSpace& address_space,
    std::uint64_t virtual_address
);

std::uint8_t vmm_run_synthetic_tests();

std::uint8_t paging_levels();
std::uint64_t active_root_physical_address();

}
