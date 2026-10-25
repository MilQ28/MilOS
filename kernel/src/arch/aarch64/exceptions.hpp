#pragma once

#include <cstdint>

namespace MilOS::AArch64 {

struct ExceptionFrame {
    std::uint64_t x[31];
    std::uint64_t esr;
    std::uint64_t elr;
    std::uint64_t spsr;
    std::uint64_t far;
    std::uint64_t far_valid;
    std::uint64_t exception_level;
    std::uint64_t vector;
    std::uint64_t esr_valid;
};

std::uint64_t install_exception_vectors();
bool run_exception_diagnostic_test();

}

extern "C" bool aarch64_exception_dispatch(
    MilOS::AArch64::ExceptionFrame* frame
);

