#include <cstdint>
#include <pmm.hpp>

namespace MilOS::Memory {

namespace {
constexpr std::uint64_t BITS_PER_BYTE = 8;
constexpr std::uint64_t MIN_USABLE_ADDRESS = 0x100000;
constexpr std::uint64_t INVALID_ADDRESS = ~std::uint64_t{0};

std::uint8_t* bitmap = nullptr;
std::uint64_t page_count = 0;
std::uint64_t free_count = 0;
std::uint64_t physical_limit = 0;

bool bit_is_set(std::uint64_t i) {
    return (bitmap[i / BITS_PER_BYTE] &
            (1u << (i % BITS_PER_BYTE))) != 0;
}

void set_bit(std::uint64_t i) {
    bitmap[i / BITS_PER_BYTE] |=
        static_cast<std::uint8_t>(1u << (i % BITS_PER_BYTE));
}

void clear_bit(std::uint64_t i) {
    bitmap[i / BITS_PER_BYTE] &=
        static_cast<std::uint8_t>(~(1u << (i % BITS_PER_BYTE)));
}

std::uint64_t align_up(std::uint64_t value) {
    if (value > INVALID_ADDRESS - (PAGE_SIZE - 1))
        return INVALID_ADDRESS;
    return (value + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}
}

bool init(std::uint64_t hhdm_offset, limine_memmap_response* memmap) {
    if (bitmap || !memmap || !memmap->entries || !memmap->entry_count)
        return false;

    std::uint64_t max_address = 0;

    for (std::uint64_t i = 0; i < memmap->entry_count; ++i) {
        auto* e = memmap->entries[i];
        if (!e || e->type != LIMINE_MEMMAP_USABLE || !e->length ||
            e->base > INVALID_ADDRESS - e->length)
            continue;

        const auto end = e->base + e->length;
        if (end > max_address)
            max_address = end;
    }

    if (max_address <= MIN_USABLE_ADDRESS)
        return false;

    page_count = max_address / PAGE_SIZE;
    if (max_address % PAGE_SIZE)
        ++page_count;

    const std::uint64_t bitmap_bytes =
        page_count / BITS_PER_BYTE +
        (page_count % BITS_PER_BYTE != 0);

    const std::uint64_t storage_size = align_up(bitmap_bytes);
    if (storage_size == INVALID_ADDRESS)
        return false;

    std::uint64_t bitmap_physical = INVALID_ADDRESS;

    for (std::uint64_t i = 0; i < memmap->entry_count; ++i) {
        auto* e = memmap->entries[i];
        if (!e || e->type != LIMINE_MEMMAP_USABLE || !e->length ||
            e->base > INVALID_ADDRESS - e->length)
            continue;

        const auto region_end = e->base + e->length;
        const auto candidate = align_up(
            e->base < MIN_USABLE_ADDRESS ? MIN_USABLE_ADDRESS : e->base);

        if (candidate == INVALID_ADDRESS || candidate >= region_end)
            continue;

        if (storage_size <= region_end - candidate) {
            bitmap_physical = candidate;
            break;
        }
    }

    if (bitmap_physical == INVALID_ADDRESS ||
        bitmap_physical > INVALID_ADDRESS - hhdm_offset)
        return false;

    auto* candidate_bitmap = reinterpret_cast<std::uint8_t*>(
        hhdm_offset + bitmap_physical);

    for (std::uint64_t i = 0; i < bitmap_bytes; ++i)
        candidate_bitmap[i] = 0xFF;

    bitmap = candidate_bitmap;
    physical_limit = max_address;

    for (std::uint64_t i = 0; i < memmap->entry_count; ++i) {
        auto* e = memmap->entries[i];
        if (!e || e->type != LIMINE_MEMMAP_USABLE || !e->length ||
            e->base > INVALID_ADDRESS - e->length)
            continue;

        const auto region_end = e->base + e->length;
        const auto start = align_up(
            e->base < MIN_USABLE_ADDRESS ? MIN_USABLE_ADDRESS : e->base);
        const auto end = region_end & ~(PAGE_SIZE - 1);

        if (start == INVALID_ADDRESS || start >= end)
            continue;

        for (auto address = start; address < end; address += PAGE_SIZE)
            clear_bit(address / PAGE_SIZE);
    }

    const auto bitmap_end = bitmap_physical + storage_size;
    for (auto address = bitmap_physical;
         address < bitmap_end; address += PAGE_SIZE)
        set_bit(address / PAGE_SIZE);

    free_count = 0;
    for (std::uint64_t i = 0; i < page_count; ++i) {
        if (!bit_is_set(i))
            ++free_count;
    }

    return true;
}

std::uint64_t allocate_page() {
    if (!bitmap)
        return 0;

    for (std::uint64_t i = MIN_USABLE_ADDRESS / PAGE_SIZE;
         i < page_count; ++i) {
        if (!bit_is_set(i)) {
            set_bit(i);
            --free_count;
            return i * PAGE_SIZE;
        }
    }

    return 0;
}

bool free_page(std::uint64_t physical_address) {
    if (!bitmap || physical_address < MIN_USABLE_ADDRESS ||
        physical_address % PAGE_SIZE != 0 ||
        physical_address >= physical_limit)
        return false;

    const auto index = physical_address / PAGE_SIZE;

    if (bit_is_set(index))
        return false;

    // Halaman yang sudah bebas tidak boleh dibebaskan lagi.
    return false;
}

std::uint64_t total_pages() {
    return page_count;
}

std::uint64_t free_pages() {
    return free_count;
}

}
