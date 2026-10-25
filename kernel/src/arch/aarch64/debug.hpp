#pragma once

#include <cstdint>

namespace MilOS::AArch64::Debug {

inline void write(const char* message) {
    if (message == nullptr)
        return;

    asm volatile(
        "mov x0, #4\n"
        "mov x1, %0\n"
        "hlt #0xf000\n"
        :
        : "r"(message)
        : "x0", "x1", "memory");
}

inline void write_hex(std::uint64_t value) {
    constexpr char digits[] = "0123456789ABCDEF";
    char buffer[16];
    unsigned int length = 0;

    do {
        buffer[length++] = digits[value & 0xF];
        value >>= 4;
    } while (value != 0);

    write("0x");
    while (length != 0) {
        const char digit[2] = {buffer[--length], '\0'};
        write(digit);
    }
}

}
