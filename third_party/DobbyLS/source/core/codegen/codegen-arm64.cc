#include "platform_macro.h"
#if defined(TARGET_ARCH_ARM64)

#include "dobby_internal.h"
#include "core/codegen/codegen-arm64.h"

namespace zz {
namespace arm64 {

void CodeGen::LiteralLdrBranch(uint64_t address) {
  auto turbo_assembler_ = reinterpret_cast<TurboAssembler *>(this->assembler_);
#define _ turbo_assembler_->

  // Use the same literal-free absolute jump for relocated-function tails.
  // This also leaves the original PACIASP/SP relationship intact.
  _ Mov(TMP_REG_0, address);
  _ br(TMP_REG_0);

#undef _
}

} // namespace arm64
} // namespace zz

#endif
