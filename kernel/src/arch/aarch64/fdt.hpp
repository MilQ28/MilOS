#pragma once

#include <cstddef>
#include <cstdint>

namespace MilOS::AArch64 {

struct Pl011Info {
    std::uint64_t register_base;
    std::uint64_t register_size;
    std::uint32_t clock_frequency;
    bool has_clock_frequency;
};

bool find_enabled_pl011(
    const void* dtb,
    std::size_t available_size,
    Pl011Info& uart
);

bool run_fdt_parser_tests();

}
