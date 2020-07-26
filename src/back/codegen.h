#ifndef MIMIC_BACK_CODEGEN_H_
#define MIMIC_BACK_CODEGEN_H_

#include <ostream>
#include <cstddef>

#include "mid/ssa.h"

namespace mimic::back {

// interface of all code generators
class CodeGenInterface {
 public:
  virtual ~CodeGenInterface() = default;

  virtual void GenerateOn(mid::LoadSSA &ssa) = 0;
  virtual void GenerateOn(mid::StoreSSA &ssa) = 0;
  virtual void GenerateOn(mid::AccessSSA &ssa) = 0;
  virtual void GenerateOn(mid::BinarySSA &ssa) = 0;
  virtual void GenerateOn(mid::UnarySSA &ssa) = 0;
  virtual void GenerateOn(mid::CastSSA &ssa) = 0;
  virtual void GenerateOn(mid::CallSSA &ssa) = 0;
  virtual void GenerateOn(mid::BranchSSA &ssa) = 0;
  virtual void GenerateOn(mid::JumpSSA &ssa) = 0;
  virtual void GenerateOn(mid::ReturnSSA &ssa) = 0;
  virtual void GenerateOn(mid::FunctionSSA &ssa) = 0;
  virtual void GenerateOn(mid::GlobalVarSSA &ssa) = 0;
  virtual void GenerateOn(mid::AllocaSSA &ssa) = 0;
  virtual void GenerateOn(mid::BlockSSA &ssa) = 0;
  virtual void GenerateOn(mid::ArgRefSSA &ssa) = 0;
  virtual void GenerateOn(mid::ConstIntSSA &ssa) = 0;
  virtual void GenerateOn(mid::ConstStrSSA &ssa) = 0;
  virtual void GenerateOn(mid::ConstStructSSA &ssa) = 0;
  virtual void GenerateOn(mid::ConstArraySSA &ssa) = 0;
  virtual void GenerateOn(mid::ConstZeroSSA &ssa) = 0;
  virtual void GenerateOn(mid::SelectSSA &ssa) = 0;
  virtual void GenerateOn(mid::UndefSSA &ssa) = 0;

  // dump code in current generator
  virtual void Dump(std::ostream &os) const = 0;
};

using CodeGen = CodeGenInterface;

}  // namespace mimic::back

#endif  // MIMIC_BACK_CODEGEN_H_
