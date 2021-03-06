#include <unordered_set>

#include "opt/pass.h"
#include "opt/passman.h"
#include "opt/helper/cast.h"
#include "opt/helper/inst.h"
#include "opt/analysis/dominance.h"
#include "opt/analysis/loopinfo.h"

using namespace mimic::mid;
using namespace mimic::opt;

namespace {

/*
  perform loop invariant code motion
  this pass will detect invariants in all loops
  and move it to a new block before loop's entry
*/
class LoopInvariantCodeMotionPass : public FunctionPass {
 public:
  LoopInvariantCodeMotionPass() {}

  bool RunOnFunction(const FuncPtr &func) override {
    if (func->is_decl()) return false;
    // run on loops
    bool changed = false;
    // prepare parent info
    // must be rescanned in each iteration since parent info
    // may be changed due to preheader insertion
    ParentScanner parent(func);
    parent_ = &parent;
    // prepare dominance checker
    dom_ = &PassManager::GetPass<DominanceInfoPass>("dom_info");
    // scan for all loops
    const auto &li = PassManager::GetPass<LoopInfoPass>("loop_info");
    const auto &loops = li.GetLoopInfo(func.get());
    for (const auto &info : loops) {
      cur_loop_ = &info;
      if (ProcessLoop()) changed = true;
    }
    return changed;
  }

  void CleanUp() override {
    invs_.clear();
  }

  void RunOn(AccessSSA &ssa) override { LogInvariant(ssa); }
  void RunOn(BinarySSA &ssa) override { LogInvariant(ssa); }
  void RunOn(UnarySSA &ssa) override { LogInvariant(ssa); }
  void RunOn(CastSSA &ssa) override { LogInvariant(ssa); }
  void RunOn(AllocaSSA &ssa) override { assert(false); }
  void RunOn(SelectSSA &ssa) override { LogInvariant(ssa); }

  // special treatment for load instructions
  // do not mark loads with pointer that has been stored in loop
  void RunOn(LoadSSA &ssa) override {
    if (stored_ptrs_.count(GetBasePointer(ssa.ptr().get()))) return;
    LogInvariant(ssa);
  }

 private:
  bool IsInvariant(const SSAPtr &val);
  void LogInvariant(User &ssa);
  Value *GetBasePointer(Value *ptr);
  void ProcessStores();
  bool ProcessLoop();

  // helper passes & analysis passes
  ParentScanner *parent_;
  const DominanceInfoPass *dom_;
  // loop that current being processed
  const LoopInfo *cur_loop_;
  // block that current being processed
  BlockSSA *cur_block_;
  // all invariants in current loop
  std::unordered_set<Value *> marked_invs_;
  SSAPtrList invs_;
  // pointers that has been stored in current loop
  std::unordered_set<Value *> stored_ptrs_;
};

}  // namespace

// register current pass
REGISTER_PASS(LoopInvariantCodeMotionPass, licm)
    .set_min_opt_level(2)
    .set_stages(PassStage::Opt)
    .Requires("dom_info")
    .Requires("loop_info")
    .Requires("loop_norm")
    .Requires("loop_reduce");


// check if value is an invariant
bool LoopInvariantCodeMotionPass::IsInvariant(const SSAPtr &val) {
  // value is constant or undef
  if (val->IsConst() || val->IsUndef()) return true;
  // value is argument reference or global variable
  if (IsSSA<ArgRefSSA>(val) || IsSSA<GlobalVarSSA>(val)) return true;
  // value is not in current loop
  if (!cur_loop_->body.count(parent_->GetParent(val.get()))) return true;
  // value is already an invariant
  if (marked_invs_.count(val.get())) return true;
  return false;
}

// mark user as an invariant if conditions permit
void LoopInvariantCodeMotionPass::LogInvariant(User &ssa) {
  // all operands must be invariant
  for (const auto &i : ssa) {
    if (!IsInvariant(i.value())) return;
  }
  // parent block of value must dominance all parent blocks of it's users
  for (const auto &i : ssa.uses()) {
    auto parent = parent_->GetParent(i->user());
    if (!cur_loop_->body.count(parent)) continue;
    if (!dom_->IsDominate(cur_block_, parent)) return;
  }
  // add to invariant list
  marked_invs_.insert(&ssa);
  if (!ssa.uses().empty()) {
    invs_.push_back(ssa.uses().front()->value());
  }
}

Value *LoopInvariantCodeMotionPass::GetBasePointer(Value *ptr) {
  // get base pointer of access/cast
  for (;;) {
    if (auto acc = SSADynCast<AccessSSA>(ptr)) {
      ptr = acc->ptr().get();
    }
    else if (auto cast = SSADynCast<CastSSA>(ptr)) {
      ptr = cast->opr().get();
    }
    else if (auto phi = SSADynCast<PhiSSA>(ptr)) {
      // TODO: tricky implementation, fixme!
      std::unordered_set<Value *> users;
      for (const auto &i : phi->uses()) {
        users.insert(i->user());
      }
      for (const auto &i : *phi) {
        auto opr = SSACast<PhiOperandSSA>(i.value().get());
        if (!users.count(opr->value().get())) {
          ptr = opr->value().get();
        }
      }
    }
    else {
      return ptr;
    }
  }
}

void LoopInvariantCodeMotionPass::ProcessStores() {
  // TODO: actually there should be a pointer alias analysis here
  stored_ptrs_.clear();
  for (const auto &block : cur_loop_->body) {
    for (const auto &i : block->insts()) {
      if (auto store = SSADynCast<StoreSSA>(i.get())) {
        // get base pointer of access/cast
        auto ptr = GetBasePointer(store->ptr().get());
        // if stored to argument, treat all arguments as stored
        // to prevent pointer alias
        if (IsSSA<ArgRefSSA>(ptr)) {
          auto &args = SSACast<FunctionSSA>(block->parent().get())->args();
          for (const auto &arg : args) {
            if (arg->type()->IsPointer()) stored_ptrs_.insert(arg.get());
          }
        }
        stored_ptrs_.insert(ptr);
      }
    }
  }
}

bool LoopInvariantCodeMotionPass::ProcessLoop() {
  // mark all stored pointers in loop
  ProcessStores();
  // scan all invariants
  marked_invs_.clear();
  invs_.clear();
  std::size_t last_size = 1;
  while (marked_invs_.size() != last_size) {
    last_size = marked_invs_.size();
    for (const auto &block : cur_loop_->body) {
      cur_block_ = block;
      for (const auto &i : block->insts()) {
        if (!marked_invs_.count(i.get())) i->RunPass(*this);
      }
    }
  }
  if (invs_.empty()) return false;
  // insert invariant instructions to preheader
  auto block = cur_loop_->preheader;
  assert(block);
  auto pos = --block->insts().end();
  block->insts().insert(pos, invs_.begin(), invs_.end());
  // remove invariant instructions from their parent
  for (const auto &i : invs_) {
    auto parent = parent_->GetParent(i.get());
    parent->insts().remove(i);
  }
  return true;
}
