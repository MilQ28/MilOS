#pragma once

#include <cstdint>
#include <limine.h>

namespace MilOS::Memory {

constexpr std::uint64_t PAGE_SIZE = 4096;

bool init(
    std::uint64_t hhdm_offset,
    limine_memmap_response* memmap
);

std::uint64_t allocate_page();
bool free_page(std::uint64_t physical_address);

std::uint64_t total_pages();
std::uint64_t free_pages();

}
