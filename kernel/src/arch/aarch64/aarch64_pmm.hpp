#pragma once

#include <cstdint>
#include <limine.h>
#include "aarch64_paging.hpp"

namespace MilOS::AArch64::Memory {

constexpr std::uint64_t PAGE_SIZE = 4096;

enum class InitializationStatus : std::uint8_t {
    success,
    already_initialized,
    unsupported_regime,
    invalid_memory_map,
    too_many_memory_map_entries,
    no_usable_memory,
    metadata_too_large,
    metadata_unavailable
};

struct Statistics {
    std::uint64_t total_pages;
    std::uint64_t free_pages;
    std::uint64_t metadata_pages;
    std::uint64_t usable_region_count;
    std::uint64_t metadata_base;
    std::uint64_t metadata_size;
};

InitializationStatus initialize(
    std::uint64_t hhdm_offset,
    const limine_memmap_response* memory_map,
    const Paging::Regime& regime
);

bool is_initialized();
std::uint64_t allocate_page();
bool release_page(std::uint64_t physical_address);
bool writable_pointer_for_allocated_page(
    std::uint64_t physical_address,
    void*& pointer
);
Statistics statistics();
const char* initialization_status_name(InitializationStatus status);
std::uint32_t run_synthetic_tests();

}
