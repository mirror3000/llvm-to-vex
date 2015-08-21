#include "llvm/Pass.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Support/raw_ostream.h"

extern "C" {
#include "libvex_ir.h"
#include "main_util.h"
}

#pragma clang diagnostic ignored "-Wswitch"

using namespace llvm;
using namespace std;

namespace {

    struct VEXLib {

        VEXLib() : irsb(0) {}
        VEXLib(IRSB *irsb) : irsb(irsb) {}

        IRSB *irsb;

        void stmt(IRStmt *st)
        {
            addStmtToIRSB(irsb, st);
        }

        void assign(IRTemp dst, IRExpr *e)
        {
            stmt(IRStmt_WrTmp(dst, e));
        }

        IRTemp newTemp(IRType ty)
        {
            vassert(isPlausibleIRType(ty));
            return newIRTemp(irsb->tyenv, ty);
        }

        static IROp mkSizedOp(IRType ty, IROp op8)
        {
            int adj;
            vassert(ty == Ity_I8 || ty == Ity_I16 || ty == Ity_I32 ||
                ty == Ity_I64);
            vassert(op8 == Iop_Add8 || op8 == Iop_Sub8
                || op8 == Iop_Mul8
                || op8 == Iop_Or8 || op8 == Iop_And8 || op8 == Iop_Xor8
                || op8 == Iop_Shl8 || op8 == Iop_Shr8 || op8 == Iop_Sar8
                || op8 == Iop_CmpEQ8 || op8 == Iop_CmpNE8
                || op8 == Iop_CasCmpNE8
                || op8 == Iop_ExpCmpNE8
                || op8 == Iop_Not8);
            switch (ty) {
            case Ity_I8:
                adj = 0;
                break;
            case Ity_I16:
                adj = 1;
                break;
            case Ity_I32:
                adj = 2;
                break;
            case Ity_I64:
                adj = 3;
                break;
            }
            return (IROp)(adj + op8);
        }
    };

    struct Hello : public FunctionPass {
        map<Value *, IRExpr *> parsedInst;
        static char ID;

        Hello() : FunctionPass(ID) {}

        IRType parseVType(Value &V) {
            IRType type = Ity_INVALID;
            Type *t = V.getType();
            switch (t->getTypeID()) {
            case Type::IntegerTyID:
                switch (t->getPrimitiveSizeInBits()) {
                case 1:
                    type = Ity_I1;
                    break;
                case 8:
                    type = Ity_I8;
                    break;
                case 16:
                    type = Ity_I16;
                    break;
                case 32:
                    type = Ity_I32;
                    break;
                case 64:
                    type = Ity_I64;
                    break;
                }
                break;
            case Type::FloatTyID:
                type = Ity_F32;
                break;
            case Type::DoubleTyID:
                type = Ity_F64;
                break;
            }
            return type;
        }

        IRExpr *parseVal(Value &V, VEXLib &vl, int level = 1) {
            IRTemp res = IRTemp_INVALID;
            IRExpr *expr = 0;
            IRType type = parseVType(V);

            map<Value *, IRExpr *>::iterator srchIt = parsedInst.find(&V);
            if (srchIt != parsedInst.end())
                return srchIt->second;

            errs() << "\n";
            for (int i = 0; i < level; i++)
                errs() << "\t";
            errs() << V << " --- ";

            if (isa<Constant>(V)) {
                errs() << "constant ";

                if (isa<ConstantInt>(V)) {
                    errs() << "int ";

                    ConstantInt &CI = (ConstantInt &)V;
                    uint64_t valInt = (CI.getZExtValue());

                    switch (type) {
                    case Ity_I1:
                        errs() << "1 ";

                        expr = IRExpr_Const(IRConst_U1((Bool)valInt));
                        break;
                    case Ity_I8:
                        errs() << "8 ";

                        expr = IRExpr_Const(IRConst_U8((uint8_t)valInt));
                        break;
                    case Ity_I16:
                        errs() << "16 ";

                        expr = IRExpr_Const(IRConst_U16((uint16_t)valInt));
                        break;
                    case Ity_I32:
                        errs() << "32 ";

                        expr = IRExpr_Const(IRConst_U32((uint32_t)valInt));
                        break;
                    case Ity_I64:
                        errs() << "64 ";

                        expr = IRExpr_Const(IRConst_U64(valInt));
                        break;
                    }
                }
            } else if (isa<Instruction>(V)) {
                errs() << "instruction ";

                Instruction &I = (Instruction &)V;

                if (isa<UnaryInstruction>(V)) {
                    errs() << "unary ";

                    Value *opd = I.getOperand(0);
                    IRExpr *parsedOpd = 0;

                    if (isa<AllocaInst>(V)) {
                        errs() << "alloca ";
                    } else if (isa<CastInst>(V)) {
                        errs() << "cast" ;
                    } else if (isa<ExtractValueInst>(V)) {
                        errs() << "extract ";
                    } else if (isa<LoadInst>(V)) {
                        errs() << "load ";

                        parsedOpd = parseVal(*opd, vl, level + 1);
                        res = vl.newTemp(type);
                        //TODO what about big endian?
                        vl.assign(res,
                                  IRExpr_Load(Iend_LE, type, parsedOpd));
                        expr = IRExpr_RdTmp(res);
                    } else if (isa<VAArgInst>(V)) {
                        errs() << "vaarg ";
                    }
                } else if (isa<BinaryOperator>(V)) {
                    errs() << "binary ";

                    IROp op = Iop_INVALID;
                    Value *opd1 = I.getOperand(0);
                    Value *opd2 = I.getOperand(1);
                    IRExpr *parsedOpd1 = 0;
                    IRExpr *parsedOpd2 = 0;

                    switch (I.getOpcode()) {
                    // Standard binary operators...
                    case Instruction::Add:
                        errs() << "add ";

                        op = VEXLib::mkSizedOp(type, Iop_Add8);
                        break;
                    case Instruction::FAdd:
                        errs() << "fadd ";

                        break;
                    case Instruction::Sub:
                        errs() << "sub ";

                        op = VEXLib::mkSizedOp(type, Iop_Sub8);
                        break;
                    case Instruction::FSub:
                        errs() << "fsub ";

                        break;
                    case Instruction::Mul:
                        errs() << "mul ";

                        op = VEXLib::mkSizedOp(type, Iop_Mul8);
                        break;
                    case Instruction::FMul:
                        errs() << "fmul ";

                        break;
                    case Instruction::UDiv:
                        errs() << "udiv ";

                        break;
                    case Instruction::SDiv:
                        errs() << "sdiv ";

                        break;
                    case Instruction::FDiv:
                        errs() << "fdiv ";

                        break;
                    case Instruction::URem:
                        errs() << "urem ";

                        break;
                    case Instruction::SRem:
                        errs() << "srem ";

                        break;
                    case Instruction::FRem:
                        errs() << "frem ";

                        break;
                    // Logical operators (integer operands)
                    case Instruction::Shl:
                        errs() << "shl ";

                        op = VEXLib::mkSizedOp(type, Iop_Shl8);
                        break;
                    case Instruction::LShr:
                        errs() << "lshl ";

                        op = VEXLib::mkSizedOp(type, Iop_Shr8);
                        break;
                    case Instruction::AShr:
                        errs() << "ashr ";

                        op = VEXLib::mkSizedOp(type, Iop_Sar8);
                        break;
                    case Instruction::And:
                        errs() << "and ";

                        op = VEXLib::mkSizedOp(type, Iop_And8);
                        break;
                    case Instruction::Or:
                        errs() << "or ";

                        op = VEXLib::mkSizedOp(type, Iop_Or8);
                        break;
                    case Instruction::Xor:
                        errs() << "xor ";

                        op = VEXLib::mkSizedOp(type, Iop_Xor8);
                        break;
                    }

                    res = vl.newTemp(type);
                    parsedOpd1 = parseVal(*opd1, vl, level + 1);
                    parsedOpd2 = parseVal(*opd2, vl, level + 1);
                    vl.assign(res, IRExpr_Binop(op, parsedOpd1, parsedOpd2));
                    expr = IRExpr_RdTmp(res);
                } else if (isa<CmpInst>(V)) {
                    errs() << "cmp ";

                    CmpInst &CmpI = (CmpInst &)V;
                    IROp pred = Iop_INVALID;
                    Value *opd1 = CmpI.getOperand(0);
                    Value *opd2 = CmpI.getOperand(1);
                    IRType cmpTy = parseVType(*opd1);
                    IRExpr *parsedOpd1 = 0;
                    IRExpr *parsedOpd2 = 0;

                    if (isa<ICmpInst>(V)) {
                        errs() << "int ";

                        switch(CmpI.getPredicate()) {
                            case CmpInst::ICMP_EQ:
                                errs() << "eq ";

                                pred = VEXLib::mkSizedOp(cmpTy, Iop_CmpEQ8);
                                break;
                            case CmpInst::ICMP_NE:
                                errs() << "ne ";

                                pred = VEXLib::mkSizedOp(cmpTy, Iop_CmpNE8);
                                break;
                        }

                    } else if (isa<FCmpInst>(V)) {
                        errs() << "float ";
                    }

                    res = vl.newTemp(type);
                    parsedOpd1 = parseVal(*opd1, vl, level + 1);
                    parsedOpd2 = parseVal(*opd2, vl, level + 1);
                    vl.assign(res, IRExpr_Binop(pred, parsedOpd1, parsedOpd2));
                    expr = IRExpr_RdTmp(res);
                } else if (isa<StoreInst>(V)) {
                    errs() << "store ";
                }
            }

            parsedInst[&V] = expr;
            return expr;
        }

        void transInstr(VEXLib &vl, Instruction  &I) {
            IRTemp res = IRTemp_INVALID;
            IROp op = Iop_INVALID;
            Value *opd1, *opd2;
            IRType type = parseVType(I);

            errs() << "\n" << I ;
            parseVal(I, vl);

            /*
            switch (I.getOpcode()) {
            case Instruction::Load:
                parseVal(I, vl);
                break;
            case Instruction::Add:
                opd1 = I.getOperand(0);
                opd2 = I.getOperand(1);

                parseVal(*opd1, vl);
                parseVal(*opd2, vl);

                res = vl.newTemp(type);
                op = VEXLib::mkSizedOp(type, Iop_Add8);
                //assign(res, binop(op,))
                break;
            }
            */
        }

        bool runOnFunction(Function &F) override {
            errs() << "Hello: ";
            errs().write_escaped(F.getName()) << '\n';

            for (BasicBlock &bb : F) {
                IRSB *irsb = emptyIRSB();
                VEXLib vl(irsb);
                for (Instruction &i : bb) {
                    transInstr(vl, i);
                }
            }

            return false;
        }
    };

}

char Hello::ID = 0;
static RegisterPass<Hello> X("hello", "Hello World Pass", false, false);
