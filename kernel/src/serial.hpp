#pragma once

#include <cstdint>
#include <cstddef>

namespace MilOS::Log {

#if defined(__x86_64__)

inline void outb(std::uint16_t port, std::uint8_t value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

inline std::uint8_t inb(std::uint16_t port) {
    std::uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

inline void init() {
    constexpr std::uint16_t COM1 = 0x3F8;

    outb(COM1 + 1, 0x00); // Disable interrupts
    outb(COM1 + 3, 0x80); // Enable divisor latch
    outb(COM1 + 0, 0x03); // Baud divisor: 38400
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); // 8 data bits, no parity, 1 stop bit
    outb(COM1 + 2, 0xC7); // Enable and clear FIFO
    outb(COM1 + 4, 0x0B); // Enable UART control signals
}

inline void putchar(char c) {
    constexpr std::uint16_t COM1 = 0x3F8;

    // Avoid hanging forever if the UART is unavailable.
    for (unsigned int i = 0; i < 100000; ++i) {
        if (inb(COM1 + 5) & 0x20) {
            outb(COM1, static_cast<std::uint8_t>(c));
            return;
        }
    }
}

inline void write(const char* message) {
    if (message == nullptr) return;

    while (*message != '\0') {
        if (*message == '\n') putchar('\r');
        putchar(*message++);
    }
}

inline void write_hex(std::uint64_t value) {
    constexpr char digits[] = "0123456789ABCDEF";
    char buffer[16];
    int i = 0;

    do {
        buffer[i++] = digits[value & 0xF];
        value >>= 4;
    } while (value != 0);

    write("0x");
    while (i > 0) {
        putchar(buffer[--i]);
    }
}

inline void write_dec(std::uint64_t value) {
    char buffer[20];
    int i = 0;

    do {
        buffer[i++] = '0' + (value % 10);
        value /= 10;
    } while (value != 0);

    while (i > 0) {
        putchar(buffer[--i]);
    }
}

inline void info(const char* message) {
    write("[INFO] ");
    write(message);
    write("\n");
}

inline void error(const char* message) {
    write("[ERROR] ");
    write(message);
    write("\n");
}

#else

inline void init() {}
inline void write(const char*) {}
inline void info(const char*) {}
inline void error(const char*) {}
inline void write_hex(std::uint64_t) {}
inline void write_dec(std::uint64_t) {}

#endif

}
