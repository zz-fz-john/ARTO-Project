//===- HexboxAnalysis.cpp -------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file performs analysis of the application to generate data that can
// be used to create a HexBox policy
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugInfo.h"
#include "json/json.h"  //From https://github.com/open-source-parsers/jsoncpp
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <regex>
#include <utility>
#include "llvm/IR/InstIterator.h"
#include "llvm/PassRegistry.h"
using namespace llvm;

#define DEBUG_TYPE "hexbox"

STATISTIC(NumFunctions, "Num Functions");

static cl::opt<std::string> HexboxAnalysisResults("hexbox-analysis-results",
                                  cl::desc("JSON File to write analysis results to"),
                                  cl::init("-"),cl::value_desc("filename"));

static cl::opt<std::string> HexboxAnalysisIndirectCallFilePath("hexbox-indirect-call-analysis-filepath",
                                    cl::desc("Path to the file for indirect call analysis"),
                                    cl::init(""), cl::value_desc("indirect-filepath"));// indirect call result file

static cl::opt<std::string> HexboxAnalysisDirectCallFilePath("hexbox-analysis-direct-callfilepath",
                                    cl::desc("Path to the file for direct call analysis"),
                                    cl::init(""), cl::value_desc("direct-filepath"));// direct call result file
namespace {

  struct HexboxAnalysis : public FunctionPass {
    static char ID;
    HexboxAnalysis() : FunctionPass(ID) {}
    std::map<std::string, int> controller_filename;
    std::map<std::string, int> sensor_actrator_filename;
    std::map<std::string, int> peripheral_filename;
    static std::map<std::pair<std::string,int>,std::set<std::string>> indirect_call_graph;//Stores the indirect call relationship obtained from pointer analysis svf + llvm built-in virtual function analysis
    static std::map<std::pair<std::string,int>,std::set<std::string>> direct_call_graph;//Stores direct calling relationship

    // Performance optimization: cache maps to avoid repeated processing
    std::map<std::string, std::set<std::string>> reverse_indirect_call_graph; // callee -> callers
    std::map<std::string, std::set<std::string>> reverse_direct_call_graph;   // callee -> callers
    std::map<std::string, std::string> global_var_to_file_map; // global var name -> filename

    Json::Value OutputJsonRoot;

    /******************************AddFunctionToJSON**************************
    * Adds the function to the Root Json value, along with all callers and
    * callees.  Also initializes the Globals object under the function
    *
    *************************************************************************/
    Json::Value & AddFunctionToJSON(Function & F){
        //errs()<<"run AddFunctionToJSON on function :"<<F.getName()<<'\n';
        //Get the name of the function and convert it to a string
        std::string name = F.getName().str();
        Json::Value *Fnode;
        Json::Value *Attr;
        std::string str;
        raw_string_ostream type_name_stream(str);
        F.getType()->print(type_name_stream);//Get the type information of this function

        //Create or get a node named name in the JSON root object
        Fnode = &OutputJsonRoot[name];
        Attr = &(*Fnode)["Attr"];

        //Set the properties of the function, such as function type, whether the address is taken, LLVM type, etc.
        (*Attr)["Type"]="Function";
        (*Attr)["Address Taken"]= F.hasAddressTaken();
        (*Attr)["LLVM_Type"] = type_name_stream.str();
        //Get the debug information of the function, find all instructions calling this function, and add caller information to JSON.
        DISubprogram * SP = F.getSubprogram();
        if ( SP ){
            std::string filename = SP->getFile()->getFilename().str();

            (*Attr)["Filename"] = filename;

            //AMI controller policy
            std::map<std::string, int>::iterator controller_it;
            controller_it = controller_filename.find(filename);
            if(controller_it != controller_filename.end()){//If it is a controller function, add a new attribute
                (*Attr)["Controller"] = filename;
            }
        }

        //Adds Callers
        //Add caller information, iterate through the users of the function, find all instructions calling this function, and add caller information to JSON
        std::vector<Json::Value> Cons;
        //org code
        // for (User * U: F.users()){
        //     if (Instruction * Inst = dyn_cast<Instruction>(U)){
        //         if (auto cs = CallSite(Inst)){
        //             Function * caller = cs.getCaller();
        //             add_connection(*Fnode,caller->getName().str(),"Caller");
        //         }
        //     }
        // }
        //Add callers of this function from reverse call graph (Performance optimization: O(K) instead of O(N))
        auto indirect_callers_it = reverse_indirect_call_graph.find(name);
        if(indirect_callers_it != reverse_indirect_call_graph.end()){
            for(const auto& caller : indirect_callers_it->second){
                add_connection(*Fnode, caller, "Caller");
            }
        }
        auto direct_callers_it = reverse_direct_call_graph.find(name);
        if(direct_callers_it != reverse_direct_call_graph.end()){
            for(const auto& caller : direct_callers_it->second){
                add_connection(*Fnode, caller, "Caller");
            }
        }
        // add to debug
        //errs()<<"run add callees on : " <<F.getName()<<'\n';
        //Adds Callees
        //Add callee information: iterate through basic blocks and instructions of the function, find all functions called by this function, and add callee information to JSON
        for ( BasicBlock &BB : F ){
            for ( Instruction & I : BB ){
                if (auto *cs = llvm::dyn_cast<llvm::CallInst>(&I)){
                    Function * callee = cs->getCalledFunction();
                    if ( callee ){
                        add_connection(*Fnode,callee->getName().str(),"Callee");
                    }else if (InlineAsm *IA = dyn_cast_or_null<InlineAsm>(cs->getCalledOperand())){
                        std::string str;
                        raw_string_ostream callee_name(str);
                        cs->print(callee_name);
                        add_connection(*Fnode,callee_name.str(),"Asm Callee");

                    }else if (ConstantExpr *ConstEx = dyn_cast_or_null<ConstantExpr>(cs->getCalledOperand())){
                        //Instruction * Inst = ConstEx->getAsInstruction();
                        // if( CastInst * CI = dyn_cast_or_null<CastInst>(Inst) ){
                        //     if ( Function * c = dyn_cast<Function>(Inst->getOperand(0)) ){
                        //         add_connection(*Fnode,c->getName().str(),"Callee");

                        //     }else{
                        //         // assert(false && "Unhandled Cast");
                        //     }
                        // }
                        if (auto *CE = dyn_cast<ConstantExpr>(cs->getCalledOperand())) {
                            if (CE->isCast()) {  // Handle only cast expressions
                                if (auto *Func = dyn_cast<Function>(CE->getOperand(0))) {
                                    add_connection(*Fnode, Func->getName().str(), "Callee");
                                }
                            }
                        }
                        else{
                            // assert(false && "Unhandled Constant");
                        }
                    }else{
                        add_indirect_calls(*Fnode, F, I, cs);//Handle indirect calls through type matching
                    }
                }
            }
        }
        // add to debug
        //errs()<<"return from addfunctiontoJson on : "<<F.getName()<<'\n';
        return (*Fnode);
    }

    //Add or update connection information in a JSON object
    void add_connection(Json::Value & Fnode, std::string name ,std::string type){
        // Define a pointer to JSON Value named Connections
        Json::Value *Connections;
        Connections = &Fnode["Connections"][name];
        (*Connections)["Type"] = type;
        (*Connections)["Count"] = (*Connections)["Count"].asUInt64() + 1;
    }

    //Perform type matching to determine if caller and callee match
    bool TypesEqual(Type *T1,Type *T2,unsigned depth = 0){

        if ( T1 == T2 ){
            return true;
        }
        if (depth > 10){
            // If we haven't found  a difference this deep just assume they are
            // the same type. We need to overapproximate (i.e. say more things
            // are equal than really are) so return true
            return true;
        }
        if (PointerType *Pty1 = dyn_cast<PointerType>(T1) ){
            if (PointerType *Pty2 = dyn_cast<PointerType>(T2)){
            return TypesEqual(Pty1->getPointerElementType(),
                              Pty2->getPointerElementType(),depth+1);
            }else{
                return false;
            }
        }
        if (FunctionType * FTy1 = dyn_cast<FunctionType>(T1)){
            if (FunctionType * FTy2 = dyn_cast<FunctionType>(T2)){

                if (FTy1->getNumParams() != FTy2->getNumParams()){
                    return false;
                }
                if (! TypesEqual(FTy1->getReturnType(),
                                 FTy2->getReturnType(), depth+1)){
                    return false;
                }
                for (unsigned i=0; i<FTy1->getNumParams(); i++){
                    if (FTy1->getParamType(i) == FTy1 &&
                          FTy2->getParamType(i) == FTy2  ){
                        continue;
                    }else if (FTy1->getParamType(i) != FTy1 &&
                              FTy2->getParamType(i) != FTy2  ){
                        FTy1->getParamType(i)->dump();
                        FTy2->getParamType(i)->dump();
                        if( !TypesEqual(FTy1->getParamType(i),
                                        FTy2->getParamType(i), depth+1)){
                         return false;
                        }
                    }else{
                        return false;
                    }
                }
                return true;

            }else{
                return false;
            }
        }
        if (StructType *STy1 = dyn_cast<StructType>(T1)){
            if (StructType *STy2 = dyn_cast<StructType>(T2)){
                if(STy2->getNumElements() != STy1->getNumElements()){
                    return false;
                }
                if(STy1->hasName() && STy2->hasName()){
                    if(STy1->getName().startswith(STy2->getName()) ||
                        STy2->getName().startswith(STy1->getName())){
                        return true;
                    }
                }
                return false;

            }else{
                return false;
            }
        }
        return false;
    }

    //Handle indirect calls and add relevant information to the JSON object
    void add_indirect_calls(Json::Value & Fnode, Function & F, Instruction & I, CallInst *cs ){
        errs()<<"run add_indirect_calls on : "<<F.getName()<<'\n';
        // Define strings and output streams to store and output the name and type of the callee
        std::string str;
        raw_string_ostream callee_name(str);
        std::string str2;
        raw_string_ostream callee_type_str(str2);
        // Define a pointer to function type and a pointer to JSON Value
        FunctionType * IndirectType;
        Json::Value *Indirect;
        // Print instruction I to callee_name output stream to get the corresponding callsite instruction
        I.print(callee_name);
        // Create or get a node with the content of the callee instruction in the JSON root object
        Indirect = &OutputJsonRoot[callee_name.str()];
        // Mark this node as an indirect call site
        (*Indirect)["Attr"]["Type"] = "Indirect";
        llvm::Metadata* incallID = cs->getMetadata("inCallID");
        auto *md = llvm::dyn_cast<llvm::MDNode>(incallID);
        int ID = cast<ConstantInt>(cast<ConstantAsMetadata>(md->getOperand(0))->getValue())->getZExtValue();
        (*Indirect)["Attr"]["ID"] = ID;
        //Set the function where this indirect call site is located
        (*Indirect)["Attr"]["Function"] = I.getFunction()->getName().str();
        // Establish connection with the JSON node corresponding to the function
        add_connection(Fnode,callee_name.str(),"Indirect Call Type");
        // Get the indirect call function type and print it to the callee_type_str output stream
        IndirectType = cs->getFunctionType();
        IndirectType->print(callee_type_str);
        //Set LLVMType in the corresponding indirect callsite node to the type information of the indirect call site (function type and parameter types)
        (*Indirect)["Attr"]["LLVMType"] = callee_type_str.str();
        //orig code
        // // Iterate through all functions in the module, check if there are functions matching the indirect call type and having their address taken
        // for (Function & Funct : F.getParent()->getFunctionList()){
        //     //if ( IndirectType == Funct.getFunctionType() &&
        //     if ( TypesEqual(IndirectType,Funct.getFunctionType()) &&
        //          Funct.hasAddressTaken() ){
        //         // Establish connection in the owning function, i.e., call graph
        //         add_connection(Fnode,Funct.getName().str(),"Indirect Call");
        //         //Establish connection in the node corresponding to indirect callsite, instruction-level graph
        //         add_connection(*Indirect,Funct.getName().str(),"Indirect Call");
        //     }
        // }
        std::pair<std::string,int> key(F.getName().str(),ID);
        //Add callees of this function from indirect_call_graph
        errs()<<"run loop on add_indirect_calls on :"<<F.getName()<<"\n";
        for(auto it =indirect_call_graph[key].begin();it!=indirect_call_graph[key].end();it++){
            add_connection(Fnode,*it,"Indirect Call");// Establish connection in the owning function, i.e., call graph
            add_connection(*Indirect,*it,"Indirect Call");//Establish connection in the node corresponding to indirect callsite, instruction-level graph
        }
    }

    class DataDependancy{

    public:
        SmallSet<Function *,32> functions;

        DataDependancy(Value * v,Type * t,const DataLayout & DL,unsigned ad=0){
            V = v;
            read = false;
            write = false;
            isArg = false;
            isParam = false;
            argNum = 0;
            ty = t;
            addr = ad;
            determineSize(DL);
            id = dd_class_id;
            dd_class_id++;
        }

        bool getRead(){
            return read;
        }

        bool getWrite(){
            return write;
        }

        void setIsArg(unsigned n){
            isArg =true;
            argNum = n;
        }

        void setIsParam(unsigned n){
            isParam =true;
            argNum = n;
        }
        void add_function(Function *F){
            functions.insert(F);
        }

        unsigned getAddr(){
            return addr;
        }

        unsigned getSize(){
            return size;
        }

        void WriteNode(Json::Value &JsonNode){
            std::stringstream ss;
            ss << ".Peripheral_"<<id;
            Json::Value & ThisNode = JsonNode[ss.str()];
            errs()<< "Writing Json Node: " << ss.str() <<"\n";
            writeJsonAttributes(ThisNode["Attr"]);
            for (Function * F : functions){
                ThisNode["Connections"][F->getName().str()]["Type"]="Peripheral";
            }

        }

        bool inside(unsigned value){

            if (this->addr <= value && value < this->addr+this->size){
                return true;
            }
            return false;
        }


        bool overlap(DataDependancy *DD){
            if (inside(DD->getAddr()) || DD->inside(this->addr)){
                 return true;
            }
            return false;
        }

        void determineSize(const DataLayout & DL){
            // size = 0;
            // if(ty){
            //     if(PointerType * PT = dyn_cast<PointerType>(ty)){
            //         size = DL.getTypeAllocSize(PT->getElementType());
            //     }
            //     else{
            //        size = DL.getTypeAllocSize(ty);
            //     }

            // }
            size = 0;
            if (ty) {
                if (PointerType *PT = dyn_cast<PointerType>(ty)) {
                    Type *ElemTy = PT->getNonOpaquePointerElementType();
                    if (ElemTy->isSized()) { // Only call getTypeAllocSize() for types with fixed size
                        size = DL.getTypeAllocSize(ElemTy);
                    } else {
                        errs() << "Warning: Pointer to unsized type. Skipping size calculation.\n";
                    }
                } else if (ty->isSized()) { // Ensure ty itself is sizable
                    size = DL.getTypeAllocSize(ty);
                } else {
                    errs() << "Warning: Unsized type encountered. Skipping size calculation.\n";
                }
            }
        }

        void writeJsonAttributes(Json::Value & Rnode){
            std::string s;
            raw_string_ostream  stream(s);

            if (addr){
                Rnode["Type"] = "Peripheral";
                Rnode["Addr"] = addr;

            }
            if (V->hasName()){
                Rnode["Name"] = V->getName().str();
            }
            if(ty){
                ty->print(stream);
                Rnode["DataType"] =stream.str();
                Rnode["DataSize"] = size;
            }
            Rnode["Read"] = read;
            Rnode["Write"] = write;
            std::string st;
            raw_string_ostream ss(st);
            V->print(ss);
            Rnode["LLVM::Value"] =ss.str();
            Rnode["IsArg"] = isArg;
            Rnode["IsParam"] = isParam;
            Rnode["ArgNum"] = argNum;

        }
        void updateProperties(Value *v){
            if(isa<LoadInst>(v)){
                read = true;
            }else if (isa<StoreInst>(v)){
                write = true;
            }
        }

        void determineAttributes(Value * v){
            errs() << "Determining Attributes\n";
            v->print(llvm::errs());
            errs() << "\n";
            if(Instruction * I =dyn_cast<Instruction>(v)){//Is an instruction
                errs() << "Adding Function ";
                errs().write_escaped(I->getFunction()->getName());
                errs()<< "\n";
                functions.insert(I->getFunction());
                if(CallInst * CI = dyn_cast<CallInst>(v)){
                    SmallSet<Function *,32> Callees;
                    getPotentialCallees(CI,Callees);
                    for(Function * Callee : Callees){
                        functions.insert(Callee);
                    }
                }else if(isa<LoadInst>(v)){
                    read = true;
                }else if (isa<StoreInst>(v)){
                    write = true;
                }
            }else{//If not an instruction, recursively traverse all users of this value
                for(User * U: v->users()){
                    determineAttributes(U);
                }
            }
        }

    private:

        unsigned id;
        Value * V;
        bool read;
        bool write;
        bool isArg;
        bool isParam;
        unsigned argNum;
        unsigned addr;
        unsigned size;
        Type * ty;
    };

    static unsigned dd_class_id;

    //Get potential callee functions
    static void getPotentialCallees(CallInst * CI,SmallSet<Function *,32> &Callees){
        Function *Callee = CI->getCalledFunction();

        if (Callee){
            Callees.insert(Callee);
        }else{

            // org code
            // for(auto & Fun : CI->getFunction()->getParent()->functions()){
            //     //to do ,don't use type-based analysis
            //      if (Fun.getFunctionType() == CI->getFunctionType()){
            //          Callees.insert(&Fun);
            //      }
            // }
            // add, get callee via indirect call relationship
            Module*M=CI->getModule();
            llvm::Metadata *incallID = CI->getMetadata("inCallID");
            if (auto *md = llvm::dyn_cast<llvm::MDNode>(incallID)) {
                if (auto *constMeta = llvm::dyn_cast<llvm::ConstantAsMetadata>(md->getOperand(0))) {
                    if (auto *constInt = llvm::dyn_cast<llvm::ConstantInt>(constMeta->getValue())) {
                        int ID = constInt->getZExtValue();
                        std::pair<std::string,int> key(CI->getFunction()->getName().str(),ID);
                        for(auto it =indirect_call_graph[key].begin();it!=indirect_call_graph[key].end();it++){
                            Function *callee=M->getFunction(*it);
                            if(callee){
                                Callees.insert(callee);
                            }
                        }
                    }
                }
            }
            
        }
    }

    //Establish relationship between functions, effectively callsite -> callee
    void getFunctionResources(Module * M){
        assert(M && "Module pointer is null!");
        errs()<<"run getFunctionResources"<<'\n';
        DenseMap<Function *,DenseMap<Value *,DataDependancy*>> DependanceMap;

        SmallSet<Constant *,32> PeripheralWorklist;
        SmallSet<Value *,32>LocalWorklist;
        SmallSet<GlobalVariable *,32> GlobalWorklist;

        for (Function & F :M->functions()){
            if(F.empty()){
                errs()<<"empty function :"<< F.getName() << "\n";
                continue;
            }
            if (!F.empty()) {
                errs() << "Processing function: " << F.getName() << "\n";
            }
            if(F.getName().startswith("llvm.dbg.")){//Ignore functions starting with llvm.dbg.
                continue;
            }
            for (inst_iterator itr=inst_begin(F); itr!=inst_end(F);++itr){
                Instruction *I = &*itr;
                if (!I) {
                    errs() << "Error: Instruction is null!\n";
                    break;
                }
                for (unsigned i=0;i<I->getNumOperands();i++){
                    Value *V = I->getOperand(i);
                    if (!V) {
                        errs() << "Error: Operand is null in function " << F.getName() << "\n";
                        break;
                    }
                    if(ConstantExpr *C = dyn_cast<ConstantExpr>(V)){
                        PeripheralWorklist.insert(C);
                    }else if (AllocaInst *AI = dyn_cast<AllocaInst>(V)){
                        LocalWorklist.insert(AI);
                    }else if (GlobalVariable *GV = dyn_cast<GlobalVariable>(V)){
                        GlobalWorklist.insert(GV);
                    }
                }
            }
            //errs() << "before AddFunctionToJSON " << F.getName() << "\n";
            AddFunctionToJSON(F);
        }

        getPeripheralDependancies(PeripheralWorklist,M->getDataLayout());

    }


    void getPeripheralDependancies(SmallSet<Constant *,32> &Worklist, const DataLayout & DL){

        DataDependancy * DD;

        for (auto * C : Worklist){
            unsigned addr;
            Type * ty=nullptr;
            errs() << "Checking Constant: ";
            C->print(llvm::errs());
            errs() << "\n";
            addr = getConstIntToPtr(C,ty);
            if (addr != 0){
                DD = new DataDependancy(C,ty,DL,addr);
                errs() << "Creating DD: ";
                C->print(llvm::errs());
                errs() << "\n";
                for (User * U : C->users()){
                    DD->determineAttributes(U);

                }
                DD->WriteNode(OutputJsonRoot);
                delete DD;
            }
        }
    }

    //Extract a constant integer address from a Value object and return it, along with the type associated with that address.
    unsigned getConstIntToPtr(Value * V,Type * &ty){
        unsigned addr = 0;
        // Check if V is an instruction of type IntToPtrInst
        if (IntToPtrInst * I2P = dyn_cast<IntToPtrInst>(V)){
            // Get operand of IntToPtrInst
            Value *Val = I2P->getOperand(0);
            // Check if operand is of type ConstantInt
            if (ConstantInt * CInt = dyn_cast<ConstantInt>(Val)){
                // Get constant integer value and limit its range to 0xFFFFFFFF
                addr = CInt->getValue().getLimitedValue(0xFFFFFFFF);
                // Get type of IntToPtrInst
                ty = I2P->getType();
                errs() << "Int: " << addr << "\n";
                errs()<<"Type: "; 
                ty->print(llvm::errs());
                errs() << "\n";
                
                return addr;
            }else{
                return 0;
            }
        }else if (Instruction * I = dyn_cast<Instruction>(V)){// Check if V is of type Instruction
            for (unsigned i=0;i<I->getNumOperands();i++){// Iterate through all operands of the instruction
                addr = getConstIntToPtr(I->getOperand(i),ty);// Recursively call getConstIntToPtr
                if (addr){
                    return addr;
                }
            }
        }
        // else if (ConstantExpr * C = dyn_cast<ConstantExpr>(V)){// Check if V is of type ConstantExpr
        //     Instruction *Instr = C->getAsInstruction();// Convert ConstantExpr to Instruction
        //     addr = getConstIntToPtr(Instr,ty);
        //     if (Instr->getParent()) {
        //         Instr->eraseFromParent();
        //     } else {
        //         delete Instr; // Release manually created Instruction
        //     }
             
        // }
        else if (ConstantExpr *C = dyn_cast<ConstantExpr>(V)) {
            if (C->isCast()) {  // Only handle cast expressions
                if (Constant *Op = C->getOperand(0)) {
                    addr = getConstIntToPtr(Op, ty);
                }
            }
        }
        else{
            return 0;
        }
        return addr;
    }

    void init_pointer_analysis_result()
    {
        // Read file content
        std::string line;
        //Regular expression
        std::regex pattern_indirect_callsite(R"(In function\s*:(\S+)\s*indirect callsite\s*:\s*(\d+))");
        std::regex pattern_direct_callsite(R"(In function\s*:(\S+)\s*direct callsite\s*:\s*(\d+))");
        std::regex pattern_target(R"(--target\s*(\S+))");
        std::string func_name;
        std::pair<std::string,int> callsite;
        if (!HexboxAnalysisIndirectCallFilePath.empty()) {
            std::ifstream file(HexboxAnalysisIndirectCallFilePath.c_str());
            if (file.is_open()) {
                while (std::getline(file, line)) {
                    // Process file content
                    //llvm::errs() << "Read line: " << line << "\n";
                    if(std::regex_match(line,pattern_indirect_callsite))
                    {
                        std::smatch result;
                        std::regex_search(line,result,pattern_indirect_callsite);
                        func_name=result[1];
                        int id=std::stoi(result[2]);
                        callsite=std::make_pair(func_name,id);
                    }
                    else if(std::regex_match(line,pattern_target))
                    {
                        std::smatch result;
                        std::regex_search(line,result,pattern_target);
                        std::string target_name=result[1];
                        indirect_call_graph[callsite].insert(target_name);
                    }

                }
                file.close();
            } else {
                llvm::errs() << "Failed to open file: " << HexboxAnalysisIndirectCallFilePath << "\n";
            }
        }
        if(!HexboxAnalysisDirectCallFilePath.empty()){
            std::ifstream file(HexboxAnalysisDirectCallFilePath.c_str());
            if (file.is_open()) {
                while (std::getline(file, line)) {
                    // Process file content
                    //llvm::errs() << "Read line: " << line << "\n";
                    if(std::regex_match(line,pattern_direct_callsite))
                    {
                        std::smatch result;
                        std::regex_search(line,result,pattern_direct_callsite);
                        func_name=result[1];
                        int id=std::stoi(result[2]);
                        callsite=std::make_pair(func_name,id);
                    }
                    else if(std::regex_match(line,pattern_target))
                    {
                        std::smatch result;
                        std::regex_search(line,result,pattern_target);
                        std::string target_name=result[1];
                        direct_call_graph[callsite].insert(target_name);
                    }

                }
                file.close();
            } else {
                llvm::errs() << "Failed to open file: " << HexboxAnalysisDirectCallFilePath << "\n";
            }
        }
    }
    void init_cpt_policy(){

        /*
        std::map<std::string, int> controller_filename;
        std::map<std::string, int> sensor_actrator_filename;
        */

        //controller initialization
        //attitude controller
        controller_filename["../../libraries/AC_AttitudeControl/AC_AttitudeControl.cpp"] = 0;
        //position controller
        controller_filename["../../libraries/AC_AttitudeControl/AC_PosControl.cpp"] = 0;
        //PID controller
        controller_filename["../../libraries/AC_PID/AC_PID.cpp"] = 0;
        //controlmonitor
        controller_filename["../../libraries/AC_AttitudeControl/ControlMonitor.cpp"] = 0;
        //avoidance
        controller_filename["../../libraries/AC_Avoidance/AC_Avoid.cpp"] = 0;
        //preciceland
        controller_filename["../../libraries/AC_PrecLand/AC_PrecLand.cpp"] = 0;
        //follow
        controller_filename["../../libraries/AP_Follow/AP_Follow.cpp"] = 0;
        //fali-safe
        controller_filename["../../libraries/AP_Follow/failsafe.cpp"] = 0;

        //sensor/actrator initialization
        // sensor_actrator_filename[""] = 0;

        // peripheral:

        // @syringe pump
        // syringePump.c
        // util.c
        // LiquidCrystal.c
        // led.c

        // @house alarm
        // util.c
        // alarm4pi.c
        // gpio_polling.c
        // pushover.c
        // public_ip.c
        // proc_helper.c
        // log_msgs.c
        // bcm_gpio.c


        //syringe pump
        controller_filename["LiquidCrystal.c"] = 0;
        controller_filename["led.c"] = 0;

        //house alarm
        controller_filename["gpio_polling.c"] = 0;
        controller_filename["bcm_gpio.c"] = 0;        
        controller_filename["alarm4pi.c"] = 0;


        return;
    }

    // Build reverse call graph for efficient caller lookup
    void buildReverseCallGraphs() {
        errs() << "Building reverse call graphs...\n";
        reverse_indirect_call_graph.clear();
        reverse_direct_call_graph.clear();
        
        for(const auto& entry : indirect_call_graph) {
            const std::string& caller = entry.first.first;
            for(const auto& callee : entry.second) {
                reverse_indirect_call_graph[callee].insert(caller);
            }
        }
        
        for(const auto& entry : direct_call_graph) {
            const std::string& caller = entry.first.first;
            for(const auto& callee : entry.second) {
                reverse_direct_call_graph[callee].insert(caller);
            }
        }
        errs() << "Reverse call graphs built.\n";
    }

    // Build global variable to filename mapping (process module debug info only once)
    void buildGlobalVarDebugInfo(Module &M) {
        errs() << "Building global variable debug info map...\n";
        global_var_to_file_map.clear();
        
        DebugInfoFinder Finder;
        Finder.processModule(M);  // Only call once for the entire module!
        
        for (DICompileUnit *DIC : Finder.compile_units()) {
            for (auto *DIG : DIC->getGlobalVariables()) {
                if (auto *GlobalVar = dyn_cast<DIGlobalVariable>(DIG)) {
                    std::string linkage_name = GlobalVar->getLinkageName().str();
                    std::string display_name = GlobalVar->getDisplayName().str();
                    std::string filename = GlobalVar->getFile()->getFilename().str();
                    
                    if (!linkage_name.empty()) {
                        global_var_to_file_map[linkage_name] = filename;
                    }
                    if (!display_name.empty()) {
                        global_var_to_file_map[display_name] = filename;
                    }
                }
            }
        }
        errs() << "Global variable debug info map built (" << global_var_to_file_map.size() << " entries).\n";
    }

    /**
     * @brief doInitialization
     * @param M
     * @return
     */
    bool doInitialization(Module &M) override{

        //jinwen comment this with return for other implementation.

        // return false;


        if ( HexboxAnalysisResults.compare("-") == 0 ){
            return false;
        }

        //AMI policy
        init_cpt_policy();

        init_pointer_analysis_result();
        
        // Performance optimization: build reverse call graphs once
        buildReverseCallGraphs();
        
        // Performance optimization: process debug info once for all global variables
        buildGlobalVarDebugInfo(M);
        
        for (GlobalVariable &GV : M.globals()){
            // if( GV.hasInitializer() && !GV.getName().startswith(".") ){
            if(!GV.getName().startswith(".") ){
                // errs() << "#######" << GV.getName().str() << "#######\n";
                // std::cout << GV.getName().str() << std::endl;
                // errs() <<"dddddddd: "<< GV.hasMetadata() << "\n";
                addFunctionUses(GV,&GV,M);
             }else{
                // errs() << "GV Has no Initializer:";
                // GV.dump();
            }
        }
        // commend here
        getFunctionResources(&M);
        return false;
    }


    StringRef getPassName() const  {
        // return StringRef("HexboxAnalysis");
        return "HexboxAnalysis";
    }


    bool runOnFunction(Function & F) override {

        if ( HexboxAnalysisResults.compare("-") == 0 ){
            errs() << "HexboxAnalysisResults: " << HexboxAnalysisResults << "\n";
            return false;
        }

        NumFunctions++;

        return false;
    }




    //Get functions using this global variable
    void addFunctionUses(GlobalVariable & GV, Value * V, Module & M){
         for (User * U : V->users()){ //users of global variables
             Json::Value * Global;
             Function * F = nullptr;

             if ( Instruction * I = dyn_cast<Instruction>(U) ) {//If user of this global variable is an instruction, get the instruction using this variable and the function containing it
                 F = I->getFunction();
                 Global = &OutputJsonRoot[GV.getName().str()];
                 add_connection(*Global,F->getName().str(),"Data");
                 (*Global)["Attr"]["Type"]="Global";
                 (*Global)["Attr"]["Size"] = static_cast<uint64_t>(M.getDataLayout().getTypeAllocSize(GV.getType()).getFixedSize());
                 
                 // Performance optimization: use pre-built map instead of processing module repeatedly
                 std::string gv_name = GV.getName().str();
                 auto file_it = global_var_to_file_map.find(gv_name);
                 if (file_it != global_var_to_file_map.end()) {
                     std::string filename = file_it->second;
                     (*Global)["Attr"]["Filename"] = filename;
                     
                     //AMI controller policy
                     auto controller_it = controller_filename.find(filename);
                     if(controller_it != controller_filename.end()){
                         (*Global)["Attr"]["Controller"] = filename;
                     }
                 }

             }else if ( Constant * C = dyn_cast<Constant>(U) ){//If user of this global variable is of constant type, recurse
                 addFunctionUses(GV,C,M);
             }else{
                 errs() << "Unknown Use";
                 U->print(llvm::errs());
                 errs() << "\n";
                 
             }
         }
    }




    bool doFinalization(Module &M) override{

        if ( HexboxAnalysisResults.compare("-") == 0 ){
            return false;
        }
        errs()<<"run doFinalization"<<'\n';
        std::ofstream jsonFile;
        jsonFile.open(HexboxAnalysisResults);
        jsonFile << OutputJsonRoot;
        jsonFile.close();

        return false;
    }


    // We don't modify the program, so we preserve all analyses.
    void getAnalysisUsage(AnalysisUsage &AU) const override {

        AU.setPreservesAll();

    }
  };

}


unsigned HexboxAnalysis::dd_class_id =0;
char HexboxAnalysis::ID = 0;
std::map<std::pair<std::string,int>,std::set<std::string>> HexboxAnalysis::indirect_call_graph={};//Stores the indirect call relationship obtained from pointer analysis svf + llvm built-in virtual function analysis
std::map<std::pair<std::string,int>,std::set<std::string>> HexboxAnalysis::direct_call_graph{};//Stores direct calling
static RegisterPass<HexboxAnalysis>X("HexboxAnaysis", "Performs HexBox LLVM Analysis",false, false);

