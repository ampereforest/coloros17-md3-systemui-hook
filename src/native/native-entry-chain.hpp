#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace md3 {

using EntryCode = std::array<std::uint32_t, 5>;
constexpr std::size_t kMaxEntryChain = 8;

struct EntryJump {
    enum Kind { kNone, kJump, kInvalid } kind = kNone;
    std::uintptr_t destination = 0;
};

inline bool AddDisplacement(std::uintptr_t base, std::int64_t offset,
                            std::uintptr_t& result) {
    if (offset < 0) {
        auto distance = static_cast<std::uint64_t>(-(offset + 1)) + 1;
        if (distance > base) return false;
        result = base - distance;
    } else {
        auto distance = static_cast<std::uint64_t>(offset);
        if (distance > UINTPTR_MAX - base) return false;
        result = base + distance;
    }
    return true;
}

// Decode only complete, argument-preserving entry veneers. No disassembly or
// arbitrary literal dereference is needed while ART's threads are suspended.
inline EntryJump DecodeEntryJump(std::uintptr_t pc, const EntryCode& code) {
    auto finish = [](bool valid, std::uintptr_t destination) -> EntryJump {
        return valid ? EntryJump{EntryJump::kJump, destination}
                     : EntryJump{EntryJump::kInvalid, 0};
    };
    std::uintptr_t destination = 0;
    auto reg = code[0] & 31U;
    auto scratch = reg == 16 || reg == 17;
    auto br = 0xd61f0000U | (reg << 5);

    // Nyako's existing 12-byte ADRP X17; ADD X17,X17,#imm; BR X17.
    if ((code[0] & 0x9f000000U) == 0x90000000U && code[2] == br) {
        if (!scratch || (code[1] & 0xff8003ffU) !=
                            (0x91000000U | (reg << 5) | reg)) {
            return {EntryJump::kInvalid, 0};
        }
        std::int64_t page_delta = ((code[0] >> 5) & 0x7ffffU) * 4U +
                                  ((code[0] >> 29) & 3U);
        if (page_delta & (1 << 20)) page_delta -= 1 << 21;
        auto add = static_cast<std::int64_t>((code[1] >> 10) & 0xfffU);
        if (code[1] & (1U << 22)) add *= 4096;
        bool valid = AddDisplacement(pc & ~std::uintptr_t{0xfff},
                                     page_delta * 4096 + add, destination);
        return finish(valid, destination);
    }

    // Fixed four-piece absolute veneer, preserving the full 64-bit address.
    if ((code[0] & 0xffe00000U) == 0xd2800000U && code[4] == br) {
        if (!scratch) return {EntryJump::kInvalid, 0};
        for (unsigned i = 0; i < 4; ++i) {
            auto opcode = i ? 0xf2800000U : 0xd2800000U;
            if ((code[i] & 0xff80001fU) != (opcode | reg) ||
                ((code[i] >> 21) & 3U) != i) return {EntryJump::kInvalid, 0};
            destination |= std::uintptr_t{(code[i] >> 5) & 0xffffU} << (16 * i);
        }
        return finish(true, destination);
    }

    // Literal veneer only when its pointer is wholly in our checked snapshot.
    if ((code[0] & 0xff000000U) == 0x58000000U && code[1] == br) {
        if (!scratch || ((code[0] >> 5) & 0x7ffffU) != 2U)
            return {EntryJump::kInvalid, 0};
        destination = std::uintptr_t{code[2]} | (std::uintptr_t{code[3]} << 32);
        return finish(true, destination);
    }

    if ((code[0] & 0xfc000000U) == 0x14000000U) {  // B, not BL.
        std::int64_t offset = code[0] & 0x03ffffffU;
        if (offset & (1 << 25)) offset -= 1 << 26;
        bool valid = AddDisplacement(pc, offset * 4, destination);
        return finish(valid, destination);
    }
    if ((code[0] & 0xfffffc1fU) == 0xd61f0000U)
        return {EntryJump::kInvalid, 0};  // Unknown indirect entry: do not overwrite.
    return {};
}

struct EntryNode {
    std::uintptr_t address = 0;
    EntryCode code{};
};

struct EntryChain {
    std::array<EntryNode, kMaxEntryChain> preserved{};
    std::size_t count = 0;
    std::uintptr_t actual = 0;
    EntryCode actual_code{};
};

template <typename Reader>
bool ResolveEntryChain(std::uintptr_t logical, std::uintptr_t replacement,
                       Reader&& read_code, EntryChain& out) {
    out = {};
    auto current = logical;
    for (;;) {
        if (!current || (current & 3U) || current == replacement) return false;
        for (std::size_t i = 0; i < out.count; ++i) {
            // Also reject jumps into an older stub's continuation/overwritten area.
            auto address = out.preserved[i].address;
            if (current >= address && current - address < sizeof(EntryCode)) return false;
        }
        EntryCode code;
        if (!read_code(current, code)) return false;
        auto jump = DecodeEntryJump(current, code);
        if (jump.kind == EntryJump::kInvalid) return false;
        if (jump.kind == EntryJump::kNone) {
            out.actual = current;
            out.actual_code = code;
            return true;
        }
        if (out.count == out.preserved.size()) return false;
        out.preserved[out.count++] = {current, code};
        current = jump.destination;
    }
}

}  // namespace md3
