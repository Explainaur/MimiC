#include "mid/module.h"

using namespace mimic::mid;
using namespace mimic::define;
using namespace mimic::front;
using namespace mimic::opt;
using namespace mimic::back;

#define CREATE_BINARY(op, lhs, rhs)                    \
  do {                                                 \
    const auto &type = lhs->type();                    \
    assert(type->IsInteger());                         \
    return CreateBinary(BinaryOp::op, lhs, rhs, type); \
  } while (0)

#define CREATE_RELOP(op, lhs, rhs)                                \
  do {                                                            \
    auto bool_ty = MakePrimType(PrimType::Type::Int32, false);    \
    assert(lhs->type()->IsInteger() || lhs->type()->IsPointer()); \
    if (lhs->type()->IsUnsigned() || lhs->type()->IsPointer()) {  \
      return CreateBinary(BinaryOp::U##op, lhs, rhs, bool_ty);    \
    }                                                             \
    else {                                                        \
      return CreateBinary(BinaryOp::S##op, lhs, rhs, bool_ty);    \
    }                                                             \
  } while (0)

#define CREATE_BITOP(op, lhs, rhs)                     \
  do {                                                 \
    const auto &type = lhs->type();                    \
    assert(type->IsInteger());                         \
    return CreateBinary(BinaryOp::op, lhs, rhs, type); \
  } while (0)

namespace {

// type aliases of operators
using BinaryOp = BinarySSA::Operator;
using UnaryOp = UnarySSA::Operator;

}  // namespace

void Module::SealGlobalCtor() {
  if (global_ctor_ && !is_ctor_sealed_) {    
    insert_point_ = ctor_entry_;
    CreateJump(ctor_exit_);
    is_ctor_sealed_ = true;
  }
}

UserPtr Module::CreateFunction(LinkageTypes link, const std::string &name,
                              const TypePtr &type) {
  // assertion for type checking
  assert(type->IsFunction());
  // create function
  auto func = MakeSSA<FunctionSSA>(link, name);
  func->set_types(type);
  // add to global variables
  funcs_.push_back(func);
  return func;
}

BlockPtr Module::CreateBlock(const UserPtr &parent) {
  return CreateBlock(parent, "");
}

BlockPtr Module::CreateBlock(const UserPtr &parent,
                             const std::string &name) {
  // assertion for type checking
  assert(parent && parent->type()->IsFunction());
  // create block
  auto block = MakeSSA<BlockSSA>(parent, name);
  block->set_types(nullptr);
  // update parent function
  parent->AddValue(block);
  return block;
}

SSAPtr Module::CreateArgRef(const SSAPtr &func, std::size_t index) {
  // assertion for type checking
  auto args_type = *func->type()->GetArgsType();
  assert(index < args_type.size());
  // create argument reference
  auto arg_ref = MakeSSA<ArgRefSSA>(func, index);
  arg_ref->set_types(args_type[index]);
  return arg_ref;
}

SSAPtr Module::CreateStore(const SSAPtr &value, const SSAPtr &pointer) {
  // get proper pointer
  auto ptr = pointer, val = value;
  while (!ptr->type()->GetDerefedType() ||
         !ptr->type()->GetDerefedType()->CanAccept(val->type())) {
    ptr = ptr->GetAddr();
    assert(ptr);
  }
  // create cast (if necessary)
  auto target_ty = ptr->type()->GetDerefedType();
  if (!val->type()->IsIdentical(target_ty)) {
    val = CreateCast(val, target_ty);
  }
  // create store
  auto store = AddInst<StoreSSA>(val, ptr);
  store->set_types(nullptr);
  return store;
}

SSAPtr Module::CreateInit(const SSAPtr &value, const SSAPtr &pointer,
                          bool is_ref) {
  auto val = value;
  // handle references
  if (is_ref) {
    val = val->GetAddr();
    assert(val);
  }
  return CreateStore(val, pointer);
}

SSAPtr Module::CreateAlloca(const TypePtr &type) {
  // assertion for type checking
  assert(!type->IsVoid());
  // create allocation
  auto alloca = AddInst<AllocaSSA>();
  alloca->set_types(MakePointer(type));
  return alloca;
}

SSAPtr Module::CreateJump(const BlockPtr &target) {
  // create jump
  auto jump = AddInst<JumpSSA>(target);
  jump->set_types(nullptr);
  // update predecessor info
  target->AddValue(insert_point_);
  return jump;
}

SSAPtr Module::CreateReturn(const SSAPtr &value) {
  // get proper return value
  const auto &func_type = insert_point_->parent()->org_type();
  auto ret_type = func_type->GetReturnType(*func_type->GetArgsType());
  auto val = value;
  // assertion for type checking
  assert((ret_type->IsVoid() && !val) ||
         ret_type->GetTrivialType()->IsIdentical(val->type()));
  // create return
  auto ret = AddInst<ReturnSSA>(val);
  ret->set_types(nullptr);
  return ret;
}

GlobalVarPtr Module::CreateGlobalVar(LinkageTypes link, bool is_var,
                                     const std::string &name,
                                     const TypePtr &type,
                                     const SSAPtr &init) {
  // assertions for type checking
  assert(!type->IsVoid());
  auto var_type = type->GetTrivialType();
  assert(!init || var_type->IsIdentical(init->type()));
  assert(!init || init->IsConst());
  // create global variable definition
  auto global = MakeSSA<GlobalVarSSA>(link, is_var, name, init);
  global->set_type(MakePointer(var_type, false));
  global->set_org_type(MakePointer(type));
  // add to global variables
  vars_.push_back(global);
  return global;
}

GlobalVarPtr Module::CreateGlobalVar(LinkageTypes link, bool is_var,
                                     const std::string &name,
                                     const TypePtr &type) {
  return CreateGlobalVar(link, is_var, name, type, nullptr);
}

SSAPtr Module::CreateBranch(const SSAPtr &cond, const BlockPtr &true_block,
                            const BlockPtr &false_block) {
  // assertion for type checking
  assert(cond->type()->IsInteger());
  // create branch
  auto branch = AddInst<BranchSSA>(cond, true_block, false_block);
  branch->set_types(nullptr);
  // update predecessor info
  true_block->AddValue(insert_point_);
  false_block->AddValue(insert_point_);
  return branch;
}

SSAPtr Module::CreateLoad(const SSAPtr &ptr, bool is_ref) {
  // assertion for type checking
  assert(ptr->type()->IsPointer());
  // create load
  auto load = AddInst<LoadSSA>(ptr);
  load->set_type(ptr->type()->GetDerefedType());
  load->set_org_type(ptr->org_type()->GetDerefedType());
  return is_ref ? CreateLoad(load, false) : load;
}

SSAPtr Module::CreateCall(const SSAPtr &callee, const SSAPtrList &args) {
  // assertion for type checking
  assert(callee->type()->IsFunction());
  auto args_type = *callee->org_type()->GetArgsType();
  assert(args_type.size() == args.size());
  // get argument list
  SSAPtrList casted_args;
  auto arg_it = args.begin();
  for (const auto &i : args_type) {
    auto arg = *arg_it++;
    // perform necessary type casting
    auto arg_ty = i->GetTrivialType();
    if (!arg->type()->IsIdentical(arg_ty)) {
      arg = CreateCast(arg, arg_ty);
    }
    casted_args.push_back(std::move(arg));
  }
  // create call
  auto call = AddInst<CallSSA>(callee, casted_args);
  auto ret_type = callee->org_type()->GetReturnType(args_type);
  call->set_types(ret_type);
  return call;
}

SSAPtr Module::CreatePtrAccess(const SSAPtr &ptr, const SSAPtr &index) {
  // assertion for type checking
  assert(ptr->type()->IsPointer() && index->type()->IsInteger());
  // create access
  auto acc_type = AccessSSA::AccessType::Pointer;
  auto access = AddInst<AccessSSA>(acc_type, ptr, index);
  access->set_type(ptr->type());
  access->set_org_type(ptr->org_type());
  return access;
}

SSAPtr Module::CreateElemAccess(const SSAPtr &ptr, const SSAPtr &index,
                                const TypePtr &type) {
  // get proper pointer
  auto pointer = ptr;
  if (!pointer->type()->IsPointer()) pointer = pointer->GetAddr();
  // assertion for type checking
  assert(pointer->type()->GetDerefedType()->GetLength() &&
         index->type()->IsInteger());
  // create access
  auto acc_type = AccessSSA::AccessType::Element;
  auto access = AddInst<AccessSSA>(acc_type, pointer, index);
  access->set_types(MakePointer(type));
  return access;
}

SSAPtr Module::CreateBinary(BinaryOp op, const SSAPtr &lhs,
                            const SSAPtr &rhs, const TypePtr &type) {
  // assertion for type checking
  assert(lhs->type()->IsIdentical(rhs->type()));
  // create binary
  auto binary = AddInst<BinarySSA>(op, lhs, rhs);
  binary->set_types(type);
  return binary;
}

SSAPtr Module::CreateUnary(UnaryOp op, const SSAPtr &opr,
                           const TypePtr &type) {
  auto unary = AddInst<UnarySSA>(op, opr);
  unary->set_types(type);
  return unary;
}

SSAPtr Module::CreateEqual(const SSAPtr &lhs, const SSAPtr &rhs) {
  auto bool_ty = MakePrimType(PrimType::Type::Int32, false);
  assert(lhs->type()->IsInteger() || lhs->type()->IsFunction() ||
         lhs->type()->IsPointer());
  return CreateBinary(BinaryOp::Equal, lhs, rhs, bool_ty);
}

SSAPtr Module::CreateNeg(const SSAPtr &opr) {
  const auto &type = opr->type();
  assert(type->IsInteger());
  return CreateUnary(UnaryOp::Neg, opr, type);
}

SSAPtr Module::CreateAdd(const SSAPtr &lhs, const SSAPtr &rhs) {
  CREATE_BINARY(Add, lhs, rhs);
}

SSAPtr Module::CreateSub(const SSAPtr &lhs, const SSAPtr &rhs) {
  CREATE_BINARY(Sub, lhs, rhs);
}

SSAPtr Module::CreateMul(const SSAPtr &lhs, const SSAPtr &rhs) {
  CREATE_BINARY(Mul, lhs, rhs);
}

SSAPtr Module::CreateDiv(const SSAPtr &lhs, const SSAPtr &rhs) {
  const auto &type = lhs->type();
  assert(type->IsInteger());
  auto op = type->IsUnsigned() ? BinaryOp::UDiv : BinaryOp::SDiv;
  return CreateBinary(op, lhs, rhs, type);
}

SSAPtr Module::CreateRem(const SSAPtr &lhs, const SSAPtr &rhs) {
  const auto &type = lhs->type();
  assert(type->IsInteger());
  auto op = type->IsUnsigned() ? BinaryOp::URem : BinaryOp::SRem;
  return CreateBinary(op, lhs, rhs, type);
}

SSAPtr Module::CreateNotEq(const SSAPtr &lhs, const SSAPtr &rhs) {
  auto bool_ty = MakePrimType(PrimType::Type::Int32, false);
  assert(lhs->type()->IsInteger() || lhs->type()->IsFunction() ||
         lhs->type()->IsPointer());
  return CreateBinary(BinaryOp::NotEq, lhs, rhs, bool_ty);
}

SSAPtr Module::CreateLess(const SSAPtr &lhs, const SSAPtr &rhs) {
  CREATE_RELOP(Less, lhs, rhs);
}

SSAPtr Module::CreateLessEq(const SSAPtr &lhs, const SSAPtr &rhs) {
  CREATE_RELOP(LessEq, lhs, rhs);
}

SSAPtr Module::CreateGreat(const SSAPtr &lhs, const SSAPtr &rhs) {
  CREATE_RELOP(Great, lhs, rhs);
}

SSAPtr Module::CreateGreatEq(const SSAPtr &lhs, const SSAPtr &rhs) {
  CREATE_RELOP(GreatEq, lhs, rhs);
}

SSAPtr Module::CreateAnd(const SSAPtr &lhs, const SSAPtr &rhs) {
  CREATE_BITOP(And, lhs, rhs);
}

SSAPtr Module::CreateOr(const SSAPtr &lhs, const SSAPtr &rhs) {
  CREATE_BITOP(Or, lhs, rhs);
}

SSAPtr Module::CreateXor(const SSAPtr &lhs, const SSAPtr &rhs) {
  CREATE_BITOP(Xor, lhs, rhs);
}

SSAPtr Module::CreateShl(const SSAPtr &lhs, const SSAPtr &rhs) {
  CREATE_BITOP(Shl, lhs, rhs);
}

SSAPtr Module::CreateShr(const SSAPtr &lhs, const SSAPtr &rhs) {
  const auto &type = lhs->type();
  assert(type->IsInteger());
  auto op = type->IsUnsigned() ? BinaryOp::LShr : BinaryOp::AShr;
  return CreateBinary(op, lhs, rhs, type);
}

SSAPtr Module::CreateCast(const SSAPtr &opr, const TypePtr &type) {
  // assertion for type checking
  const auto &opr_ty = opr->type();
  auto target = type->GetTrivialType();
  assert(opr_ty->CanCastTo(target));
  // check if is redundant type casting
  if (opr_ty->IsIdentical(target)) return opr;
  // get address of array
  auto operand = opr;
  if (opr_ty->IsArray()) {
    operand = operand->GetAddr();
    assert(operand);
  }
  // create type casting
  SSAPtr cast;
  if (operand->IsConst()) {
    // create a constant type casting, do not insert as an instruction
    cast = MakeSSA<CastSSA>(operand);
  }
  else {
    // create a non-constant type casting
    assert(insert_point_);
    cast = AddInst<CastSSA>(operand);
  }
  cast->set_type(target);
  cast->set_org_type(type);
  return cast;
}

SSAPtr Module::CreateLogicNot(const SSAPtr &opr) {
  // assertion for type checking
  const auto &type = opr->type();
  assert(type->IsInteger());
  static_cast<void>(type);
  // create logic not operation
  auto bool_ty = MakePrimType(PrimType::Type::Int32, false);
  return CreateUnary(UnaryOp::LogicNot, opr, bool_ty);
}

SSAPtr Module::CreateNot(const SSAPtr &opr) {
  const auto &type = opr->type();
  assert(type->IsInteger());
  return CreateUnary(UnaryOp::Not, opr, type);
}

SSAPtr Module::GetZero(const TypePtr &type) {
  // assertion for type checking
  assert(type->IsBasic() || type->IsStruct() || type->IsArray());
  // create constant zero
  auto zero = MakeSSA<ConstZeroSSA>();
  zero->set_types(type);
  return zero;
}

SSAPtr Module::GetInt(std::uint32_t value, const TypePtr &type) {
  // assertion for type checking
  assert(type->IsInteger() || type->IsEnum());
  // create constant integer
  auto const_int = MakeSSA<ConstIntSSA>(value);
  const_int->set_types(type);
  return const_int;
}

SSAPtr Module::GetInt32(std::uint32_t value) {
  auto type = MakePrimType(PrimType::Type::Int32, false);
  return GetInt(value, type);
}

SSAPtr Module::GetBool(bool value) {
  auto type = MakePrimType(PrimType::Type::Int32, false);
  return GetInt(value, type);
}

SSAPtr Module::GetString(const std::string &str, const TypePtr &type) {
  // assertion for type checking
  assert(type->IsPointer() && type->GetDerefedType()->IsInteger() &&
         type->GetDerefedType()->GetSize() == 1);
  // create constant string
  auto const_str = MakeSSA<ConstStrSSA>(str);
  const_str->set_types(type);
  return const_str;
}

SSAPtr Module::GetStruct(const SSAPtrList &elems, const TypePtr &type) {
  // assertions for type checking
  assert(type->IsStruct() && type->GetLength() == elems.size());
  auto struct_ty = type->GetTrivialType();
  int index = 0;
  static_cast<void>(index);
  for (const auto &i : elems) {
    assert(i->IsConst());
    assert(struct_ty->GetElem(index++)->IsIdentical(i->type()));
    static_cast<void>(i);
  }
  // create constant struct
  auto const_struct = MakeSSA<ConstStructSSA>(elems);
  const_struct->set_type(struct_ty);
  const_struct->set_org_type(type);
  return const_struct;
}

SSAPtr Module::GetArray(const SSAPtrList &elems, const TypePtr &type) {
  // assertions for type checking
  assert(type->IsArray() && type->GetLength() == elems.size());
  auto array_ty = type->GetTrivialType();
  for (const auto &i : elems) {
    assert(i->IsConst());
    assert(array_ty->GetDerefedType()->IsIdentical(i->type()));
    static_cast<void>(i);
  }
  // create constant array
  auto const_array = MakeSSA<ConstArraySSA>(elems);
  const_array->set_type(array_ty);
  const_array->set_org_type(type);
  return const_array;
}

xstl::Guard Module::SetContext(const Logger &logger) {
  auto log = std::make_shared<Logger>(logger);
  loggers_.push(log);
  return xstl::Guard([this] { loggers_.pop(); });
}

xstl::Guard Module::EnterGlobalCtor() {
  // get current insert point
  auto cur_block = insert_point_;
  // initialize global function if it does not exist
  if (!global_ctor_) {
    // create function
    auto link = LinkageTypes::GlobalCtor;
    auto ty = std::make_shared<FuncType>(TypePtrList(), MakeVoid(), true);
    global_ctor_ = CreateFunction(link, "_$ctor", ty);
    // create basic blocks
    ctor_entry_ = CreateBlock(global_ctor_, "entry");
    ctor_exit_ = CreateBlock(global_ctor_, "exit");
    insert_point_ = ctor_exit_;
    CreateReturn(nullptr);
    // mark as not sealed
    is_ctor_sealed_ = false;
  }
  // switch to global function's body block
  insert_point_ = ctor_entry_;
  return xstl::Guard([this, cur_block] { insert_point_ = cur_block; });
}

void Module::Dump(std::ostream &os) {
  IdManager idm;
  SealGlobalCtor();
  // dump global variables
  for (const auto &i : vars_) {
    i->Dump(os, idm);
    os << std::endl;
  }
  // dump global functions
  for (const auto &i : funcs_) {
    i->Dump(os, idm);
    os << std::endl;
  }
}

void Module::RunPasses(PassManager &pass_man) {
  SealGlobalCtor();
  pass_man.set_vars(&vars_);
  pass_man.set_funcs(&funcs_);
  pass_man.RunPasses();
}

void Module::GenerateCode(CodeGen &gen) {
  SealGlobalCtor();
  // generate global variables
  for (const auto &i : vars_) {
    i->GenerateCode(gen);
  }
  // generate global functions
  for (const auto &i : funcs_) {
    i->GenerateCode(gen);
  }
}
