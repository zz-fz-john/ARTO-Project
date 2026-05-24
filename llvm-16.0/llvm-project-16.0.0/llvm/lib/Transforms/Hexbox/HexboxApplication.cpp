//===- HexboxApplication.cpp - Example code from "Writing an LLVM Pass" ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements application of the HexBox policy
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <iostream>
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "json/json.h" //From https://github.com/open-source-parsers/jsoncpp
#include <map>
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "hexbox"

//STATISTIC(Stat_NumFunctions, "Num Functions");

static cl::opt<std::string> HexboxPolicy("hexbox-policy",
                                  cl::desc("JSON Defining the policy"),
                                  cl::init("-"),cl::value_desc("filename"));



Json::Value CFGRoot;

//the map that records the sections a variable should locate in
std::map<std::string, std::string> var2sec;

#define SHARED_DATA_REGION ".DATA_REGION_0__d"


//#define NUM_MPU_REGIONS 8
#define ACCESS_ARRAY_SIZE 200

namespace {
  // Hello - The first implementation, without getAnalysisUsage.
  struct HexboxApplication : public ModulePass {
    static char ID; // Pass identification, replacement for typeid
    HexboxApplication () : ModulePass(ID) {}

    StringMap<GlobalVariable *> CompName2GVMap;
    DenseMap<Function *, Function *> Function2Wrapper;
    GlobalVariable * DefaultCompartment;

    bool doInitialization(Module &M) override{
        return true;
    }

    /**
     assignLinkerSections
     Reads the sections from the policy file and assigns functions and globals
     to specific sections.  These sections define tell the linker where to place
     the functions and globals. They compose regions of a compartment

    */
    void assignLinkerSections(Module &M, Json::Value &Root){
        Json::Value PolicyRegions=Root.get("Regions","");
        for(auto RegionName: PolicyRegions.getMemberNames()){
            Json::Value Region = PolicyRegions[RegionName];
            Json::Value region_type = Region["Type"];
            if ( region_type.compare("Code") == 0 ){
                for (auto funct : Region.get("Objects","")){
                    Function * F = M.getFunction(StringRef(funct.asString()));
                    if (F){
                        F->setSection(StringRef(RegionName));
                    }else{
                        std::cout << "No Name Function for: "<< funct <<"\n";
                    }
                }
            }else{
                std::string DataSection(RegionName+"_data");
                std::string BSSSection(RegionName+"_bss");
                for (auto gvName : Region.get("Objects","")){
                    GlobalVariable *GV;
                    GV = M.getGlobalVariable(StringRef(gvName.asString()),true);
                    if (GV){
                        if ( GV->hasInitializer() ){
                            if (GV->getInitializer()->isZeroValue()){// if initialized to 0, place in bss section
                                GV->setSection(StringRef(BSSSection));
                             }else{
                                GV->setSection(StringRef(DataSection));
                             }
                         }else{
                             errs() << "Warning: GV '" << gvName.asString()<< "' has no initializer. Skipping.\n";
                         }
                     }
                     else{
                         std::cout << "No Name GV for: "<< gvName <<"\n";
                     }
                 }//for
            }
        }
    }

    //AMI shared variables identification
    void AMI_identifySharedVar(Module &M){

        for (auto& F : M) {
            for (auto& BB : F) {
                for (auto& I : BB) {
                    if (auto* storeInst = llvm::dyn_cast<llvm::StoreInst>(&I)) {
                        llvm::Value* ptrOperand = storeInst->getPointerOperand();
                        if (ptrOperand->hasName()) {
                            if (llvm::isa<llvm::GlobalVariable>(ptrOperand)) {
                                // llvm::errs() << "Stored global variable: " << ptrOperand->getName() << "\n";
                                std::string curr_glb_var_name = ptrOperand->getName().str();

                                //global variable appear the first time
                                if(var2sec.find(curr_glb_var_name) == var2sec.end()){
                                    var2sec[curr_glb_var_name] = F.getSection();
                                }
                                else{
                                    std::string existing_section = var2sec[curr_glb_var_name];
                                    //check whether the global variable is accessed from multiple differnt sections
                                    if(existing_section != F.getSection()){
                                        var2sec[curr_glb_var_name] = SHARED_DATA_REGION; 
                                        //reassign the variable section, untested code
                                        GlobalVariable *GV;
                                        GV = M.getGlobalVariable(StringRef(curr_glb_var_name), true);
                                        GV->setSection(StringRef(SHARED_DATA_REGION));
                                    }                                    
                                }
                                //modifying local variables
                            } else if (llvm::isa<llvm::AllocaInst>(ptrOperand)) {
                                // llvm::errs() << "Stored local variable: " << ptrOperand->getName() << "\n";
                                //TODO stack isolation, implement local variable isolation in backend pass
                            }
                        }
                        else{
                            if (ptrOperand->getType()->isPointerTy()) {
                                // errs() << "The type of the pointer operand is: " << *(ptrOperand->getType()) << "\n";
                                // errs() << "found pointers";
                                llvm::Type* ptrType = ptrOperand->getType();
                                    // It is a pointer type, get the pointee type
                                llvm::Type* pointeeType = ptrType->getPointerElementType();
                                //TODO type based pointer analysis, move all variables that are possibly
                                //accessed by multiple sections to global variable section
                                //TODO stack isolation, implement local variable isolation in backend pass     
                                // If you want to print the pointee type
                                // llvm::errs() << "Pointee type: " << *pointeeType << "\n";
                            }
                        }
                    }
                }
            }
        }

    }

    /**************************************************************************
     * runOnModule
     * Reads in a policy JSON file and moves functions and data to the
     * designated regions
     *
     *************************************************************************/
    bool runOnModule(Module &M) override {

        // return false;

        if ( HexboxPolicy.compare("-") == 0 )
            return false;

        //Read in Policy File
        Json::Value PolicyRoot;
        std::ifstream policyFile;
        policyFile.open(HexboxPolicy);
        policyFile >> PolicyRoot;
        // errs() << "pos1\n";
        //assigning sections for each compartment
        assignLinkerSections(M,PolicyRoot);
        //jinwen comment this for debug
        //identify shared variables, reassign these variables to shared variable section
        AMI_identifySharedVar(M);
        return true;
    }



    bool doFinalization(Module &M) override{

        if ( HexboxPolicy.compare("-") == 0 )
            return false;

        return false;
    }


    void getAnalysisUsage(AnalysisUsage &AU) const override {
      //AU.setPreservesAll();
    }
  };

}
char HexboxApplication::ID = 0;

static RegisterPass<HexboxApplication> X("hexbox-application", "Hexbox Application Pass");  

