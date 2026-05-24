
import networkx as nx
import re
recursive_func=set()
checkpoint_func=set()
def find_corresponding_node_in_D(callee_name,graph):
    for node in list(graph.nodes):
        function_name=graph.nodes[node]['label']
        if function_name==callee_name:
            return node
    return None
def main():
    global recursive_func
    dot_file_path="../output/final_sub_call_graph.dot"

    graph = nx.nx_agraph.read_dot(dot_file_path)
    start_node_name="_ZN6Copter18update_flight_modeEv"
    end_node_name="_ZN7HALSITL9Scheduler13_run_io_procsEv"
    # start_node=find_corresponding_node_in_D(start_node_name,graph)
    # end_node=find_corresponding_node_in_D(end_node_name,graph)
    # paths=list(nx.all_simple_paths(graph,source=start_node_name,target=end_node_name))
    # print(paths)
    # path = nx.shortest_path(graph,source=start_node,target=end_node)
    # for node in path:
    #     print(graph.nodes[node]['label'])
    # path = nx.shortest_path(graph,source=start_node_name,target=end_node_name)
    # for node in path:
    #     print(node)
    cycles =nx.simple_cycles(graph)

    #print(len(cycles))
    for cycle in cycles:
        print("The graph has cycles.")
    #     # if(len(cycle)==1):
    #     #     recursive_func.add(cycle[0])
    #     # if(len(cycle)>=2):
    #     #     checkpoint_func.add(cycle[0])
    #     # print(cycle[0])
        for node in cycle:
            print(node)
            # print(graph.nodes[node]["label"])
    # for func in checkpoint_func:
    #     print(func)


if __name__ == '__main__':
    main()