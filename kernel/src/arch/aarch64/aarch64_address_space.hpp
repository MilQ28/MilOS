#pragma once

#include <cstdint>
#include "aarch64_paging.hpp"

namespace MilOS::AArch64::AddressSpace {

// A 2-GiB 4-KiB-leaf HHDM needs about 1,028 tables before kernel branches.
constexpr std::uint32_t MAX_OWNED_TABLE_PAGES = 2048;
constexpr std::uint32_t MAX_CONCURRENT_HIERARCHIES = 2;

enum class Status : std::uint8_t {
    success,
    already_created,
    not_created,
    unsupported_regime,
    unsupported_normal_attribute,
    invalid_virtual_address,
    invalid_physical_address,
    mapping_exists,
    not_mapped,
    allocation_failure,
    ownership_capacity_exceeded,
    invalid_ownership,
    malformed_descriptor,
    rollback_failed,
    unsupported_policy,
    invalid_attribute_index,
    invalid_memory_map,
    invalid_region,
    virtual_overlap,
    conflicting_alias,
    missing_required_mapping,
    verification_failed
};

enum class LeafPolicy : std::uint8_t {
    kernel_text,
    read_only,
    writable,
    writable_executable
};

struct Translation {
    Status status;
    std::uint64_t physical_address;
};

struct MappingRegion {
    std::uint64_t virtual_base;
    std::uint64_t physical_base;
    std::uint64_t size;
    std::uint64_t memory_map_type;
    std::uint32_t role_mask;
    LeafPolicy policy;
    std::uint8_t attribute_index;
};

struct ExceptionDependency {
    std::uint64_t virtual_base;
    std::uint64_t physical_base;
    std::uint64_t size;
    std::uint32_t role_mask;
    LeafPolicy policy;
};

constexpr std::uint32_t MAX_KERNEL_MAPPING_REGIONS = 320;
constexpr std::uint32_t ROLE_KERNEL_REQUESTS = 1u << 0;
constexpr std::uint32_t ROLE_KERNEL_TEXT = 1u << 1;
constexpr std::uint32_t ROLE_KERNEL_RODATA = 1u << 2;
constexpr std::uint32_t ROLE_KERNEL_WRITABLE = 1u << 3;
constexpr std::uint32_t ROLE_STACK = 1u << 4;
constexpr std::uint32_t ROLE_BOOT_DATA = 1u << 5;
constexpr std::uint32_t ROLE_PMM_METADATA = 1u << 6;
constexpr std::uint32_t ROLE_TABLE_PAGES = 1u << 7;
constexpr std::uint32_t ROLE_DTB = 1u << 8;
constexpr std::uint32_t ROLE_ACTIVE_TABLES = 1u << 9;
constexpr std::uint32_t ROLE_HHDM_USABLE = 1u << 10;
constexpr std::uint32_t ROLE_EXCEPTION_VECTORS = 1u << 11;
constexpr std::uint32_t ROLE_EXCEPTION_DATA = 1u << 12;

enum class VerificationState : std::uint8_t {
    pass,
    fail,
    unknown
};

enum class VerificationCheck : std::uint8_t {
    candidate_root,
    owned_table_pages,
    required_roles,
    mapped_regions,
    physical_backing,
    descriptor_policy,
    alias_safety,
    hhdm_coverage,
    active_table_backing,
    boot_object_lifetime,
    stack_continuity,
    exception_vectors,
    exception_dependencies,
    pmm_accessibility,
    remaining_capacity,
    unsupported_device_mappings,
    count
};

struct VerificationReport {
    VerificationState overall;
    VerificationState checks[
        static_cast<std::uint32_t>(VerificationCheck::count)
    ];
    VerificationCheck first_non_pass;
    std::uint64_t failing_address;
};

struct VerificationEvidence {
    std::uint64_t hhdm_offset;
    Paging::RegisterSnapshot active_registers;
    const std::uint64_t* active_table_pages;
    std::uint64_t active_table_page_count;
    bool active_inventory_complete;
    bool hhdm_inventory_complete;
    bool boot_object_lifetime_established;
    const ExceptionDependency* exception_dependencies;
    std::uint32_t exception_dependency_count;
    std::uint64_t vector_base;
    std::uint64_t expected_vector_base;
    std::uint64_t vector_size;
    std::uint64_t normal_stack_base;
    std::uint64_t normal_stack_size;
    std::uint64_t exception_stack_base;
    std::uint64_t exception_stack_size;
    std::uint64_t active_sp_el0;
    std::uint64_t active_sp_el1;
    std::uint64_t pmm_metadata_base;
    std::uint64_t pmm_metadata_size;
    std::uint32_t required_additional_table_pages;
    bool additional_capacity_requirement_known;
};

class Hierarchy {
public:
    Hierarchy() = default;
    Hierarchy(const Hierarchy&) = delete;
    Hierarchy& operator=(const Hierarchy&) = delete;

    Status create(
        const Paging::Regime& regime,
        std::uint64_t mair_value
    );
    // Leaf pages are borrowed; keep each allocated while its leaf is mapped.
    Status map_page(
        std::uint64_t virtual_address,
        std::uint64_t physical_address
    );
    Status map_page(
        std::uint64_t virtual_address,
        std::uint64_t physical_address,
        LeafPolicy policy,
        std::uint8_t attribute_index = 1
    );
    Translation translate(std::uint64_t virtual_address) const;
    // rollback_failed still removes the leaf; the retained empty table branch
    // remains owned and can be released by destroy().
    Status unmap_page(std::uint64_t virtual_address);
    Status destroy();

    bool is_created() const;
    std::uint64_t root_physical_address() const;
    std::uint32_t owned_table_page_count() const;
    std::uint64_t owned_table_page_physical_address(
        std::uint32_t index
    ) const;
    VerificationReport verify_candidate(
        const limine_memmap_response* memory_map,
        const MappingRegion* regions,
        std::uint32_t region_count,
        std::uint32_t required_roles,
        const VerificationEvidence& evidence
    ) const;

private:
    struct PageProvider {
        void* context;
        std::uint64_t (*allocate)(void* context);
        bool (*release)(void* context, std::uint64_t physical_address);
        bool (*writable_pointer)(
            void* context,
            std::uint64_t physical_address,
            void*& pointer
        );
    };

    struct OwnedTablePage {
        std::uint64_t physical_address;
        std::uint64_t* entries;
    };
    inline static OwnedTablePage ownership_storage_[
        MAX_CONCURRENT_HIERARCHIES
    ][MAX_OWNED_TABLE_PAGES]{};
    inline static bool ownership_storage_in_use_[
        MAX_CONCURRENT_HIERARCHIES
    ]{};

    Status create_with_provider(
        const Paging::Regime& regime,
        std::uint64_t mair_value,
        const PageProvider& provider,
        std::uint32_t table_page_limit = MAX_OWNED_TABLE_PAGES
    );
    bool acquire_ownership_storage();
    void release_ownership_storage();
    Status allocate_owned_table(
        std::uint64_t& physical_address,
        std::uint64_t*& entries
    );
    std::int32_t find_owned_table(
        std::uint64_t physical_address
    ) const;
    Status checked_table_entries(
        std::uint64_t physical_address,
        std::uint64_t*& entries
    ) const;
    Status release_owned_table(std::uint64_t physical_address);
    Status validate_owned_tree() const;
    Status map_page_internal(
        std::uint64_t virtual_address,
        std::uint64_t physical_address,
        LeafPolicy policy,
        std::uint8_t attribute_index,
        bool require_allocated_leaf
    );

    OwnedTablePage* pages_ = nullptr;
    std::uint64_t physical_address_mask_ = 0;
    std::uint64_t mair_value_ = 0;
    std::uint32_t page_count_ = 0;
    std::uint32_t table_page_limit_ = MAX_OWNED_TABLE_PAGES;
    std::uint8_t ownership_storage_index_ = 0xff;
    PageProvider provider_{};
    bool created_ = false;

    friend Status build_kernel_mappings(
        Hierarchy& hierarchy,
        const Paging::Regime& regime,
        std::uint64_t mair_value,
        std::uint64_t hhdm_offset,
        const limine_memmap_response* memory_map,
        const MappingRegion* regions,
        std::uint32_t region_count,
        std::uint32_t required_roles
    );
    friend std::uint32_t run_synthetic_tests();
    friend std::uint32_t run_builder_synthetic_tests();
    friend std::uint32_t run_pre_activation_verifier_tests();
    friend std::uint32_t run_live_tests(
        const Paging::Regime& regime,
        std::uint64_t hhdm_offset,
        const limine_memmap_response* memory_map,
        const Paging::RegisterSnapshot& registers
    );
};

const char* status_name(Status status);
const char* verification_state_name(VerificationState state);
const char* verification_check_name(VerificationCheck check);
Status build_kernel_mappings(
    Hierarchy& hierarchy,
    const Paging::Regime& regime,
    std::uint64_t mair_value,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    const MappingRegion* regions,
    std::uint32_t region_count,
    std::uint32_t required_roles
);
std::uint32_t run_synthetic_tests();
std::uint32_t run_builder_synthetic_tests();
std::uint32_t run_pre_activation_verifier_tests();
std::uint32_t run_live_tests(
    const Paging::Regime& regime,
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    const Paging::RegisterSnapshot& registers
);

}
