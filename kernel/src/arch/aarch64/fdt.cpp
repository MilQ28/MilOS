#include <cstddef>
#include <cstdint>
#include "debug.hpp"
#include "fdt.hpp"

namespace {

constexpr std::uint32_t FDT_MAGIC = 0xD00DFEED;
constexpr std::uint32_t FDT_BEGIN_NODE = 1;
constexpr std::uint32_t FDT_END_NODE = 2;
constexpr std::uint32_t FDT_PROP = 3;
constexpr std::uint32_t FDT_NOP = 4;
constexpr std::uint32_t FDT_END = 9;
constexpr std::size_t FDT_HEADER_SIZE = 40;
constexpr std::size_t MAX_DTB_DEPTH = 32;

struct NodeState {
    std::uint32_t child_address_cells;
    std::uint32_t child_size_cells;
    std::uint32_t parent_address_cells;
    std::uint32_t parent_size_cells;
    const std::uint8_t* ranges;
    std::size_t ranges_size;
    std::uint64_t register_base;
    std::uint64_t register_size;
    std::uint32_t clock_frequency;
    bool has_address_cells_property;
    bool has_size_cells_property;
    bool has_ranges_property;
    bool has_compatible_property;
    bool has_status_property;
    bool has_register_property;
    bool has_clock_property;
    bool unsupported_register;
    bool compatible_pl011;
    bool enabled;
    bool has_register;
    bool has_clock_frequency;
};

struct DtbLayout {
    const std::uint8_t* bytes;
    std::size_t total_size;
    std::size_t structure_offset;
    std::size_t structure_size;
    std::size_t strings_offset;
    std::size_t strings_size;
};

std::uint32_t read_be32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           static_cast<std::uint32_t>(bytes[3]);
}

std::uint64_t read_be_cells(
    const std::uint8_t* bytes,
    std::uint32_t cell_count
) {
    std::uint64_t value = 0;
    for (std::uint32_t i = 0; i < cell_count; ++i)
        value = (value << 32) | read_be32(bytes + i * 4);
    return value;
}

bool add_without_overflow(
    std::uint64_t left,
    std::uint64_t right,
    std::uint64_t& result
) {
    if (left > ~std::uint64_t{0} - right)
        return false;
    result = left + right;
    return true;
}

bool translate_address(
    const NodeState* nodes,
    std::size_t bus_index,
    std::uint64_t& address,
    std::uint64_t size
) {
    std::uint64_t address_end;
    if (size == 0 || !add_without_overflow(address, size, address_end))
        return false;

    while (bus_index != 0) {
        const NodeState& bus = nodes[bus_index];
        if (bus.child_address_cells == 0 ||
            bus.child_address_cells > 2 ||
            bus.parent_address_cells == 0 ||
            bus.parent_address_cells > 2 ||
            bus.child_size_cells == 0 ||
            bus.child_size_cells > 2 ||
            bus.ranges == nullptr)
            return false;

        if (bus.ranges_size == 0) {
            if (bus.parent_address_cells == 1 &&
                address_end > (std::uint64_t{1} << 32))
                return false;
            --bus_index;
            continue;
        }

        const std::size_t child_bytes =
            static_cast<std::size_t>(bus.child_address_cells) * 4;
        const std::size_t parent_bytes =
            static_cast<std::size_t>(bus.parent_address_cells) * 4;
        const std::size_t size_bytes =
            static_cast<std::size_t>(bus.child_size_cells) * 4;
        const std::size_t tuple_bytes =
            child_bytes + parent_bytes + size_bytes;
        if (tuple_bytes == 0 || bus.ranges_size % tuple_bytes != 0)
            return false;

        bool matched = false;
        std::uint64_t translated = 0;
        for (std::size_t offset = 0;
             offset < bus.ranges_size;
             offset += tuple_bytes) {
            const std::uint64_t child_base =
                read_be_cells(bus.ranges + offset, bus.child_address_cells);
            const std::uint64_t parent_base =
                read_be_cells(
                    bus.ranges + offset + child_bytes,
                    bus.parent_address_cells);
            const std::uint64_t span = read_be_cells(
                bus.ranges + offset + child_bytes + parent_bytes,
                bus.child_size_cells);
            std::uint64_t child_limit;
            std::uint64_t parent_limit;
            if (span == 0 ||
                !add_without_overflow(child_base, span, child_limit) ||
                !add_without_overflow(parent_base, span, parent_limit))
                return false;
            if ((bus.child_address_cells == 1 &&
                 child_limit > (std::uint64_t{1} << 32)) ||
                (bus.parent_address_cells == 1 &&
                 parent_limit > (std::uint64_t{1} << 32)))
                return false;
            if (address < child_base || address_end > child_limit)
                continue;

            std::uint64_t result;
            const std::uint64_t displacement = address - child_base;
            if (!add_without_overflow(parent_base, displacement, result) ||
                result > ~std::uint64_t{0} - size)
                return false;
            if (bus.parent_address_cells == 1 &&
                result + size > (std::uint64_t{1} << 32))
                return false;
            if (matched)
                return false;
            matched = true;
            translated = result;
        }
        if (!matched)
            return false;
        address = translated;
        if (!add_without_overflow(address, size, address_end))
            return false;
        --bus_index;
    }
    return true;
}

bool range_is_valid(
    std::size_t offset,
    std::size_t length,
    std::size_t total_size
) {
    return offset <= total_size && length <= total_size - offset;
}

bool validate_layout(
    const void* dtb,
    std::size_t available_size,
    DtbLayout& layout
) {
    if (dtb == nullptr || available_size < FDT_HEADER_SIZE)
        return false;

    const auto* bytes = static_cast<const std::uint8_t*>(dtb);
    if (read_be32(bytes) != FDT_MAGIC)
        return false;

    const std::size_t total_size = read_be32(bytes + 4);
    const std::size_t structure_offset = read_be32(bytes + 8);
    const std::size_t strings_offset = read_be32(bytes + 12);
    const std::size_t reserve_offset = read_be32(bytes + 16);
    const std::uint32_t version = read_be32(bytes + 20);
    const std::uint32_t last_compatible_version = read_be32(bytes + 24);
    const std::size_t strings_size = read_be32(bytes + 32);
    const std::size_t structure_size = read_be32(bytes + 36);

    if (total_size < FDT_HEADER_SIZE ||
        total_size > available_size ||
        version < 17 ||
        last_compatible_version > 17 ||
        last_compatible_version > version ||
        structure_offset % 4 != 0 ||
        reserve_offset % 8 != 0 ||
        structure_size < 4 ||
        !range_is_valid(structure_offset, structure_size, total_size) ||
        !range_is_valid(strings_offset, strings_size, total_size) ||
        !range_is_valid(reserve_offset, 16, total_size))
        return false;

    bool reserve_terminator_found = false;
    std::size_t reserve_cursor = reserve_offset;
    while (range_is_valid(reserve_cursor, 16, total_size)) {
        const std::uint64_t address =
            (static_cast<std::uint64_t>(
                 read_be32(bytes + reserve_cursor)) << 32) |
            read_be32(bytes + reserve_cursor + 4);
        const std::uint64_t size =
            (static_cast<std::uint64_t>(
                 read_be32(bytes + reserve_cursor + 8)) << 32) |
            read_be32(bytes + reserve_cursor + 12);
        reserve_cursor += 16;
        if (address == 0 && size == 0) {
            reserve_terminator_found = true;
            break;
        }
    }
    if (!reserve_terminator_found)
        return false;

    layout = {
        bytes,
        total_size,
        structure_offset,
        structure_size,
        strings_offset,
        strings_size
    };
    return true;
}

bool bounded_string_equals(
    const std::uint8_t* bytes,
    std::size_t size,
    const char* expected
) {
    std::size_t index = 0;
    while (index < size && expected[index] != '\0') {
        if (bytes[index] != static_cast<std::uint8_t>(expected[index]))
            return false;
        ++index;
    }
    return index < size &&
           expected[index] == '\0' &&
           bytes[index] == '\0' &&
           index + 1 == size;
}

bool compatible_list_has_pl011(
    const std::uint8_t* value,
    std::size_t size,
    bool& found
) {
    if (size == 0)
        return false;

    std::size_t cursor = 0;
    while (cursor < size) {
        std::size_t end = cursor;
        while (end < size && value[end] != '\0')
            ++end;
        if (end == size)
            return false;
        if (bounded_string_equals(
                value + cursor, end - cursor + 1, "arm,pl011"))
            found = true;
        cursor = end + 1;
    }
    return cursor == size;
}

bool string_at(
    const DtbLayout& layout,
    std::uint32_t name_offset,
    const char*& name,
    std::size_t& name_size
) {
    if (name_offset >= layout.strings_size)
        return false;
    const auto* start = layout.bytes + layout.strings_offset + name_offset;
    const auto remaining = layout.strings_size - name_offset;
    std::size_t length = 0;
    while (length < remaining && start[length] != '\0')
        ++length;
    if (length == remaining)
        return false;
    name = reinterpret_cast<const char*>(start);
    name_size = length;
    return true;
}

bool name_equals(const char* name, std::size_t length, const char* expected) {
    std::size_t i = 0;
    while (i < length && expected[i] != '\0') {
        if (name[i] != expected[i])
            return false;
        ++i;
    }
    return i == length && expected[i] == '\0';
}

bool parse_property(
    NodeState& node,
    const std::uint8_t* value,
    std::size_t value_size,
    const char* name,
    std::size_t name_size
) {
    if (name_equals(name, name_size, "#address-cells")) {
        if (value_size != 4 || node.has_address_cells_property)
            return false;
        const auto cells = read_be32(value);
        if (cells > 4)
            return false;
        node.child_address_cells = cells;
        node.has_address_cells_property = true;
    } else if (name_equals(name, name_size, "#size-cells")) {
        if (value_size != 4 || node.has_size_cells_property)
            return false;
        const auto cells = read_be32(value);
        if (cells > 4)
            return false;
        node.child_size_cells = cells;
        node.has_size_cells_property = true;
    } else if (name_equals(name, name_size, "compatible")) {
        if (node.has_compatible_property)
            return false;
        if (!compatible_list_has_pl011(
                value, value_size, node.compatible_pl011))
            return false;
        node.has_compatible_property = true;
    } else if (name_equals(name, name_size, "status")) {
        if (value_size == 0 || value[value_size - 1] != '\0' ||
            node.has_status_property)
            return false;
        for (std::size_t i = 0; i + 1 < value_size; ++i) {
            if (value[i] == '\0')
                return false;
        }
        const bool okay =
            bounded_string_equals(value, value_size, "okay") ||
            bounded_string_equals(value, value_size, "ok");
        node.enabled = node.enabled && okay;
        node.has_status_property = true;
    } else if (name_equals(name, name_size, "reg")) {
        if (node.has_register_property)
            return false;
        const std::uint32_t address_cells = node.parent_address_cells;
        const std::uint32_t size_cells = node.parent_size_cells;
        node.has_register_property = true;
        if (address_cells == 0 || address_cells > 2 || size_cells > 2) {
            node.unsupported_register = true;
            return true;
        }
        const std::size_t address_bytes =
            static_cast<std::size_t>(address_cells) * 4;
        const std::size_t size_bytes =
            static_cast<std::size_t>(size_cells) * 4;
        const std::size_t tuple_bytes = address_bytes + size_bytes;
        if (tuple_bytes == 0 || value_size < tuple_bytes ||
            value_size % tuple_bytes != 0)
            return false;
        for (std::size_t offset = 0;
             offset < value_size;
             offset += tuple_bytes) {
            const std::uint64_t address =
                read_be_cells(value + offset, address_cells);
            const std::uint64_t size = size_cells == 0
                ? 0
                : read_be_cells(value + offset + address_bytes, size_cells);
            if (size != 0 && address > ~std::uint64_t{0} - size)
                return false;
            if (offset == 0) {
                node.register_base = address;
                node.register_size = size;
            }
        }
        node.has_register = node.register_size != 0 &&
            node.register_base <= ~std::uint64_t{0} - node.register_size;
    } else if (name_equals(name, name_size, "clock-frequency")) {
        if (value_size != 4 || node.has_clock_property)
            return false;
        node.clock_frequency = read_be32(value);
        node.has_clock_frequency = true;
        node.has_clock_property = true;
    } else if (name_equals(name, name_size, "ranges")) {
        if (node.has_ranges_property)
            return false;
        node.ranges = value;
        node.ranges_size = value_size;
        node.has_ranges_property = true;
    }

    return true;
}

void write_be32(std::uint8_t* bytes, std::uint32_t value) {
    bytes[0] = static_cast<std::uint8_t>(value >> 24);
    bytes[1] = static_cast<std::uint8_t>(value >> 16);
    bytes[2] = static_cast<std::uint8_t>(value >> 8);
    bytes[3] = static_cast<std::uint8_t>(value);
}

void append_u32(
    std::uint8_t* buffer,
    std::size_t& cursor,
    std::uint32_t value
) {
    write_be32(buffer + cursor, value);
    cursor += 4;
}

void append_property(
    std::uint8_t* buffer,
    std::size_t& cursor,
    std::uint32_t name_offset,
    const std::uint8_t* value,
    std::size_t value_size
) {
    append_u32(buffer, cursor, FDT_PROP);
    append_u32(buffer, cursor, static_cast<std::uint32_t>(value_size));
    append_u32(buffer, cursor, name_offset);
    for (std::size_t i = 0; i < value_size; ++i)
        buffer[cursor++] = value[i];
    while (cursor % 4 != 0)
        buffer[cursor++] = 0;
}

}

namespace MilOS::AArch64 {

bool find_enabled_pl011(
    const void* dtb,
    std::size_t available_size,
    Pl011Info& uart
) {
    DtbLayout layout{};
    if (!validate_layout(dtb, available_size, layout))
        return false;

    NodeState nodes[MAX_DTB_DEPTH] = {};
    std::size_t depth = 0;
    std::size_t cursor = layout.structure_offset;
    const std::size_t structure_end =
        layout.structure_offset + layout.structure_size;
    bool root_seen = false;
    bool tree_ended = false;
    bool uart_found = false;
    Pl011Info candidate{};

    while (cursor < structure_end) {
        if (!range_is_valid(cursor, 4, structure_end))
            return false;
        const std::uint32_t token = read_be32(layout.bytes + cursor);
        cursor += 4;

        if (token == FDT_BEGIN_NODE) {
            if (depth >= MAX_DTB_DEPTH || (depth == 0 && root_seen))
                return false;
            std::size_t name_end = cursor;
            while (name_end < structure_end &&
                   layout.bytes[name_end] != '\0')
                ++name_end;
            if (name_end == structure_end)
                return false;
            if (depth == 0) {
                if (name_end != cursor)
                    return false;
                root_seen = true;
            }
            cursor = name_end + 1;
            if (cursor > static_cast<std::size_t>(-1) - 3)
                return false;
            cursor = (cursor + 3) & ~std::size_t{3};
            if (cursor > structure_end)
                return false;

            NodeState node{};
            node.child_address_cells = 2;
            node.child_size_cells = 1;
            node.enabled = depth == 0 || nodes[depth - 1].enabled;
            if (depth != 0) {
                node.parent_address_cells =
                    nodes[depth - 1].child_address_cells;
                node.parent_size_cells =
                    nodes[depth - 1].child_size_cells;
            }
            nodes[depth++] = node;
        } else if (token == FDT_END_NODE) {
            if (depth == 0)
                return false;
            const NodeState& node = nodes[depth - 1];
            if (node.compatible_pl011 && node.enabled) {
                if (node.unsupported_register || !node.has_register)
                    return false;
                std::uint64_t translated_base = node.register_base;
                if (depth < 2 ||
                    !translate_address(
                        nodes, depth - 2,
                        translated_base, node.register_size))
                    return false;
                if (uart_found)
                    return false;
                uart_found = true;
                candidate = {
                    translated_base,
                    node.register_size,
                    node.clock_frequency,
                    node.has_clock_frequency
                };
            }
            --depth;
        } else if (token == FDT_PROP) {
            if (depth == 0 ||
                !range_is_valid(cursor, 8, structure_end))
                return false;
            const std::size_t value_size =
                read_be32(layout.bytes + cursor);
            const std::uint32_t name_offset =
                read_be32(layout.bytes + cursor + 4);
            cursor += 8;
            if (!range_is_valid(cursor, value_size, structure_end))
                return false;

            const char* name;
            std::size_t name_size;
            if (!string_at(layout, name_offset, name, name_size) ||
                !parse_property(
                    nodes[depth - 1],
                    layout.bytes + cursor,
                    value_size,
                    name,
                    name_size))
                return false;

            cursor += value_size;
            if (cursor > static_cast<std::size_t>(-1) - 3)
                return false;
            cursor = (cursor + 3) & ~std::size_t{3};
            if (cursor > structure_end)
                return false;
        } else if (token == FDT_NOP) {
            continue;
        } else if (token == FDT_END) {
            tree_ended = true;
            break;
        } else {
            return false;
        }
    }

    if (!tree_ended || !root_seen || depth != 0 ||
        cursor != structure_end || !uart_found)
        return false;

    uart = candidate;
    return true;
}

bool run_fdt_parser_tests() {
    constexpr char strings[] =
        "#address-cells\0#size-cells\0compatible\0reg\0clock-frequency\0"
        "ranges\0";
    constexpr std::uint32_t ADDRESS_CELLS_OFFSET = 0;
    constexpr std::uint32_t SIZE_CELLS_OFFSET =
        sizeof("#address-cells");
    constexpr std::uint32_t COMPATIBLE_OFFSET =
        SIZE_CELLS_OFFSET + sizeof("#size-cells");
    constexpr std::uint32_t REG_OFFSET =
        COMPATIBLE_OFFSET + sizeof("compatible");
    constexpr std::uint32_t CLOCK_OFFSET =
        REG_OFFSET + sizeof("reg");
    constexpr std::uint32_t RANGES_OFFSET =
        CLOCK_OFFSET + sizeof("clock-frequency");

    enum class Fixture {
        direct,
        translated,
        empty_ranges,
        nested,
        missing_ranges,
        malformed_ranges,
        overflow,
        ambiguous_ranges,
        unsupported_cells,
        missing_reg,
        malformed_reg
    };

    const auto make_fixture = [&](Fixture fixture, std::uint8_t* dtb) {
        std::size_t cursor = 56;
        append_u32(dtb, cursor, FDT_BEGIN_NODE);
        dtb[cursor++] = 0;
        while (cursor % 4 != 0)
            dtb[cursor++] = 0;

        const std::uint8_t root_address_cells[] = {0, 0, 0, 2};
        const std::uint8_t root_size_cells[] = {0, 0, 0, 1};
        append_property(dtb, cursor, ADDRESS_CELLS_OFFSET,
                        root_address_cells, sizeof(root_address_cells));
        append_property(dtb, cursor, SIZE_CELLS_OFFSET,
                        root_size_cells, sizeof(root_size_cells));

        const bool nested = fixture == Fixture::nested;
        const bool translated = fixture == Fixture::translated || nested;
        if (fixture == Fixture::direct) {
            append_u32(dtb, cursor, FDT_BEGIN_NODE);
            const char node_name[] = "pl011@9000000";
            for (char c : node_name)
                dtb[cursor++] = static_cast<std::uint8_t>(c);
            while (cursor % 4 != 0)
                dtb[cursor++] = 0;
        } else {
            append_u32(dtb, cursor, FDT_BEGIN_NODE);
            const char bus_name[] = "bus@0";
            for (char c : bus_name)
                dtb[cursor++] = static_cast<std::uint8_t>(c);
            while (cursor % 4 != 0)
                dtb[cursor++] = 0;
            const std::uint8_t one_cell[] = {0, 0, 0, 1};
            const std::uint8_t unsupported_cells[] = {0, 0, 0, 3};
            append_property(dtb, cursor, ADDRESS_CELLS_OFFSET,
                            fixture == Fixture::unsupported_cells
                                ? unsupported_cells
                                : one_cell,
                            sizeof(one_cell));
            append_property(dtb, cursor, SIZE_CELLS_OFFSET,
                            one_cell, sizeof(one_cell));

            if (fixture == Fixture::empty_ranges) {
                append_property(dtb, cursor, RANGES_OFFSET, nullptr, 0);
            } else if (fixture == Fixture::missing_ranges) {
            } else if (fixture == Fixture::malformed_ranges) {
                const std::uint8_t malformed[] = {0};
                append_property(dtb, cursor, RANGES_OFFSET,
                                malformed, sizeof(malformed));
            } else if (fixture == Fixture::overflow) {
                const std::uint8_t range[] = {
                    0, 0, 0, 0,
                    0xFF, 0xFF, 0xFF, 0xFF,
                    0xFF, 0xFF, 0xFF, 0xF0,
                    0, 0, 1, 0
                };
                append_property(dtb, cursor, RANGES_OFFSET,
                                range, sizeof(range));
            } else if (fixture == Fixture::ambiguous_ranges) {
                const std::uint8_t ranges[] = {
                    0, 0, 0x10, 0,
                    0, 0, 0, 0, 0x09, 0, 0, 0,
                    0, 1, 0, 0,
                    0, 0, 0x10, 0,
                    0, 0, 0, 0, 0x09, 0, 0, 0,
                    0, 1, 0, 0
                };
                append_property(dtb, cursor, RANGES_OFFSET,
                                ranges, sizeof(ranges));
            } else if (translated) {
                if (nested) {
                    const std::uint8_t range[] = {
                        0, 1, 0, 0,
                        0, 0, 0, 0, 0x09, 0, 0, 0,
                        0, 2, 0, 0
                    };
                    append_property(dtb, cursor, RANGES_OFFSET,
                                    range, sizeof(range));
                } else {
                    const std::uint8_t range[] = {
                        0, 0, 0x10, 0,
                        0, 0, 0, 0, 0x09, 0, 0, 0,
                        0, 1, 0, 0
                    };
                    append_property(dtb, cursor, RANGES_OFFSET,
                                    range, sizeof(range));
                }
            }

            if (nested) {
                append_u32(dtb, cursor, FDT_BEGIN_NODE);
                const char nested_bus_name[] = "nested@2000";
                for (char c : nested_bus_name)
                    dtb[cursor++] = static_cast<std::uint8_t>(c);
                while (cursor % 4 != 0)
                    dtb[cursor++] = 0;
                append_property(dtb, cursor, ADDRESS_CELLS_OFFSET,
                                one_cell, sizeof(one_cell));
                append_property(dtb, cursor, SIZE_CELLS_OFFSET,
                                one_cell, sizeof(one_cell));
                const std::uint8_t nested_range[] = {
                    0, 0, 0x20, 0,
                    0, 1, 0x10, 0,
                    0, 0, 0x10, 0
                };
                append_property(dtb, cursor, RANGES_OFFSET,
                                nested_range, sizeof(nested_range));
            }

            append_u32(dtb, cursor, FDT_BEGIN_NODE);
            const char uart_name[] = "pl011@2100";
            for (char c : uart_name)
                dtb[cursor++] = static_cast<std::uint8_t>(c);
            while (cursor % 4 != 0)
                dtb[cursor++] = 0;
        }

        constexpr char compatible[] = "arm,pl011";
        append_property(dtb, cursor, COMPATIBLE_OFFSET,
                        reinterpret_cast<const std::uint8_t*>(compatible),
                        sizeof(compatible));
        if (fixture != Fixture::missing_reg) {
            std::uint8_t reg[16] = {};
            std::size_t reg_size = 0;
            if (fixture == Fixture::direct) {
                const std::uint8_t direct_reg[] = {
                    0, 0, 0, 0, 0x09, 0, 0, 0,
                    0, 0, 0x10, 0
                };
                for (std::uint8_t byte : direct_reg)
                    reg[reg_size++] = byte;
            } else if (fixture == Fixture::nested) {
                const std::uint8_t nested_reg[] =
                    {0, 0, 0x21, 0, 0, 0, 1, 0};
                for (std::uint8_t byte : nested_reg)
                    reg[reg_size++] = byte;
            } else if (fixture == Fixture::empty_ranges) {
                const std::uint8_t identity_reg[] =
                    {0, 0, 0x09, 0, 0, 0, 0x10, 0};
                for (std::uint8_t byte : identity_reg)
                    reg[reg_size++] = byte;
            } else {
                const std::uint8_t bus_reg[] =
                    {0, 0, 0x20, 0, 0, 0, 0x10, 0};
                for (std::uint8_t byte : bus_reg)
                    reg[reg_size++] = byte;
            }
            if (fixture == Fixture::malformed_reg) {
                reg_size = 3;
            }
            append_property(dtb, cursor, REG_OFFSET, reg, reg_size);
        }

        if (fixture == Fixture::direct) {
            const std::uint8_t clock[] = {1, 0x6E, 0x36, 0};
            append_property(dtb, cursor, CLOCK_OFFSET,
                            clock, sizeof(clock));
        }

        append_u32(dtb, cursor, FDT_END_NODE);
        if (nested)
            append_u32(dtb, cursor, FDT_END_NODE);
        if (fixture != Fixture::direct)
            append_u32(dtb, cursor, FDT_END_NODE);
        append_u32(dtb, cursor, FDT_END_NODE);
        append_u32(dtb, cursor, FDT_END);
        const std::size_t structure_size = cursor - 56;
        const std::size_t strings_offset = cursor;
        for (char c : strings)
            dtb[cursor++] = static_cast<std::uint8_t>(c);

        write_be32(dtb, FDT_MAGIC);
        write_be32(dtb + 4, static_cast<std::uint32_t>(cursor));
        write_be32(dtb + 8, 56);
        write_be32(dtb + 12, static_cast<std::uint32_t>(strings_offset));
        write_be32(dtb + 16, 40);
        write_be32(dtb + 20, 17);
        write_be32(dtb + 24, 16);
        write_be32(dtb + 32, sizeof(strings));
        write_be32(dtb + 36, static_cast<std::uint32_t>(structure_size));
        return cursor;
    };

    const auto expect = [&](Fixture fixture, bool valid,
                            std::uint64_t expected_base) {
        std::uint8_t dtb[2048] = {};
        const std::size_t size = make_fixture(fixture, dtb);
        Pl011Info uart{};
        const bool parsed = find_enabled_pl011(dtb, size, uart);
        const bool passed = parsed == valid &&
            (!valid || (uart.register_base == expected_base &&
                        uart.register_size != 0));
        if (!passed) {
            MilOS::AArch64::Debug::write("FDT fixture failed: ");
            MilOS::AArch64::Debug::write_hex(
                static_cast<std::uint64_t>(fixture));
            MilOS::AArch64::Debug::write(" parsed=");
            MilOS::AArch64::Debug::write_hex(parsed);
            MilOS::AArch64::Debug::write(" base=");
            MilOS::AArch64::Debug::write_hex(uart.register_base);
            MilOS::AArch64::Debug::write("\n");
        }
        return passed;
    };

    if (!expect(Fixture::direct, true, 0x09000000) ||
        !expect(Fixture::translated, true, 0x09001000) ||
        !expect(Fixture::empty_ranges, true, 0x00000900) ||
        !expect(Fixture::nested, true, 0x09001100) ||
        !expect(Fixture::missing_ranges, false, 0) ||
        !expect(Fixture::malformed_ranges, false, 0) ||
        !expect(Fixture::overflow, false, 0) ||
        !expect(Fixture::ambiguous_ranges, false, 0) ||
        !expect(Fixture::unsupported_cells, false, 0) ||
        !expect(Fixture::missing_reg, false, 0) ||
        !expect(Fixture::malformed_reg, false, 0))
        return false;
    Pl011Info invalid_input{};
    return !find_enabled_pl011(nullptr, 0, invalid_input);
}

}
