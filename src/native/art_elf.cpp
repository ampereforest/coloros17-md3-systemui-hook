#include "art_elf.hpp"

#include <android/log.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <initializer_list>
#include <limits.h>

namespace {
struct ArtLocation {
    char path[PATH_MAX]{};
    std::uintptr_t bias = 0;
};

int FindArt(dl_phdr_info* info, std::size_t, void* data) {
    if (!info->dlpi_name) return 0;
    const char* base = std::strrchr(info->dlpi_name, '/');
    base = base ? base + 1 : info->dlpi_name;
    if (std::strcmp(base, "libart.so") != 0) return 0;
    auto* location = static_cast<ArtLocation*>(data);
    std::strncpy(location->path, info->dlpi_name, sizeof(location->path) - 1);
    location->bias = static_cast<std::uintptr_t>(info->dlpi_addr);
    return 1;
}
}  // namespace

ArtElf::~ArtElf() {
    if (image_) munmap(image_, size_);
}

const void* ArtElf::At(std::size_t offset, std::size_t length) const {
    if (!image_ || offset > size_ || length > size_ - offset) return nullptr;
    return static_cast<const unsigned char*>(image_) + offset;
}

bool ArtElf::Open() {
    if (header_) return true;
    ArtLocation location;
    dl_iterate_phdr(FindArt, &location);
    if (!location.path[0]) {
        __android_log_print(ANDROID_LOG_ERROR, "MD3Runtime", "libart.so mapping not found");
        return false;
    }
    int fd = open(location.path, O_RDONLY | O_CLOEXEC);
    struct stat statbuf{};
    if (fd < 0 || fstat(fd, &statbuf) != 0 || statbuf.st_size < sizeof(Elf64_Ehdr)) {
        if (fd >= 0) close(fd);
        __android_log_print(ANDROID_LOG_ERROR, "MD3Runtime", "cannot read ART ELF %s", location.path);
        return false;
    }
    size_ = static_cast<std::size_t>(statbuf.st_size);
    image_ = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (image_ == MAP_FAILED) {
        image_ = nullptr;
        return false;
    }
    header_ = static_cast<const Elf64_Ehdr*>(At(0, sizeof(Elf64_Ehdr)));
    if (!header_ || std::memcmp(header_->e_ident, ELFMAG, SELFMAG) != 0 ||
        header_->e_ident[EI_CLASS] != ELFCLASS64 ||
        header_->e_ident[EI_DATA] != ELFDATA2LSB || header_->e_machine != EM_AARCH64 ||
        header_->e_shentsize != sizeof(Elf64_Shdr) || !header_->e_shnum) {
        header_ = nullptr;
        return false;
    }
    sections_ = static_cast<const Elf64_Shdr*>(
        At(header_->e_shoff, std::size_t(header_->e_shnum) * sizeof(Elf64_Shdr)));
    segments_ = header_->e_phentsize == sizeof(Elf64_Phdr)
        ? static_cast<const Elf64_Phdr*>(At(header_->e_phoff,
              std::size_t(header_->e_phnum) * sizeof(Elf64_Phdr))) : nullptr;
    if (!sections_ || !segments_) {
        header_ = nullptr;
        return false;
    }
    bias_ = location.bias;
    __android_log_print(ANDROID_LOG_INFO, "MD3Runtime", "ART ELF ready: %s bias=%p",
                       location.path, reinterpret_cast<void*>(bias_));
    return true;
}

void* ArtElf::Find(std::string_view name, bool prefix) const {
    if (!header_) return nullptr;
    // Prefer the full static symbol table, falling back to exported symbols.
    for (auto section_type : {SHT_SYMTAB, SHT_DYNSYM}) {
        for (std::size_t i = 0; i < header_->e_shnum; ++i) {
            const auto& section = sections_[i];
            if (section.sh_type != section_type || section.sh_entsize != sizeof(Elf64_Sym) ||
                section.sh_link >= header_->e_shnum) continue;
            auto* symbols = static_cast<const Elf64_Sym*>(At(section.sh_offset, section.sh_size));
            const auto& strsection = sections_[section.sh_link];
            auto* strings = static_cast<const char*>(At(strsection.sh_offset, strsection.sh_size));
            if (!symbols || !strings) continue;
            for (std::size_t j = 0; j < section.sh_size / sizeof(Elf64_Sym); ++j) {
                const auto& symbol = symbols[j];
                if (symbol.st_shndx == SHN_UNDEF || symbol.st_name >= strsection.sh_size ||
                    symbol.st_value == 0) continue;
                const char* text = strings + symbol.st_name;
                auto remaining = strsection.sh_size - symbol.st_name;
                auto* end = static_cast<const char*>(std::memchr(text, 0, remaining));
                if (!end) continue;
                std::string_view candidate(text, static_cast<std::size_t>(end - text));
                if (prefix ? candidate.starts_with(name) : candidate == name) {
                    auto value = symbol.st_shndx == SHN_ABS ? symbol.st_value : bias_ + symbol.st_value;
                    if (symbol.st_shndx < header_->e_shnum &&
                        !(sections_[symbol.st_shndx].sh_flags & SHF_ALLOC)) continue;
                    if (ELF64_ST_TYPE(symbol.st_info) == STT_FUNC &&
                        !IsExecutable(reinterpret_cast<void*>(value))) continue;
                    __android_log_print(ANDROID_LOG_INFO, "MD3Runtime", "ART resolved %.*s -> %p (+%zx type=%u)",
                        static_cast<int>(candidate.size()), candidate.data(),
                        reinterpret_cast<void*>(value), static_cast<std::size_t>(value - bias_),
                        static_cast<unsigned>(ELF64_ST_TYPE(symbol.st_info)));
                    return reinterpret_cast<void*>(value);
                }
            }
        }
    }
    return nullptr;
}

bool ArtElf::IsExecutable(const void* address, std::size_t length) const {
    if (!header_ || !segments_) return false;
    auto value = reinterpret_cast<std::uintptr_t>(address);
    if (value < bias_) return false;
    auto offset = value - bias_;
    for (std::size_t i = 0; i < header_->e_phnum; ++i) {
        const auto& segment = segments_[i];
        if (segment.p_type == PT_LOAD && (segment.p_flags & PF_X) &&
            offset >= segment.p_vaddr && offset - segment.p_vaddr <= segment.p_memsz &&
            length <= segment.p_memsz - (offset - segment.p_vaddr)) return true;
    }
    return false;
}
