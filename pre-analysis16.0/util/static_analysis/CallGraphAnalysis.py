# Analyze dot graph to obtain related functions on the path
import networkx as nx
import re
D = nx.DiGraph()
AvoidFuncSet=set()
ToInsertFunc=set()
CheckPointFunc=set()
visited=set()
indirect_func_target_map={}
## Generate indirect calls based on the final result of SVF. Previously combined with meta_data and SVF to generate a more accurate result.
def initialize_map(indirect_target_map_file):
    global indirect_func_target_map
    IRCallsite_pattern=re.compile(r"In function\s*:\s*([^\s]+)\s*indirect callsite\s*:\s*(.*)")
    target_pattern=re.compile(r'--target\s*(\S+)')
    IndirectCallsiteFunc=None
    with open(indirect_target_map_file,'r') as file:
        for line  in file:
            line= line.strip()
            IRcallsite_match=IRCallsite_pattern.match(line)
            target_match=target_pattern.match(line)
            if IRcallsite_match:
                IndirectCallsiteFunc=IRcallsite_match.group(1)
                if IndirectCallsiteFunc not in indirect_func_target_map:
                    indirect_func_target_map[IndirectCallsiteFunc]=set()
            elif target_match:
                if target_match:
                    target_function=target_match.group(1)
                    indirect_func_target_map[IndirectCallsiteFunc].add(target_function)
# def filt_wrong_target_in_gragh(graph):
#     global indirect_func_target_map
#     for node in list(graph.nodes):
#         match=re.search(r'fun: ([\w\d_]+)',graph.nodes[node]['label'])
#         callsite_function_name=match.group(1)
#         edges=graph.out_edges(node)
#         for edge in edges:
#             edge_data=graph.get_edge_data(*edge)
#             if edge_data.get(0)["color"]=="red":
#                 u,v=edge              
#                 match2=re.search(r'fun: ([\w\d_]+)',graph.nodes[v]['label'])
#                 target_func_name=match2.group(1)
#                 if target_func_name not in indirect_func_target_map[callsite_function_name]:
#                     graph.remove_edge(edge)
#                     ## No need to worry about whether the deleted one is a direct or indirect call, because if all are indirect calls, they will all be deleted,
#                     # # If one is an indirect call and the other is a direct call, only one will be deleted. Ultimately, if one is preserved, it will still be a direct call.
def judge_in_valid_edge(callsite,callee):
    global indirect_func_target_map
    if callsite in indirect_func_target_map:
        if callee in indirect_func_target_map[callsite]:
            return True
    return False
def find_corresponding_node(callee_name,graph): # Find the function node corresponding to the current callee
    for node in list(graph.nodes):
        match=re.search(r'fun: ([\w\d_]+)',graph.nodes[node]['label'])
        function_name=match.group(1)
        if function_name==callee_name:
            return node
    return None
def find_corresponding_node_in_D(callee_name,graph):
    for node in list(graph.nodes):
        function_name=graph.nodes[node]['label']
        if function_name==callee_name:
            return node
    return None
def custom_dfs(graph, node):
    global D
    global AvoidFuncSet
    global indirect_func_target_map
    global visited
    if visited is None:
        visited = set()  # Initialize the set of visited nodes
    if node not in visited:
        visited.add(node)  # Mark as visited
        #print(node)  # Process node (printing here)
        # Recursively visit all adjacent unvisited nodes
        match=re.search(r'fun: ([\w\d_]+)',graph.nodes[node]['label'])
        callsite_name=match.group(1)
        ## Add edges based on final_result
        if not D.has_node(node):
            D.add_node(node,label=callsite_name)
        if callsite_name  in indirect_func_target_map:
            for target in indirect_func_target_map[callsite_name]:
                node_current=find_corresponding_node(target,graph)
                if node_current !=None:
                    if not D.has_node(node_current):
                        D.add_node(node_current,label=target)
                    if not D.has_edge(node,node_current):
                        D.add_edge(node,node_current,color="red")        
                if node_current !=None:
                    if not graph.has_edge(node,node_current):
                        graph.add_edge(node,node_current,color="red")
        successors = list(graph.successors(node))
        for neighbor in successors:
            edge_data=graph.get_edge_data(node,neighbor)
            print(edge_data)
            match2=re.search(r'fun: ([\w\d_]+)',graph.nodes[neighbor]['label'])
            callee_name= match2.group(1)
            if callee_name.lower().startswith("llvm"):
                continue
            if callee_name in AvoidFuncSet:
                continue
            if edge_data is not None:
                edge_color = edge_data.get(0)['color']
                if edge_color=="black":
                    if not D.has_node(neighbor):
                        D.add_node(neighbor,label=callee_name)
                    if not D.has_edge(node,neighbor):
                        D.add_edge(node,neighbor,color="black")
                # if edge_color=="red": ## Determine if the edge appears in indirect_func_target_map, requiring identification of the calling function and then the called function
                #     if judge_in_valid_edge(callsite_name,callee_name)==True:
                #         if not D.has_node(node):
                #             D.add_node(node,label=callee_name)
                #         if not D.has_edge(node,neighbor):
                #             D.add_edge(node,neighbor,color="red")
            custom_dfs(graph, neighbor)

def dfs_visit(graph,node):
    global AvoidFuncSet
    global ToInsertFunc
    global visited
    if visited is None:
        visited = set()  # Initialize the set of visited nodes
    if node not in visited:
        visited.add(node)
        successors = list(graph.successors(node))
        for neighbor in successors:
            function_name=graph.nodes[neighbor]["label"]
            if function_name.lower().startswith("llvm"):
                continue
            if function_name not in AvoidFuncSet:
                ToInsertFunc.add(function_name)
                dfs_visit(graph,neighbor)
def analysis_dot(mainjob_file_path): 
    global AvoidFuncSet
    global ToInsertFunc
    global D
    # Read .dot file
    #dot_file_path = 'path/to/your/graph.dot'  # Replace with your .dot file path
    graph = D
    print("Number of nodes in the graph:", graph.number_of_nodes())
    print("Number of edges in the graph:", graph.number_of_edges())
    with open(mainjob_file_path,'r')as file:
        for line  in file :
            start_node_name=line.strip()
            #print("Node list:", list(graph.nodes))
            #start_node="Node0x560e91943230" ## For testing purposes only
            start_node=find_corresponding_node_in_D(start_node_name,graph)
            dfs_visit(graph,start_node)


                                            


            #print(graph.nodes[node]['label'])
def init_set(avoid_handle_function):
    global AvoidFuncSet
    with open(avoid_handle_function,'r') as f:
        for avoid_line in f:
            avoid_func_name=avoid_line.strip()
            AvoidFuncSet.add(avoid_func_name)
    
def write_file(output_file,funclist):
    with open(output_file,'w') as file:
        for func in funclist:
            file.write(func+'\n')
    file.close()
            
def main():
    global ToInsertFunc
    global CheckPointFunc
    global D
    global visited
    dot_file_path="~/ARTO/pre-analysis16.0/svf_analysis_result/supa-arducopter16_devirt_svf_patched_3_type_match.dot"

    mainjob_file_path="~/ARTO/ardupilot/path_start.txt"
    avoid_handle_function="~/ARTO/ardupilot/build/SITL_arm_linux_gnueabihf/avoid_handle_function.txt"
    # indirect_target_map_file="../SITL_modify_ander.txt"
    ToInsertFuncFile="../output/ToInsertFunc.txt"
    CheckpointFuncfile="../output/chekckpointfunc.txt"
    final_file="../output/static_final_result.txt"
    test_file="../output/test_valid.txt"
    graph = nx.nx_agraph.read_dot(dot_file_path)
    # initialize_map(indirect_target_map_file)
    init_set(avoid_handle_function)
    initialize_map(final_file)
    with open(test_file,'w') as file:
        for callsite,targets in indirect_func_target_map.items():
            functionName=callsite
            file.write("In function :"+functionName+'\n')
            for target in targets:
                file.write("--target  "+target+'\n')
    file.close()
    with open(mainjob_file_path,"r") as file:
        for line in file:
            start_node_name=line.strip()
            #print("Node list:", list(graph.nodes))
            #start_node="Node0x560e91943230" ## For testing purposes only
            for node in list(graph.nodes): ## Look for the corresponding node
                match=re.search(r'fun: ([\w\d_]+)',graph.nodes[node]['label'])
                function_name=match.group(1)
                if function_name==start_node_name:
                    start_node=node
                    custom_dfs(graph,start_node)
                    continue
    file.close()
    nx.nx_agraph.write_dot(D,"../output/final_sub_call_graph.dot")
    visited=set()
    analysis_dot(mainjob_file_path)
    write_file(ToInsertFuncFile,ToInsertFunc)
    # write_file(CheckpointFuncfile,CheckPointFunc)


    
if __name__ == '__main__':
    main()