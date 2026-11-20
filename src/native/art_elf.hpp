#pragma once

#include <elf.h>
#include <cstddef>
#include <cstdint>
#include <string_view>

// libart hidden symbols are not all exported: resolve BOTH symtab and dynsym.
// Open once before LSPlant installs hooks; lookup does no JNI or file I/O.
class ArtElf final {
public:
    ~ArtElf();
    bool Open();
    void* Find(std::string_view name, bool prefix = false) const;
    bool IsExecutable(const void* address, std::size_t length = 4) const;
    std::uintptr_t Offset(const void* address) const {
        return reinterpret_cast<std::uintptr_t>(address) - bias_;
    }

private:
    const void* At(std::size_t offset, std::size_t length) const;
    void* image_ = nullptr;
    std::size_t size_ = 0;
    std::uintptr_t bias_ = 0;
    const Elf64_Ehdr* header_ = nullptr;
    const Elf64_Shdr* sections_ = nullptr;
    const Elf64_Phdr* segments_ = nullptr;
};
