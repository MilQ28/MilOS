#include <cstdint>
#include <cstddef>
#include <limine.h>

// Set the base revision to 6, this is recommended as this is the latest
// base revision described by the Limine boot protocol specification.
// See specification for further info.

namespace {

__attribute__((used, section(".limine_requests")))
volatile std::uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(6);

}

// The Limine requests can be placed anywhere, but it is important that
// the compiler does not optimise them away, so, usually, they should
// be made volatile or equivalent, _and_ they should be accessed at least
// once or marked as used with the "used" attribute as done here.

namespace {

__attribute__((used, section(".limine_requests")))
volatile limine_framebuffer_request framebuffer_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

}

// Finally, define the start and end markers for the Limine requests.
// These can also be moved anywhere, to any .cpp file, as seen fit.

namespace {

__attribute__((used, section(".limine_requests_start")))
volatile std::uint64_t limine_requests_start_marker[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
volatile std::uint64_t limine_requests_end_marker[] = LIMINE_REQUESTS_END_MARKER;

}

// Halt and catch fire function, defined below.
namespace {

void hcf();

}

// The following stubs are required by the Itanium C++ ABI (the one we use,
// regardless of the "Itanium" nomenclature).
// Like the memory functions in memory.cpp, these stubs can be moved to a different .cpp file,
// but should not be removed, unless you know what you are doing.
extern "C" {
    int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }
    void __cxa_pure_virtual() { hcf(); }
    void __cxa_deleted_virtual() { hcf(); }
    void *__dso_handle;
    // Not thread safe: replace before statics can be initialised concurrently.
    int __cxa_guard_acquire(std::uint64_t *guard) { return *reinterpret_cast<std::uint8_t *>(guard) == 0; }
    void __cxa_guard_release(std::uint64_t *guard) { *reinterpret_cast<std::uint8_t *>(guard) = 1; }
}

// Extern declarations for global constructors array.
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

// The same for .ctors, which Clang 10 and older emit for x86 and which runs backwards.
extern void (*__ctors_start[])();
extern void (*__ctors_end[])();

// Halt and catch fire function.
namespace {

void hcf() {
    for (;;) {
#if defined (__x86_64__)
        asm ("hlt");
#elif defined (__aarch64__) || defined (__riscv)
        asm ("wfi");
#elif defined (__loongarch64)
        asm ("idle 0");
#endif
    }
}

// Scale an 8-bit colour channel value to the size the framebuffer gives the
// channel and move it into place within a pixel.
std::uint32_t fb_channel(std::uint8_t value, std::uint8_t mask_size, std::uint8_t mask_shift) {
    std::uint64_t max = (std::uint64_t{1} << mask_size) - 1;
    return static_cast<std::uint32_t>((value * max / 255) << mask_shift);
}

// Build a pixel from 8-bit red, green and blue values following the channel
// layout of the framebuffer.
std::uint32_t fb_pixel(limine_framebuffer *fb, std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    return fb_channel(red, fb->red_mask_size, fb->red_mask_shift)
         | fb_channel(green, fb->green_mask_size, fb->green_mask_shift)
         | fb_channel(blue, fb->blue_mask_size, fb->blue_mask_shift);
}


// Return one row of a simple 5x7 bitmap font.
std::uint8_t glyph_row(char c, std::size_t row) {
    static const std::uint8_t blank[7] = {};
    const std::uint8_t *g = blank;

    static const std::uint8_t M[7] = {17, 27, 21, 21, 17, 17, 17};
    static const std::uint8_t I[7] = {31, 4, 4, 4, 4, 4, 31};
    static const std::uint8_t L[7] = {16, 16, 16, 16, 16, 16, 31};
    static const std::uint8_t O[7] = {14, 17, 17, 17, 17, 17, 14};
    static const std::uint8_t S[7] = {15, 16, 16, 14, 1, 1, 30};
    static const std::uint8_t B[7] = {30, 17, 17, 30, 17, 17, 30};
    static const std::uint8_t T[7] = {31, 4, 4, 4, 4, 4, 4};
    static const std::uint8_t E[7] = {31, 16, 16, 30, 16, 16, 31};
    static const std::uint8_t D[7] = {30, 17, 17, 17, 17, 17, 30};

    switch (c) {
        case 'M': g = M; break;
        case 'I': g = I; break;
        case 'L': g = L; break;
        case 'O': g = O; break;
        case 'S': g = S; break;
        case 'B': g = B; break;
        case 'T': g = T; break;
        case 'E': g = E; break;
        case 'D': g = D; break;
        default: break;
    }

    return g[row];
}

void fb_pattern(limine_framebuffer *fb) {
    auto *pixels = static_cast<volatile std::uint32_t *>(fb->address);

    // Dark background.
    for (std::size_t y = 0; y < fb->height; ++y) {
        for (std::size_t x = 0; x < fb->width; ++x) {
            pixels[y * (fb->pitch / 4) + x] =
                fb_pixel(fb, 18, 18, 26);
        }
    }

    const char text[] = "MILOS BOOTED";
    const std::size_t scale = 4;
    const std::size_t text_width = (sizeof(text) - 2) * 6 * scale;
    const std::size_t start_x =
        fb->width > text_width ? (fb->width - text_width) / 2 : 0;
    const std::size_t start_y = fb->height / 2 - 14;

    for (std::size_t i = 0; text[i] != '\0'; ++i) {
        for (std::size_t row = 0; row < 7; ++row) {
            const std::uint8_t bits = glyph_row(text[i], row);

            for (std::size_t col = 0; col < 5; ++col) {
                if ((bits & (1u << (4 - col))) == 0) {
                    continue;
                }

                for (std::size_t dy = 0; dy < scale; ++dy) {
                    for (std::size_t dx = 0; dx < scale; ++dx) {
                        const std::size_t x =
                            start_x + (i * 6 + col) * scale + dx;
                        const std::size_t y =
                            start_y + row * scale + dy;

                        if (x < fb->width && y < fb->height) {
                            pixels[y * (fb->pitch / 4) + x] =
                                fb_pixel(fb, 240, 240, 245);
                        }
                    }
                }
            }
        }
    }
}


}

// The following will be our kernel's entry point.
// If renaming kmain() to something else, make sure to change the
// linker script accordingly.
extern "C" void kmain() {
    // Ensure the bootloader actually understands our base revision (see spec).
    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        hcf();
    }

    // Call global constructors.
    for (void (**ctor)() = __init_array_start; ctor < __init_array_end;) {
        (*ctor++)();
    }
    for (void (**ctor)() = __ctors_end; ctor > __ctors_start;) {
        (*--ctor)();
    }

    // Ensure we got a framebuffer.
    if (framebuffer_request.response == nullptr
     || framebuffer_request.response->framebuffer_count < 1) {
        hcf();
    }

    // Print the pattern to every framebuffer.
    for (std::uint64_t i = 0; i < framebuffer_request.response->framebuffer_count; i++) {
        limine_framebuffer *framebuffer = framebuffer_request.response->framebuffers[i];

        // Ensure the framebuffer has 32-bit RGB pixels, the only kind we handle.
        if (framebuffer->memory_model != LIMINE_FRAMEBUFFER_RGB || framebuffer->bpp != 32) {
            hcf();
        }

        fb_pattern(framebuffer);
    }

    // We're done, just hang...
    hcf();
}
