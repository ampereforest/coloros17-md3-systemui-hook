#include "platform_macro.h"
#if defined(TARGET_ARCH_ARM64)

#include "dobby_internal.h"

#include "core/assembler/assembler-arm64.h"
#include "core/codegen/codegen-arm64.h"

#include "MemoryAllocator/NearMemoryAllocator.h"
#include "InstructionRelocation/arm64/InstructionRelocationARM64.h"
#include "InterceptRouting/RoutingPlugin/RoutingPlugin.h"

using namespace zz::arm64;

CodeBufferBase *GenerateNormalTrampolineBuffer(addr_t from, addr_t to) {
  TurboAssembler turbo_assembler_((void *)from);
#define _ turbo_assembler_.

  // A fixed-width absolute jump avoids both ADRP range assumptions and a
  // runtime literal-pool load. Keep all four pieces, including bits 32..63:
  // Android's address randomization can place ART/module mappings >4 GiB apart.
  _ Mov(TMP_REG_0, static_cast<uint64_t>(to));
  _ br(TMP_REG_0);
#undef _

  // Bind all labels
  turbo_assembler_.RelocBind();

  auto result = turbo_assembler_.GetCodeBuffer()->Copy();
  return result;
}

#endif
