#include "addMetadata.h"
#include "vCallAnalysis.h"
#include "inCallUtil.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/WithColor.h>
#include <iostream>
#include <string>
#include <vector>
#include "SVF-LLVM/LLVMUtil.h"
#include "Graphs/SVFG.h"
#include "WPA/Andersen.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVF-LLVM/LLVMModule.h"
#include "Util/Options.h"
#include <boost/program_options.hpp>
#include "macro.h"
#include <fstream>
#include<algorithm>
#include "Analyzer.h"
#include "CallGraph.h"
using namespace llvm;
using namespace SVF;
namespace po = boost::program_options;
typedef std::set<std::string> CandidateSet; //use to store candidates of indirect
std::map<int, incall::IndirectCallEdge> incallEdges;//store llvm virtual call result
std::map<int, incall::IndirectCallEdge> incallEdges_SMLTA;
std::map<int, incall::IndirectCallEdge> incallEdges_final;
std::map<int, incall::IndirectCallEdge> incallEdges_SVF;
void incall::dumpIndirectCalls(std::string output){
    // dump to yaml file
    std::string DebugInfoFilename="../output/callsite_target_map.txt";//保存demangle后的函数名
    //std::ofstream outfile1(DebugInfoFilename);
    
    std::error_code EC;
    llvm::raw_fd_ostream OutFile(output, EC, llvm::sys::fs::OF_None);
    llvm::raw_fd_ostream OutFile1(DebugInfoFilename, EC, llvm::sys::fs::OF_None);
    if (EC) {
        llvm::errs() << "Could not open file: " << EC.message() << "\n";
        return;
    }
    //OutFile << "indirectCalls:\n";
    for (auto &pair : incallEdges_final){
        std::string function_name=pair.second.vcallsite.getParent()->getParent()->getName().str();
        //根据debug信息，找到对应的源代码行，并输出间接调用目标函数名称
        if (pair.second.isVirtual){//是虚函数调用,则找到虚函数在文件中的函数名
            if (auto *CI = dyn_cast<CallInst>(&pair.second.vcallsite)){
                if (DILocation *Loc = pair.second.vcallsite.getDebugLoc())
                {
                    //std::string inst=instructionToString(&pair.second.vcallsite);
                    unsigned Line = Loc->getLine();
                    unsigned Col = Loc->getColumn();
                    StringRef File = Loc->getFilename();
                    StringRef Dir = Loc->getDirectory();
                    //outfile1<<"In function :"<<function_name<<"  indirect callsite :   "<<inst<<"\n";
                    //outfile1<<"In function :"<<function_name<<"  indirect callsite :   "<<pair.first<<"\n";
                    OutFile1 <<"In function :" << function_name ;
                    OutFile1 << "  indirect callsite :   " << pair.first << "\n";
                    // 通过获取源文件路径和行号来输出对应的源代码行内容
                    std::ifstream sourceFile((Dir + "/" + File).str());
                    if (sourceFile.is_open()) {
                        std::string lineContent;
                        for (unsigned i = 0; i < Line && std::getline(sourceFile, lineContent); ++i) {
                            if (i == Line - 1) {
                                std::string line_funcname=lineContent.substr(Col-1);
                                size_t pos=line_funcname.find('(');
                                line_funcname=line_funcname.substr(0,pos);
                                OutFile1<<"--target  "<< line_funcname << "\n";
                                // OutFile1 << "    targets:\n";
                                // OutFile1 << "      - " << line_funcname << "\n";
                            }
                        }
                        
                    } 
                    else {
                        errs() << "Unable to open source file: " << Dir + "/" + File << "\n";
                    }
                }
            }
        }
        OutFile <<"In function :" << function_name ;
        OutFile << "  indirect callsite :   " << pair.first << "\n";
        // OutFile << "\tcallsite: " << edge.vcallsite << "\n";
        if (pair.second.candidates.empty()) {
            OutFile << "--target  None\n";
        }
        else{
            for (auto &target : pair.second.candidates){
                OutFile << "--target  " << target->getName() << "\n";
                }
        }

    }
}
void handleProgramOptions(int argc, char **argv, bool &isGUID,bool &useSVF,std::string & inputfile,std::string &outputfile) {
    // 输入参数为 --timer 时 isTimer 为 true
    boost::program_options::options_description desc("Allowed options");
    desc.add_options()
        ("help,h", "Show me help message")
        ("guid,id", "Additional instrument GUID metadata, default is false")
        ("usesvf,svf","whether use SVF result,default is false")
        ("input,i",po::value<std::string >()->required(),"input_file");
    

    boost::program_options::variables_map vm;
    // 如果有未知的参数，直接报错
    try {
        boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);
        boost::program_options::notify(vm);
    if (vm.count("help")){
        std::cout << desc << "\n";
        exit(0);
    }
    if (vm.count("guid")) {
        isGUID = true;
    }
    if(vm.count("usesvf")){
        useSVF=true;
    }
    if (isGUID)
        std::cout<<"Add GUID metadata for indirect call, Processing..."<<'\n';
    else
        std::cout<<"Start analysis indirect call, Processing..."<<'\n';
    if(useSVF)
    {
        std::cout<<"use svf result"<<'\n';
    }
    else
    {
        std::cout<<"only use llvm-type-metadata and SMLTA"<<'\n';
    }
    if(vm.count("input")){
        //std::cout << argv[2];
       std::cout<<vm["input"].as<std::string>()<<'\n';
        
        //const std::string out1=vm["input"].as<std::string>();
        inputfile=vm["input"].as<std::string>();

    }
    else{
        std::cout << "未指定输入文件.\n";
    }
    } 
    catch (const boost::program_options::unknown_option& e) {
        std::cout << desc << "\n";
        ERROR("Unknown option: " << e.what() << "\n");
        
        exit(1);
    }

}
void merge_result(bool useSVF){
    int max_id=0;
    if(!useSVF)
    {
        if(!incallEdges.empty()&&!incallEdges_SMLTA .empty())
        {
            max_id=std::max(incallEdges.rbegin()->first,incallEdges_SMLTA.rbegin()->first);
        }
        else if(!incallEdges.empty())
        {
            max_id=incallEdges.rbegin()->first;
        }
        else if(!incallEdges_SMLTA.empty())
        {
            max_id=incallEdges_SMLTA.rbegin()->first;
        }
        for(int i=0;i<=max_id;i++)
        {
            auto it_llvm=incallEdges.find(i);
            auto it_SMLTA=incallEdges_SMLTA.find(i);
            if(it_llvm!=incallEdges.end())//说明在incallEdges中找到了，则该callsite是一个虚函数调用
                {
                    CallBase*CB_llvm=&it_llvm->second.vcallsite;
                    if(!it_llvm->second.candidates.empty())//incallEdges中该callsite的目标函数集合不为空,使用incallEdgs中的信息
                    {
                        incallEdges_final.emplace(i,incall::IndirectCallEdge(*CB_llvm,true,it_llvm->second.candidates));
                    }
                    else{//incallEdges中该callsite的目标函数集合为空
                        if(it_SMLTA!=incallEdges_SMLTA.end())//incallEdges_SMLTA中有这个ID
                        {
                            CallBase *CB_SMLTA=&it_SMLTA->second.vcallsite;
                            incallEdges_final.emplace(i,incall::IndirectCallEdge(*CB_llvm,true,it_SMLTA->second.candidates));
                            
                        }
                        else{//incallEdges_SMLTA中没有这个ID，则只能使用incallEdges中的信息
                            std::cout<<"can not analysis indirect-call targets for ID: "<<i<<'\n';
                        }
                    }
                }
            else {//incallEdges中没有这个ID,说明不是一个虚函数
                if(it_SMLTA!=incallEdges_SMLTA.end())//incallEdges_SMLTA中有这个ID
                {
                    CallBase *CB_SMLTA=&it_SMLTA->second.vcallsite;
                    incallEdges_final.emplace(i,incall::IndirectCallEdge(*CB_SMLTA,false,it_SMLTA->second.candidates));

                }
                else{//该ID没有找到
                    std::cout<<"can not analysis indirect-call targets for ID: "<<i<<'\n';
                }
            }
        }
    }
    else{// without svf
        max_id=std::max(incallEdges.rbegin()->first,incallEdges_SMLTA.rbegin()->first);
        max_id=std::max(max_id,incallEdges_SVF.rbegin()->first);
        for(int i=0;i<=max_id;i++)
        {
            auto it_llvm=incallEdges.find(i);
            auto it_SMLTA=incallEdges_SMLTA.find(i);
            auto it_SVF=incallEdges_SVF.find(i);
            if(it_llvm!=incallEdges.end())//说明在incallEdges中找到了，则该callsite是一个虚函数调用
                {
                    CallBase*CB_llvm=&it_llvm->second.vcallsite;
                    if(!it_llvm->second.candidates.empty())//incallEdges中该callsite的目标函数集合不为空,使用incallEdgs中的信息
                    {
                        incallEdges_final.emplace(i,incall::IndirectCallEdge(*CB_llvm,true,it_llvm->second.candidates));
                    }
                    else{//incallEdges中该callsite的目标函数集合为空
                        if(it_SMLTA!=incallEdges_SMLTA.end()&&it_SVF==incallEdges_SVF.end())//incallEdges_SMLTA中有这个ID,而svf中没有这个id，则使用smlta的结果
                        {
                            CallBase *CB_SMLTA=&it_SMLTA->second.vcallsite;
                            incallEdges_final.emplace(i,incall::IndirectCallEdge(*CB_llvm,true,it_SMLTA->second.candidates));
                            
                        }
                        else if(it_SMLTA==incallEdges_SMLTA.end()&&it_SVF!=incallEdges_SVF.end())//smlta中没有，但是svf中有，使用svf的结果
                        {
                            incallEdges_final.emplace(i,incall::IndirectCallEdge(*CB_llvm,true,it_SVF->second.candidates));
                        }
                        else if(it_SMLTA!=incallEdges_SMLTA.end()&&it_SVF!=incallEdges_SVF.end())//两者都有，取交集
                        {
                            std::set<llvm::Function*>vCallCandidates;
                            std::set_intersection(it_SMLTA->second.candidates.begin(), it_SMLTA->second.candidates.end(),
                            it_SVF->second.candidates.begin(), it_SVF->second.candidates.end(),
                            std::inserter(vCallCandidates, vCallCandidates.begin()));
                            incallEdges_final.emplace(i,incall::IndirectCallEdge(*CB_llvm,true,vCallCandidates));
                        }
                        else{//三者都没有，则输出一个信息
                            std::cout<<"can not analysis indirect-call targets for ID:"<<i<<std::endl;
                        }
                    }
                }
            else {//incallEdges中没有这个ID,说明不是一个虚函数
                if(it_SMLTA!=incallEdges_SMLTA.end()&&it_SVF!=incallEdges_SVF.end())//incallEdges_SMLTA和svf中都有这个ID
                {
                    CallBase *CB_SMLTA=&it_SMLTA->second.vcallsite;
                    std::set<llvm::Function*>vCallCandidates;
                    std::set_intersection(it_SMLTA->second.candidates.begin(), it_SMLTA->second.candidates.end(),
                                            it_SVF->second.candidates.begin(), it_SVF->second.candidates.end(),
                                            std::inserter(vCallCandidates, vCallCandidates.begin()));
                    incallEdges_final.emplace(i,incall::IndirectCallEdge(*CB_SMLTA,false,vCallCandidates));
                }
                else if(it_SMLTA==incallEdges_SMLTA.end()&&it_SVF!=incallEdges_SVF.end())//smlta中没有，但是svf中有，使用svf的结果
                {
                    CallBase *CB_SVF=&it_SVF->second.vcallsite;
                    incallEdges_final.emplace(i,incall::IndirectCallEdge(*CB_SVF,false,it_SVF->second.candidates));
                }
                else if(it_SMLTA!=incallEdges_SMLTA.end()&&it_SVF==incallEdges_SVF.end())//incallEdges_SMLTA中有这个ID,而svf中没有这个id，则使用smlta的结果
                {
                    CallBase *CB_SMLTA=&it_SMLTA->second.vcallsite;
                    incallEdges_final.emplace(i,incall::IndirectCallEdge(*CB_SMLTA,false,it_SMLTA->second.candidates));
                }
                else{//该ID没有找到
                    std::cout<<"can not analysis indirect-call targets for ID: "<<i<<'\n';
                }
            }
        }
    }


}
int runSVF(std::string inputfile)
{
    std::vector<std::string>moduleNameVec;
    moduleNameVec.push_back(inputfile);
    if(Options::WriteAnder()=="ir_annotator")
    {
        LLVMModuleSet::getLLVMModuleSet()->preProcessBCs(moduleNameVec);
    }
    // LLVMModuleSet::buildSVFModule(moduleNameVec);
    SVF::SVFModule *svfModule=SVF::LLVMModuleSet::getLLVMModuleSet()->buildSVFModule(moduleNameVec);
    SVF::SVFIRBuilder builder(svfModule);
    SVF::SVFIR* pag=builder.build();
    //create Andersen's pointer analysis
    SVF::Andersen *ander=SVF::AndersenWaveDiff::createAndersenWaveDiff(pag);
    /// Call Graph
    PTACallGraph* pta = ander->getPTACallGraph();
    PointerAnalysis::CallEdgeMap ander_result=pta->getIndCallMap();
    for(auto it=pag->getIndirectCallsites().begin();it!=pag->getIndirectCallsites().end();it++)
    {
        const SVF::CallICFGNode * ind_call_node=it->first;
        if(ander_result.find(ind_call_node)==ander_result.end())
        {
            continue;
        }
        if(ander_result[ind_call_node].empty())
        {
            continue;
        }
        const SVF::SVFInstruction * ind_callsite_svf=ind_call_node->getCallSite();
        const llvm::Value * ind_callsite_llvm=SVF::LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(ind_callsite_svf);
        const llvm::Instruction * ind_callsite=llvm::dyn_cast<const llvm::Instruction>(ind_callsite_llvm);
        llvm::Metadata *inCallID=ind_callsite->getMetadata("inCallID");
        if(!inCallID)
        {
            llvm::errs()<<"No inCallID metadata found for callsite : "<<ind_callsite<<'\n';
            exit(1);
        }
        auto * md=llvm::dyn_cast<llvm::MDNode>(inCallID);
        int ID=cast<ConstantInt>(cast<ConstantAsMetadata>(md->getOperand(0))->getValue())->getZExtValue();
        std::set<llvm::Function*> InCallCandidates;

        for (auto it2=ander_result[ind_call_node].begin();it2!=ander_result[ind_call_node].end();it2++)
        {
            const SVF::SVFFunction *target =*it2;
            const llvm::Value * target_value=SVF::LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(target);
            llvm::Function* target_function=const_cast<llvm::Function*>(SVF::LLVMUtil::getLLVMFunction(target_value));
            InCallCandidates.insert(target_function);
            std::string function_name=target_function->getName().str();
            
        }
        llvm::CallBase *CB=const_cast<llvm::CallBase*>(llvm::dyn_cast<CallBase>(ind_callsite));
        incallEdges_SVF.emplace(ID,incall::IndirectCallEdge(*CB,true,InCallCandidates));
    }
}
GlobalContext GlobalCtx;
int main(int argc, char **argv) {

    bool isGUID = false;
    bool useSVF=false;
    std::string outputfile;
    std::string inputfile;
    // 检查参数数量是否正确
    // if (argc > 4||argc<3) {
    //     std::cerr << "使用方法: " << argv[0] << " <选择模式:insert/analysis><输入文件> <输出文件>\n";
    //     return 1;
    // }
    
    // std::string mode=argv[1];
    // inputfile=argv[2];
    // if(mode=="insert")
    // {
    //     isGUID=true;
    //     outputfile=argv[3];
    // }
    // else if(mode=="analysis")
    // {
    //     isGUID=false;
    // }
    // else{
    //     std::cout<<"please type insert or analysis"<<'\n';
    // }
    
    
    handleProgramOptions(argc, argv, isGUID,useSVF,inputfile,outputfile);

    llvm::LLVMContext context;
    llvm::SMDiagnostic error;
    std::error_code EC;

    context.setOpaquePointers(false);
    std::unique_ptr<llvm::Module> module = parseIRFile(inputfile, error, context);
    
    if (!module) {
        error.print("Error parsing IR file", llvm::errs());
        return 1;
    }

    if (isGUID){//添加元数据
        addmd::runAddMetaData(*module);
        llvm::raw_fd_ostream OutFile(inputfile, EC, llvm::sys::fs::OF_None);
        if (EC) {
            llvm::errs() << "Could not open file: " << EC.message() << "\n";
            abort();
        }   
        module->print(OutFile, nullptr);
    }
    else{//indirect call analysis 
        Module *Module=module.release();
        StringRef MName = StringRef(strdup(inputfile.data()));
        GlobalCtx.Modules.push_back(std::make_pair(Module, MName));
        GlobalCtx.ModuleMaps[Module] = MName;
        // Build global callgraph.
        CallGraphPass CGPass(&GlobalCtx);
        CGPass.run(GlobalCtx.Modules);
        // Print SMLTA final results
	    PrintResults(&GlobalCtx);
        //run llvm type-metadata based analysis
        vcall::runVirtualCallAnalysis(*Module);
        if(useSVF)
        {
            runSVF(inputfile);
        }
        // merge_result(incallEdges,incallEdges_SMLTA,incallEdges_final);
        merge_result(useSVF);
        incall::dumpIndirectCalls("../output/indirectcall.txt");
    }
        


    return 0;
}

