#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/Utils/LowerSwitch.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include <iostream>
#include <map>
#include <utility>
#include <fstream>
#include<vector>
#include<algorithm>
#include <set>
using namespace llvm;
#define DEBUG_TYPE "Break-Constant-GEPs"
// Statistics
STATISTIC (GEPChanges,   "Number of Converted GEP Constant Expressions");
STATISTIC (TotalChanges, "Number of Converted Constant Expressions");
STATISTIC(NumOfCriticalFunc,"Number of Critical function");

class BreakConstantGEPs:public ModulePass{
    public:
        static char ID;
        std::vector<std::string> critical_func;
    BreakConstantGEPs() : ModulePass(ID) {}
    StringRef getPassName() const
    {
        return "Remove Constant GEP Expressions";
    } 
     ConstantExpr *hasConstantGEP (Value*  V)
    {
        if (ConstantExpr * CE = dyn_cast<ConstantExpr>(V))
        {
            if (CE->getOpcode() == Instruction::GetElementPtr)
            {
                return CE;
            }
            else
            {
                for (unsigned index = 0; index < CE->getNumOperands(); ++index)
                    {
                if (hasConstantGEP (CE->getOperand(index)))
                    return CE;
                }
            }
        }

        return nullptr;
    }

    bool runOnModule (Module & module)override;
    std::string getStringFromValue(Value *V);
    // Description:
//  This function determines whether the given value is a constant expression
//  that has a constant binary or unary operator expression embedded within it.
     ConstantExpr *hasConstantBinaryOrUnaryOp (Value*  V)
    {
        if (ConstantExpr * CE =dyn_cast<ConstantExpr>(V))
        {
            if (Instruction::isBinaryOp(CE->getOpcode()) || Instruction::isUnaryOp(CE->getOpcode()))
            {
                return CE;
            }
            else
            {
                for (unsigned index = 0; index < CE->getNumOperands(); ++index)
            {
                if (hasConstantBinaryOrUnaryOp (CE->getOperand(index)))
                    return CE;
            }
        }
    }

    return nullptr;
    }
// Description:
// Return true if this is a constant Gep or binaryOp or UnaryOp expression
ConstantExpr *
hasConstantExpr (Value*  V)
{
    if (ConstantExpr * gep = hasConstantGEP(V))
    {
        return gep;
    }
    else if (ConstantExpr * buop = hasConstantBinaryOrUnaryOp(V))
    {
        return buop;
    }
    else
    {
        return nullptr;
    }
}

// Function: convertExpression()
//
// Description:
//  Convert a constant expression into an instruction.  This routine does *not*
//  perform any recursion, so the resulting instruction may have constant
//  expression operands.
//
 Instruction*
convertExpression (ConstantExpr * CE, Instruction*  InsertPt)
{
    //
    // Convert this constant expression into a regular instruction.
    //
    if (CE->getOpcode() == Instruction::GetElementPtr)
        ++GEPChanges;
    ++TotalChanges;
    Instruction* Result = CE->getAsInstruction();
    Result->insertBefore(InsertPt);
    return Result;
}

};

// Method: runOnFunction()
//
// Description:
//  Entry point for this LLVM pass.
//
// Return value:
//  true  - The function was modified.
//  false - The function was not modified.
//
bool
BreakConstantGEPs::runOnModule (Module & module) 
{
    bool modified = false;

    for (Module::iterator F = module.begin(), E = module.end(); F != E; ++F)
    {
        // Worklist of values to check for constant GEP expressions
        std::vector<Instruction* > Worklist;

        //
        // Initialize the worklist by finding all instructions that have one or more
        // operands containing a constant GEP expression.
        //
        for (Function::iterator BB = (*F).begin(); BB != (*F).end(); ++BB)
        {
            for (BasicBlock::iterator i = BB->begin(); i != BB->end(); ++i)
            {
                //
                // Scan through the operands of this instruction.  If it is a constant
                // expression GEP, insert an instruction GEP before the instruction.
                //
                Instruction*  I = &(*i);
                for (unsigned index = 0; index < I->getNumOperands(); ++index)
                {
                    if (hasConstantExpr(I->getOperand(index)))
                    {
                        Worklist.push_back (I);
                    }
                }
            }
        }

        //
        // Determine whether we will modify anything.
        //
        if (Worklist.size()) modified = true;

        //
        // While the worklist is not empty, take an item from it, convert the
        // operands into instructions if necessary, and determine if the newly
        // added instructions need to be processed as well.
        //
        while (Worklist.size())
        {
            Instruction*  I = Worklist.back();
            Worklist.pop_back();

            //
            // Scan through the operands of this instruction and convert each into an
            // instruction.  Note that this works a little differently for phi
            // instructions because the new instruction must be added to the
            // appropriate predecessor block.
            //
            if (PHINode * PHI = dyn_cast<PHINode>(I))
            {
                for (unsigned index = 0; index < PHI->getNumIncomingValues(); ++index)
                {
                    //
                    // For PHI Nodes, if an operand is a constant expression with a GEP, we
                    // want to insert the new instructions in the predecessor basic block.
                    //
                    // Note: It seems that it's possible for a phi to have the same
                    // incoming basic block listed multiple times; this seems okay as long
                    // the same value is listed for the incoming block.
                    //
                    Instruction*  InsertPt = PHI->getIncomingBlock(index)->getTerminator();
                    if (ConstantExpr * CE = hasConstantExpr(PHI->getIncomingValue(index)))
                    {
                        Instruction*  NewInst = convertExpression (CE, InsertPt);
                        for (unsigned i2 = index; i2 < PHI->getNumIncomingValues(); ++i2)
                        {
                            if ((PHI->getIncomingBlock (i2)) == PHI->getIncomingBlock (index))
                                PHI->setIncomingValue (i2, NewInst);
                        }
                        Worklist.push_back (NewInst);
                    }
                }
            }
            else
            {
                for (unsigned index = 0; index < I->getNumOperands(); ++index)
                {
                    //
                    // For other instructions, we want to insert instructions replacing
                    // constant expressions immediately before the instruction using the
                    // constant expression.
                    //
                    if (ConstantExpr * CE = hasConstantExpr(I->getOperand(index)))
                    {
                        Instruction*  NewInst = convertExpression (CE, I);
                        I->replaceUsesOfWith (CE, NewInst);
                        Worklist.push_back (NewInst);
                    }
                }
            }
        }
    }

    GlobalVariable *GlobalAnnotations = module.getNamedGlobal("llvm.global.annotations");
    if (!GlobalAnnotations) {
    errs() << "No @llvm.global.annotations found in the module.\n";
    return false;
    }

  
    if (!GlobalAnnotations->hasInitializer()) {
    errs() << "@llvm.global.annotations has no initializer.\n";
    return false;
    }


    ConstantArray *AnnotationsArray = dyn_cast<ConstantArray>(GlobalAnnotations->getInitializer());
    if (!AnnotationsArray) {
    errs() << "@llvm.global.annotations is not a ConstantArray.\n";
    return false;
    }


    for (unsigned i = 0; i < AnnotationsArray->getNumOperands(); ++i) 
    {
        ConstantStruct *AnnotationStruct = dyn_cast<ConstantStruct>(AnnotationsArray->getOperand(i));
        if (!AnnotationStruct) {
            errs() << "Annotation element is not a ConstantStruct.\n";
            continue;
        }

        
        if (AnnotationStruct->getNumOperands() != 5) {
            errs() << "Unexpected number of operands in annotation struct.\n";
            continue;
        }

      
        Value *AnnotatedEntity = AnnotationStruct->getOperand(0); 
        if (ConstantExpr *CE = dyn_cast<ConstantExpr>(AnnotatedEntity)) {
            if (CE->getOpcode() == Instruction::BitCast) {
                AnnotatedEntity = CE->getOperand(0); 
            }
        }
        Value *AnnotationString = AnnotationStruct->getOperand(1); 
        std::string annotation=getStringFromValue(AnnotationString);
        Value *FileName = AnnotationStruct->getOperand(2);         
        ConstantInt *LineNumber = dyn_cast<ConstantInt>(AnnotationStruct->getOperand(3)); 
        Value *ExtraInfo = AnnotationStruct->getOperand(4);        

        // 打印信息
        errs() << "Annotation " << i << ":\n";
        if (auto *Entity = dyn_cast<Function>(AnnotatedEntity)) {
            errs() << "  Function: " << Entity->getName() << "\n";
            std::string func_name=Entity->getName().str();
            //use to debug
            // errs()<<annotation<<'\n';
            // errs() << "Annotation (hex): ";
            // for (char c : annotation) {
            //     errs().write_hex(c) << " ";
            // }
            // errs() << "\n";
            // llvm::StringRef annotation_ref(annotation);
            // annotation_ref = annotation_ref.trim(); 
            // errs() << "Annotation (hex): ";
            // for (char c : annotation_ref.str()) {
            //     errs().write_hex(c) << " ";
            // }
            // errs() << "\n";
            if (annotation.find("critical function") != std::string::npos)
            {
                errs()<<"find critical function"<<'\n';
                critical_func.push_back(func_name);
                NumOfCriticalFunc++;
            }
        } else if (auto *GV = dyn_cast<GlobalVariable>(AnnotatedEntity)) {
            errs() << "  Global Variable: " << GV->getName() << "\n";
        } else {
            errs() << "  Annotated Entity: (unknown)\n";
        }

        errs() << "  Annotation String: " << getStringFromValue(AnnotationString) << "\n";

        errs() << "  File Name: " << getStringFromValue(FileName) << "\n";

        if (LineNumber) {
            errs() << "  Line Number: " << LineNumber->getZExtValue() << "\n";
        } else {
            errs() << "  Line Number: (unknown)\n";
        }

        errs() << "  Extra Info: " << (ExtraInfo ? "Non-null" : "null") << "\n";
    }
    // for (auto &F :module)
    // {
    //     if(F.isDeclaration())
    //         continue;
    //     std::string name=F.getName().str();
    //     if(std::find(critical_func.begin(),critical_func.end(),name)!=critical_func.end())
    //     {
    //         errs()<<"process function:" <<F.getName()<<"\n";
    //         BasicBlock &entry=F.getEntryBlock();
    //         Instruction * firstisnt=&*entry.begin();
    //         IRBuilder<> builder(firstisnt);
    //         Module *M=builder.GetInsertBlock()->getModule();
    //         Type * voidty=builder.getVoidTy();
    //         FunctionCallee  FuncStart=M->getOrInsertFunction("start_collecting",voidty); 
    //         builder.CreateCall(FuncStart);
    //         for(auto &BB:F)
    //         {
    //             Instruction* termInst=BB.getTerminator();
    //             if(isa<ReturnInst>(termInst))
    //             {
    //                 IRBuilder<> ReturnBuilder(termInst);
    //                 FunctionCallee FuncEnd=M->getOrInsertFunction("end_collecting",voidty);
    //                 ReturnBuilder.CreateCall(FuncEnd);
    //             }
    //         }
    //         modified = true;
    //     }
    // }
    
    errs()<<"Critical function:\n";
    std::ofstream outfile("critical_function.txt");
    for(auto &func:critical_func)
    {
        errs()<<func<<"\n";
        outfile<<func<<"\n";
    }
    outfile.close();
    return modified;
}

  std::string BreakConstantGEPs::getStringFromValue(Value *V) {
    if (!V) return "(null)";

    if (ConstantExpr *CE = dyn_cast<ConstantExpr>(V)) {
        if (CE->getOpcode() == Instruction::GetElementPtr) {
            V = CE->getOperand(0);
        }
    }

    if (GlobalVariable *GVar = dyn_cast<GlobalVariable>(V)) {
        if (GVar->hasInitializer()) {
            if (ConstantDataArray *CDA = dyn_cast<ConstantDataArray>(GVar->getInitializer())) {
                if (CDA->isString()) {
                    return CDA->getAsString().str();
                }
            }
        }
    }

    return "(unknown)";
  }


char BreakConstantGEPs::ID = 0;
static RegisterPass<BreakConstantGEPs> X("Break-Constant-GEPs","Change constant GEPs into GEP instructions");