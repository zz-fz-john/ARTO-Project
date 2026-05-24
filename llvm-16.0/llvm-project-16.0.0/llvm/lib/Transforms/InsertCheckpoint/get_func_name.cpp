
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>
#include <string>
#include<algorithm>
#include <fstream>
#include <iostream>
using namespace llvm;

namespace {
struct FunctionNamePass : public ModulePass {
    static char ID;
    FunctionNamePass() : ModulePass(ID) {}

    bool runOnModule(Module &M) override {
        std::vector<std::string> filenames;
        std::vector<std::string> functionNames;
       
        std::ifstream file("../../source_filename.txt");
        if (!file) {
            errs() << "Error opening file\n";
            return false;
        }
        std::string targetFileNameStr;
        while (std::getline(file, targetFileNameStr)) {
            targetFileNameStr.erase(std::remove(targetFileNameStr.begin(), targetFileNameStr.end(), '\n'), targetFileNameStr.end());
            filenames.push_back(targetFileNameStr);
            std::cout<<targetFileNameStr<<std::endl;
        }
        file.close();

        
        for (Function &F : M) {
           
            if (DISubprogram *D = F.getSubprogram()) {
                StringRef fileNameref = D->getFilename();
                std::string fileName = fileNameref.str();
                //std::cout<<fileName<<std::endl;
                
                if(std::find(filenames.begin(), filenames.end(), fileName) != filenames.end()) {
                    //std::cout<<F.getName().str()<<std::endl;
                    std::string functionName = F.getName().str();
                    functionNames.push_back(functionName);
                }
            }
        }
        
        std::ofstream output("../../path_start.txt");
        for (std::string functionName : functionNames) {
            output << functionName << "\n";
        }
        output.close();
        return false; 
    }
} ;

char FunctionNamePass::ID = 0;
static RegisterPass<FunctionNamePass> X("function-name-pass", "Function Name Pass", false, false);
} // end anonymous namespace
