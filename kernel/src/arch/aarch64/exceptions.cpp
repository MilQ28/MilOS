#include <cstddef>
#include <cstdint>
#include "debug.hpp"
#include "exceptions.hpp"

static_assert(offsetof(MilOS::AArch64::ExceptionFrame, esr) == 248);
static_assert(offsetof(MilOS::AArch64::ExceptionFrame, x) == 0);
static_assert(offsetof(MilOS::AArch64::ExceptionFrame, elr) == 256);
static_assert(offsetof(MilOS::AArch64::ExceptionFrame, spsr) == 264);
static_assert(offsetof(MilOS::AArch64::ExceptionFrame, far) == 272);
static_assert(offsetof(MilOS::AArch64::ExceptionFrame, far_valid) == 280);
static_assert(offsetof(MilOS::AArch64::ExceptionFrame, exception_level) == 288);
static_assert(offsetof(MilOS::AArch64::ExceptionFrame, vector) == 296);
static_assert(offsetof(MilOS::AArch64::ExceptionFrame, esr_valid) == 304);
static_assert(sizeof(MilOS::AArch64::ExceptionFrame) == 312);
static_assert(alignof(MilOS::AArch64::ExceptionFrame) == 8);
static_assert(320 >= sizeof(MilOS::AArch64::ExceptionFrame));
static_assert(320 % 16 == 0);

extern "C" std::uint64_t aarch64_install_exception_vectors();
extern "C" void aarch64_exception_test_fault();
extern "C" void aarch64_exception_test_instruction();

namespace {

constexpr std::uint64_t EXCEPTION_CLASS_SHIFT = 26;
constexpr std::uint64_t EXCEPTION_CLASS_MASK = 0x3F;
constexpr std::uint64_t UNKNOWN_INSTRUCTION_EXCEPTION = 0;

volatile bool diagnostic_test_active = false;
volatile bool diagnostic_test_handled = false;
volatile std::uint64_t diagnostic_test_elr = 0;
std::uint64_t active_exception_level = 0;

void write_hex_field(const char* label, std::uint64_t value) {
    MilOS::AArch64::Debug::write(label);
    MilOS::AArch64::Debug::write_hex(value);
    MilOS::AArch64::Debug::write("\n");
}

}

namespace MilOS::AArch64 {

std::uint64_t install_exception_vectors() {
    active_exception_level = aarch64_install_exception_vectors();
    return active_exception_level;
}

bool run_exception_diagnostic_test() {
#if defined(MILOS_AARCH64_EXCEPTION_TEST)
    diagnostic_test_handled = false;
    diagnostic_test_elr = reinterpret_cast<std::uint64_t>(
        &aarch64_exception_test_instruction);
    diagnostic_test_active = true;
    asm volatile("" ::: "memory");
    aarch64_exception_test_fault();
    asm volatile("" ::: "memory");
    diagnostic_test_active = false;
    return diagnostic_test_handled;
#else
    return false;
#endif
}

}

extern "C" bool aarch64_exception_dispatch(
    MilOS::AArch64::ExceptionFrame* frame
) {
    if (frame == nullptr)
        return false;

    const std::uint64_t exception_class =
        (frame->esr >> EXCEPTION_CLASS_SHIFT) & EXCEPTION_CLASS_MASK;
    if (diagnostic_test_active &&
        frame->exception_level == active_exception_level &&
        frame->vector == 4 &&
        frame->esr_valid != 0 &&
        exception_class == UNKNOWN_INSTRUCTION_EXCEPTION &&
        frame->elr == diagnostic_test_elr) {
        diagnostic_test_handled = true;
        diagnostic_test_active = false;
        frame->elr += 4;
        MilOS::AArch64::Debug::write(
            "AArch64 exception test captured ESR/ELR/SPSR\n");
        write_hex_field("vector=", frame->vector);
        write_hex_field("ESR=", frame->esr);
        write_hex_field("ELR=", frame->elr - 4);
        write_hex_field("SPSR=", frame->spsr);
        return true;
    }

    MilOS::AArch64::Debug::write("AArch64 fatal exception\n");
    write_hex_field("EL=", frame->exception_level);
    write_hex_field("vector=", frame->vector);
    if (frame->esr_valid != 0) {
        write_hex_field("ESR=", frame->esr);
        write_hex_field("ELR=", frame->elr);
        write_hex_field("SPSR=", frame->spsr);
        if (frame->far_valid != 0)
            write_hex_field("FAR=", frame->far);
    } else {
        write_hex_field("ELR=", frame->elr);
        write_hex_field("SPSR=", frame->spsr);
        MilOS::AArch64::Debug::write(
            "Syndrome unavailable for asynchronous exception\n");
    }
    return false;
}
