#include <iostream>
#include <unordered_map>

#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/InstCombine/InstCombine.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"

#include "jit.h"
#include "rgd_op.h"
#include "rgdJit.h"
#include "util.h"

using namespace llvm;
using namespace rgd;

std::unique_ptr<GradJit> JIT;

static llvm::Value* codegen(llvm::IRBuilder<> &Builder,
    const AstNode* node,
    std::map<uint32_t, uint32_t> &local_map, llvm::Value* arg,
    std::unordered_map<uint32_t, llvm::Value*> &value_cache,
    std::unordered_map<uint32_t, AstNode*> &expr_cache) {
  llvm::Value* ret = nullptr;
  //std::cout << "code gen and nargs is " << nargs << std::endl;
  auto r1 = expr_cache.find(node->label());

  if (node->label() != 0 &&
      r1 != expr_cache.end()) {
    node = expr_cache[node->label()];
  }

  auto itr = value_cache.find(node->label());
  if (node->label() != 0
      && itr != value_cache.end()) {
    //std::cout << " value cache hit and label is " << node->label() << std::endl;
    return itr->second;
  }

  switch (node->kind()) {
    case rgd::Bool: {
      // getTrue is actually 1 bit integer 1
      if (node->boolvalue())
        ret = llvm::ConstantInt::getTrue(Builder.getContext());
      else
        ret = llvm::ConstantInt::getFalse(Builder.getContext());
      break;
    }
    case rgd::Constant: {
      // The constant is now loading from arguments
      uint32_t start = node->index();
      uint32_t length = node->bits() / 8;

      llvm::Value* idx[1];
      idx[0] = llvm::ConstantInt::get(Builder.getInt32Ty(), start + RET_OFFSET);
      ret = Builder.CreateLoad(Builder.CreateGEP(arg, idx));
      ret = Builder.CreateTrunc(ret,
          llvm::Type::getIntNTy(Builder.getContext(), node->bits()));
      break;
    }
    case rgd::Read: {
      uint32_t start = local_map[node->index()];
      size_t length = node->bits() / 8;
      //std::cout << "read index " << start << " length " << length << std::endl;
      llvm::Value* idx[1];
      idx[0] = llvm::ConstantInt::get(Builder.getInt32Ty(), start + RET_OFFSET);
      ret = Builder.CreateLoad(Builder.CreateGEP(arg, idx));
      for (uint32_t k = 1; k < length; k++) {
        idx[0] = llvm::ConstantInt::get(Builder.getInt32Ty(), start + k + RET_OFFSET);
        llvm::Value* tmp = Builder.CreateLoad(Builder.CreateGEP(arg,idx));
        tmp = Builder.CreateShl(tmp, 8 * k);
        ret = Builder.CreateAdd(ret, tmp);
      }
      ret = Builder.CreateTrunc(ret,
          llvm::Type::getIntNTy(Builder.getContext(), node->bits()));
      break;
    }
    case rgd::Concat: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      uint32_t bits = rc1->bits() + rc2->bits(); 
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateOr(
          Builder.CreateShl(
            Builder.CreateZExt(c2,llvm::Type::getIntNTy(Builder.getContext(),bits)),
            rc1->bits()),
          Builder.CreateZExt(c1, llvm::Type::getIntNTy(Builder.getContext(), bits)));
      break;
    }
    case rgd::Extract: {
#if DEBUG
      //std::cerr << "Extract expression" << std::endl;
#endif
      const AstNode* rc = &node->children(0);
      llvm::Value* c = codegen(Builder, rc, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateTrunc(
          Builder.CreateLShr(c, node->index()),
          llvm::Type::getIntNTy(Builder.getContext(), node->bits()));
      break;
    }
    case rgd::ZExt: {
#if DEBUG
      // std::cerr << "ZExt the bits is " << node->bits() << std::endl;
#endif
      const AstNode* rc = &node->children(0);
      llvm::Value* c = codegen(Builder, rc, local_map, arg, value_cache, expr_cache);
      //FIXME: we may face ZEXT to boolean expr
      ret = Builder.CreateZExtOrTrunc(c,
          llvm::Type::getIntNTy(Builder.getContext(), node->bits()));
      break;
    }
    case rgd::SExt: {
#if DEBUG
      // std::cerr << "SExt the bits is " << node->bits() << std::endl;
#endif
      const AstNode* rc = &node->children(0);
      llvm::Value* c = codegen(Builder, rc,local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateSExt(c,
          llvm::Type::getIntNTy(Builder.getContext(), node->bits()));
      break;
    }
    case rgd::Add: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateAdd(c1, c2);
      break;
    }
    case rgd::Sub: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateSub(c1, c2);
      break;
    }
    case rgd::Mul: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateMul(c1, c2);
      break;
    }
    case rgd::UDiv: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      llvm::Value* VA0 = llvm::ConstantInt::get(llvm::Type::getIntNTy(Builder.getContext(), node->bits()), 0);
      llvm::Value* VA1 = llvm::ConstantInt::get(llvm::Type::getIntNTy(Builder.getContext(), node->bits()), 1);
      // FIXME: this is a hack to avoid division by zero, but should use a better way
      // FIXME: should record the divisor to avoid gradient vanish
      llvm::Value* cond = Builder.CreateICmpEQ(c2, VA0);
      llvm::Value* divisor = Builder.CreateSelect(cond, VA1, c2);
      ret = Builder.CreateUDiv(c1, divisor);
      break;
    }
    case rgd::SDiv: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      llvm::Value* VA0 = llvm::ConstantInt::get(llvm::Type::getIntNTy(Builder.getContext(), node->bits()), 0);
      llvm::Value* VA1 = llvm::ConstantInt::get(llvm::Type::getIntNTy(Builder.getContext(), node->bits()), 1);
      // FIXME: this is a hack to avoid division by zero, but should use a better way
      // FIXME: should record the divisor to avoid gradient vanish
      llvm::Value* cond = Builder.CreateICmpEQ(c2, VA0);
      llvm::Value* divisor = Builder.CreateSelect(cond, VA1, c2);
      ret = Builder.CreateSDiv(c1, divisor);
      break;
    }
    case rgd::URem: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      llvm::Value* VA0 = llvm::ConstantInt::get(llvm::Type::getIntNTy(Builder.getContext(), node->bits()), 0);
      llvm::Value* VA1 = llvm::ConstantInt::get(llvm::Type::getIntNTy(Builder.getContext(), node->bits()), 1);
      // FIXME: this is a hack to avoid division by zero, but should use a better way
      // FIXME: should record the divisor to avoid gradient vanish
      llvm::Value* cond = Builder.CreateICmpEQ(c2, VA0);
      llvm::Value* divisor = Builder.CreateSelect(cond, VA1, c2);
      ret = Builder.CreateURem(c1, divisor);
      break;
    }
    case rgd::SRem: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      llvm::Value* VA0 = llvm::ConstantInt::get(llvm::Type::getIntNTy(Builder.getContext(), node->bits()), 0);
      llvm::Value* VA1 = llvm::ConstantInt::get(llvm::Type::getIntNTy(Builder.getContext(), node->bits()), 1);
      // FIXME: this is a hack to avoid division by zero, but should use a better way
      // FIXME: should record the divisor to avoid gradient vanish
      llvm::Value* cond = Builder.CreateICmpEQ(c2, VA0);
      llvm::Value* divisor = Builder.CreateSelect(cond, VA1, c2);
      ret = Builder.CreateSRem(c1, divisor);
      break;
    }
    case rgd::Neg: {
      const AstNode* rc = &node->children(0);
      llvm::Value* c = codegen(Builder, rc, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateNeg(c);
      break;
    }
    case rgd::Not: {
      const AstNode* rc = &node->children(0);
      llvm::Value* c = codegen(Builder, rc, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateNot(c);
      break;
    }
    case rgd::And: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateAnd(c1, c2);
      break;
    }
    case rgd::Or: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateOr(c1, c2);
      break;
    }
    case rgd::Xor: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateXor(c1, c2);
      break;
    }
    case rgd::Shl: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateShl(c1, c2);
      break;
    }
    case rgd::LShr: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateLShr(c1, c2);
      break;
    }
    case rgd::AShr: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      ret = Builder.CreateAShr(c1, c2);
      break;
    }
    // all the following ICmp expressions should be top level
    case rgd::Equal: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      // extend to 64-bit to avoid overflow
      llvm::Value* c1e = Builder.CreateZExt(c1, Builder.getInt64Ty());
      llvm::Value* c2e = Builder.CreateZExt(c2, Builder.getInt64Ty());

      // save the comparison operands to the output args
      // so it's easier to negate the condition
      llvm::Value* idx[1];
      idx[0] = llvm::ConstantInt::get(Builder.getInt32Ty(), 0);
      Builder.CreateStore(c1e, Builder.CreateGEP(arg, idx));
      idx[0] = llvm::ConstantInt::get(Builder.getInt32Ty(), 1);
      Builder.CreateStore(c2e, Builder.CreateGEP(arg, idx));

      ret = nullptr;
      break;
    }
    case rgd::Distinct: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      // extend to 64-bit to avoid overflow
      llvm::Value* c1e = Builder.CreateZExt(c1, Builder.getInt64Ty());
      llvm::Value* c2e = Builder.CreateZExt(c2, Builder.getInt64Ty());

      // save the comparison operands to the output args
      // so it's easier to negate the condition
      llvm::Value* idx[1];
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 0);
      Builder.CreateStore(c1e, Builder.CreateGEP(arg, idx));
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 1);
      Builder.CreateStore(c2e, Builder.CreateGEP(arg, idx));

      ret = nullptr;
      break;
    }
    case rgd::Ult: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      // extend to 64-bit to avoid overflow
      llvm::Value* c1e = Builder.CreateZExt(c1, Builder.getInt64Ty());
      llvm::Value* c2e = Builder.CreateZExt(c2, Builder.getInt64Ty());

      // save the comparison operands to the output args
      // so it's easier to negate the condition
      llvm::Value* idx[1];
      idx[0] = llvm::ConstantInt::get(Builder.getInt32Ty(), 0);
      Builder.CreateStore(c1e, Builder.CreateGEP(arg, idx));
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 1);
      Builder.CreateStore(c2e, Builder.CreateGEP(arg, idx));

      ret = nullptr;
      break;
    }
    case rgd::Ule: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      // extend to 64-bit to avoid overflow
      llvm::Value* c1e = Builder.CreateZExt(c1, Builder.getInt64Ty());
      llvm::Value* c2e = Builder.CreateZExt(c2, Builder.getInt64Ty());

      // save the comparison operands to the output args
      // so it's easier to negate the condition
      llvm::Value* idx[1];
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 0);
      Builder.CreateStore(c1e, Builder.CreateGEP(arg, idx));
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 1);
      Builder.CreateStore(c2e, Builder.CreateGEP(arg, idx));

      ret = nullptr;
      break;
    }
    case rgd::Ugt: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      // extend to 64-bit to avoid overflow
      llvm::Value* c1e = Builder.CreateZExt(c1, Builder.getInt64Ty());
      llvm::Value* c2e = Builder.CreateZExt(c2, Builder.getInt64Ty());

      // save the comparison operands to the output args
      // so it's easier to negate the condition
      llvm::Value* idx[1];
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(),0);
      Builder.CreateStore(c1e, Builder.CreateGEP(arg,idx));
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(),1);
      Builder.CreateStore(c2e, Builder.CreateGEP(arg,idx));

      ret = nullptr;
      break;
    }
    case rgd::Uge: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      // extend to 64-bit to avoid overflow
      llvm::Value* c1e = Builder.CreateZExt(c1, Builder.getInt64Ty());
      llvm::Value* c2e = Builder.CreateZExt(c2, Builder.getInt64Ty());

      // save the comparison operands to the output args
      // so it's easier to negate the condition
      llvm::Value* idx[1];
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 0);
      Builder.CreateStore(c1e, Builder.CreateGEP(arg, idx));
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 1);
      Builder.CreateStore(c2e, Builder.CreateGEP(arg, idx));

      ret = nullptr;
      break;
    }
    case rgd::Slt: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      // extend to 64-bit to avoid overflow
      llvm::Value* c1e = Builder.CreateSExt(c1, Builder.getInt64Ty());
      llvm::Value* c2e = Builder.CreateSExt(c2, Builder.getInt64Ty());

      // save the comparison operands to the output args
      // so it's easier to negate the condition
      llvm::Value* idx[1];
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 0);
      Builder.CreateStore(c1e, Builder.CreateGEP(arg, idx));
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 1);
      Builder.CreateStore(c2e, Builder.CreateGEP(arg, idx));

      ret = nullptr;
      break;
    }
    case rgd::Sle: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      // extend to 64-bit to avoid overflow
      llvm::Value* c1e = Builder.CreateSExt(c1, Builder.getInt64Ty());
      llvm::Value* c2e = Builder.CreateSExt(c2, Builder.getInt64Ty());

      // save the comparison operands to the output args
      // so it's easier to negate the condition
      llvm::Value* idx[1];
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 0);
      Builder.CreateStore(c1e, Builder.CreateGEP(arg, idx));
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 1);
      Builder.CreateStore(c2e, Builder.CreateGEP(arg, idx));

      ret = nullptr;
      break;
    }
    case rgd::Sgt: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      // extend to 64-bit to avoid overflow
      llvm::Value* c1e = Builder.CreateSExt(c1, Builder.getInt64Ty());
      llvm::Value* c2e = Builder.CreateSExt(c2, Builder.getInt64Ty());

      // save the comparison operands to the output args
      // so it's easier to negate the condition
      llvm::Value* idx[1];
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 0);
      Builder.CreateStore(c1e, Builder.CreateGEP(arg, idx));
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 1);
      Builder.CreateStore(c2e, Builder.CreateGEP(arg, idx));

      ret = nullptr;
      break;
    }
    case rgd::Sge: {
      const AstNode* rc1 = &node->children(0);
      const AstNode* rc2 = &node->children(1);
      llvm::Value* c1 = codegen(Builder, rc1, local_map, arg, value_cache, expr_cache);
      llvm::Value* c2 = codegen(Builder, rc2, local_map, arg, value_cache, expr_cache);
      // extend to 64-bit to avoid overflow
      llvm::Value* c1e = Builder.CreateSExt(c1, Builder.getInt64Ty());
      llvm::Value* c2e = Builder.CreateSExt(c2, Builder.getInt64Ty());

      // save the comparison operands to the output args
      // so it's easier to negate the condition
      llvm::Value* idx[1];
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 0);
      Builder.CreateStore(c1e, Builder.CreateGEP(arg, idx));
      idx[0]= llvm::ConstantInt::get(Builder.getInt32Ty(), 1);
      Builder.CreateStore(c2e, Builder.CreateGEP(arg, idx));

      ret = nullptr;
      break;
    }
    // this should never happen!
    case rgd::LOr: {
      assert(false && "LOr expression");
      break;
    }
    case rgd::LAnd: {
      assert(false && "LAnd expression");
      break;
    }
    case rgd::LNot: {
      assert(false && "LNot expression");
      break;
    }
    case rgd::Ite: {
      // don't handle ITE for now, doesn't work with GD
#if DEUBG
      std::cerr << "ITE expr codegen" << std::endl;
#endif
#if 0
      const AstNode* rcond = &node->children(0);
      const AstNode* rtv = &node->children(1);
      const AstNode* rfv = &node->children(2);
      llvm::Value* cond = codegen(rcond, local_map, arg, value_cache);
      llvm::Value* tv = codegen(rtv, local_map, arg, value_cache);
      llvm::Value* fv = codegen(rfv, local_map, arg, value_cache);
      ret = Builder.CreateSelect(cond, tv, fv);
#endif
      break;
    }
    default:
      //std::cerr << "WARNING: unhandled expr: ";
      printExpression(node);
      break;
  }

  // add to cache
  if (ret && node->label() != 0) {
    value_cache.insert({node->label(), ret});
  }

  return ret; 
}

int addFunction(const AstNode* node,
    std::map<uint32_t,uint32_t> &local_map,
    uint64_t id,
    std::unordered_map<uint32_t,AstNode*> &expr_cache) {

  assert(isRelational(node->kind()) && "non-relational expr");

  // Open a new module.
  std::string moduleName = "rgdjit_m" + std::to_string(id);
  std::string funcName = "rgdjit_f" + std::to_string(id);

  auto TheCtx = std::make_unique<llvm::LLVMContext>();
  auto TheModule = std::make_unique<Module>(moduleName, *TheCtx);
  TheModule->setDataLayout(JIT->getDataLayout());
  llvm::IRBuilder<> Builder(*TheCtx);

  std::vector<llvm::Type*> input_type(1,
      llvm::PointerType::getUnqual(Builder.getInt64Ty()));
  llvm::FunctionType *funcType;
  funcType = llvm::FunctionType::get(Builder.getVoidTy(), input_type, false);
  auto *fooFunc = llvm::Function::Create(funcType, llvm::Function::ExternalLinkage,
      funcName, TheModule.get());
  auto *po = llvm::BasicBlock::Create(Builder.getContext(), "entry", fooFunc);
  Builder.SetInsertPoint(po);
  uint32_t idx = 0;

  auto args = fooFunc->arg_begin();
  llvm::Value* var = &(*args);
  std::unordered_map<uint32_t, llvm::Value*> value_cache;
  auto *body = codegen(Builder, node, local_map, var, value_cache, expr_cache);
  assert(body == nullptr && "non-comparison expr");
  Builder.CreateRet(body);

  llvm::raw_ostream *stream = &llvm::outs();
  llvm::verifyFunction(*fooFunc, stream);
#if DEBUG
  // TheModule->print(llvm::errs(), nullptr);
#endif

  JIT->addModule(std::move(TheModule), std::move(TheCtx));

  return 0;
}

test_fn_type performJit(uint64_t id) {
  std::string funcName = "rgdjit_f" + std::to_string(id);
  auto ExprSymbol = JIT->lookup(funcName).get();
  auto func = (test_fn_type)ExprSymbol.getAddress();
  return func;
}
