#include <cstdint>
#include <cstddef>
#include <limine.h>
#include "serial.hpp"
#include <pmm.hpp>
#include <vmm.hpp>

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

namespace {

__attribute__((used, section(".limine_requests")))
volatile limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".limine_requests")))
volatile limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

__attribute__((used, section(".limine_requests")))
volatile limine_executable_address_request executable_address_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
    .response = nullptr
};

}

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
    MilOS::Log::init();
    MilOS::Log::info("MilOS kernel entered");

    if (LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision) == false) {
        MilOS::Log::error("Unsupported Limine base revision");
        hcf();
    }

    MilOS::Log::info("Limine protocol check passed");

    if (memmap_request.response == nullptr) {
        MilOS::Log::error("Memory map unavailable");
        hcf();
    }

    MilOS::Log::info("Memory map received");

    std::uint64_t usable_bytes = 0;
    std::uint64_t usable_regions = 0;

    for (std::uint64_t i = 0;
         i < memmap_request.response->entry_count; i++) {

        limine_memmap_entry* entry =
            memmap_request.response->entries[i];

        MilOS::Log::write("[INFO] Region ");
        MilOS::Log::write_dec(i);
        MilOS::Log::write(": base=");
        MilOS::Log::write_hex(entry->base);
        MilOS::Log::write(" length=");
        MilOS::Log::write_hex(entry->length);
        MilOS::Log::write(" type=");
        MilOS::Log::write_dec(entry->type);
        MilOS::Log::write("\n");

        if (entry->type == LIMINE_MEMMAP_USABLE) {
            usable_bytes += entry->length;
            usable_regions++;
        }
    }

    MilOS::Log::write("[INFO] Usable regions: ");
    MilOS::Log::write_dec(usable_regions);
    MilOS::Log::write("\n");

    MilOS::Log::write("[INFO] Total usable RAM bytes: ");
    MilOS::Log::write_dec(usable_bytes);
    MilOS::Log::write("\n");


    // Call global constructors.
    for (void (**ctor)() = __init_array_start; ctor < __init_array_end;) {
        (*ctor++)();
    }
    for (void (**ctor)() = __ctors_end; ctor > __ctors_start;) {
        (*--ctor)();
    }

    if (hhdm_request.response == nullptr) {
        MilOS::Log::error("HHDM unavailable");
        hcf();
    }

    if (!MilOS::Memory::init(
            hhdm_request.response->offset,
            const_cast<limine_memmap_response*>(
                memmap_request.response))) {
        MilOS::Log::error("PMM initialization failed");
        hcf();
    }

    MilOS::Log::info("Physical memory manager initialized");

    MilOS::Log::write("[INFO] Total physical pages: ");
    MilOS::Log::write_dec(MilOS::Memory::total_pages());
    MilOS::Log::write("\n");

    MilOS::Log::write("[INFO] Free physical pages: ");
    MilOS::Log::write_dec(MilOS::Memory::free_pages());
    MilOS::Log::write("\n");

    // PMM validation tests.
    const auto free_before = MilOS::Memory::free_pages();

    const auto page1 = MilOS::Memory::allocate_page();
    const auto page2 = MilOS::Memory::allocate_page();
    const auto page3 = MilOS::Memory::allocate_page();

    if (page1 == 0 || page2 == 0 || page3 == 0 ||
        page1 == page2 || page1 == page3 || page2 == page3 ||
        page1 % MilOS::Memory::PAGE_SIZE != 0 ||
        page2 % MilOS::Memory::PAGE_SIZE != 0 ||
        page3 % MilOS::Memory::PAGE_SIZE != 0 ||
        MilOS::Memory::free_pages() != free_before - 3) {
        MilOS::Log::error("PMM unique allocation test failed");
        hcf();
    }

    // Invalid addresses must not alter allocator state.
    if (MilOS::Memory::free_page(page1 + 1) ||
        MilOS::Memory::free_page(0x100000) ||
        MilOS::Memory::free_pages() != free_before - 3) {
        MilOS::Log::error("PMM invalid-free protection failed");
        hcf();
    }

    if (!MilOS::Memory::free_page(page1) ||
        MilOS::Memory::free_page(page1)) {
        MilOS::Log::error("PMM double-free protection failed");
        hcf();
    }

    if (!MilOS::Memory::free_page(page2) ||
        !MilOS::Memory::free_page(page3) ||
        MilOS::Memory::free_pages() != free_before) {
        MilOS::Log::error("PMM release test failed");
        hcf();
    }

    MilOS::Log::info("PMM validation tests passed");

    MilOS::Log::write("[INFO] Tested page 1: ");
    MilOS::Log::write_hex(page1);
    MilOS::Log::write("\n");

    MilOS::Log::write("[INFO] Tested page 2: ");
    MilOS::Log::write_hex(page2);
    MilOS::Log::write("\n");

    MilOS::Log::write("[INFO] Tested page 3: ");
    MilOS::Log::write_hex(page3);
    MilOS::Log::write("\n");

    MilOS::Log::write("[INFO] Free pages restored: ");
    MilOS::Log::write_dec(MilOS::Memory::free_pages());
    MilOS::Log::write("\n");

#if defined(__x86_64__)
    const auto free_before_vmm = MilOS::Memory::free_pages();
    if (!MilOS::Memory::vmm_init(
            hhdm_request.response->offset,
            const_cast<limine_memmap_response*>(
                memmap_request.response))) {
        MilOS::Log::error("VMM initialization failed");
        hcf();
    }

    MilOS::Log::write("[INFO] Active paging levels: ");
    MilOS::Log::write_dec(MilOS::Memory::paging_levels());
    MilOS::Log::write("\n");

    const auto kernel_virtual_address =
        reinterpret_cast<std::uint64_t>(&kmain);
    std::uint64_t kernel_physical_address;
    std::uint64_t kernel_page_size;
    if (executable_address_request.response == nullptr ||
        kernel_virtual_address <
            executable_address_request.response->virtual_base) {
        MilOS::Log::error("Limine executable address unavailable");
        hcf();
    }

    const auto kernel_offset =
        kernel_virtual_address -
        executable_address_request.response->virtual_base;
    if (executable_address_request.response->physical_base >
        ~std::uint64_t{0} - kernel_offset) {
        MilOS::Log::error("Expected kernel physical address overflowed");
        hcf();
    }

    const auto expected_kernel_physical =
        executable_address_request.response->physical_base + kernel_offset;
    if (!MilOS::Memory::translate(
            kernel_virtual_address,
            kernel_physical_address,
            &kernel_page_size) ||
        kernel_physical_address != expected_kernel_physical ||
        (kernel_page_size != MilOS::Memory::PAGE_SIZE &&
         kernel_page_size != (1ull << 21) &&
         kernel_page_size != (1ull << 30))) {
        MilOS::Log::error("VMM kernel-symbol translation failed");
        hcf();
    }
    MilOS::Log::info("VMM kernel-symbol translation passed");

    const auto root_physical_address =
        MilOS::Memory::active_root_physical_address();
    if (root_physical_address >
        ~std::uint64_t{0} - hhdm_request.response->offset) {
        MilOS::Log::error("VMM HHDM test address overflowed");
        hcf();
    }

    const auto root_hhdm_address =
        hhdm_request.response->offset + root_physical_address;
    std::uint64_t translated_root = 0;
    std::uint64_t root_page_size = 0;
    const bool root_translation_succeeded = MilOS::Memory::translate(
        root_hhdm_address, translated_root, &root_page_size);
    if (!root_translation_succeeded ||
        translated_root != root_physical_address ||
        (root_page_size != MilOS::Memory::PAGE_SIZE &&
         root_page_size != (1ull << 21) &&
         root_page_size != (1ull << 30))) {
        MilOS::Log::write("[ERROR] Root physical address: ");
        MilOS::Log::write_hex(root_physical_address);
        MilOS::Log::write(" HHDM virtual address: ");
        MilOS::Log::write_hex(root_hhdm_address);
        MilOS::Log::write(" translated: ");
        MilOS::Log::write_hex(translated_root);
        MilOS::Log::write(" page size: ");
        MilOS::Log::write_hex(root_page_size);
        MilOS::Log::write("\n");
        MilOS::Log::error("VMM HHDM translation failed");
        hcf();
    }
    MilOS::Log::info("VMM HHDM translation passed");

    const auto synthetic_test_failure =
        MilOS::Memory::vmm_run_synthetic_tests();
    if (synthetic_test_failure != 0) {
        MilOS::Log::write("[ERROR] VMM synthetic test case: ");
        MilOS::Log::write_dec(synthetic_test_failure);
        MilOS::Log::write("\n");
        MilOS::Log::error("VMM synthetic walker tests failed");
        hcf();
    }
    MilOS::Log::info("VMM walker, mapping, and address-space tests passed");

    // Limine base revision 1 and later do not provide an identity mapping.
    std::uint64_t null_translation = ~std::uint64_t{0};
    if (MilOS::Memory::translate(0, null_translation) ||
        null_translation != ~std::uint64_t{0}) {
        MilOS::Log::error("VMM unmapped-address rejection failed");
        hcf();
    }
    MilOS::Log::info("VMM unmapped-address rejection passed");

    if (kernel_page_size > MilOS::Memory::PAGE_SIZE ||
        root_page_size > MilOS::Memory::PAGE_SIZE) {
        MilOS::Log::info("VMM large-page translation passed");
    } else {
        MilOS::Log::info("VMM large-page test not available");
    }

    if (MilOS::Memory::free_pages() != free_before_vmm) {
        MilOS::Log::error("VMM changed the PMM free-page count");
        hcf();
    }
    MilOS::Log::info("VMM preserved the PMM free-page count");
#endif

    if (framebuffer_request.response == nullptr
     || framebuffer_request.response->framebuffer_count < 1) {
        MilOS::Log::error("Framebuffer unavailable");
        hcf();
    }

    MilOS::Log::info("Framebuffer initialized");

    for (std::uint64_t i = 0;
         i < framebuffer_request.response->framebuffer_count; i++) {
        limine_framebuffer *framebuffer =
            framebuffer_request.response->framebuffers[i];

        if (framebuffer->memory_model != LIMINE_FRAMEBUFFER_RGB
         || framebuffer->bpp != 32) {
            MilOS::Log::error("Unsupported framebuffer format");
            hcf();
        }

        fb_pattern(framebuffer);
    }

    MilOS::Log::info("Boot screen rendered");
    hcf();
}
