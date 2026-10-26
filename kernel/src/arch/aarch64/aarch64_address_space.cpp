#include <cstddef>
#include <cstdint>
#include "aarch64_address_space.hpp"
#include "aarch64_pmm.hpp"

namespace MilOS::AArch64::AddressSpace {
namespace {

constexpr std::uint64_t PAGE_SIZE = 4096;
constexpr std::uint64_t PAGE_MASK = PAGE_SIZE - 1;
constexpr std::uint64_t DESCRIPTOR_VALID = 1;
constexpr std::uint64_t DESCRIPTOR_TABLE_OR_PAGE = 2;
constexpr std::uint64_t TABLE_DESCRIPTOR = 3;
constexpr std::uint64_t REQUIRED_NORMAL_MAIR_BYTE = 0xff;
constexpr std::uint8_t NORMAL_ATTRIBUTE_INDEX = 1;
constexpr std::uint8_t INNER_SHAREABLE = 3;
constexpr std::uint8_t REQUIRED_LEVELS = 4;
constexpr std::uint8_t REQUIRED_VIRTUAL_ADDRESS_BITS = 48;
constexpr std::uint64_t MAX_PHYSICAL_ADDRESS_BITS = 48;
constexpr std::uint64_t INVALID_VALUE = ~std::uint64_t{0};
constexpr std::uint64_t SYNTHETIC_VIRTUAL_BASE =
    0xffff800000100000ull;
constexpr std::uint64_t SYNTHETIC_OTHER_ROOT_BASE =
    SYNTHETIC_VIRTUAL_BASE + (std::uint64_t{1} << 39);
constexpr std::uint64_t SYNTHETIC_PHYSICAL_BASE = 0x200000;
constexpr std::uint32_t SYNTHETIC_PAGE_COUNT = 96;

struct SyntheticProvider {
    alignas(PAGE_SIZE) std::uint8_t backing[
        SYNTHETIC_PAGE_COUNT
    ][PAGE_SIZE];
    bool allocated[SYNTHETIC_PAGE_COUNT];
    std::int32_t allocation_budget;
    bool force_allocation_once;
    std::uint64_t forced_physical_address;
    bool fail_release_once;
    std::int32_t release_failure_countdown;
};

SyntheticProvider synthetic_provider{};

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

bool supported_regime(const Paging::Regime& regime) {
    if (regime.levels != REQUIRED_LEVELS ||
        regime.virtual_address_bits != REQUIRED_VIRTUAL_ADDRESS_BITS ||
        regime.physical_address_bits < 32 ||
        regime.physical_address_bits > MAX_PHYSICAL_ADDRESS_BITS)
        return false;
    const std::uint64_t expected_mask =
        ((std::uint64_t{1} << regime.physical_address_bits) - 1) &
        ~PAGE_MASK;
    return regime.physical_address_mask == expected_mask;
}

bool canonical_address(std::uint64_t address) {
    constexpr std::uint64_t ADDRESS_MASK =
        (std::uint64_t{1} << REQUIRED_VIRTUAL_ADDRESS_BITS) - 1;
    constexpr std::uint64_t LOWER_LIMIT = ADDRESS_MASK;
    constexpr std::uint64_t UPPER_BASE = INVALID_VALUE - ADDRESS_MASK;
    return address <= LOWER_LIMIT || address >= UPPER_BASE;
}

bool valid_virtual_page(std::uint64_t address) {
    std::uint64_t end;
    return (address & PAGE_MASK) == 0 &&
        add_without_overflow(address, PAGE_SIZE - 1, end) &&
        canonical_address(address) &&
        canonical_address(end);
}

std::uint16_t table_index(
    std::uint64_t virtual_address,
    std::uint8_t level
) {
    const std::uint8_t shift =
        static_cast<std::uint8_t>(39 - 9 * level);
    return static_cast<std::uint16_t>(
        (virtual_address >> shift) & 0x1ff
    );
}

bool supported_policy(LeafPolicy policy) {
    return policy == LeafPolicy::kernel_text ||
        policy == LeafPolicy::read_only ||
        policy == LeafPolicy::writable;
}

std::uint64_t normal_page_descriptor(
    std::uint64_t physical_address,
    LeafPolicy policy,
    std::uint8_t attribute_index
) {
    std::uint64_t attributes =
        (std::uint64_t{attribute_index} << 2) |
        (std::uint64_t{INNER_SHAREABLE} << 8) |
        (std::uint64_t{1} << 10);
    switch (policy) {
        case LeafPolicy::kernel_text:
            attributes |=
                (std::uint64_t{2} << 6) |
                (std::uint64_t{1} << 54);
            break;
        case LeafPolicy::read_only:
            attributes |=
                (std::uint64_t{2} << 6) |
                (std::uint64_t{1} << 53) |
                (std::uint64_t{1} << 54);
            break;
        case LeafPolicy::writable:
            attributes |=
                (std::uint64_t{1} << 53) |
                (std::uint64_t{1} << 54);
            break;
        case LeafPolicy::writable_executable:
            return 0;
    }
    return physical_address |
        DESCRIPTOR_VALID |
        DESCRIPTOR_TABLE_OR_PAGE |
        attributes;
}

bool is_supported_page_descriptor(
    std::uint64_t descriptor,
    std::uint64_t physical_address_mask,
    std::uint64_t mair_value
) {
    if ((descriptor & DESCRIPTOR_VALID) == 0 ||
        (descriptor & DESCRIPTOR_TABLE_OR_PAGE) == 0)
        return false;
    const std::uint64_t physical_address =
        descriptor & physical_address_mask;
    for (std::uint8_t attribute_index = 0;
         attribute_index < 8;
         ++attribute_index) {
        const std::uint8_t attribute = static_cast<std::uint8_t>(
            mair_value >> (attribute_index * 8)
        );
        if (attribute != REQUIRED_NORMAL_MAIR_BYTE)
            continue;
        constexpr LeafPolicy policies[] = {
            LeafPolicy::kernel_text,
            LeafPolicy::read_only,
            LeafPolicy::writable
        };
        for (LeafPolicy policy : policies) {
            if (descriptor == normal_page_descriptor(
                    physical_address,
                    policy,
                    attribute_index))
                return true;
        }
    }
    return false;
}

std::uint64_t pmm_allocate(void*) {
    return Memory::allocate_page();
}

bool pmm_release(void*, std::uint64_t physical_address) {
    return Memory::release_page(physical_address);
}

bool pmm_writable_pointer(
    void*,
    std::uint64_t physical_address,
    void*& pointer
) {
    return Memory::writable_pointer_for_allocated_page(
        physical_address,
        pointer
    );
}

std::uint64_t synthetic_allocate(void* context) {
    auto& provider = *static_cast<SyntheticProvider*>(context);
    if (provider.force_allocation_once) {
        provider.force_allocation_once = false;
        return provider.forced_physical_address;
    }
    if (provider.allocation_budget == 0)
        return 0;
    for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i) {
        if (provider.allocated[i])
            continue;
        provider.allocated[i] = true;
        if (provider.allocation_budget > 0)
            --provider.allocation_budget;
        return SYNTHETIC_PHYSICAL_BASE +
            std::uint64_t{i} * PAGE_SIZE;
    }
    return 0;
}

bool synthetic_release(
    void* context,
    std::uint64_t physical_address
) {
    auto& provider = *static_cast<SyntheticProvider*>(context);
    if (provider.fail_release_once) {
        provider.fail_release_once = false;
        return false;
    }
    if (provider.release_failure_countdown == 0) {
        provider.release_failure_countdown = -1;
        return false;
    }
    if (provider.release_failure_countdown > 0)
        --provider.release_failure_countdown;
    if (physical_address < SYNTHETIC_PHYSICAL_BASE ||
        ((physical_address - SYNTHETIC_PHYSICAL_BASE) & PAGE_MASK) != 0)
        return false;
    const std::uint64_t index =
        (physical_address - SYNTHETIC_PHYSICAL_BASE) / PAGE_SIZE;
    if (index >= SYNTHETIC_PAGE_COUNT || !provider.allocated[index])
        return false;
    provider.allocated[index] = false;
    return true;
}

bool synthetic_writable_pointer(
    void* context,
    std::uint64_t physical_address,
    void*& pointer
) {
    pointer = nullptr;
    auto& provider = *static_cast<SyntheticProvider*>(context);
    if (physical_address < SYNTHETIC_PHYSICAL_BASE ||
        ((physical_address - SYNTHETIC_PHYSICAL_BASE) & PAGE_MASK) != 0)
        return false;
    const std::uint64_t index =
        (physical_address - SYNTHETIC_PHYSICAL_BASE) / PAGE_SIZE;
    if (index >= SYNTHETIC_PAGE_COUNT || !provider.allocated[index])
        return false;
    pointer = provider.backing[index];
    return true;
}

Paging::Regime synthetic_regime() {
    return {
        ((std::uint64_t{1} << 44) - 1) & ~PAGE_MASK,
        44,
        REQUIRED_VIRTUAL_ADDRESS_BITS,
        REQUIRED_LEVELS
    };
}

bool expect(
    bool condition,
    std::uint32_t code,
    std::uint32_t& failure
) {
    if (!condition) {
        failure = code;
        return false;
    }
    return true;
}

bool supported_memory_map_type(std::uint64_t type) {
    return type == LIMINE_MEMMAP_USABLE ||
        type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
        type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES;
}

bool memory_map_range_has_type(
    const limine_memmap_response* memory_map,
    std::uint64_t physical_base,
    std::uint64_t physical_end,
    std::uint64_t expected_type,
    std::uint64_t physical_limit
) {
    if (memory_map == nullptr ||
        memory_map->entries == nullptr ||
        memory_map->entry_count == 0 ||
        memory_map->entry_count > 128 ||
        !supported_memory_map_type(expected_type))
        return false;

    std::uint64_t cursor = physical_base;
    while (cursor < physical_end) {
        const limine_memmap_entry* matching_entry = nullptr;
        std::uint64_t matching_end = 0;
        for (std::uint64_t i = 0; i < memory_map->entry_count; ++i) {
            const limine_memmap_entry* entry = memory_map->entries[i];
            if (entry == nullptr || entry->length == 0 ||
                entry->base > ~std::uint64_t{0} - entry->length)
                return false;
            const std::uint64_t entry_end = entry->base + entry->length;
            if (entry_end > physical_limit)
                return false;
            if (cursor < entry->base || cursor >= entry_end)
                continue;
            if (matching_entry != nullptr ||
                entry->type != expected_type)
                return false;
            matching_entry = entry;
            matching_end = entry_end;
        }
        if (matching_entry == nullptr || matching_end <= cursor)
            return false;
        cursor = matching_end < physical_end
            ? matching_end
            : physical_end;
    }
    return true;
}

bool valid_memory_map_response(
    const limine_memmap_response* memory_map,
    std::uint64_t physical_address_bits
) {
    if (memory_map == nullptr ||
        (reinterpret_cast<std::uint64_t>(memory_map) &
         (alignof(limine_memmap_response) - 1)) != 0 ||
        memory_map->entries == nullptr ||
        memory_map->entry_count == 0 ||
        memory_map->entry_count > 128)
        return false;

    const std::uint64_t entries_address =
        reinterpret_cast<std::uint64_t>(memory_map->entries);
    const std::uint64_t entries_size =
        memory_map->entry_count * sizeof(limine_memmap_entry*);
    if ((entries_address &
         (alignof(limine_memmap_entry*) - 1)) != 0 ||
        entries_address > ~std::uint64_t{0} - entries_size)
        return false;

    const std::uint64_t physical_limit =
        std::uint64_t{1} << physical_address_bits;
    std::uint64_t previous_base = 0;
    for (std::uint64_t i = 0; i < memory_map->entry_count; ++i) {
        const limine_memmap_entry* entry = memory_map->entries[i];
        if (entry == nullptr ||
            (reinterpret_cast<std::uint64_t>(entry) &
             (alignof(limine_memmap_entry) - 1)) != 0 ||
            entry->type > LIMINE_MEMMAP_RESERVED_MAPPED ||
            entry->length == 0 ||
            entry->base > ~std::uint64_t{0} - entry->length)
            return false;
        const std::uint64_t end = entry->base + entry->length;
        if (end > physical_limit ||
            (i != 0 && entry->base < previous_base))
            return false;
        previous_base = entry->base;
        if (entry->type == LIMINE_MEMMAP_USABLE &&
            ((entry->base & PAGE_MASK) != 0 ||
             (entry->length & PAGE_MASK) != 0))
            return false;
        if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
            ((entry->base & PAGE_MASK) != 0 ||
             (entry->length & PAGE_MASK) != 0))
            return false;
        for (std::uint64_t j = 0; j < i; ++j) {
            const limine_memmap_entry* previous =
                memory_map->entries[j];
            const std::uint64_t previous_end =
                previous->base + previous->length;
            if (entry->base < previous_end &&
                previous->base < end)
                return false;
        }
    }
    return true;
}

}

Status Hierarchy::create(
    const Paging::Regime& regime,
    std::uint64_t mair_value
) {
    const Hierarchy::PageProvider provider{
        nullptr,
        pmm_allocate,
        pmm_release,
        pmm_writable_pointer
    };
    return create_with_provider(regime, mair_value, provider);
}

Status Hierarchy::create_with_provider(
    const Paging::Regime& regime,
    std::uint64_t mair_value,
    const PageProvider& provider,
    std::uint32_t table_page_limit
) {
    if (created_)
        return Status::already_created;
    if (table_page_limit == 0 ||
        table_page_limit > MAX_OWNED_TABLE_PAGES)
        return Status::ownership_capacity_exceeded;
    if (!supported_regime(regime))
        return Status::unsupported_regime;
    const std::uint8_t normal_attribute =
        static_cast<std::uint8_t>(
            mair_value >> (NORMAL_ATTRIBUTE_INDEX * 8)
        );
    if (normal_attribute != REQUIRED_NORMAL_MAIR_BYTE)
        return Status::unsupported_normal_attribute;
    if (provider.allocate == nullptr ||
        provider.release == nullptr ||
        provider.writable_pointer == nullptr)
        return Status::invalid_ownership;
    if (!acquire_ownership_storage())
        return Status::ownership_capacity_exceeded;

    physical_address_mask_ = regime.physical_address_mask;
    mair_value_ = mair_value;
    provider_ = provider;
    table_page_limit_ = table_page_limit;
    std::uint64_t root = 0;
    std::uint64_t* entries = nullptr;
    const Status result = allocate_owned_table(root, entries);
    if (result != Status::success) {
        physical_address_mask_ = 0;
        mair_value_ = 0;
        provider_ = {};
        table_page_limit_ = MAX_OWNED_TABLE_PAGES;
        release_ownership_storage();
        return result;
    }
    created_ = true;
    return Status::success;
}

bool Hierarchy::acquire_ownership_storage() {
    if (pages_ != nullptr)
        return true;
    for (std::uint8_t index = 0;
         index < MAX_CONCURRENT_HIERARCHIES;
         ++index) {
        if (ownership_storage_in_use_[index])
            continue;
        ownership_storage_in_use_[index] = true;
        ownership_storage_index_ = index;
        pages_ = ownership_storage_[index];
        for (std::uint32_t i = 0;
             i < MAX_OWNED_TABLE_PAGES;
             ++i)
            pages_[i] = {};
        page_count_ = 0;
        return true;
    }
    return false;
}

void Hierarchy::release_ownership_storage() {
    if (pages_ == nullptr ||
        ownership_storage_index_ >= MAX_CONCURRENT_HIERARCHIES)
        return;
    for (std::uint32_t i = 0;
         i < MAX_OWNED_TABLE_PAGES;
         ++i)
        pages_[i] = {};
    ownership_storage_in_use_[ownership_storage_index_] = false;
    ownership_storage_index_ = 0xff;
    pages_ = nullptr;
    page_count_ = 0;
}

Status Hierarchy::allocate_owned_table(
    std::uint64_t& physical_address,
    std::uint64_t*& entries
) {
    physical_address = 0;
    entries = nullptr;
    if (page_count_ >= table_page_limit_)
        return Status::ownership_capacity_exceeded;

    const std::uint64_t allocated =
        provider_.allocate(provider_.context);
    if (allocated == 0)
        return Status::allocation_failure;
    if (find_owned_table(allocated) >= 0)
        return Status::invalid_ownership;
    if ((allocated & PAGE_MASK) != 0 ||
        (allocated & ~physical_address_mask_) != 0) {
        if (!provider_.release(provider_.context, allocated))
            return Status::rollback_failed;
        return Status::invalid_ownership;
    }

    void* pointer = nullptr;
    if (!provider_.writable_pointer(
            provider_.context,
            allocated,
            pointer) ||
        pointer == nullptr ||
        (reinterpret_cast<std::uint64_t>(pointer) & PAGE_MASK) != 0) {
        if (!provider_.release(provider_.context, allocated))
            return Status::rollback_failed;
        return Status::invalid_ownership;
    }

    auto* table = static_cast<std::uint64_t*>(pointer);
    for (std::uint32_t i = 0; i < PAGE_SIZE / sizeof(std::uint64_t); ++i)
        table[i] = 0;
    pages_[page_count_++] = {allocated, table};
    physical_address = allocated;
    entries = table;
    return Status::success;
}

std::int32_t Hierarchy::find_owned_table(
    std::uint64_t physical_address
) const {
    for (std::uint32_t i = 0; i < page_count_; ++i) {
        if (pages_[i].physical_address == physical_address)
            return static_cast<std::int32_t>(i);
    }
    return -1;
}

Status Hierarchy::checked_table_entries(
    std::uint64_t physical_address,
    std::uint64_t*& entries
) const {
    entries = nullptr;
    const std::int32_t index = find_owned_table(physical_address);
    if (index < 0)
        return Status::invalid_ownership;
    void* pointer = nullptr;
    if (!provider_.writable_pointer(
            provider_.context,
            physical_address,
            pointer) ||
        pointer == nullptr ||
        pointer != pages_[index].entries)
        return Status::invalid_ownership;
    entries = static_cast<std::uint64_t*>(pointer);
    return Status::success;
}

Status Hierarchy::release_owned_table(std::uint64_t physical_address) {
    const std::int32_t index = find_owned_table(physical_address);
    if (index < 0)
        return Status::invalid_ownership;
    std::uint64_t* entries = nullptr;
    if (checked_table_entries(physical_address, entries) != Status::success)
        return Status::invalid_ownership;
    if (!provider_.release(provider_.context, physical_address))
        return Status::rollback_failed;
    const std::uint32_t last = page_count_ - 1;
    if (static_cast<std::uint32_t>(index) != last)
        pages_[index] = pages_[last];
    pages_[last] = {};
    --page_count_;
    return Status::success;
}

Status Hierarchy::map_page(
    std::uint64_t virtual_address,
    std::uint64_t physical_address
) {
    return map_page(
        virtual_address,
        physical_address,
        LeafPolicy::writable,
        NORMAL_ATTRIBUTE_INDEX
    );
}

Status Hierarchy::map_page(
    std::uint64_t virtual_address,
    std::uint64_t physical_address,
    LeafPolicy policy,
    std::uint8_t attribute_index
) {
    return map_page_internal(
        virtual_address,
        physical_address,
        policy,
        attribute_index,
        true
    );
}

Status Hierarchy::map_page_internal(
    std::uint64_t virtual_address,
    std::uint64_t physical_address,
    LeafPolicy policy,
    std::uint8_t attribute_index,
    bool require_allocated_leaf
) {
    if (!created_)
        return Status::not_created;
    if (!valid_virtual_page(virtual_address))
        return Status::invalid_virtual_address;
    if (!supported_policy(policy))
        return Status::unsupported_policy;
    if (attribute_index >= 8 ||
        static_cast<std::uint8_t>(
            mair_value_ >> (attribute_index * 8)
        ) != REQUIRED_NORMAL_MAIR_BYTE)
        return Status::invalid_attribute_index;
    const std::uint64_t physical_limit =
        physical_address_mask_ | PAGE_MASK;
    if ((physical_address & PAGE_MASK) != 0 ||
        physical_address > physical_limit - PAGE_MASK)
        return Status::invalid_physical_address;
    if (require_allocated_leaf &&
        find_owned_table(physical_address) >= 0)
        return Status::invalid_physical_address;
    if (require_allocated_leaf) {
        void* borrowed_pointer = nullptr;
        if (!provider_.writable_pointer(
                provider_.context,
                physical_address,
                borrowed_pointer) ||
            borrowed_pointer == nullptr)
            return Status::invalid_physical_address;
    }

    const std::uint64_t root = pages_[0].physical_address;
    std::uint64_t current_physical = root;
    std::uint64_t* parent_entries = nullptr;
    Status result =
        checked_table_entries(current_physical, parent_entries);
    if (result != Status::success)
        return result;

    std::uint64_t* newly_linked_entries[REQUIRED_LEVELS - 1]{};
    std::uint64_t newly_allocated_pages[REQUIRED_LEVELS - 1]{};
    std::uint8_t newly_allocated_count = 0;

    auto rollback = [&]() {
        while (newly_allocated_count != 0) {
            const std::uint8_t index =
                static_cast<std::uint8_t>(
                    newly_allocated_count - 1
                );
            std::uint64_t* parent_entry =
                newly_linked_entries[index];
            const std::uint64_t descriptor = *parent_entry;
            *parent_entry = 0;
            if (release_owned_table(
                    newly_allocated_pages[index]) != Status::success) {
                *parent_entry = descriptor;
                return false;
            }
            --newly_allocated_count;
        }
        return true;
    };

    for (std::uint8_t level = 0; level < REQUIRED_LEVELS - 1; ++level) {
        const std::uint16_t index =
            table_index(virtual_address, level);
        std::uint64_t& descriptor = parent_entries[index];
        if (descriptor == 0) {
            std::uint64_t child_physical = 0;
            std::uint64_t* child_entries = nullptr;
            result = allocate_owned_table(
                child_physical,
                child_entries
            );
            if (result != Status::success)
                return rollback() ? result : Status::rollback_failed;
            descriptor = child_physical | TABLE_DESCRIPTOR;
            newly_linked_entries[newly_allocated_count] = &descriptor;
            newly_allocated_pages[newly_allocated_count] =
                child_physical;
            ++newly_allocated_count;
            current_physical = child_physical;
            parent_entries = child_entries;
            continue;
        }

        if ((descriptor & DESCRIPTOR_VALID) == 0 ||
            (descriptor & DESCRIPTOR_TABLE_OR_PAGE) == 0 ||
            (descriptor & ~(physical_address_mask_ |
                            DESCRIPTOR_VALID |
                            DESCRIPTOR_TABLE_OR_PAGE)) != 0)
            return rollback()
                ? Status::malformed_descriptor
                : Status::rollback_failed;

        current_physical = descriptor & physical_address_mask_;
        if (descriptor !=
            (current_physical | TABLE_DESCRIPTOR))
            return rollback()
                ? Status::malformed_descriptor
                : Status::rollback_failed;
        result = checked_table_entries(current_physical, parent_entries);
        if (result != Status::success)
            return rollback() ? result : Status::rollback_failed;
    }

    const std::uint16_t leaf_index =
        table_index(virtual_address, REQUIRED_LEVELS - 1);
    std::uint64_t& leaf = parent_entries[leaf_index];
    if (leaf != 0) {
        result = (leaf & DESCRIPTOR_VALID) != 0 &&
            is_supported_page_descriptor(
                    leaf,
                physical_address_mask_,
                mair_value_)
        ? Status::mapping_exists
        : Status::malformed_descriptor;
        return rollback() ? result : Status::rollback_failed;
    }
    leaf = normal_page_descriptor(
        physical_address,
        policy,
        attribute_index
    );
    return Status::success;
}

Translation Hierarchy::translate(
    std::uint64_t virtual_address
) const {
    if (!created_)
        return {Status::not_created, 0};
    if (!valid_virtual_page(virtual_address & ~PAGE_MASK))
        return {Status::invalid_virtual_address, 0};
    std::uint64_t current_physical = pages_[0].physical_address;
    std::uint64_t* entries = nullptr;
    Status result = checked_table_entries(current_physical, entries);
    if (result != Status::success)
        return {result, 0};

    for (std::uint8_t level = 0; level < REQUIRED_LEVELS; ++level) {
        const std::uint64_t descriptor =
            entries[table_index(virtual_address, level)];
        if (descriptor == 0)
            return {Status::not_mapped, 0};
        if ((descriptor & DESCRIPTOR_VALID) == 0)
            return {Status::malformed_descriptor, 0};
        if (level == REQUIRED_LEVELS - 1) {
            if (!is_supported_page_descriptor(
                    descriptor,
                    physical_address_mask_,
                    mair_value_))
                return {Status::malformed_descriptor, 0};
            return {
                Status::success,
                (descriptor & physical_address_mask_) |
                    (virtual_address & PAGE_MASK)
            };
        }
        if ((descriptor & DESCRIPTOR_TABLE_OR_PAGE) == 0 ||
            (descriptor & ~(physical_address_mask_ |
                            DESCRIPTOR_VALID |
                            DESCRIPTOR_TABLE_OR_PAGE)) != 0)
            return {Status::malformed_descriptor, 0};
        current_physical = descriptor & physical_address_mask_;
        if (descriptor !=
            (current_physical | TABLE_DESCRIPTOR))
            return {Status::malformed_descriptor, 0};
        result = checked_table_entries(current_physical, entries);
        if (result != Status::success)
            return {result, 0};
    }
    return {Status::malformed_descriptor, 0};
}

Status Hierarchy::unmap_page(std::uint64_t virtual_address) {
    if (!created_)
        return Status::not_created;
    if (!valid_virtual_page(virtual_address))
        return Status::invalid_virtual_address;

    std::uint64_t table_physical[REQUIRED_LEVELS]{};
    std::uint64_t* table_entries[REQUIRED_LEVELS]{};
    std::uint64_t* parent_descriptors[REQUIRED_LEVELS - 1]{};
    table_physical[0] = pages_[0].physical_address;
    Status result = checked_table_entries(
        table_physical[0],
        table_entries[0]
    );
    if (result != Status::success)
        return result;

    for (std::uint8_t level = 0; level < REQUIRED_LEVELS - 1; ++level) {
        std::uint64_t& descriptor =
            table_entries[level][table_index(virtual_address, level)];
        parent_descriptors[level] = &descriptor;
        if (descriptor == 0)
            return Status::not_mapped;
        if ((descriptor & DESCRIPTOR_VALID) == 0 ||
            (descriptor & DESCRIPTOR_TABLE_OR_PAGE) == 0 ||
            (descriptor & ~(physical_address_mask_ |
                            DESCRIPTOR_VALID |
                            DESCRIPTOR_TABLE_OR_PAGE)) != 0)
            return Status::malformed_descriptor;
        table_physical[level + 1] =
            descriptor & physical_address_mask_;
        if (descriptor !=
            (table_physical[level + 1] | TABLE_DESCRIPTOR))
            return Status::malformed_descriptor;
        result = checked_table_entries(
            table_physical[level + 1],
            table_entries[level + 1]
        );
        if (result != Status::success)
            return result;
    }

    std::uint64_t& leaf = table_entries[REQUIRED_LEVELS - 1][
        table_index(virtual_address, REQUIRED_LEVELS - 1)
    ];
    if (leaf == 0)
        return Status::not_mapped;
    if (!is_supported_page_descriptor(
            leaf,
            physical_address_mask_,
            mair_value_))
        return Status::malformed_descriptor;
    leaf = 0;

    for (std::uint8_t child_level = REQUIRED_LEVELS - 1;
         child_level > 0;
         --child_level) {
        bool empty = true;
        for (std::uint32_t i = 0; i < PAGE_SIZE / sizeof(std::uint64_t); ++i) {
            if (table_entries[child_level][i] != 0) {
                empty = false;
                break;
            }
        }
        if (!empty)
            break;

        std::uint64_t* parent_descriptor =
            parent_descriptors[child_level - 1];
        const std::uint64_t saved_descriptor = *parent_descriptor;
        *parent_descriptor = 0;
        result = release_owned_table(table_physical[child_level]);
        if (result != Status::success) {
            *parent_descriptor = saved_descriptor;
            return Status::rollback_failed;
        }
    }
    return Status::success;
}

Status Hierarchy::validate_owned_tree() const {
    if (!created_ || page_count_ == 0 ||
        page_count_ > table_page_limit_)
        return Status::invalid_ownership;

    std::uint8_t references[MAX_OWNED_TABLE_PAGES]{};
    for (std::uint32_t i = 0; i < page_count_; ++i) {
        const std::uint64_t physical = pages_[i].physical_address;
        if (physical == 0 ||
            (physical & PAGE_MASK) != 0 ||
            (physical & ~physical_address_mask_) != 0)
            return Status::invalid_ownership;
        for (std::uint32_t j = i + 1; j < page_count_; ++j) {
            if (pages_[j].physical_address == physical)
                return Status::invalid_ownership;
        }
        std::uint64_t* checked_entries = nullptr;
        if (checked_table_entries(physical, checked_entries) !=
                Status::success ||
            checked_entries != pages_[i].entries)
            return Status::invalid_ownership;
    }
    references[0] = 1;

    auto visit = [&](auto&& self,
                     std::uint32_t record_index,
                     std::uint8_t level) -> Status {
        std::uint64_t* entries = nullptr;
        const Status access_status = checked_table_entries(
            pages_[record_index].physical_address,
            entries
        );
        if (access_status != Status::success)
            return access_status;
        for (std::uint32_t i = 0;
             i < PAGE_SIZE / sizeof(std::uint64_t);
             ++i) {
            const std::uint64_t descriptor = entries[i];
            if (descriptor == 0)
                continue;
            if ((descriptor & DESCRIPTOR_VALID) == 0)
                return Status::malformed_descriptor;
            if (level == REQUIRED_LEVELS - 1) {
                if (!is_supported_page_descriptor(
                        descriptor,
                        physical_address_mask_,
                        mair_value_))
                    return Status::malformed_descriptor;
                continue;
            }
            if ((descriptor & DESCRIPTOR_TABLE_OR_PAGE) == 0 ||
                (descriptor & ~(physical_address_mask_ |
                                DESCRIPTOR_VALID |
                                DESCRIPTOR_TABLE_OR_PAGE)) != 0)
                return Status::malformed_descriptor;
            const std::uint64_t child =
                descriptor & physical_address_mask_;
            if (descriptor != (child | TABLE_DESCRIPTOR))
                return Status::malformed_descriptor;
            const std::int32_t child_index = find_owned_table(child);
            if (child_index < 0 ||
                ++references[child_index] != 1)
                return Status::invalid_ownership;
            const Status child_status = self(
                self,
                static_cast<std::uint32_t>(child_index),
                static_cast<std::uint8_t>(level + 1)
            );
            if (child_status != Status::success)
                return child_status;
        }
        return Status::success;
    };

    const Status tree_status = visit(visit, 0, 0);
    if (tree_status != Status::success)
        return tree_status;
    for (std::uint32_t i = 0; i < page_count_; ++i) {
        if (references[i] != 1)
            return Status::invalid_ownership;
    }
    return Status::success;
}

Status Hierarchy::destroy() {
    if (!created_)
        return Status::not_created;
    const Status validation = validate_owned_tree();
    if (validation != Status::success)
        return validation;

    const std::uint64_t root = pages_[0].physical_address;
    auto release_children = [&](auto&& self,
                                std::uint64_t table_physical,
                                std::uint8_t level) -> Status {
        std::uint64_t* entries = nullptr;
        const Status access_status =
            checked_table_entries(table_physical, entries);
        if (access_status != Status::success)
            return access_status;
        if (level >= REQUIRED_LEVELS - 1)
            return Status::success;

        for (std::uint32_t i = 0;
             i < PAGE_SIZE / sizeof(std::uint64_t);
             ++i) {
            const std::uint64_t descriptor = entries[i];
            if (descriptor == 0)
                continue;
            const std::uint64_t child =
                descriptor & physical_address_mask_;
            const Status child_status = self(
                self,
                child,
                static_cast<std::uint8_t>(level + 1)
            );
            if (child_status != Status::success)
                return child_status;

            entries[i] = 0;
            const Status release_status = release_owned_table(child);
            if (release_status != Status::success) {
                entries[i] = descriptor;
                return release_status;
            }
        }
        return Status::success;
    };

    const Status children_status = release_children(
        release_children,
        root,
        0
    );
    if (children_status != Status::success)
        return children_status;
    const Status root_status = release_owned_table(root);
    if (root_status != Status::success)
        return root_status;
    physical_address_mask_ = 0;
    mair_value_ = 0;
    provider_ = {};
    created_ = false;
    table_page_limit_ = MAX_OWNED_TABLE_PAGES;
    release_ownership_storage();
    return Status::success;
}

bool Hierarchy::is_created() const {
    return created_;
}

std::uint64_t Hierarchy::root_physical_address() const {
    return created_ && page_count_ != 0
        ? pages_[0].physical_address
        : 0;
}

std::uint32_t Hierarchy::owned_table_page_count() const {
    return page_count_;
}

std::uint64_t Hierarchy::owned_table_page_physical_address(
    std::uint32_t index
) const {
    return index < page_count_
        ? pages_[index].physical_address
        : 0;
}

VerificationReport Hierarchy::verify_candidate(
    const limine_memmap_response* memory_map,
    const MappingRegion* regions,
    std::uint32_t region_count,
    std::uint32_t required_roles,
    const VerificationEvidence& evidence
) const {
    VerificationReport report{};
    for (VerificationState& state : report.checks)
        state = VerificationState::unknown;
    report.overall = VerificationState::unknown;
    report.first_non_pass = VerificationCheck::count;

    const auto set_check = [&](VerificationCheck check,
                               VerificationState state,
                               std::uint64_t address = 0) {
        const std::uint32_t index =
            static_cast<std::uint32_t>(check);
        report.checks[index] = state;
        if (state != VerificationState::pass &&
            report.first_non_pass == VerificationCheck::count) {
            report.first_non_pass = check;
            report.failing_address = address;
        }
    };
    const auto has_role = [&](std::uint32_t role) {
        for (std::uint32_t i = 0; i < region_count; ++i) {
            if ((regions[i].role_mask & role) != 0)
                return true;
        }
        return false;
    };
    const auto range_has_role = [&](std::uint64_t virtual_base,
                                    std::uint64_t size,
                                    std::uint32_t role,
                                    std::uint64_t physical_base) {
        if (regions == nullptr || size == 0 ||
            virtual_base > INVALID_VALUE - size ||
            physical_base > INVALID_VALUE - size)
            return false;
        const std::uint64_t virtual_end = virtual_base + size;
        const std::uint64_t physical_end = physical_base + size;
        for (std::uint32_t i = 0; i < region_count; ++i) {
            const MappingRegion& region = regions[i];
            if ((region.role_mask & role) == 0 ||
                region.virtual_base > virtual_base ||
                region.virtual_base > INVALID_VALUE - region.size ||
                region.physical_base > INVALID_VALUE - region.size)
                continue;
            const std::uint64_t region_virtual_end =
                region.virtual_base + region.size;
            const std::uint64_t region_physical_end =
                region.physical_base + region.size;
            if (region_virtual_end < virtual_end ||
                region.physical_base > physical_base ||
                region_physical_end < physical_end ||
                physical_base - region.physical_base !=
                    virtual_base - region.virtual_base)
                continue;
            return true;
        }
        return false;
    };
    const auto range_is_mapped = [&](std::uint64_t virtual_base,
                                     std::uint64_t size,
                                     std::uint64_t expected_physical) {
        if (size == 0 || virtual_base > INVALID_VALUE - size ||
            expected_physical > INVALID_VALUE - size)
            return false;
        const std::uint64_t end = virtual_base + size;
        for (std::uint64_t address = virtual_base;
             address < end;) {
            const Translation translation = translate(address);
            if (translation.status != Status::success ||
                translation.physical_address !=
                    expected_physical + (address - virtual_base))
                return false;
            const std::uint64_t next_page =
                (address & ~PAGE_MASK) + PAGE_SIZE;
            if (next_page <= address)
                return false;
            address = next_page < end ? next_page : end;
        }
        return true;
    };
    const auto range_has_descriptors = [&](
        std::uint64_t virtual_base,
        std::uint64_t physical_base,
        std::uint64_t size,
        LeafPolicy policy
    ) {
        if (size == 0 ||
            virtual_base > INVALID_VALUE - size ||
            physical_base > INVALID_VALUE - size)
            return false;
        const std::uint64_t end = virtual_base + size;
        for (std::uint64_t address = virtual_base;
             address < end;) {
            std::uint64_t table_physical =
                root_physical_address();
            std::uint64_t* table_entries = nullptr;
            if (checked_table_entries(
                    table_physical,
                    table_entries) != Status::success)
                return false;
            bool leaf_matches = false;
            for (std::uint8_t level = 0;
                 level < REQUIRED_LEVELS;
                 ++level) {
                const std::uint64_t descriptor =
                    table_entries[table_index(address, level)];
                if (level == REQUIRED_LEVELS - 1) {
                    leaf_matches = descriptor ==
                        normal_page_descriptor(
                            physical_base + (address - virtual_base),
                            policy,
                            NORMAL_ATTRIBUTE_INDEX
                        );
                    break;
                }
                table_physical =
                    descriptor & physical_address_mask_;
                if (checked_table_entries(
                        table_physical,
                        table_entries) != Status::success)
                    break;
            }
            if (!leaf_matches)
                return false;
            const std::uint64_t next_page =
                (address & ~PAGE_MASK) + PAGE_SIZE;
            if (next_page <= address)
                return false;
            address = next_page < end ? next_page : end;
        }
        return true;
    };
    const auto physical_range_is_usable = [&](std::uint64_t base,
                                               std::uint64_t size) {
        if (size == 0 || base > INVALID_VALUE - size ||
            physical_address_mask_ == 0)
            return false;
        const std::uint64_t physical_limit =
            physical_address_mask_ + PAGE_SIZE;
        return memory_map_range_has_type(
            memory_map,
            base,
            base + size,
            LIMINE_MEMMAP_USABLE,
            physical_limit
        );
    };

    const bool has_regions = regions != nullptr &&
        region_count != 0 &&
        region_count <= MAX_KERNEL_MAPPING_REGIONS;
    Paging::Regime active_regime{};
    const bool active_regime_valid =
        Paging::detect_regime(
            evidence.active_registers,
            active_regime) == Paging::RegimeStatus::supported &&
        evidence.active_registers.current_el == 4 &&
        evidence.active_registers.mair_el1 == mair_value_ &&
        active_regime.physical_address_mask ==
            physical_address_mask_;
    const std::uint64_t active_ttbr0_root =
        evidence.active_registers.ttbr0_el1 &
        physical_address_mask_;
    const std::uint64_t active_ttbr1_root =
        evidence.active_registers.ttbr1_el1 &
        physical_address_mask_;
    if (!active_regime_valid ||
        !created_ || pages_ == nullptr || page_count_ == 0 ||
        page_count_ > table_page_limit_ ||
        root_physical_address() == 0 ||
        (root_physical_address() & PAGE_MASK) != 0 ||
        (root_physical_address() & ~physical_address_mask_) != 0 ||
        root_physical_address() == active_ttbr0_root ||
        root_physical_address() == active_ttbr1_root) {
        set_check(VerificationCheck::candidate_root,
                  VerificationState::fail,
                  root_physical_address());
    } else {
        set_check(VerificationCheck::candidate_root,
                  VerificationState::pass);
    }

    const Status ownership_status = created_
        ? validate_owned_tree()
        : Status::not_created;
    set_check(
        VerificationCheck::owned_table_pages,
        ownership_status == Status::success
            ? VerificationState::pass
            : VerificationState::fail,
        ownership_status == Status::success
            ? 0
            : root_physical_address()
    );

    if (!has_regions) {
        set_check(VerificationCheck::required_roles,
                  VerificationState::unknown);
        set_check(VerificationCheck::mapped_regions,
                  VerificationState::unknown);
        set_check(VerificationCheck::physical_backing,
                  VerificationState::unknown);
        set_check(VerificationCheck::descriptor_policy,
                  VerificationState::unknown);
        set_check(VerificationCheck::alias_safety,
                  VerificationState::unknown);
        set_check(VerificationCheck::hhdm_coverage,
                  VerificationState::unknown);
        set_check(VerificationCheck::active_table_backing,
                  VerificationState::unknown);
        set_check(VerificationCheck::boot_object_lifetime,
                  evidence.boot_object_lifetime_established
                      ? VerificationState::unknown
                      : VerificationState::unknown);
        set_check(VerificationCheck::stack_continuity,
                  VerificationState::unknown);
        set_check(VerificationCheck::exception_vectors,
                  VerificationState::unknown);
        set_check(VerificationCheck::exception_dependencies,
                  VerificationState::unknown);
        set_check(VerificationCheck::pmm_accessibility,
                  VerificationState::unknown);
        set_check(VerificationCheck::unsupported_device_mappings,
                  VerificationState::unknown);
    } else {
        std::uint32_t present_roles = 0;
        bool regions_valid = true;
        bool backing_valid = true;
        bool descriptors_valid = true;
        bool aliases_valid = true;
        bool unsupported_device = false;
        for (std::uint32_t i = 0; i < region_count; ++i) {
            const MappingRegion& region = regions[i];
            present_roles |= region.role_mask;
            std::uint64_t virtual_end = 0;
            std::uint64_t physical_end = 0;
            const bool bounds_valid =
                region.size != 0 &&
                (region.virtual_base & PAGE_MASK) == 0 &&
                (region.physical_base & PAGE_MASK) == 0 &&
                (region.size & PAGE_MASK) == 0 &&
                add_without_overflow(
                    region.virtual_base, region.size, virtual_end) &&
                add_without_overflow(
                    region.physical_base, region.size, physical_end) &&
                virtual_end != 0 &&
                physical_end != 0 &&
                canonical_address(region.virtual_base) &&
                canonical_address(virtual_end - 1) &&
                canonical_address(region.virtual_base) ==
                    canonical_address(virtual_end - 1) &&
                ((region.virtual_base >> 47) & 1) ==
                    ((virtual_end - 1) >> 47 & 1);
            if (!bounds_valid) {
                regions_valid = false;
                backing_valid = false;
                descriptors_valid = false;
                aliases_valid = false;
                report.failing_address = region.virtual_base;
                continue;
            }
            if (region.memory_map_type != LIMINE_MEMMAP_USABLE &&
                region.memory_map_type !=
                    LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE &&
                region.memory_map_type !=
                    LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
                unsupported_device = true;
                backing_valid = false;
            } else if (!memory_map_range_has_type(
                           memory_map,
                           region.physical_base,
                           physical_end,
                           region.memory_map_type,
                           physical_address_mask_ + PAGE_SIZE)) {
                backing_valid = false;
                report.failing_address = region.physical_base;
            }
            if (!supported_policy(region.policy) ||
                region.attribute_index >= 8 ||
                static_cast<std::uint8_t>(
                    mair_value_ >> (region.attribute_index * 8)
                ) != REQUIRED_NORMAL_MAIR_BYTE ||
                !supported_policy(region.policy)) {
                descriptors_valid = false;
                report.failing_address = region.virtual_base;
            }
            for (std::uint32_t j = 0; j < i; ++j) {
                const MappingRegion& previous = regions[j];
                const std::uint64_t previous_virtual_end =
                    previous.virtual_base + previous.size;
                const std::uint64_t previous_physical_end =
                    previous.physical_base + previous.size;
                if (region.virtual_base < previous_virtual_end &&
                    previous.virtual_base < virtual_end) {
                    regions_valid = false;
                    report.failing_address = region.virtual_base;
                }
                if (region.physical_base < previous_physical_end &&
                    previous.physical_base < physical_end &&
                    (region.policy != previous.policy ||
                     region.attribute_index !=
                         previous.attribute_index)) {
                    aliases_valid = false;
                    report.failing_address = region.physical_base;
                }
            }

            for (std::uint64_t offset = 0;
                 offset < region.size;
                 offset += PAGE_SIZE) {
                const std::uint64_t virtual_address =
                    region.virtual_base + offset;
                const std::uint64_t physical_address =
                    region.physical_base + offset;
                const Translation translation =
                    translate(virtual_address);
                if (translation.status != Status::success ||
                    translation.physical_address != physical_address) {
                    regions_valid = false;
                    descriptors_valid = false;
                    report.failing_address = virtual_address;
                    break;
                }
                if (!memory_map_range_has_type(
                        memory_map,
                        physical_address,
                        physical_address + PAGE_SIZE,
                        region.memory_map_type,
                        physical_address_mask_ + PAGE_SIZE)) {
                    backing_valid = false;
                    report.failing_address = physical_address;
                }
                std::uint64_t table_physical =
                    root_physical_address();
                std::uint64_t* table_entries = nullptr;
                bool leaf_matches = false;
                if (checked_table_entries(
                        table_physical,
                        table_entries) == Status::success) {
                    for (std::uint8_t level = 0;
                         level < REQUIRED_LEVELS;
                         ++level) {
                        const std::uint64_t descriptor =
                            table_entries[table_index(
                                virtual_address, level)];
                        if (level == REQUIRED_LEVELS - 1) {
                            leaf_matches = descriptor ==
                                normal_page_descriptor(
                                    physical_address,
                                    region.policy,
                                    region.attribute_index
                                );
                            break;
                        }
                        table_physical =
                            descriptor & physical_address_mask_;
                        if (checked_table_entries(
                                table_physical,
                                table_entries) != Status::success)
                            break;
                    }
                }
                if (!leaf_matches) {
                    descriptors_valid = false;
                    report.failing_address = virtual_address;
                    break;
                }
            }
        }
        set_check(
            VerificationCheck::mapped_regions,
            regions_valid ? VerificationState::pass
                          : VerificationState::fail,
            report.failing_address
        );
        set_check(
            VerificationCheck::physical_backing,
            backing_valid ? VerificationState::pass
                          : VerificationState::fail,
            report.failing_address
        );
        set_check(
            VerificationCheck::descriptor_policy,
            descriptors_valid ? VerificationState::pass
                              : VerificationState::fail,
            report.failing_address
        );
        set_check(
            VerificationCheck::alias_safety,
            aliases_valid ? VerificationState::pass
                          : VerificationState::fail,
            report.failing_address
        );

        bool hhdm_regions_valid = true;
        bool found_hhdm_region = false;
        for (std::uint32_t i = 0; i < region_count; ++i) {
            const MappingRegion& region = regions[i];
            if ((region.role_mask & ROLE_HHDM_USABLE) == 0)
                continue;
            found_hhdm_region = true;
            if (region.memory_map_type != LIMINE_MEMMAP_USABLE ||
                region.virtual_base !=
                    evidence.hhdm_offset + region.physical_base ||
                !range_is_mapped(
                    region.virtual_base,
                    region.size,
                    region.physical_base)) {
                hhdm_regions_valid = false;
                report.failing_address = region.virtual_base;
            }
        }
        if (evidence.hhdm_inventory_complete &&
            valid_memory_map_response(
                memory_map,
                physical_address_mask_ == 0
                    ? 0
                    : 64 - __builtin_clzll(
                          physical_address_mask_ + PAGE_MASK))) {
            for (std::uint64_t i = 0;
                 i < memory_map->entry_count;
                 ++i) {
                const limine_memmap_entry* entry =
                    memory_map->entries[i];
                if (entry == nullptr ||
                    entry->type != LIMINE_MEMMAP_USABLE)
                    continue;
                if (entry->base >
                        INVALID_VALUE - evidence.hhdm_offset ||
                    !range_has_role(
                        evidence.hhdm_offset + entry->base,
                        entry->length,
                        ROLE_HHDM_USABLE,
                        entry->base) ||
                    !range_is_mapped(
                        evidence.hhdm_offset + entry->base,
                        entry->length,
                        entry->base)) {
                    hhdm_regions_valid = false;
                    report.failing_address = entry->base;
                }
            }
        }
        const VerificationState hhdm_state =
            !found_hhdm_region || !hhdm_regions_valid
                ? VerificationState::fail
                : evidence.hhdm_inventory_complete
                    ? VerificationState::pass
                    : VerificationState::unknown;
        set_check(VerificationCheck::hhdm_coverage, hhdm_state);

        bool owned_table_aliases_valid =
            ownership_status == Status::success;
        for (std::uint32_t i = 0;
             i < page_count_ && owned_table_aliases_valid;
             ++i) {
            const std::uint64_t physical =
                pages_[i].physical_address;
            if (physical > INVALID_VALUE - evidence.hhdm_offset ||
                !physical_range_is_usable(physical, PAGE_SIZE) ||
                !range_is_mapped(
                    evidence.hhdm_offset + physical,
                    PAGE_SIZE,
                    physical)) {
                owned_table_aliases_valid = false;
                report.failing_address = physical;
            }
        }
        if (owned_table_aliases_valid)
            present_roles |= ROLE_TABLE_PAGES;
        else
            set_check(
                VerificationCheck::owned_table_pages,
                VerificationState::fail,
                report.failing_address
            );
        if ((present_roles & (required_roles | ROLE_TABLE_PAGES)) !=
            (required_roles | ROLE_TABLE_PAGES))
            set_check(
                VerificationCheck::required_roles,
                VerificationState::fail
            );
        else
            set_check(
                VerificationCheck::required_roles,
                VerificationState::pass
            );

        VerificationState active_table_state =
            evidence.active_inventory_complete &&
                    evidence.active_table_pages != nullptr &&
                    evidence.active_table_page_count != 0
                ? VerificationState::pass
                : VerificationState::unknown;
        if (active_table_state == VerificationState::pass) {
            for (std::uint64_t i = 0;
                 i < evidence.active_table_page_count;
                 ++i) {
                const std::uint64_t physical =
                    evidence.active_table_pages[i];
                if ((physical & PAGE_MASK) != 0 ||
                    physical > INVALID_VALUE - evidence.hhdm_offset) {
                    active_table_state = VerificationState::fail;
                    report.failing_address = physical;
                    break;
                }
                const Translation translation = translate(
                    evidence.hhdm_offset + physical
                );
                if (
                    translation.status != Status::success ||
                    translation.physical_address != physical) {
                    active_table_state = VerificationState::fail;
                    report.failing_address = physical;
                    break;
                }
            }
        }
        set_check(
            VerificationCheck::active_table_backing,
            active_table_state,
            report.failing_address
        );
        set_check(
            VerificationCheck::boot_object_lifetime,
            evidence.boot_object_lifetime_established
                ? VerificationState::pass
                : VerificationState::unknown
        );

        const bool normal_stack_valid =
            evidence.normal_stack_size != 0 &&
            evidence.normal_stack_base >= evidence.hhdm_offset &&
            evidence.active_sp_el0 >= evidence.normal_stack_base &&
            evidence.normal_stack_base <=
                INVALID_VALUE - evidence.normal_stack_size &&
            evidence.active_sp_el0 <
                evidence.normal_stack_base +
                    evidence.normal_stack_size &&
            range_has_role(
                evidence.normal_stack_base,
                evidence.normal_stack_size,
                ROLE_STACK,
                evidence.normal_stack_base -
                    evidence.hhdm_offset
            ) &&
            range_is_mapped(
                evidence.normal_stack_base,
                evidence.normal_stack_size,
                evidence.normal_stack_base -
                    evidence.hhdm_offset) &&
            range_has_descriptors(
                evidence.normal_stack_base,
                evidence.normal_stack_base -
                    evidence.hhdm_offset,
                evidence.normal_stack_size,
                LeafPolicy::writable);
        const bool exception_stack_valid =
            evidence.exception_stack_size != 0 &&
            evidence.exception_stack_base >= evidence.hhdm_offset &&
            evidence.exception_stack_base <=
                INVALID_VALUE - evidence.exception_stack_size &&
            evidence.active_sp_el1 >= evidence.exception_stack_base &&
            evidence.active_sp_el1 <=
                evidence.exception_stack_base +
                    evidence.exception_stack_size &&
            range_has_role(
                evidence.exception_stack_base,
                evidence.exception_stack_size,
                ROLE_STACK,
                evidence.exception_stack_base -
                    evidence.hhdm_offset
            ) &&
            range_is_mapped(
                evidence.exception_stack_base,
                evidence.exception_stack_size,
                evidence.exception_stack_base -
                    evidence.hhdm_offset) &&
            range_has_descriptors(
                evidence.exception_stack_base,
                evidence.exception_stack_base -
                    evidence.hhdm_offset,
                evidence.exception_stack_size,
                LeafPolicy::writable);
        set_check(
            VerificationCheck::stack_continuity,
            normal_stack_valid && exception_stack_valid
                ? VerificationState::pass
                : VerificationState::fail,
            normal_stack_valid
                ? evidence.exception_stack_base
                : evidence.normal_stack_base
        );

        const auto vector_physical_base = [&]() {
            for (std::uint32_t i = 0;
                 i < region_count;
                 ++i) {
                const MappingRegion& region = regions[i];
                if ((region.role_mask & ROLE_EXCEPTION_VECTORS) != 0 &&
                    evidence.vector_base >= region.virtual_base &&
                    evidence.vector_base <
                        region.virtual_base + region.size)
                    return region.physical_base +
                        (evidence.vector_base - region.virtual_base);
            }
            return INVALID_VALUE;
        };
        const auto vector_mapping_size = [&]() {
            if (evidence.vector_size == 0 ||
                evidence.vector_base >
                    INVALID_VALUE - evidence.vector_size)
                return std::uint64_t{0};
            const std::uint64_t vector_end =
                evidence.vector_base + evidence.vector_size;
            if (vector_end >
                INVALID_VALUE - PAGE_MASK)
                return std::uint64_t{0};
            const std::uint64_t mapping_end =
                (vector_end + PAGE_MASK) & ~PAGE_MASK;
            const std::uint64_t mapping_base =
                evidence.vector_base & ~PAGE_MASK;
            return mapping_end > mapping_base
                ? mapping_end - mapping_base
                : std::uint64_t{0};
        };
        const std::uint64_t vector_physical =
            vector_physical_base();
        const std::uint64_t vector_pages_size =
            vector_mapping_size();
        const std::uint64_t vector_page_offset =
            evidence.vector_base & PAGE_MASK;
        const bool vector_page_alignment_valid =
            vector_physical != INVALID_VALUE &&
            (vector_physical & PAGE_MASK) == vector_page_offset;
        const bool vector_valid =
            evidence.vector_size != 0 &&
            evidence.vector_base == evidence.expected_vector_base &&
            (evidence.vector_base & 0x7ff) == 0 &&
            has_role(ROLE_EXCEPTION_VECTORS) &&
            vector_page_alignment_valid &&
            range_has_role(
                evidence.vector_base,
                evidence.vector_size,
                ROLE_EXCEPTION_VECTORS,
                vector_physical) &&
            range_is_mapped(
                evidence.vector_base,
                evidence.vector_size,
                vector_physical) &&
            range_has_descriptors(
                evidence.vector_base & ~PAGE_MASK,
                vector_physical & ~PAGE_MASK,
                vector_pages_size,
                LeafPolicy::kernel_text);
        set_check(
            VerificationCheck::exception_vectors,
            vector_valid ? VerificationState::pass
                          : VerificationState::fail,
            evidence.vector_base
        );
        constexpr std::uint32_t EXCEPTION_DEPENDENCY_COUNT = 3;
        constexpr std::uint32_t REQUIRED_EXCEPTION_DEPENDENCY_ROLES =
            ROLE_KERNEL_TEXT |
            ROLE_KERNEL_RODATA |
            ROLE_KERNEL_WRITABLE;
        const auto dependency_memory_map_type = [&](
            const ExceptionDependency& dependency
        ) {
            if (dependency.virtual_base >
                    INVALID_VALUE - dependency.size ||
                dependency.physical_base >
                    INVALID_VALUE - dependency.size)
                return INVALID_VALUE;
            const std::uint64_t virtual_end =
                dependency.virtual_base + dependency.size;
            const std::uint64_t physical_end =
                dependency.physical_base + dependency.size;
            for (std::uint32_t i = 0; i < region_count; ++i) {
                const MappingRegion& region = regions[i];
                if ((region.role_mask & dependency.role_mask) == 0 ||
                    region.policy != dependency.policy ||
                    region.attribute_index != NORMAL_ATTRIBUTE_INDEX ||
                    region.virtual_base >
                        INVALID_VALUE - region.size ||
                    region.physical_base >
                        INVALID_VALUE - region.size)
                    continue;
                const std::uint64_t region_virtual_end =
                    region.virtual_base + region.size;
                const std::uint64_t region_physical_end =
                    region.physical_base + region.size;
                if (region.virtual_base <=
                        dependency.virtual_base &&
                    region_virtual_end >= virtual_end &&
                    region.physical_base <=
                        dependency.physical_base &&
                    region_physical_end >= physical_end &&
                    dependency.physical_base -
                            region.physical_base ==
                        dependency.virtual_base -
                            region.virtual_base)
                    return region.memory_map_type;
            }
            return INVALID_VALUE;
        };
        VerificationState exception_dependency_state =
            evidence.exception_dependencies == nullptr ||
                    evidence.exception_dependency_count <
                        EXCEPTION_DEPENDENCY_COUNT
                ? VerificationState::unknown
                : VerificationState::pass;
        if (evidence.exception_dependency_count >
            EXCEPTION_DEPENDENCY_COUNT)
            exception_dependency_state = VerificationState::fail;
        if (exception_dependency_state == VerificationState::pass) {
            std::uint32_t dependency_roles = 0;
            for (std::uint32_t i = 0;
                 i < evidence.exception_dependency_count;
                 ++i) {
                const ExceptionDependency& dependency =
                    evidence.exception_dependencies[i];
                LeafPolicy expected_policy{};
                if (dependency.role_mask == ROLE_KERNEL_TEXT)
                    expected_policy = LeafPolicy::kernel_text;
                else if (dependency.role_mask == ROLE_KERNEL_RODATA)
                    expected_policy = LeafPolicy::read_only;
                else if (dependency.role_mask == ROLE_KERNEL_WRITABLE)
                    expected_policy = LeafPolicy::writable;
                else {
                    exception_dependency_state =
                        VerificationState::fail;
                    break;
                }

                std::uint64_t virtual_end = 0;
                std::uint64_t physical_end = 0;
                const std::uint64_t memory_map_type =
                    dependency_memory_map_type(dependency);
                if ((dependency_roles & dependency.role_mask) != 0 ||
                    dependency.policy != expected_policy ||
                    dependency.size == 0 ||
                    (dependency.virtual_base & PAGE_MASK) != 0 ||
                    (dependency.physical_base & PAGE_MASK) != 0 ||
                    (dependency.size & PAGE_MASK) != 0 ||
                    !add_without_overflow(
                        dependency.virtual_base,
                        dependency.size,
                        virtual_end) ||
                    !add_without_overflow(
                        dependency.physical_base,
                        dependency.size,
                        physical_end) ||
                    !canonical_address(dependency.virtual_base) ||
                    !canonical_address(virtual_end - 1) ||
                    canonical_address(dependency.virtual_base) !=
                        canonical_address(virtual_end - 1) ||
                    ((dependency.virtual_base >> 47) & 1) !=
                        ((virtual_end - 1) >> 47 & 1) ||
                    !range_has_role(
                        dependency.virtual_base,
                        dependency.size,
                        dependency.role_mask,
                        dependency.physical_base) ||
                    !range_is_mapped(
                        dependency.virtual_base,
                        dependency.size,
                        dependency.physical_base) ||
                    !range_has_descriptors(
                        dependency.virtual_base,
                        dependency.physical_base,
                        dependency.size,
                        dependency.policy) ||
                    memory_map_type == INVALID_VALUE ||
                    !memory_map_range_has_type(
                        memory_map,
                        dependency.physical_base,
                        physical_end,
                        memory_map_type,
                        physical_address_mask_ + PAGE_SIZE)) {
                    exception_dependency_state =
                        VerificationState::fail;
                    report.failing_address =
                        dependency.virtual_base;
                    break;
                }
                dependency_roles |= dependency.role_mask;
            }
            if (exception_dependency_state == VerificationState::pass &&
                dependency_roles !=
                    REQUIRED_EXCEPTION_DEPENDENCY_ROLES)
                exception_dependency_state = VerificationState::fail;
        }
        set_check(
            VerificationCheck::exception_dependencies,
            exception_dependency_state,
            report.failing_address
        );

        const bool pmm_metadata_valid =
            evidence.pmm_metadata_size != 0 &&
            evidence.pmm_metadata_base <=
                INVALID_VALUE - evidence.hhdm_offset &&
            evidence.pmm_metadata_size <=
                INVALID_VALUE -
                    (evidence.hhdm_offset +
                     evidence.pmm_metadata_base) &&
            range_has_role(
                evidence.hhdm_offset +
                    evidence.pmm_metadata_base,
                evidence.pmm_metadata_size,
                ROLE_PMM_METADATA,
                evidence.pmm_metadata_base) &&
            range_is_mapped(
                evidence.hhdm_offset +
                    evidence.pmm_metadata_base,
                evidence.pmm_metadata_size,
                evidence.pmm_metadata_base) &&
            physical_range_is_usable(
                evidence.pmm_metadata_base,
                evidence.pmm_metadata_size);
        set_check(
            VerificationCheck::pmm_accessibility,
            pmm_metadata_valid ? VerificationState::pass
                               : VerificationState::fail,
            evidence.pmm_metadata_base
        );

        const bool capacity_valid =
            page_count_ <= table_page_limit_ &&
            evidence.required_additional_table_pages <=
                table_page_limit_ - page_count_;
        set_check(
            VerificationCheck::remaining_capacity,
            capacity_valid
                ? evidence.additional_capacity_requirement_known
                    ? VerificationState::pass
                    : VerificationState::unknown
                : VerificationState::fail,
            page_count_
        );
        set_check(
            VerificationCheck::unsupported_device_mappings,
            unsupported_device ? VerificationState::fail
                               : VerificationState::pass
        );
    }

    bool any_fail = false;
    bool any_unknown = false;
    for (const VerificationState state : report.checks) {
        any_fail = any_fail || state == VerificationState::fail;
        any_unknown = any_unknown || state == VerificationState::unknown;
    }
    report.overall = any_fail
        ? VerificationState::fail
        : any_unknown
            ? VerificationState::unknown
            : VerificationState::pass;
    return report;
}

const char* status_name(Status status) {
    switch (status) {
        case Status::success: return "success";
        case Status::already_created: return "already created";
        case Status::not_created: return "not created";
        case Status::unsupported_regime: return "unsupported translation regime";
        case Status::unsupported_normal_attribute:
            return "unsupported Normal-memory MAIR attribute";
        case Status::invalid_virtual_address:
            return "invalid virtual address";
        case Status::invalid_physical_address:
            return "invalid or unowned physical page";
        case Status::mapping_exists: return "mapping already exists";
        case Status::not_mapped: return "virtual address is not mapped";
        case Status::allocation_failure:
            return "physical page allocation failed";
        case Status::ownership_capacity_exceeded:
            return "owned table-page capacity exceeded";
        case Status::invalid_ownership:
            return "invalid table-page ownership";
        case Status::malformed_descriptor:
            return "malformed translation descriptor";
        case Status::rollback_failed:
            return "failed to roll back partial operation";
        case Status::unsupported_policy:
            return "unsupported mapping policy";
        case Status::invalid_attribute_index:
            return "invalid Normal-memory attribute index";
        case Status::invalid_memory_map:
            return "invalid physical memory map";
        case Status::invalid_region:
            return "invalid mapping region";
        case Status::virtual_overlap:
            return "overlapping virtual mapping regions";
        case Status::conflicting_alias:
            return "conflicting physical alias";
        case Status::missing_required_mapping:
            return "required mapping role is missing";
        case Status::verification_failed:
            return "pre-activation verification failed";
    }
    return "unknown status";
}

const char* verification_state_name(VerificationState state) {
    switch (state) {
        case VerificationState::pass: return "PASS";
        case VerificationState::fail: return "FAIL";
        case VerificationState::unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

const char* verification_check_name(VerificationCheck check) {
    switch (check) {
        case VerificationCheck::candidate_root: return "candidate-root";
        case VerificationCheck::owned_table_pages: return "owned-table-pages";
        case VerificationCheck::required_roles: return "required-roles";
        case VerificationCheck::mapped_regions: return "mapped-regions";
        case VerificationCheck::physical_backing: return "physical-backing";
        case VerificationCheck::descriptor_policy: return "descriptor-policy";
        case VerificationCheck::alias_safety: return "alias-safety";
        case VerificationCheck::hhdm_coverage: return "hhdm-coverage";
        case VerificationCheck::active_table_backing:
            return "active-table-backing";
        case VerificationCheck::boot_object_lifetime:
            return "boot-object-lifetime";
        case VerificationCheck::stack_continuity:
            return "stack-continuity";
        case VerificationCheck::exception_vectors:
            return "exception-vectors";
        case VerificationCheck::exception_dependencies:
            return "exception-dependencies";
        case VerificationCheck::pmm_accessibility:
            return "pmm-accessibility";
        case VerificationCheck::remaining_capacity:
            return "remaining-capacity";
        case VerificationCheck::unsupported_device_mappings:
            return "unsupported-device-mappings";
        case VerificationCheck::count: return "count";
    }
    return "unknown-check";
}

Status build_kernel_mappings(
    Hierarchy& hierarchy,
    const Paging::Regime& regime,
    std::uint64_t mair_value,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    const MappingRegion* regions,
    std::uint32_t region_count,
    std::uint32_t required_roles
) {
    if (!hierarchy.created_)
        return Status::not_created;
    if (hierarchy.physical_address_mask_ != regime.physical_address_mask ||
        hierarchy.mair_value_ != mair_value ||
        !supported_regime(regime))
        return Status::unsupported_regime;
    if (regions == nullptr || region_count == 0 ||
        region_count > MAX_KERNEL_MAPPING_REGIONS ||
        !valid_memory_map_response(
            memory_map,
            regime.physical_address_bits) ||
        (hhdm_offset & PAGE_MASK) != 0 ||
        !canonical_address(hhdm_offset) ||
        hhdm_offset < INVALID_VALUE -
            ((std::uint64_t{1} << REQUIRED_VIRTUAL_ADDRESS_BITS) - 1))
        return Status::invalid_memory_map;

    const std::uint64_t physical_limit =
        std::uint64_t{1} << regime.physical_address_bits;
    std::uint32_t present_roles = 0;
    std::uint64_t virtual_ends[MAX_KERNEL_MAPPING_REGIONS]{};
    std::uint64_t physical_ends[MAX_KERNEL_MAPPING_REGIONS]{};
    for (std::uint32_t i = 0; i < region_count; ++i) {
        const MappingRegion& region = regions[i];
        if (!supported_policy(region.policy))
            return Status::unsupported_policy;
        if (region.size == 0 ||
            region.role_mask == 0 ||
            (region.virtual_base & PAGE_MASK) != 0 ||
            (region.physical_base & PAGE_MASK) != 0 ||
            (region.size & PAGE_MASK) != 0)
            return Status::invalid_region;
        if (region.attribute_index >= 8 ||
            static_cast<std::uint8_t>(
                mair_value >> (region.attribute_index * 8)
            ) != REQUIRED_NORMAL_MAIR_BYTE)
            return Status::invalid_attribute_index;
        if (!add_without_overflow(
                region.virtual_base,
                region.size,
                virtual_ends[i]) ||
            !add_without_overflow(
                region.physical_base,
                region.size,
                physical_ends[i]) ||
            physical_ends[i] > physical_limit)
            return Status::invalid_region;
        const std::uint64_t last_virtual = virtual_ends[i] - 1;
        if (!valid_virtual_page(region.virtual_base) ||
            !valid_virtual_page(last_virtual & ~PAGE_MASK) ||
            canonical_address(region.virtual_base) !=
                canonical_address(last_virtual) ||
            ((region.virtual_base >> 47) & 1) !=
                ((last_virtual >> 47) & 1))
            return Status::invalid_region;
        if (!supported_memory_map_type(region.memory_map_type) ||
            !memory_map_range_has_type(
                memory_map,
                region.physical_base,
                physical_ends[i],
                region.memory_map_type,
                physical_limit))
            return Status::invalid_memory_map;

        present_roles |= region.role_mask;
        for (std::uint32_t j = 0; j < i; ++j) {
            const bool virtual_overlap =
                region.virtual_base < virtual_ends[j] &&
                regions[j].virtual_base < virtual_ends[i];
            if (virtual_overlap)
                return Status::virtual_overlap;

            const bool physical_overlap =
                region.physical_base < physical_ends[j] &&
                regions[j].physical_base < physical_ends[i];
            if (physical_overlap &&
                (region.policy != regions[j].policy ||
                 region.attribute_index !=
                     regions[j].attribute_index))
                return Status::conflicting_alias;
        }
    }
    if (((present_roles | ROLE_TABLE_PAGES) & required_roles) !=
        required_roles)
        return Status::missing_required_mapping;

    auto rollback = [&](Status cause) {
        return hierarchy.destroy() == Status::success
            ? cause
            : Status::rollback_failed;
    };
    for (std::uint32_t i = 0; i < region_count; ++i) {
        const MappingRegion& region = regions[i];
        for (std::uint64_t offset = 0;
             offset < region.size;
             offset += PAGE_SIZE) {
            const Status map_status = hierarchy.map_page_internal(
                region.virtual_base + offset,
                region.physical_base + offset,
                region.policy,
                region.attribute_index,
                false
            );
            if (map_status != Status::success)
                return rollback(map_status);
        }
    }

    const auto existing_leaf_matches = [&](std::uint64_t virtual_address,
                                           std::uint64_t physical_address,
                                           LeafPolicy policy,
                                           std::uint8_t attribute_index) {
        std::uint64_t current_physical =
            hierarchy.pages_[0].physical_address;
        std::uint64_t* entries = nullptr;
        if (hierarchy.checked_table_entries(current_physical, entries) !=
            Status::success)
            return false;
        for (std::uint8_t level = 0; level < REQUIRED_LEVELS; ++level) {
            const std::uint64_t descriptor =
                entries[table_index(virtual_address, level)];
            if (descriptor == 0 ||
                (descriptor & DESCRIPTOR_VALID) == 0)
                return false;
            if (level == REQUIRED_LEVELS - 1)
                return descriptor == normal_page_descriptor(
                    physical_address,
                    policy,
                    attribute_index
                );
            current_physical =
                descriptor & hierarchy.physical_address_mask_;
            if (descriptor !=
                    (current_physical | TABLE_DESCRIPTOR) ||
                hierarchy.checked_table_entries(
                    current_physical,
                    entries) != Status::success)
                return false;
        }
        return false;
    };

    for (std::uint32_t page_index = 0;
         page_index < hierarchy.page_count_;
         ++page_index) {
        const std::uint64_t table_physical =
            hierarchy.pages_[page_index].physical_address;
        if (!memory_map_range_has_type(
                memory_map,
                table_physical,
                table_physical + PAGE_SIZE,
                LIMINE_MEMMAP_USABLE,
                physical_limit))
            return rollback(Status::invalid_memory_map);
        if (table_physical >
            ~std::uint64_t{0} - hhdm_offset)
            return rollback(Status::invalid_region);
        const std::uint64_t table_virtual =
            hhdm_offset + table_physical;

        for (std::uint32_t i = 0; i < region_count; ++i) {
            const MappingRegion& region = regions[i];
            const bool physical_overlap =
                table_physical < physical_ends[i] &&
                region.physical_base < table_physical + PAGE_SIZE;
            if (physical_overlap &&
                (region.policy != LeafPolicy::writable ||
                 region.attribute_index != NORMAL_ATTRIBUTE_INDEX))
                return rollback(Status::conflicting_alias);

            const bool virtual_overlap =
                table_virtual < virtual_ends[i] &&
                region.virtual_base < table_virtual + PAGE_SIZE;
            if (virtual_overlap &&
                !existing_leaf_matches(
                    table_virtual,
                    table_physical,
                    LeafPolicy::writable,
                    NORMAL_ATTRIBUTE_INDEX))
                return rollback(Status::conflicting_alias);
        }

        if (existing_leaf_matches(
                table_virtual,
                table_physical,
                LeafPolicy::writable,
                NORMAL_ATTRIBUTE_INDEX))
            continue;
        const Status alias_status = hierarchy.map_page_internal(
            table_virtual,
            table_physical,
            LeafPolicy::writable,
            NORMAL_ATTRIBUTE_INDEX,
            false
        );
        if (alias_status != Status::success)
            return rollback(alias_status);
    }
    return Status::success;
}

std::uint32_t run_synthetic_tests() {
    for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i)
        synthetic_provider.allocated[i] = false;
    synthetic_provider.allocation_budget = -1;
    synthetic_provider.force_allocation_once = false;
    synthetic_provider.forced_physical_address = 0;
    synthetic_provider.fail_release_once = false;
    synthetic_provider.release_failure_countdown = -1;
    const Hierarchy::PageProvider provider{
        &synthetic_provider,
        synthetic_allocate,
        synthetic_release,
        synthetic_writable_pointer
    };
    const Paging::Regime regime = synthetic_regime();
    std::uint32_t failure = 0;
    Hierarchy hierarchy;

    if (!expect(
            hierarchy.create_with_provider(
                regime,
                0x00ff,
                provider) == Status::unsupported_normal_attribute,
            1,
            failure))
        return failure;
    auto unsupported = regime;
    unsupported.levels = 3;
    if (!expect(
            hierarchy.create_with_provider(
                unsupported,
                0xffff,
                provider) == Status::unsupported_regime,
            2,
            failure))
        return failure;

    synthetic_provider.allocation_budget = 0;
    if (!expect(
            hierarchy.create_with_provider(
                regime,
                0xffff,
                provider) == Status::allocation_failure &&
            hierarchy.owned_table_page_count() == 0,
            3,
            failure))
        return failure;
    synthetic_provider.allocation_budget = -1;

    const std::uint64_t unrelated_page =
        synthetic_allocate(&synthetic_provider);
    const std::uint64_t leaf_pages[4] = {
        synthetic_allocate(&synthetic_provider),
        synthetic_allocate(&synthetic_provider),
        synthetic_allocate(&synthetic_provider),
        synthetic_allocate(&synthetic_provider)
    };
    if (!expect(
            unrelated_page != 0 &&
            leaf_pages[0] != 0 &&
            leaf_pages[1] != 0 &&
            leaf_pages[2] != 0 &&
            leaf_pages[3] != 0,
            4,
            failure))
        return failure;
    const std::uint32_t baseline_allocated = 5;

    if (!expect(
            hierarchy.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            hierarchy.owned_table_page_count() == 1 &&
            hierarchy.root_physical_address() != 0 &&
            hierarchy.create_with_provider(
                regime,
                0xffff,
                provider) == Status::already_created,
            5,
            failure))
        return failure;
    const std::uint64_t root = hierarchy.root_physical_address();
    if (!expect(
            root != unrelated_page &&
            root != leaf_pages[0] &&
            hierarchy.translate(SYNTHETIC_VIRTUAL_BASE).status ==
                Status::not_mapped,
            6,
            failure))
        return failure;
    Hierarchy empty;
    if (!expect(
            empty.create_with_provider(regime, 0xffff, provider) ==
                Status::success &&
            empty.owned_table_page_count() == 1 &&
            empty.destroy() == Status::success,
            25,
            failure))
        return failure;

    Hierarchy duplicate_allocation;
    if (!expect(
            duplicate_allocation.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success,
            27,
            failure))
        return failure;
    synthetic_provider.force_allocation_once = true;
    synthetic_provider.forced_physical_address =
        duplicate_allocation.root_physical_address();
    if (!expect(
            duplicate_allocation.map_page(
                SYNTHETIC_VIRTUAL_BASE,
                leaf_pages[0]) == Status::invalid_ownership &&
            duplicate_allocation.owned_table_page_count() == 1 &&
            synthetic_provider.allocated[
                (duplicate_allocation.root_physical_address() -
                 SYNTHETIC_PHYSICAL_BASE) / PAGE_SIZE
            ] &&
            duplicate_allocation.destroy() == Status::success,
            28,
            failure))
        return failure;

    if (!expect(
            hierarchy.map_page(
                SYNTHETIC_VIRTUAL_BASE,
                leaf_pages[0]) == Status::success &&
            hierarchy.map_page(
                SYNTHETIC_VIRTUAL_BASE,
                leaf_pages[0]) == Status::mapping_exists &&
            hierarchy.map_page(
                SYNTHETIC_VIRTUAL_BASE,
                leaf_pages[1]) == Status::mapping_exists,
            7,
            failure))
        return failure;
    const Translation first_start =
        hierarchy.translate(SYNTHETIC_VIRTUAL_BASE);
    const Translation first_middle =
        hierarchy.translate(SYNTHETIC_VIRTUAL_BASE + 0x321);
    const Translation first_end =
        hierarchy.translate(SYNTHETIC_VIRTUAL_BASE + PAGE_SIZE - 1);
    if (!expect(
            first_start.status == Status::success &&
            first_start.physical_address == leaf_pages[0] &&
            first_middle.status == Status::success &&
            first_middle.physical_address == leaf_pages[0] + 0x321 &&
            first_end.status == Status::success &&
            first_end.physical_address ==
                leaf_pages[0] + PAGE_SIZE - 1,
            8,
            failure))
        return failure;
    const std::uint64_t first_leaf_descriptor =
        hierarchy.pages_[3].entries[
            table_index(SYNTHETIC_VIRTUAL_BASE, REQUIRED_LEVELS - 1)
        ];
    if (!expect(
            first_leaf_descriptor ==
                normal_page_descriptor(
                    leaf_pages[0],
                    LeafPolicy::writable,
                    NORMAL_ATTRIBUTE_INDEX) &&
            ((first_leaf_descriptor >> 2) & 0x7) ==
                NORMAL_ATTRIBUTE_INDEX &&
            ((first_leaf_descriptor >> 6) & 0x3) == 0 &&
            ((first_leaf_descriptor >> 8) & 0x3) == 3 &&
            ((first_leaf_descriptor >> 10) & 1) == 1 &&
            ((first_leaf_descriptor >> 53) & 1) == 1 &&
            ((first_leaf_descriptor >> 54) & 1) == 1,
            43,
            failure))
        return failure;
    if (!expect(
            hierarchy.map_page(SYNTHETIC_VIRTUAL_BASE + 1, leaf_pages[1]) ==
                Status::invalid_virtual_address &&
            hierarchy.map_page(
                0x0001000000000000ull,
                leaf_pages[1]) == Status::invalid_virtual_address &&
            hierarchy.map_page(
                SYNTHETIC_VIRTUAL_BASE,
                leaf_pages[1] + 1) == Status::invalid_physical_address &&
            hierarchy.map_page(
                SYNTHETIC_VIRTUAL_BASE,
                root) == Status::invalid_physical_address &&
            hierarchy.map_page(
                SYNTHETIC_VIRTUAL_BASE + 3 * PAGE_SIZE,
                SYNTHETIC_PHYSICAL_BASE +
                    20 * PAGE_SIZE) ==
                Status::invalid_physical_address &&
            hierarchy.map_page(
                SYNTHETIC_VIRTUAL_BASE + 4 * PAGE_SIZE,
                std::uint64_t{1} << regime.physical_address_bits) ==
                Status::invalid_physical_address &&
            hierarchy.translate(
                0x0001000000000000ull
            ).status == Status::invalid_virtual_address,
            9,
            failure))
        return failure;

    const std::uint64_t lowest_canonical_page = 0;
    const std::uint64_t highest_canonical_page =
        0xfffffffffffff000ull;
    Hierarchy boundary_hierarchy;
    const std::uint64_t allocations_before_boundaries = [&]() {
        std::uint64_t count = 0;
        for (bool allocated : synthetic_provider.allocated) {
            if (allocated)
                ++count;
        }
        return count;
    }();
    if (!expect(
            boundary_hierarchy.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            boundary_hierarchy.map_page(
                lowest_canonical_page,
                leaf_pages[1]) == Status::success &&
            boundary_hierarchy.map_page(
                highest_canonical_page,
                leaf_pages[2]) == Status::success &&
            boundary_hierarchy.translate(0).status == Status::success &&
            boundary_hierarchy.translate(0).physical_address ==
                leaf_pages[1] &&
            boundary_hierarchy.translate(
                highest_canonical_page + PAGE_SIZE - 1
            ).physical_address == leaf_pages[2] + PAGE_SIZE - 1 &&
            boundary_hierarchy.translate(
                highest_canonical_page
            ).status == Status::success &&
            boundary_hierarchy.unmap_page(1) ==
                Status::invalid_virtual_address &&
            boundary_hierarchy.unmap_page(
                highest_canonical_page + 1
            ) == Status::invalid_virtual_address &&
            boundary_hierarchy.destroy() == Status::success,
            29,
            failure))
        return failure;
    std::uint64_t allocations_after_boundaries = 0;
    for (bool allocated : synthetic_provider.allocated) {
        if (allocated)
            ++allocations_after_boundaries;
    }
    if (!expect(
            allocations_after_boundaries ==
                allocations_before_boundaries,
            30,
            failure))
        return failure;

    bool allocations_before_pruning_failure[SYNTHETIC_PAGE_COUNT]{};
    for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i)
        allocations_before_pruning_failure[i] =
            synthetic_provider.allocated[i];
    Hierarchy pruning_failure;
    if (!expect(
            pruning_failure.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            pruning_failure.map_page(
                SYNTHETIC_VIRTUAL_BASE,
                leaf_pages[3]) == Status::success &&
            pruning_failure.owned_table_page_count() == 4,
            46,
            failure))
        return failure;
    synthetic_provider.fail_release_once = true;
    if (!expect(
            pruning_failure.unmap_page(SYNTHETIC_VIRTUAL_BASE) ==
                Status::rollback_failed &&
            pruning_failure.translate(SYNTHETIC_VIRTUAL_BASE).status ==
                Status::not_mapped &&
            pruning_failure.owned_table_page_count() == 4 &&
            synthetic_provider.allocated[
                (leaf_pages[3] - SYNTHETIC_PHYSICAL_BASE) / PAGE_SIZE
            ] &&
            pruning_failure.destroy() == Status::success,
            47,
            failure))
        return failure;
    for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i) {
        if (!expect(
                synthetic_provider.allocated[i] ==
                    allocations_before_pruning_failure[i],
                48,
                failure))
            return failure;
    }

    Hierarchy malformed;
    if (!expect(
            malformed.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            malformed.map_page(
                SYNTHETIC_VIRTUAL_BASE,
                leaf_pages[0]) == Status::success,
            44,
            failure))
        return failure;
    const std::uint16_t malformed_root_index =
        table_index(SYNTHETIC_VIRTUAL_BASE, 0);
    const std::uint64_t saved_table_descriptor =
        malformed.pages_[0].entries[malformed_root_index];
    malformed.pages_[0].entries[malformed_root_index] |=
        std::uint64_t{1} << 55;
    if (!expect(
            malformed.translate(SYNTHETIC_VIRTUAL_BASE).status ==
                Status::malformed_descriptor &&
            malformed.map_page(
                SYNTHETIC_VIRTUAL_BASE + PAGE_SIZE,
                leaf_pages[1]) == Status::malformed_descriptor &&
            malformed.unmap_page(SYNTHETIC_VIRTUAL_BASE) ==
                Status::malformed_descriptor &&
            malformed.destroy() == Status::malformed_descriptor &&
            malformed.owned_table_page_count() == 4,
            45,
            failure))
        return failure;
    malformed.pages_[0].entries[malformed_root_index] =
        saved_table_descriptor;
    auto* malformed_leaf_table = malformed.pages_[3].entries;
    const std::uint16_t malformed_leaf_index =
        table_index(SYNTHETIC_VIRTUAL_BASE, REQUIRED_LEVELS - 1);
    const std::uint64_t saved_leaf_descriptor =
        malformed_leaf_table[malformed_leaf_index];
    malformed_leaf_table[malformed_leaf_index] |=
        std::uint64_t{1} << 55;
    if (!expect(
            malformed.translate(SYNTHETIC_VIRTUAL_BASE).status ==
                Status::malformed_descriptor &&
            malformed.unmap_page(SYNTHETIC_VIRTUAL_BASE) ==
                Status::malformed_descriptor &&
            malformed.destroy() == Status::malformed_descriptor,
            46,
            failure))
        return failure;
    malformed_leaf_table[malformed_leaf_index] = saved_leaf_descriptor;
    if (!expect(
            malformed.destroy() == Status::success,
            47,
            failure))
        return failure;

    if (!expect(
            hierarchy.map_page(
                SYNTHETIC_VIRTUAL_BASE + PAGE_SIZE,
                leaf_pages[1]) == Status::success &&
            hierarchy.map_page(
                SYNTHETIC_VIRTUAL_BASE + 2 * PAGE_SIZE,
                leaf_pages[2]) == Status::success &&
            hierarchy.unmap_page(
                SYNTHETIC_VIRTUAL_BASE + 3 * PAGE_SIZE) ==
                Status::not_mapped &&
            hierarchy.owned_table_page_count() == 4 &&
            hierarchy.map_page(
                SYNTHETIC_OTHER_ROOT_BASE,
                leaf_pages[3]) == Status::success &&
            hierarchy.owned_table_page_count() == 7,
            10,
            failure))
        return failure;
    const Translation other_root =
        hierarchy.translate(SYNTHETIC_OTHER_ROOT_BASE + 0x555);
    if (!expect(
            other_root.status == Status::success &&
            other_root.physical_address == leaf_pages[3] + 0x555,
            11,
            failure))
        return failure;

    if (!expect(
            hierarchy.unmap_page(SYNTHETIC_VIRTUAL_BASE + PAGE_SIZE) ==
                Status::success &&
            hierarchy.owned_table_page_count() == 7 &&
            hierarchy.translate(SYNTHETIC_VIRTUAL_BASE + PAGE_SIZE).status ==
                Status::not_mapped &&
            hierarchy.unmap_page(SYNTHETIC_VIRTUAL_BASE + PAGE_SIZE) ==
                Status::not_mapped &&
            hierarchy.map_page(
                SYNTHETIC_VIRTUAL_BASE + PAGE_SIZE,
                leaf_pages[1]) == Status::success,
            12,
            failure))
        return failure;

    if (!expect(
            hierarchy.unmap_page(SYNTHETIC_VIRTUAL_BASE) ==
                Status::success &&
            hierarchy.owned_table_page_count() == 7 &&
            hierarchy.unmap_page(
                SYNTHETIC_VIRTUAL_BASE + PAGE_SIZE) ==
                Status::success &&
            hierarchy.owned_table_page_count() == 7 &&
            hierarchy.unmap_page(
                SYNTHETIC_VIRTUAL_BASE + 2 * PAGE_SIZE) ==
                Status::success &&
            hierarchy.owned_table_page_count() == 4 &&
            hierarchy.unmap_page(SYNTHETIC_OTHER_ROOT_BASE) ==
                Status::success &&
            hierarchy.owned_table_page_count() == 1,
            13,
            failure))
        return failure;
    if (!expect(
            hierarchy.destroy() == Status::success &&
            !hierarchy.is_created() &&
            hierarchy.owned_table_page_count() == 0 &&
            hierarchy.destroy() == Status::not_created,
            14,
            failure))
        return failure;

    if (!expect(
            synthetic_provider.allocated[0] &&
            synthetic_provider.allocated[1] &&
            synthetic_provider.allocated[2] &&
            synthetic_provider.allocated[3] &&
            synthetic_provider.allocated[4] &&
            !synthetic_provider.allocated[5],
            15,
            failure))
        return failure;

    for (std::int32_t allocations_before_failure = 0;
         allocations_before_failure < 3;
         ++allocations_before_failure) {
        Hierarchy failed_map;
        synthetic_provider.allocation_budget = -1;
        if (failed_map.create_with_provider(
                regime,
                0xffff,
                provider) != Status::success)
            return 16;
        const std::uint32_t allocated_before =
            baseline_allocated +
            failed_map.owned_table_page_count();
        bool allocated_snapshot[SYNTHETIC_PAGE_COUNT]{};
        for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i)
            allocated_snapshot[i] = synthetic_provider.allocated[i];
        synthetic_provider.allocation_budget =
            allocations_before_failure;
        if (!expect(
                failed_map.map_page(
                    SYNTHETIC_VIRTUAL_BASE,
                    leaf_pages[0]) == Status::allocation_failure &&
                failed_map.owned_table_page_count() == 1 &&
                failed_map.translate(SYNTHETIC_VIRTUAL_BASE).status !=
                    Status::success,
                17,
                failure))
            return failure;
        std::uint32_t allocated_now = 0;
        for (bool allocated : synthetic_provider.allocated) {
            if (allocated)
                ++allocated_now;
        }
        if (!expect(allocated_now == allocated_before, 18, failure))
            return failure;
        for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i) {
            if (!expect(
                    synthetic_provider.allocated[i] ==
                        allocated_snapshot[i],
                    31,
                    failure))
                return failure;
        }
        synthetic_provider.allocation_budget = -1;
        if (!expect(
                failed_map.destroy() == Status::success,
                19,
                failure))
            return failure;
    }
    synthetic_provider.allocation_budget = -1;

    Hierarchy rollback_release_failure;
    bool rollback_failure_snapshot[SYNTHETIC_PAGE_COUNT]{};
    for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i)
        rollback_failure_snapshot[i] = synthetic_provider.allocated[i];
    if (!expect(
            rollback_release_failure.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success,
            39,
            failure))
        return failure;
    synthetic_provider.allocation_budget = 2;
    synthetic_provider.fail_release_once = true;
    if (!expect(
            rollback_release_failure.map_page(
                SYNTHETIC_VIRTUAL_BASE,
                leaf_pages[0]) == Status::rollback_failed &&
            rollback_release_failure.owned_table_page_count() == 3,
            40,
            failure))
        return failure;
    synthetic_provider.allocation_budget = -1;
    if (!expect(
            rollback_release_failure.destroy() == Status::success,
            41,
            failure))
        return failure;
    for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i) {
        if (!expect(
                synthetic_provider.allocated[i] ==
                    rollback_failure_snapshot[i],
                42,
                failure))
            return failure;
    }

    bool destroy_failure_snapshot[SYNTHETIC_PAGE_COUNT]{};
    for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i)
        destroy_failure_snapshot[i] = synthetic_provider.allocated[i];
    Hierarchy destroy_release_failure;
    if (!expect(
            destroy_release_failure.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            destroy_release_failure.map_page(
                SYNTHETIC_VIRTUAL_BASE,
                leaf_pages[0]) == Status::success &&
            destroy_release_failure.owned_table_page_count() == 4,
            49,
            failure))
        return failure;
    synthetic_provider.release_failure_countdown = 1;
    if (!expect(
            destroy_release_failure.destroy() ==
                Status::rollback_failed &&
            destroy_release_failure.is_created() &&
            destroy_release_failure.owned_table_page_count() == 3 &&
            destroy_release_failure.translate(
                SYNTHETIC_VIRTUAL_BASE
            ).status == Status::not_mapped &&
            destroy_release_failure.destroy() == Status::success,
            50,
            failure))
        return failure;
    for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i) {
        if (!expect(
                synthetic_provider.allocated[i] ==
                    destroy_failure_snapshot[i],
                51,
                failure))
            return failure;
    }

    Hierarchy existing_branch;
    if (!expect(
            existing_branch.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            existing_branch.map_page(
                SYNTHETIC_VIRTUAL_BASE,
                leaf_pages[0]) == Status::success,
            32,
            failure))
        return failure;
    bool existing_branch_snapshot[SYNTHETIC_PAGE_COUNT]{};
    for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i)
        existing_branch_snapshot[i] = synthetic_provider.allocated[i];
    synthetic_provider.allocation_budget = 1;
    const std::uint64_t second_branch =
        SYNTHETIC_VIRTUAL_BASE + (std::uint64_t{1} << 30);
    if (!expect(
            existing_branch.map_page(
                second_branch,
                leaf_pages[1]) == Status::allocation_failure &&
            existing_branch.owned_table_page_count() == 4,
            33,
            failure))
        return failure;
    for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i) {
        if (!expect(
                synthetic_provider.allocated[i] ==
                    existing_branch_snapshot[i],
                34,
                failure))
            return failure;
    }
    synthetic_provider.allocation_budget = -1;
    if (!expect(
            existing_branch.map_page(
                second_branch,
                leaf_pages[1]) == Status::success &&
            existing_branch.owned_table_page_count() == 6 &&
            existing_branch.destroy() == Status::success,
            35,
            failure))
        return failure;

    Hierarchy capacity_hierarchy;
    const bool capacity_setup_valid =
        capacity_hierarchy.create_with_provider(
            regime,
            0xffff,
            provider,
            4) == Status::success &&
        capacity_hierarchy.map_page(
            SYNTHETIC_VIRTUAL_BASE,
            leaf_pages[0]) == Status::success;
    if (!expect(
            capacity_setup_valid &&
            capacity_hierarchy.owned_table_page_count() == 4 &&
            capacity_hierarchy.map_page(
                SYNTHETIC_OTHER_ROOT_BASE,
                leaf_pages[1]) == Status::ownership_capacity_exceeded &&
            capacity_hierarchy.owned_table_page_count() ==
                4 &&
            capacity_hierarchy.translate(
                SYNTHETIC_OTHER_ROOT_BASE).status ==
                Status::not_mapped,
            36,
            failure))
        return failure;
    if (!expect(
            capacity_hierarchy.unmap_page(
                SYNTHETIC_VIRTUAL_BASE) == Status::success &&
            capacity_hierarchy.owned_table_page_count() == 1 &&
            capacity_hierarchy.map_page(
                SYNTHETIC_OTHER_ROOT_BASE,
                leaf_pages[1]) == Status::success &&
            capacity_hierarchy.owned_table_page_count() == 4 &&
            capacity_hierarchy.translate(
                SYNTHETIC_OTHER_ROOT_BASE).status ==
                Status::success &&
            capacity_hierarchy.destroy() == Status::success,
            37,
            failure))
        return failure;

    Hierarchy corrupt;
    if (!expect(
            corrupt.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            corrupt.map_page(
                SYNTHETIC_VIRTUAL_BASE,
                leaf_pages[0]) == Status::success,
            20,
            failure))
        return failure;
    std::uint64_t* root_entries =
        corrupt.pages_[0].entries;
    const std::uint16_t root_index =
        table_index(SYNTHETIC_VIRTUAL_BASE, 0);
    const std::uint64_t root_descriptor = root_entries[root_index];
    root_entries[root_index + 1] = root_descriptor;
    const std::uint32_t before_invalid_destroy = [&]() {
        std::uint32_t count = 0;
        for (bool allocated : synthetic_provider.allocated) {
            if (allocated)
                ++count;
        }
        return count;
    }();
    if (!expect(
            corrupt.destroy() == Status::invalid_ownership,
            21,
            failure))
        return failure;
    std::uint32_t after_invalid_destroy = 0;
    for (bool allocated : synthetic_provider.allocated) {
        if (allocated)
            ++after_invalid_destroy;
    }
    root_entries[root_index + 1] = 0;
    std::uint64_t* saved_root_pointer = corrupt.pages_[0].entries;
    corrupt.pages_[0].entries = nullptr;
    if (!expect(
            corrupt.destroy() == Status::invalid_ownership,
            26,
            failure))
        return failure;
    corrupt.pages_[0].entries = saved_root_pointer;
    if (!expect(
            before_invalid_destroy == after_invalid_destroy &&
            corrupt.destroy() == Status::success,
            22,
            failure))
        return failure;

    if (!expect(
            synthetic_release(&synthetic_provider, unrelated_page) &&
            synthetic_release(&synthetic_provider, leaf_pages[0]) &&
            synthetic_release(&synthetic_provider, leaf_pages[1]) &&
            synthetic_release(&synthetic_provider, leaf_pages[2]) &&
            synthetic_release(&synthetic_provider, leaf_pages[3]),
            23,
            failure))
        return failure;
    for (bool allocated : synthetic_provider.allocated) {
        if (allocated)
            return 24;
    }
    return 0;
}

std::uint32_t run_builder_synthetic_tests() {
    for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i)
        synthetic_provider.allocated[i] = false;
    synthetic_provider.allocation_budget = -1;
    synthetic_provider.force_allocation_once = false;
    synthetic_provider.fail_release_once = false;
    synthetic_provider.release_failure_countdown = -1;

    const Hierarchy::PageProvider provider{
        &synthetic_provider,
        synthetic_allocate,
        synthetic_release,
        synthetic_writable_pointer
    };
    const Paging::Regime regime = synthetic_regime();
    constexpr std::uint64_t HHDM_OFFSET = 0xffff000000000000ull;
    limine_memmap_entry map_entry{
        0,
        0x10000000,
        LIMINE_MEMMAP_USABLE
    };
    limine_memmap_entry* map_entries[] = {&map_entry};
    limine_memmap_response memory_map{
        0,
        1,
        map_entries
    };

    const MappingRegion valid_regions[] = {
        {
            SYNTHETIC_VIRTUAL_BASE,
            0x400000,
            PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_KERNEL_REQUESTS,
            LeafPolicy::read_only,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            SYNTHETIC_VIRTUAL_BASE + 0x100000,
            0x500000,
            2 * PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_KERNEL_TEXT,
            LeafPolicy::kernel_text,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            SYNTHETIC_VIRTUAL_BASE + 0x200000,
            0x600000,
            PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_KERNEL_RODATA,
            LeafPolicy::read_only,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            SYNTHETIC_VIRTUAL_BASE + 0x300000,
            0x700000,
            2 * PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_KERNEL_WRITABLE,
            LeafPolicy::writable,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            HHDM_OFFSET + 0x800000,
            0x800000,
            PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_STACK,
            LeafPolicy::writable,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            HHDM_OFFSET + 0x900000,
            0x900000,
            PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_PMM_METADATA | ROLE_BOOT_DATA,
            LeafPolicy::writable,
            NORMAL_ATTRIBUTE_INDEX
        }
    };
    constexpr std::uint32_t REQUIRED_ROLES =
        ROLE_KERNEL_REQUESTS |
        ROLE_KERNEL_TEXT |
        ROLE_KERNEL_RODATA |
        ROLE_KERNEL_WRITABLE |
        ROLE_STACK |
        ROLE_BOOT_DATA |
        ROLE_PMM_METADATA |
        ROLE_TABLE_PAGES;
    std::uint32_t failure = 0;
    auto expect = [&](bool condition, std::uint32_t code) {
        if (!condition)
            failure = code;
        return condition;
    };

    Hierarchy hierarchy;
    if (!expect(
            hierarchy.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success,
            100))
        return failure;
    const Status build_status = build_kernel_mappings(
        hierarchy,
        regime,
        0xffff,
        HHDM_OFFSET,
        &memory_map,
        valid_regions,
        sizeof(valid_regions) / sizeof(valid_regions[0]),
        REQUIRED_ROLES
    );
    if (!expect(
            build_status == Status::success,
            200 + static_cast<std::uint32_t>(build_status)))
        return failure;

    const auto leaf_descriptor = [&](std::uint64_t virtual_address) {
        std::uint64_t physical =
            hierarchy.pages_[0].physical_address;
        std::uint64_t* entries = nullptr;
        if (hierarchy.checked_table_entries(physical, entries) !=
            Status::success)
            return std::uint64_t{0};
        for (std::uint8_t level = 0; level < REQUIRED_LEVELS; ++level) {
            const std::uint64_t descriptor =
                entries[table_index(virtual_address, level)];
            if (level == REQUIRED_LEVELS - 1)
                return descriptor;
            physical = descriptor & hierarchy.physical_address_mask_;
            if (hierarchy.checked_table_entries(physical, entries) !=
                Status::success)
                return std::uint64_t{0};
        }
        return std::uint64_t{0};
    };
    const std::uint64_t text_va =
        valid_regions[1].virtual_base;
    const std::uint64_t rodata_va =
        valid_regions[2].virtual_base;
    const std::uint64_t writable_va =
        valid_regions[3].virtual_base;
    const auto policy_bits_valid = [&](std::uint64_t virtual_address,
                                       std::uint64_t physical_address,
                                       LeafPolicy policy) {
        const std::uint64_t descriptor =
            leaf_descriptor(virtual_address);
        const std::uint64_t expected_ap =
            policy == LeafPolicy::writable ? 0 : 2;
        return descriptor == normal_page_descriptor(
                   physical_address,
                   policy,
                   NORMAL_ATTRIBUTE_INDEX) &&
            ((descriptor >> 6) & 0x3) == expected_ap &&
            ((descriptor >> 53) & 1) ==
                (policy == LeafPolicy::kernel_text ? 0 : 1) &&
            ((descriptor >> 54) & 1) == 1;
    };
    bool complete_coverage = true;
    for (const MappingRegion& region : valid_regions) {
        for (std::uint64_t offset = 0; offset < region.size;
             offset += PAGE_SIZE) {
            const Translation translation =
                hierarchy.translate(region.virtual_base + offset);
            complete_coverage = complete_coverage &&
                translation.status == Status::success &&
                translation.physical_address ==
                    region.physical_base + offset;
        }
    }
    bool table_aliases_valid = true;
    for (std::uint32_t i = 0;
         i < hierarchy.owned_table_page_count();
         ++i) {
        const std::uint64_t physical =
            hierarchy.pages_[i].physical_address;
        const Translation alias = hierarchy.translate(
            HHDM_OFFSET + physical
        );
        table_aliases_valid = table_aliases_valid &&
            alias.status == Status::success &&
            alias.physical_address == physical &&
            leaf_descriptor(HHDM_OFFSET + physical) ==
                normal_page_descriptor(
                    physical,
                    LeafPolicy::writable,
                    NORMAL_ATTRIBUTE_INDEX
                );
    }
    if (!expect(
            complete_coverage &&
            table_aliases_valid &&
            hierarchy.translate(text_va + PAGE_SIZE).physical_address ==
                0x501000 &&
            hierarchy.translate(rodata_va).physical_address == 0x600000 &&
            hierarchy.translate(writable_va).physical_address == 0x700000 &&
            hierarchy.translate(
                SYNTHETIC_VIRTUAL_BASE + 0x310000
            ).status == Status::not_mapped &&
            leaf_descriptor(valid_regions[0].virtual_base) ==
                normal_page_descriptor(
                    valid_regions[0].physical_base,
                    LeafPolicy::read_only,
                    NORMAL_ATTRIBUTE_INDEX
                ) &&
            leaf_descriptor(text_va) ==
                normal_page_descriptor(
                    valid_regions[1].physical_base,
                    LeafPolicy::kernel_text,
                    NORMAL_ATTRIBUTE_INDEX
                ) &&
            leaf_descriptor(rodata_va) ==
                normal_page_descriptor(
                    valid_regions[2].physical_base,
                    LeafPolicy::read_only,
                    NORMAL_ATTRIBUTE_INDEX
                ) &&
            leaf_descriptor(writable_va) ==
                normal_page_descriptor(
                    valid_regions[3].physical_base,
                    LeafPolicy::writable,
                    NORMAL_ATTRIBUTE_INDEX
                ) &&
            policy_bits_valid(
                text_va,
                valid_regions[1].physical_base,
                LeafPolicy::kernel_text) &&
            policy_bits_valid(
                rodata_va,
                valid_regions[2].physical_base,
                LeafPolicy::read_only) &&
            policy_bits_valid(
                writable_va,
                valid_regions[3].physical_base,
                LeafPolicy::writable),
            102))
        return failure;
    if (!expect(hierarchy.destroy() == Status::success, 103))
        return failure;
    for (bool allocated : synthetic_provider.allocated) {
        if (!expect(!allocated, 104))
            return failure;
    }

    limine_memmap_entry sparse_entries[] = {
        {
            0x200000,
            0x400000,
            LIMINE_MEMMAP_USABLE
        },
        {
            0x600000,
            0x200000,
            LIMINE_MEMMAP_RESERVED
        },
        {
            0x800000,
            0x200000,
            LIMINE_MEMMAP_USABLE
        }
    };
    limine_memmap_entry* sparse_entry_pointers[] = {
        &sparse_entries[0],
        &sparse_entries[1],
        &sparse_entries[2]
    };
    limine_memmap_response sparse_memory_map{
        0,
        3,
        sparse_entry_pointers
    };
    const MappingRegion sparse_regions[] = {
        {
            HHDM_OFFSET + 0x400000,
            0x400000,
            0x200000,
            LIMINE_MEMMAP_USABLE,
            ROLE_PMM_METADATA,
            LeafPolicy::writable,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            HHDM_OFFSET + 0x800000,
            0x800000,
            0x200000,
            LIMINE_MEMMAP_USABLE,
            ROLE_PMM_METADATA,
            LeafPolicy::writable,
            NORMAL_ATTRIBUTE_INDEX
        }
    };
    Hierarchy sparse_hierarchy;
    if (!expect(
            sparse_hierarchy.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            build_kernel_mappings(
                sparse_hierarchy,
                regime,
                0xffff,
                HHDM_OFFSET,
                &sparse_memory_map,
                sparse_regions,
                2,
                ROLE_PMM_METADATA | ROLE_TABLE_PAGES
            ) == Status::success,
            125))
        return failure;
    const Translation sparse_first =
        sparse_hierarchy.translate(HHDM_OFFSET + 0x400000);
    const Translation sparse_last =
        sparse_hierarchy.translate(HHDM_OFFSET + 0x9ff000);
    const Translation sparse_reserved =
        sparse_hierarchy.translate(HHDM_OFFSET + 0x600000);
    const Translation sparse_before =
        sparse_hierarchy.translate(HHDM_OFFSET + 0x3ff000);
    const auto sparse_leaf_descriptor = [&](std::uint64_t virtual_address) {
        std::uint64_t physical =
            sparse_hierarchy.pages_[0].physical_address;
        std::uint64_t* entries = nullptr;
        if (sparse_hierarchy.checked_table_entries(
                physical,
                entries) != Status::success)
            return std::uint64_t{0};
        for (std::uint8_t level = 0; level < REQUIRED_LEVELS; ++level) {
            const std::uint64_t descriptor =
                entries[table_index(virtual_address, level)];
            if (level == REQUIRED_LEVELS - 1)
                return descriptor;
            physical =
                descriptor & sparse_hierarchy.physical_address_mask_;
            if (sparse_hierarchy.checked_table_entries(
                    physical,
                    entries) != Status::success)
                return std::uint64_t{0};
        }
        return std::uint64_t{0};
    };
    if (!expect(
            sparse_first.status == Status::success &&
            sparse_first.physical_address == 0x400000 &&
            sparse_last.status == Status::success &&
            sparse_last.physical_address == 0x9ff000 &&
            sparse_reserved.status == Status::not_mapped &&
            sparse_before.status == Status::not_mapped &&
            sparse_leaf_descriptor(HHDM_OFFSET + 0x400000) ==
                normal_page_descriptor(
                    0x400000,
                    LeafPolicy::writable,
                    NORMAL_ATTRIBUTE_INDEX) &&
            sparse_leaf_descriptor(HHDM_OFFSET + 0x9ff000) ==
                normal_page_descriptor(
                    0x9ff000,
                    LeafPolicy::writable,
                    NORMAL_ATTRIBUTE_INDEX) &&
            sparse_hierarchy.owned_table_page_count() <=
                MAX_OWNED_TABLE_PAGES &&
            sparse_hierarchy.destroy() == Status::success,
            126))
        return failure;
    for (bool allocated : synthetic_provider.allocated) {
        if (!expect(!allocated, 127))
            return failure;
    }

    const auto expect_rejected =
        [&](const MappingRegion* regions,
            std::uint32_t region_count,
            std::uint32_t required_roles,
            Status expected,
            std::uint32_t code) {
            Hierarchy test_hierarchy;
            if (test_hierarchy.create_with_provider(
                    regime,
                    0xffff,
                    provider) != Status::success)
                return expect(false, code);
            const Status result = build_kernel_mappings(
                test_hierarchy,
                regime,
                0xffff,
                HHDM_OFFSET,
                &memory_map,
                regions,
                region_count,
                required_roles
            );
            const bool clean = test_hierarchy.destroy() ==
                Status::success;
            return expect(result == expected && clean, code);
        };

    MappingRegion invalid = valid_regions[0];
    invalid.attribute_index = 8;
    if (!expect_rejected(
            &invalid, 1, 0, Status::invalid_attribute_index, 105))
        return failure;
    invalid = valid_regions[0];
    invalid.attribute_index = 0;
    if (!expect_rejected(
            &invalid, 1, 0, Status::success, 106))
        return failure;
    invalid = valid_regions[0];
    invalid.policy = LeafPolicy::writable_executable;
    if (!expect_rejected(
            &invalid, 1, 0, Status::unsupported_policy, 107))
        return failure;
    invalid = valid_regions[0];
    invalid.virtual_base = 0x0001000000000000ull;
    if (!expect_rejected(
            &invalid, 1, 0, Status::invalid_region, 108))
        return failure;
    invalid = valid_regions[0];
    invalid.virtual_base = 0xfffffffffffff000ull;
    invalid.size = 2 * PAGE_SIZE;
    if (!expect_rejected(
            &invalid, 1, 0, Status::invalid_region, 109))
        return failure;
    invalid = valid_regions[0];
    invalid.physical_base += 1;
    if (!expect_rejected(
            &invalid, 1, 0, Status::invalid_region, 110))
        return failure;
    invalid = valid_regions[0];
    invalid.physical_base = 0x10000000;
    invalid.size = PAGE_SIZE;
    if (!expect_rejected(
            &invalid, 1, 0, Status::invalid_memory_map, 111))
        return failure;
    invalid = valid_regions[0];
    invalid.physical_base = 0xfffffffffffff000ull;
    invalid.size = 2 * PAGE_SIZE;
    if (!expect_rejected(
            &invalid, 1, 0, Status::invalid_region, 120))
        return failure;
    invalid = valid_regions[0];
    invalid.memory_map_type = LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE;
    if (!expect_rejected(
            &invalid, 1, 0, Status::invalid_memory_map, 121))
        return failure;
    invalid = valid_regions[0];
    invalid.memory_map_type = LIMINE_MEMMAP_FRAMEBUFFER;
    if (!expect_rejected(
            &invalid, 1, 0, Status::invalid_memory_map, 128))
        return failure;
    Hierarchy invalid_hhdm;
    if (!expect(
            invalid_hhdm.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            build_kernel_mappings(
                invalid_hhdm,
                regime,
                0xffff,
                0,
                &memory_map,
                &valid_regions[0],
                1,
                0
            ) == Status::invalid_memory_map &&
            invalid_hhdm.destroy() == Status::success,
            123))
        return failure;
    limine_memmap_entry overlapping_map_entry{
        0x800000,
        PAGE_SIZE,
        LIMINE_MEMMAP_RESERVED
    };
    limine_memmap_entry* overlapping_map_entries[] = {
        &map_entry,
        &overlapping_map_entry
    };
    limine_memmap_response overlapping_memory_map{
        0,
        2,
        overlapping_map_entries
    };
    Hierarchy overlapping_memory_map_hierarchy;
    if (!expect(
            overlapping_memory_map_hierarchy.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            build_kernel_mappings(
                overlapping_memory_map_hierarchy,
                regime,
                0xffff,
                HHDM_OFFSET,
                &overlapping_memory_map,
                &valid_regions[0],
                1,
                0
            ) == Status::invalid_memory_map &&
            overlapping_memory_map_hierarchy.destroy() ==
                Status::success,
            124))
        return failure;
    limine_memmap_entry boundary_map_entry{
        (std::uint64_t{1} << regime.physical_address_bits) - PAGE_SIZE,
        2 * PAGE_SIZE,
        LIMINE_MEMMAP_USABLE
    };
    limine_memmap_entry* boundary_map_entries[] = {
        &boundary_map_entry
    };
    limine_memmap_response boundary_memory_map{
        0,
        1,
        boundary_map_entries
    };
    Hierarchy boundary_memory_map_hierarchy;
    if (!expect(
            boundary_memory_map_hierarchy.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            build_kernel_mappings(
                boundary_memory_map_hierarchy,
                regime,
                0xffff,
                HHDM_OFFSET,
                &boundary_memory_map,
                &valid_regions[0],
                1,
                0
            ) == Status::invalid_memory_map &&
            boundary_memory_map_hierarchy.destroy() ==
                Status::success,
            129))
        return failure;
    if (!expect_rejected(
            valid_regions,
            sizeof(valid_regions) / sizeof(valid_regions[0]),
            REQUIRED_ROLES | ROLE_DTB,
            Status::missing_required_mapping,
            112))
        return failure;

    MappingRegion overlaps[2] = {
        valid_regions[0],
        valid_regions[0]
    };
    if (!expect_rejected(
            overlaps, 2, 0, Status::virtual_overlap, 113))
        return failure;
    MappingRegion conflicting_aliases[2] = {
        valid_regions[0],
        valid_regions[0]
    };
    conflicting_aliases[1].virtual_base += 0x400000;
    conflicting_aliases[1].policy = LeafPolicy::writable;
    if (!expect_rejected(
            conflicting_aliases,
            2,
            0,
            Status::conflicting_alias,
            114))
        return failure;
    auto unsupported_regime = regime;
    unsupported_regime.levels = 3;
    Hierarchy regime_hierarchy;
    if (!expect(
            regime_hierarchy.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            build_kernel_mappings(
                regime_hierarchy,
                unsupported_regime,
                0xffff,
                HHDM_OFFSET,
                &memory_map,
                valid_regions,
                sizeof(valid_regions) / sizeof(valid_regions[0]),
                0
            ) == Status::unsupported_regime &&
            regime_hierarchy.destroy() == Status::success,
            115))
        return failure;
    auto unsupported_pa_width = regime;
    unsupported_pa_width.physical_address_bits = 49;
    unsupported_pa_width.physical_address_mask =
        ((std::uint64_t{1} << 49) - 1) & ~PAGE_MASK;
    Hierarchy pa_width_hierarchy;
    if (!expect(
            pa_width_hierarchy.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success &&
            build_kernel_mappings(
                pa_width_hierarchy,
                unsupported_pa_width,
                0xffff,
                HHDM_OFFSET,
                &memory_map,
                valid_regions,
                sizeof(valid_regions) / sizeof(valid_regions[0]),
                0
            ) == Status::unsupported_regime &&
            pa_width_hierarchy.destroy() == Status::success,
            122))
        return failure;
    Hierarchy mair_hierarchy;
    MappingRegion invalid_mair_region = valid_regions[0];
    invalid_mair_region.attribute_index = 0;
    if (!expect(
            mair_hierarchy.create_with_provider(
                regime,
                0xff00,
                provider) == Status::success &&
            build_kernel_mappings(
                mair_hierarchy,
                regime,
                0xff00,
                HHDM_OFFSET,
                &memory_map,
                &invalid_mair_region,
                1,
                0
            ) == Status::invalid_attribute_index &&
            mair_hierarchy.destroy() == Status::success,
            116))
        return failure;

    Hierarchy rollback_hierarchy;
    if (!expect(
            rollback_hierarchy.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success,
            117))
        return failure;
    synthetic_provider.allocation_budget = 3;
    synthetic_provider.fail_release_once = true;
    const MappingRegion rollback_regions[] = {
        valid_regions[1],
        {
            SYNTHETIC_OTHER_ROOT_BASE,
            0xA00000,
            PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_KERNEL_RODATA,
            LeafPolicy::read_only,
            NORMAL_ATTRIBUTE_INDEX
        }
    };
    const Status rollback_status = build_kernel_mappings(
        rollback_hierarchy,
        regime,
        0xffff,
        HHDM_OFFSET,
        &memory_map,
        rollback_regions,
        2,
        ROLE_KERNEL_TEXT | ROLE_KERNEL_RODATA
    );
    synthetic_provider.allocation_budget = -1;
    if (!expect(
            rollback_status == Status::rollback_failed &&
            rollback_hierarchy.is_created() &&
            rollback_hierarchy.destroy() == Status::success,
            118))
        return failure;
    for (bool allocated : synthetic_provider.allocated) {
        if (!expect(!allocated, 119))
            return failure;
    }
    return 0;
}

std::uint32_t run_pre_activation_verifier_tests() {
    for (std::uint32_t i = 0; i < SYNTHETIC_PAGE_COUNT; ++i)
        synthetic_provider.allocated[i] = false;
    synthetic_provider.allocation_budget = -1;
    synthetic_provider.force_allocation_once = false;
    synthetic_provider.fail_release_once = false;
    synthetic_provider.release_failure_countdown = -1;

    const Hierarchy::PageProvider provider{
        &synthetic_provider,
        synthetic_allocate,
        synthetic_release,
        synthetic_writable_pointer
    };
    const Paging::Regime regime = synthetic_regime();
    constexpr std::uint64_t HHDM_OFFSET = 0xffff000000000000ull;
    limine_memmap_entry map_entry{
        0,
        0x10000000,
        LIMINE_MEMMAP_USABLE
    };
    limine_memmap_entry* map_entries[] = {&map_entry};
    limine_memmap_response memory_map{
        0,
        1,
        map_entries
    };
    MappingRegion regions[] = {
        {
            SYNTHETIC_VIRTUAL_BASE,
            0x400000,
            PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_KERNEL_REQUESTS,
            LeafPolicy::read_only,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            SYNTHETIC_VIRTUAL_BASE + 0x100000,
            0x500000,
            2 * PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_KERNEL_TEXT | ROLE_EXCEPTION_VECTORS,
            LeafPolicy::kernel_text,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            SYNTHETIC_VIRTUAL_BASE + 0x200000,
            0x600000,
            PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_KERNEL_RODATA,
            LeafPolicy::read_only,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            SYNTHETIC_VIRTUAL_BASE + 0x300000,
            0x700000,
            2 * PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_KERNEL_WRITABLE | ROLE_EXCEPTION_DATA,
            LeafPolicy::writable,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            HHDM_OFFSET + 0x800000,
            0x800000,
            PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_STACK,
            LeafPolicy::writable,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            HHDM_OFFSET + 0x810000,
            0x810000,
            PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_STACK,
            LeafPolicy::writable,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            HHDM_OFFSET + 0x900000,
            0x900000,
            PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_PMM_METADATA | ROLE_BOOT_DATA,
            LeafPolicy::writable,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            HHDM_OFFSET + SYNTHETIC_PHYSICAL_BASE,
            SYNTHETIC_PHYSICAL_BASE,
            SYNTHETIC_PAGE_COUNT * PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_HHDM_USABLE,
            LeafPolicy::writable,
            NORMAL_ATTRIBUTE_INDEX
        },
        {
            HHDM_OFFSET + 0xa00000,
            0xa00000,
            PAGE_SIZE,
            LIMINE_MEMMAP_USABLE,
            ROLE_HHDM_USABLE | ROLE_ACTIVE_TABLES,
            LeafPolicy::writable,
            NORMAL_ATTRIBUTE_INDEX
        }
    };
    constexpr std::uint32_t REGION_COUNT =
        sizeof(regions) / sizeof(regions[0]);
    const ExceptionDependency exception_dependencies[] = {
        {
            regions[1].virtual_base,
            regions[1].physical_base,
            regions[1].size,
            ROLE_KERNEL_TEXT,
            LeafPolicy::kernel_text
        },
        {
            regions[2].virtual_base,
            regions[2].physical_base,
            regions[2].size,
            ROLE_KERNEL_RODATA,
            LeafPolicy::read_only
        },
        {
            regions[3].virtual_base,
            regions[3].physical_base,
            regions[3].size,
            ROLE_KERNEL_WRITABLE,
            LeafPolicy::writable
        }
    };
    constexpr std::uint32_t REQUIRED_ROLES =
        ROLE_KERNEL_REQUESTS |
        ROLE_KERNEL_TEXT |
        ROLE_KERNEL_RODATA |
        ROLE_KERNEL_WRITABLE |
        ROLE_STACK |
        ROLE_BOOT_DATA |
        ROLE_PMM_METADATA |
        ROLE_HHDM_USABLE |
        ROLE_EXCEPTION_VECTORS |
        ROLE_EXCEPTION_DATA;
    const std::uint64_t active_pages[] = {0xa00000};
    VerificationEvidence evidence{};
    evidence.hhdm_offset = HHDM_OFFSET;
    evidence.active_registers = {
        4,
        0x30d0199d,
        0x4b5103510,
        0x1000,
        0x3000,
        0xffff,
        0x1124
    };
    evidence.active_table_pages = active_pages;
    evidence.active_table_page_count = 1;
    evidence.active_inventory_complete = true;
    evidence.hhdm_inventory_complete = false;
    evidence.boot_object_lifetime_established = true;
    evidence.exception_dependencies = exception_dependencies;
    evidence.exception_dependency_count =
        sizeof(exception_dependencies) /
        sizeof(exception_dependencies[0]);
    evidence.vector_base = SYNTHETIC_VIRTUAL_BASE + 0x100000;
    evidence.expected_vector_base = evidence.vector_base;
    evidence.vector_size = 2048;
    evidence.normal_stack_base = HHDM_OFFSET + 0x800000;
    evidence.normal_stack_size = PAGE_SIZE;
    evidence.exception_stack_base = HHDM_OFFSET + 0x810000;
    evidence.exception_stack_size = PAGE_SIZE;
    evidence.active_sp_el0 = evidence.normal_stack_base + 0x800;
    evidence.active_sp_el1 =
        evidence.exception_stack_base + PAGE_SIZE;
    evidence.pmm_metadata_base = 0x900000;
    evidence.pmm_metadata_size = PAGE_SIZE;
    evidence.required_additional_table_pages =
        regime.levels - 1;
    evidence.additional_capacity_requirement_known = true;

    std::uint32_t failure = 0;
    const auto expect = [&](bool condition, std::uint32_t code) {
        if (!condition)
            failure = code;
        return condition;
    };
    Hierarchy hierarchy;
    if (!expect(
            hierarchy.create_with_provider(
                regime,
                0xffff,
                provider) == Status::success,
            1))
        return failure;
    const Status build_status = build_kernel_mappings(
        hierarchy,
        regime,
        0xffff,
        HHDM_OFFSET,
        &memory_map,
        regions,
        REGION_COUNT,
        REQUIRED_ROLES
    );
    if (!expect(build_status == Status::success, 2))
        return failure;

    const auto verify = [&](const MappingRegion* candidate_regions,
                            std::uint32_t candidate_region_count,
                            const VerificationEvidence& candidate_evidence,
                            std::uint32_t required_roles =
                                REQUIRED_ROLES) {
        return hierarchy.verify_candidate(
            &memory_map,
            candidate_regions,
            candidate_region_count,
            required_roles,
            candidate_evidence
        );
    };
    const auto check_state = [&](const VerificationReport& report,
                                 VerificationCheck check,
                                 VerificationState expected) {
        return report.checks[static_cast<std::uint32_t>(check)] ==
                expected &&
            (expected == VerificationState::pass ||
             report.overall != VerificationState::pass);
    };

    VerificationReport report =
        verify(regions, REGION_COUNT, evidence);
    if (!expect(
            report.overall == VerificationState::unknown, 40))
        return failure;
    const VerificationCheck expected_pass_checks[] = {
        VerificationCheck::candidate_root,
        VerificationCheck::owned_table_pages,
        VerificationCheck::required_roles,
        VerificationCheck::mapped_regions,
        VerificationCheck::physical_backing,
        VerificationCheck::descriptor_policy,
        VerificationCheck::alias_safety,
        VerificationCheck::active_table_backing,
        VerificationCheck::stack_continuity,
        VerificationCheck::exception_vectors,
        VerificationCheck::exception_dependencies,
        VerificationCheck::pmm_accessibility,
        VerificationCheck::remaining_capacity,
        VerificationCheck::unsupported_device_mappings
    };
    for (std::uint32_t i = 0;
         i < sizeof(expected_pass_checks) /
                 sizeof(expected_pass_checks[0]);
         ++i) {
        if (!expect(
                check_state(
                    report,
                    expected_pass_checks[i],
                    VerificationState::pass),
                41 + i))
            return failure;
    }

    MappingRegion changed_regions[REGION_COUNT + 1]{};
    for (std::uint32_t i = 0; i < REGION_COUNT; ++i)
        changed_regions[i] = regions[i];
    changed_regions[1].role_mask &= ~ROLE_KERNEL_TEXT;
    report = verify(changed_regions, REGION_COUNT, evidence);
    if (!expect(
            check_state(
                report,
                VerificationCheck::required_roles,
                VerificationState::fail),
            4))
        return failure;
    if (!expect(
            check_state(
                report,
                VerificationCheck::exception_dependencies,
                VerificationState::fail),
            28))
        return failure;
    for (std::uint32_t i = 0; i < REGION_COUNT; ++i)
        changed_regions[i] = regions[i];

    VerificationEvidence bad_stack = evidence;
    bad_stack.normal_stack_base += 2 * PAGE_SIZE;
    report = verify(regions, REGION_COUNT, bad_stack);
    if (!expect(
            check_state(
                report,
                VerificationCheck::stack_continuity,
                VerificationState::fail),
            5))
        return failure;

    VerificationEvidence bad_vector = evidence;
    bad_vector.vector_base += PAGE_SIZE;
    report = verify(regions, REGION_COUNT, bad_vector);
    if (!expect(
            check_state(
                report,
                VerificationCheck::exception_vectors,
                VerificationState::fail),
            6))
        return failure;

    MappingRegion missing_vectors[REGION_COUNT]{};
    for (std::uint32_t i = 0; i < REGION_COUNT; ++i)
        missing_vectors[i] = regions[i];
    missing_vectors[1].role_mask &= ~ROLE_EXCEPTION_VECTORS;
    report = verify(missing_vectors, REGION_COUNT, evidence);
    if (!expect(
            check_state(
                report,
                VerificationCheck::exception_vectors,
                VerificationState::fail),
            29))
        return failure;

    VerificationEvidence incomplete_dependencies = evidence;
    incomplete_dependencies.exception_dependency_count = 2;
    report = verify(regions, REGION_COUNT, incomplete_dependencies);
    if (!expect(
            check_state(
                report,
                VerificationCheck::exception_dependencies,
                VerificationState::unknown),
            60))
        return failure;

    VerificationEvidence invalid_dependency_range = evidence;
    ExceptionDependency invalid_dependencies[3]{};
    for (std::uint32_t i = 0; i < 3; ++i)
        invalid_dependencies[i] = exception_dependencies[i];
    invalid_dependencies[0].virtual_base =
        INVALID_VALUE - PAGE_MASK;
    invalid_dependencies[0].size = 2 * PAGE_SIZE;
    invalid_dependency_range.exception_dependencies =
        invalid_dependencies;
    report = verify(regions, REGION_COUNT, invalid_dependency_range);
    if (!expect(
            check_state(
                report,
                VerificationCheck::exception_dependencies,
                VerificationState::fail),
            61))
        return failure;

    report = verify(regions, REGION_COUNT - 1, evidence);
    if (!expect(
            report.overall == VerificationState::unknown &&
            check_state(
                report,
                VerificationCheck::hhdm_coverage,
                VerificationState::unknown),
            7))
        return failure;

    VerificationEvidence complete_hhdm = evidence;
    complete_hhdm.hhdm_inventory_complete = true;
    report = verify(regions, REGION_COUNT, complete_hhdm);
    if (!expect(
            check_state(
                report,
                VerificationCheck::hhdm_coverage,
                VerificationState::fail),
            8))
        return failure;

    const std::uint64_t unmapped_active_pages[] = {0xb00000};
    VerificationEvidence bad_active_inventory = evidence;
    bad_active_inventory.active_table_pages = unmapped_active_pages;
    report = verify(regions, REGION_COUNT, bad_active_inventory);
    if (!expect(
            check_state(
                report,
                VerificationCheck::active_table_backing,
                VerificationState::fail),
            9))
        return failure;

    const std::uint64_t saved_owned_page =
        hierarchy.pages_[1].physical_address;
    hierarchy.pages_[1].physical_address = 0xdead000;
    report = verify(regions, REGION_COUNT, evidence);
    hierarchy.pages_[1].physical_address = saved_owned_page;
    if (!expect(
            check_state(
                report,
                VerificationCheck::owned_table_pages,
                VerificationState::fail),
            10))
        return failure;

    auto leaf_descriptor_pointer = [&](std::uint64_t virtual_address) {
        std::uint64_t physical = hierarchy.root_physical_address();
        std::uint64_t* entries = nullptr;
        if (hierarchy.checked_table_entries(physical, entries) !=
            Status::success)
            return static_cast<std::uint64_t*>(nullptr);
        for (std::uint8_t level = 0; level < REQUIRED_LEVELS; ++level) {
            std::uint64_t* descriptor =
                &entries[table_index(virtual_address, level)];
            if (level == REQUIRED_LEVELS - 1)
                return descriptor;
            physical = *descriptor & hierarchy.physical_address_mask_;
            if (hierarchy.checked_table_entries(physical, entries) !=
                Status::success)
                return static_cast<std::uint64_t*>(nullptr);
        }
        return static_cast<std::uint64_t*>(nullptr);
    };
    std::uint64_t* text_descriptor =
        leaf_descriptor_pointer(regions[1].virtual_base);
    if (!expect(text_descriptor != nullptr, 11))
        return failure;
    const std::uint64_t saved_descriptor = *text_descriptor;
    *text_descriptor ^= std::uint64_t{1} << 6;
    report = verify(regions, REGION_COUNT, evidence);
    *text_descriptor = saved_descriptor;
    if (!expect(
            check_state(
                report,
                VerificationCheck::descriptor_policy,
                VerificationState::fail),
            12))
        return failure;

    *text_descriptor = normal_page_descriptor(
        regions[1].physical_base,
        LeafPolicy::writable,
        NORMAL_ATTRIBUTE_INDEX
    );
    report = verify(regions, REGION_COUNT, evidence);
    *text_descriptor = saved_descriptor;
    if (!expect(
            check_state(
                report,
                VerificationCheck::exception_dependencies,
                VerificationState::fail),
            32))
        return failure;

    std::uint64_t* writable_descriptor =
        leaf_descriptor_pointer(regions[3].virtual_base);
    if (!expect(writable_descriptor != nullptr, 33))
        return failure;
    const std::uint64_t saved_writable_descriptor =
        *writable_descriptor;
    *writable_descriptor = 0;
    report = verify(regions, REGION_COUNT, evidence);
    *writable_descriptor = saved_writable_descriptor;
    if (!expect(
            check_state(
                report,
                VerificationCheck::exception_dependencies,
                VerificationState::fail),
            34))
        return failure;

    ExceptionDependency missing_handler_page[3]{};
    for (std::uint32_t i = 0; i < 3; ++i)
        missing_handler_page[i] = exception_dependencies[i];
    missing_handler_page[0].virtual_base +=
        4 * PAGE_SIZE;
    VerificationEvidence missing_handler = evidence;
    missing_handler.exception_dependencies = missing_handler_page;
    report = verify(regions, REGION_COUNT, missing_handler);
    if (!expect(
            check_state(
                report,
                VerificationCheck::exception_dependencies,
                VerificationState::fail),
            35))
        return failure;

    changed_regions[REGION_COUNT] = {
        SYNTHETIC_OTHER_ROOT_BASE,
        regions[1].physical_base,
        PAGE_SIZE,
        LIMINE_MEMMAP_USABLE,
        ROLE_BOOT_DATA,
        LeafPolicy::writable,
        NORMAL_ATTRIBUTE_INDEX
    };
    report = verify(changed_regions, REGION_COUNT + 1, evidence);
    if (!expect(
            check_state(
                report,
                VerificationCheck::alias_safety,
                VerificationState::fail),
            13))
        return failure;

    for (std::uint32_t i = 0; i < REGION_COUNT; ++i)
        changed_regions[i] = regions[i];
    changed_regions[0].policy = LeafPolicy::writable_executable;
    report = verify(changed_regions, REGION_COUNT, evidence);
    if (!expect(
            check_state(
                report,
                VerificationCheck::descriptor_policy,
                VerificationState::fail),
            14))
        return failure;

    changed_regions[0] = regions[0];
    changed_regions[0].attribute_index = 2;
    report = verify(changed_regions, REGION_COUNT, evidence);
    if (!expect(
            check_state(
                report,
                VerificationCheck::descriptor_policy,
                VerificationState::fail),
            15))
        return failure;

    changed_regions[0] = regions[0];
    changed_regions[0].virtual_base = 0x0001000000000000ull;
    report = verify(changed_regions, REGION_COUNT, evidence);
    if (!expect(
            check_state(
                report,
                VerificationCheck::mapped_regions,
                VerificationState::fail),
            16))
        return failure;

    changed_regions[0] = regions[0];
    changed_regions[0].virtual_base = 0xfffffffffffff000ull;
    changed_regions[0].size = 2 * PAGE_SIZE;
    report = verify(changed_regions, REGION_COUNT, evidence);
    if (!expect(
            check_state(
                report,
                VerificationCheck::mapped_regions,
                VerificationState::fail),
            17))
        return failure;

    changed_regions[0] = regions[0];
    changed_regions[0].physical_base = 0x10000000;
    report = verify(changed_regions, REGION_COUNT, evidence);
    if (!expect(
            check_state(
                report,
                VerificationCheck::physical_backing,
                VerificationState::fail),
            18))
        return failure;

    VerificationEvidence unknown_lifetime = evidence;
    unknown_lifetime.boot_object_lifetime_established = false;
    report = verify(regions, REGION_COUNT, unknown_lifetime);
    if (!expect(
            check_state(
                report,
                VerificationCheck::boot_object_lifetime,
                VerificationState::unknown),
            19))
        return failure;

    VerificationEvidence insufficient_capacity = evidence;
    insufficient_capacity.required_additional_table_pages =
        MAX_OWNED_TABLE_PAGES;
    report = verify(regions, REGION_COUNT, insufficient_capacity);
    if (!expect(
            check_state(
                report,
                VerificationCheck::remaining_capacity,
                VerificationState::fail),
            20))
        return failure;

    *text_descriptor = 0;
    report = verify(regions, REGION_COUNT, evidence);
    *text_descriptor = saved_descriptor;
    if (!expect(
            check_state(
                report,
                VerificationCheck::mapped_regions,
                VerificationState::fail),
            21))
        return failure;

    VerificationEvidence incomplete_inventory = evidence;
    incomplete_inventory.active_inventory_complete = false;
    report = verify(regions, REGION_COUNT, incomplete_inventory);
    if (!expect(
            check_state(
                report,
                VerificationCheck::active_table_backing,
                VerificationState::unknown),
            22))
        return failure;
    incomplete_inventory.exception_dependency_count = 2;
    report = verify(regions, REGION_COUNT, incomplete_inventory);
    if (!expect(
            check_state(
                report,
                VerificationCheck::exception_dependencies,
                VerificationState::unknown),
            23))
        return failure;

    VerificationEvidence active_root_collision = evidence;
    active_root_collision.active_registers.ttbr1_el1 =
        hierarchy.root_physical_address();
    report = verify(regions, REGION_COUNT, active_root_collision);
    if (!expect(
            check_state(
                report,
                VerificationCheck::candidate_root,
                VerificationState::fail),
            24))
        return failure;

    const Status destroy_status = hierarchy.destroy();
    if (!expect(destroy_status == Status::success, 25))
        return failure;

    synthetic_provider.allocation_budget = 0;
    Hierarchy allocation_failure;
    const Status allocation_status = allocation_failure.create_with_provider(
        regime,
        0xffff,
        provider
    );
    report = allocation_failure.verify_candidate(
        &memory_map,
        regions,
        REGION_COUNT,
        REQUIRED_ROLES,
        evidence
    );
    synthetic_provider.allocation_budget = -1;
    if (!expect(
            allocation_status == Status::allocation_failure &&
            report.overall == VerificationState::fail &&
            check_state(
                report,
                VerificationCheck::candidate_root,
                VerificationState::fail),
            26))
        return failure;

    for (bool allocated : synthetic_provider.allocated) {
        if (!expect(!allocated, 27))
            return failure;
    }
    return 0;
}

std::uint32_t run_live_tests(
    const Paging::Regime& regime,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    const Paging::RegisterSnapshot& registers
) {
    const auto starting_statistics = Memory::statistics();
    const std::uint64_t unrelated_page = Memory::allocate_page();
    std::uint64_t data_pages[4]{};
    Hierarchy hierarchy;
    std::uint32_t failure = 0;
    std::uint32_t allocated_data_pages = 0;
    bool unrelated_owned = unrelated_page != 0;
    for (std::uint64_t& page : data_pages) {
        page = Memory::allocate_page();
        if (page == 0) {
            failure = 1;
            break;
        }
        ++allocated_data_pages;
    }
    if (unrelated_page == 0 && failure == 0)
        failure = 1;

    const auto verify_page_pointer = [](std::uint64_t page) {
        void* pointer = nullptr;
        return page != 0 &&
            Memory::writable_pointer_for_allocated_page(page, pointer) &&
            pointer != nullptr;
    };
    if (failure == 0) {
        for (const std::uint64_t page : data_pages) {
            if (!verify_page_pointer(page)) {
                failure = 2;
                break;
            }
        }
    }
    if (failure == 0 && !verify_page_pointer(unrelated_page))
        failure = 2;

    if (failure == 0) {
        const Status create_status = hierarchy.create(
            regime,
            Paging::read_registers().mair_el1
        );
        if (create_status != Status::success ||
            hierarchy.owned_table_page_count() != 1)
            failure = 3;
    }

    if (failure == 0) {
        const std::uint64_t root = hierarchy.root_physical_address();
        const auto active_registers = Paging::read_registers();
        if (root == (active_registers.ttbr0_el1 &
                     regime.physical_address_mask) ||
            root == (active_registers.ttbr1_el1 &
                     regime.physical_address_mask))
            failure = 4;
    }

    const std::uint64_t addresses[4] = {
        SYNTHETIC_VIRTUAL_BASE,
        SYNTHETIC_VIRTUAL_BASE + PAGE_SIZE,
        SYNTHETIC_VIRTUAL_BASE + 2 * PAGE_SIZE,
        SYNTHETIC_OTHER_ROOT_BASE
    };
    for (std::uint32_t i = 0; i < 4 && failure == 0; ++i) {
        const Status map_status =
            hierarchy.map_page(addresses[i], data_pages[i]);
        if (map_status != Status::success)
            failure = 5 + i;
    }

    if (failure == 0 &&
        (hierarchy.owned_table_page_count() != 7 ||
         hierarchy.map_page(addresses[0], data_pages[0]) !=
             Status::mapping_exists))
        failure = 9;
    const auto active_hhdm_mapping_is_valid = [&](std::uint64_t physical) {
        if (physical > ~std::uint64_t{0} - hhdm_offset)
            return false;
        const Paging::ActiveTranslation translation =
            Paging::translate_active_address(
                registers,
                hhdm_offset,
                memory_map,
                hhdm_offset + physical
            );
        return translation.status ==
                Paging::ActiveTranslationStatus::mapped &&
            translation.mapping.translated_physical_address == physical &&
            translation.mapping.privileged_writable &&
            translation.mapping.memory_type == Paging::MemoryType::normal;
    };
    for (std::uint32_t i = 0;
         i < hierarchy.page_count_ && failure == 0;
         ++i) {
        if (!active_hhdm_mapping_is_valid(
                hierarchy.pages_[i].physical_address))
            failure = 25;
    }
    for (const std::uint64_t page : data_pages) {
        if (failure != 0)
            break;
        if (!active_hhdm_mapping_is_valid(page))
            failure = 25;
    }
    if (failure == 0 && !active_hhdm_mapping_is_valid(unrelated_page))
        failure = 25;
    for (std::uint32_t i = 0; i < 4 && failure == 0; ++i) {
        const Translation translated =
            hierarchy.translate(addresses[i] + 0x6a5);
        if (translated.status != Status::success ||
            translated.physical_address != data_pages[i] + 0x6a5)
            failure = 10 + i;
    }
    if (failure == 0 &&
        hierarchy.translate(SYNTHETIC_VIRTUAL_BASE + 0x3000).status !=
            Status::not_mapped)
        failure = 14;
    if (failure == 0 &&
        (hierarchy.unmap_page(addresses[1]) != Status::success ||
         hierarchy.translate(addresses[1]).status != Status::not_mapped ||
         hierarchy.unmap_page(addresses[1]) != Status::not_mapped ||
         hierarchy.map_page(addresses[1], data_pages[1]) != Status::success))
        failure = 15;

    if (failure == 0) {
        for (const std::uint64_t address : addresses) {
            if (hierarchy.unmap_page(address) != Status::success) {
                failure = 16;
                break;
            }
        }
    }
    if (failure == 0 && hierarchy.owned_table_page_count() != 1)
        failure = 17;
    if (failure == 0 &&
        hierarchy.map_page(addresses[0], data_pages[0]) != Status::success)
        failure = 18;

    if (hierarchy.is_created()) {
        const Status destroy_status = hierarchy.destroy();
        if (failure == 0 && destroy_status != Status::success)
            failure = 19;
    }

    bool leaves_still_owned = true;
    for (std::uint64_t i = 0; i < allocated_data_pages; ++i)
        leaves_still_owned =
            leaves_still_owned && verify_page_pointer(data_pages[i]);
    void* unrelated_pointer = nullptr;
    unrelated_owned = unrelated_owned &&
        Memory::writable_pointer_for_allocated_page(
            unrelated_page,
            unrelated_pointer
        ) &&
        unrelated_pointer != nullptr;
    for (std::uint64_t i = 0; i < allocated_data_pages; ++i) {
        if (!Memory::release_page(data_pages[i]) && failure == 0)
            failure = 20;
    }
    if (unrelated_page != 0 &&
        !Memory::release_page(unrelated_page) &&
        failure == 0)
        failure = 21;

    const auto final_statistics = Memory::statistics();
    if (!leaves_still_owned || !unrelated_owned)
        return 22;
    if (final_statistics.free_pages != starting_statistics.free_pages &&
        failure == 0)
        failure = 23;
    if (failure == 0 && allocated_data_pages != 4)
        failure = 24;
    return failure;
}

}
