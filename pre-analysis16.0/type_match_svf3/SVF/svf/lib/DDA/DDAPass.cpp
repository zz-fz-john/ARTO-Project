//===- DDAPass.cpp -- Demand-driven analysis driver pass-------------//
//
//                     SVF: Static Value-Flow Analysis
//
// Copyright (C) <2013->  <Yulei Sui>
//

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.

// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
//===----------------------------------------------------------------------===//


/*
 * @file: DDAPass.cpp
 * @author: Yulei Sui
 * @date: 01/07/2014
 */

#include "SVF-LLVM/LLVMUtil.h"
#include "SVF-LLVM/SVFIRBuilder.h"
#include "SVF-LLVM/LLVMModule.h"
#include "Util/Options.h"
#include "MemoryModel/PointerAnalysisImpl.h"
#include "DDA/DDAPass.h"
#include "DDA/FlowDDA.h"
#include "DDA/ContextDDA.h"
#include "DDA/DDAClient.h"
#include <string>
#include <sstream>
#include <limits.h>
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Analysis/TypeMetadataUtils.h"
#include "llvm/IR/Constants.h"
using namespace SVF;
using namespace SVFUtil;
using namespace std;
typedef std::set<std::string> CandidateSet; //use to store candidates of indirect callsite;
typedef std::map<const llvm::CallBase*, CandidateSet> IndirectCallCandsMap;//used to store indirect callsite and its candidates;
char DDAPass::ID = 0;

DDAPass::~DDAPass()
{
    // _pta->dumpStat();
    if (_client != nullptr)
        delete _client;
}

bool isTypeMachted_in_dda(const llvm::CallBase* indirectcallsite,const llvm::Function * func ){
    //llvm::outs()<<"dbg is type start"<<'\n';
    int nCallsiteArg=0,nFuncArg=0;
    std::vector<llvm::Type*> CallsiteArgList,FuncArgList;
     llvm::Type *rty=func->getReturnType();
    nFuncArg=func->arg_size();
    for(llvm::Function::const_arg_iterator AI=func->arg_begin(),AE=func->arg_end();AI!=AE;++AI){
        const llvm::Value *arg=AI;
        llvm::Type *argType=arg->getType();
        FuncArgList.push_back(argType);
    }
    if(llvm::isa<CallInst>(indirectcallsite))
        {
            const llvm::CallInst *cBase=llvm::dyn_cast<const CallInst>(indirectcallsite);
            if(cBase->getFunctionType()->getReturnType()!=rty)
            {
                return false;
            }
            nCallsiteArg=cBase->arg_size();
           //llvm::outs()<<"dbg for getargoperand"<<'\n';
            for (int i=0;i<nCallsiteArg;i++)
            {
                //llvm::outs()<<"nacallsitearg is "<<nCallsiteArg<<"i is "<<i<<"arg size is "<<cBase->arg_size()<<"funcarg size is"<< nFuncArg<<'\n';

                llvm::Value *arg=cBase->getArgOperand(i);
                llvm::Type *argType=arg->getType();
                CallsiteArgList.push_back(argType);
            }

        }
    if(nCallsiteArg==nFuncArg)
    {
        //llvm::outs()<<"if statement is true"<<'\n';
        for(int i=0;i<nFuncArg;i++)
        {
            //llvm::outs()<<FuncArgList[i]<<"   "<<CallsiteArgList[i]<<'\n';
            if(FuncArgList[i]!=CallsiteArgList[i])
            {

                return false;
            }
        }
        return true;
    }
    else 
    {
        return false;
    }
    return false;


}
void DDAPass::runOnModule(SVFIR* pag)
{
    /// initialization for llvm alias analyzer
    //InitializeAliasAnalysis(this, getDataLayout(&module));

    selectClient(pag->getModule());

    for (u32_t i = PointerAnalysis::FlowS_DDA;
            i < PointerAnalysis::Default_PTA; i++)
    {
        PointerAnalysis::PTATY iPtTy = static_cast<PointerAnalysis::PTATY>(i);
        if (Options::DDASelected(iPtTy))
            runPointerAnalysis(pag, i);
    }
    SVF::PTACallGraph  *pta=_pta->getPTACallGraph();
    SVF::PointerAnalysis::CallEdgeMap dda_result=pta->getIndCallMap();
    for(auto it= pag->getIndirectCallsites().begin();it!=pag->getIndirectCallsites().end();it++)
    {
        const SVF::CallICFGNode*ind_call_node = it->first;
        const SVF::SVFInstruction *ind_callsite_svf=ind_call_node->getCallSite();
        const llvm::Value * ind_callsite_llvm=SVF::LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(ind_callsite_svf);
        const llvm::Instruction *ind_callsite=llvm::dyn_cast<const llvm::Instruction>(ind_callsite_llvm);
        CandidateSet candidates_dda;
        for (auto it2=dda_result[ind_call_node].begin();it2!=dda_result[ind_call_node].end();it2++)
        {
                const SVF::SVFFunction *target =*it2;
                const llvm::Value * target_value=SVF::LLVMModuleSet::getLLVMModuleSet()->getLLVMValue(target);
                const Function* tartget_function=SVF::LLVMUtil::getLLVMFunction(target_value);
                std::string function_name=tartget_function->getName().str();
                //use type matched to filt wrong targets
                const llvm::CallBase *cBase=llvm::dyn_cast<const CallBase>(ind_callsite);
                //candidates_dda.insert(function_name);
                if(isTypeMachted_in_dda(cBase,tartget_function)==true)
                {
                    candidates_dda.insert(function_name);
                }
                
        }
        assert(llvm::dyn_cast<const CallBase>(ind_callsite)&&"indirect callsite is not callbase type!\n");
        if(const CallBase * ci = llvm::dyn_cast<const CallBase >(ind_callsite))
         {
            llvm::Metadata* incallID = ci ->getMetadata("inCallID");
            if (incallID) {
                auto *md = llvm::dyn_cast<llvm::MDNode>(incallID);
                int ind_ID = llvm::cast<llvm::ConstantInt>(llvm::cast<llvm::ConstantAsMetadata>(md->getOperand(0))->getValue())->getZExtValue();
                std::string function_name=ci->getFunction()->getName().str();
        //_indirect_call_cands_map.insert(std::make_pair(ci, candidates));
            llvm::outs()<<"In function :"<<function_name<<"  ";
            llvm::outs() <<"indirect callsite : "<<ind_ID<<"\n";

            for(auto it3=candidates_dda.begin();it3!=candidates_dda.end();it3++)
                {
                //const Function *f=*it3;
               llvm::outs() <<"--target  "<<*it3<<"\n";
                }
            }
        }
        else
         {
            llvm::errs() << ind_callsite << " --indirect callsite is not CallBase type!\n";
         }
    }

}

/// select a client to initialize queries
void DDAPass::selectClient(SVFModule* module)
{

    if (!Options::UserInputQuery().empty())
    {
        /// solve function pointer
        if (Options::UserInputQuery() == "funptr")
        {
            _client = new FunptrDDAClient(module);
        }
        else if (Options::UserInputQuery() == "alias")
        {
            _client = new AliasDDAClient(module);
        }
        /// allow user specify queries
        else
        {
            _client = new DDAClient(module);
            if (Options::UserInputQuery() != "all")
            {
                u32_t buf; // Have a buffer
                stringstream ss(Options::UserInputQuery()); // Insert the user input string into a stream
                while (ss >> buf)
                    _client->setQuery(buf);
            }
        }
    }
    else
    {
        assert(false && "Please specify query options!");
    }

    _client->initialise(module);
}

/// Create pointer analysis according to specified kind and analyze the module.
void DDAPass::runPointerAnalysis(SVFIR* pag, u32_t kind)
{

    ContextCond::setMaxPathLen(Options::MaxPathLen());
    ContextCond::setMaxCxtLen(Options::MaxContextLen());

    /// Initialize pointer analysis.
    switch (kind)
    {
    case PointerAnalysis::Cxt_DDA:
    {
        _pta = std::make_unique<ContextDDA>(pag, _client);
        break;
    }
    case PointerAnalysis::FlowS_DDA:
    {
        _pta = std::make_unique<FlowDDA>(pag, _client);
        break;
    }
    default:
        outs() << "This pointer analysis has not been implemented yet.\n";
        break;
    }

    if(Options::WPANum())
    {
        _client->collectWPANum(pag->getModule());
    }
    else
    {
        ///initialize
        _pta->initialize();
        ///compute points-to
        _client->answerQueries(_pta.get());
        ///finalize
        _pta->finalize();
        if(Options::PrintCPts())
            _pta->dumpCPts();

        if (_pta->printStat())
            _client->performStat(_pta.get());

        if (Options::PrintQueryPts())
            printQueryPTS();
    }
}


/*!
 * Initialize context insensitive Edge for DDA
 */
void DDAPass::initCxtInsensitiveEdges(PointerAnalysis* pta, const SVFG* svfg,const SVFGSCC* svfgSCC, SVFGEdgeSet& insensitveEdges)
{
    if(Options::InsenRecur())
        collectCxtInsenEdgeForRecur(pta,svfg,insensitveEdges);
    else if(Options::InsenCycle())
        collectCxtInsenEdgeForVFCycle(pta,svfg,svfgSCC,insensitveEdges);
}

/*!
 * Whether SVFG edge in a SCC cycle
 */
bool DDAPass::edgeInSVFGSCC(const SVFGSCC* svfgSCC,const SVFGEdge* edge)
{
    return (svfgSCC->repNode(edge->getSrcID()) == svfgSCC->repNode(edge->getDstID()));
}

/*!
 *  Whether call graph edge in SVFG SCC
 */
bool DDAPass::edgeInCallGraphSCC(PointerAnalysis* pta,const SVFGEdge* edge)
{
    const SVFFunction* srcFun = edge->getSrcNode()->getICFGNode()->getFun();
    const SVFFunction* dstFun = edge->getDstNode()->getICFGNode()->getFun();

    if(srcFun && dstFun)
    {
        return pta->inSameCallGraphSCC(srcFun,dstFun);
    }

    assert(edge->isRetVFGEdge() == false && "should not be an inter-procedural return edge" );

    return false;
}

/*!
 * Mark insensitive edge for function recursions
 */
void DDAPass::collectCxtInsenEdgeForRecur(PointerAnalysis* pta, const SVFG* svfg,SVFGEdgeSet& insensitveEdges)
{

    for (SVFG::SVFGNodeIDToNodeMapTy::const_iterator it = svfg->begin(),eit = svfg->end(); it != eit; ++it)
    {

        SVFGEdge::SVFGEdgeSetTy::const_iterator edgeIt = it->second->InEdgeBegin();
        SVFGEdge::SVFGEdgeSetTy::const_iterator edgeEit = it->second->InEdgeEnd();
        for (; edgeIt != edgeEit; ++edgeIt)
        {
            const SVFGEdge* edge = *edgeIt;
            if(edge->isCallVFGEdge() || edge->isRetVFGEdge())
            {
                if(edgeInCallGraphSCC(pta,edge))
                    insensitveEdges.insert(edge);
            }
        }
    }
}

/*!
 * Mark insensitive edge for value-flow cycles
 */
void DDAPass::collectCxtInsenEdgeForVFCycle(PointerAnalysis* pta, const SVFG* svfg,const SVFGSCC* svfgSCC, SVFGEdgeSet& insensitveEdges)
{

    OrderedSet<NodePair> insensitvefunPairs;

    for (SVFG::SVFGNodeIDToNodeMapTy::const_iterator it = svfg->begin(),eit = svfg->end(); it != eit; ++it)
    {

        SVFGEdge::SVFGEdgeSetTy::const_iterator edgeIt = it->second->InEdgeBegin();
        SVFGEdge::SVFGEdgeSetTy::const_iterator edgeEit = it->second->InEdgeEnd();
        for (; edgeIt != edgeEit; ++edgeIt)
        {
            const SVFGEdge* edge = *edgeIt;
            if(edge->isCallVFGEdge() || edge->isRetVFGEdge())
            {
                if(this->edgeInSVFGSCC(svfgSCC,edge))
                {

                    const SVFFunction* srcFun = edge->getSrcNode()->getICFGNode()->getFun();
                    const SVFFunction* dstFun = edge->getDstNode()->getICFGNode()->getFun();

                    if(srcFun && dstFun)
                    {
                        NodeID src = pta->getPTACallGraph()->getCallGraphNode(srcFun)->getId();
                        NodeID dst = pta->getPTACallGraph()->getCallGraphNode(dstFun)->getId();
                        insensitvefunPairs.insert(std::make_pair(src,dst));
                        insensitvefunPairs.insert(std::make_pair(dst,src));
                    }
                    else
                        assert(edge->isRetVFGEdge() == false && "should not be an inter-procedural return edge" );
                }
            }
        }
    }

    for(SVFG::SVFGNodeIDToNodeMapTy::const_iterator it = svfg->begin(),eit = svfg->end(); it != eit; ++it)
    {
        SVFGEdge::SVFGEdgeSetTy::const_iterator edgeIt = it->second->InEdgeBegin();
        SVFGEdge::SVFGEdgeSetTy::const_iterator edgeEit = it->second->InEdgeEnd();
        for (; edgeIt != edgeEit; ++edgeIt)
        {
            const SVFGEdge* edge = *edgeIt;

            if(edge->isCallVFGEdge() || edge->isRetVFGEdge())
            {
                const SVFFunction* srcFun = edge->getSrcNode()->getICFGNode()->getFun();
                const SVFFunction* dstFun = edge->getDstNode()->getICFGNode()->getFun();

                if(srcFun && dstFun)
                {
                    NodeID src = pta->getPTACallGraph()->getCallGraphNode(srcFun)->getId();
                    NodeID dst = pta->getPTACallGraph()->getCallGraphNode(dstFun)->getId();
                    if(insensitvefunPairs.find(std::make_pair(src,dst))!=insensitvefunPairs.end())
                        insensitveEdges.insert(edge);
                    else if(insensitvefunPairs.find(std::make_pair(dst,src))!=insensitvefunPairs.end())
                        insensitveEdges.insert(edge);
                }
            }
        }
    }
}

AliasResult DDAPass::alias(NodeID node1, NodeID node2)
{
    SVFIR* pag = _pta->getPAG();

    if(pag->isValidTopLevelPtr(pag->getGNode(node1)))
        _pta->computeDDAPts(node1);

    if(pag->isValidTopLevelPtr(pag->getGNode(node2)))
        _pta->computeDDAPts(node2);

    return _pta->alias(node1,node2);
}
/*!
 * Return alias results based on our points-to/alias analysis
 * TODO: Need to handle PartialAlias and MustAlias here.
 */
AliasResult DDAPass::alias(const SVFValue* V1, const SVFValue* V2)
{
    SVFIR* pag = _pta->getPAG();

    /// TODO: When this method is invoked during compiler optimizations, the IR
    ///       used for pointer analysis may been changed, so some Values may not
    ///       find corresponding SVFIR node. In this case, we only check alias
    ///       between two Values if they both have SVFIR nodes. Otherwise, MayAlias
    ///       will be returned.
    if (pag->hasValueNode(V1) && pag->hasValueNode(V2))
    {
        PAGNode* node1 = pag->getGNode(pag->getValueNode(V1));
        if(pag->isValidTopLevelPtr(node1))
            _pta->computeDDAPts(node1->getId());

        PAGNode* node2 = pag->getGNode(pag->getValueNode(V2));
        if(pag->isValidTopLevelPtr(node2))
            _pta->computeDDAPts(node2->getId());

        return _pta->alias(V1,V2);
    }

    return AliasResult::MayAlias;
}

/*!
 * Print queries' pts
 */
void DDAPass::printQueryPTS()
{
    const OrderedNodeSet& candidates = _client->getCandidateQueries();
    for (OrderedNodeSet::const_iterator it = candidates.begin(), eit = candidates.end(); it != eit; ++it)
    {
        const PointsTo& pts = _pta->getPts(*it);
        _pta->dumpPts(*it,pts);
    }
}
