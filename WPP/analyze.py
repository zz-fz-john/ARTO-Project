#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
详细分析脚本：探索 Sequitur Grammar 结构，验证规则展开
"""

import logging
import sys
from pathlib import Path
from parseData import parse_infile, compress_with_sequitur
from rules_packing import RulesPacker


def setup_logging():
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(levelname)s - %(message)s'
    )


def verify_compression(original_data, grammar):
    """
    验证压缩的有效性：展开起始规则是否恢复原始数据
    
    Args:
        original_data: 原始的 uint64 序列
        grammar: Sequitur 生成的 Grammar 对象
        
    Returns:
        (是否完全匹配, 匹配元素数, 总元素数)
    """
    if not grammar:
        return False, 0, 0
    
    # 使用 Grammar 的 expand() 方法展开起始规则
    try:
        from sksequitur.api import Production
        expanded = list(grammar.expand(Production(0)))
    except Exception as e:
        logging.error(f"Expansion failed: {e}")
        return False, 0, 0
    
    if len(expanded) != len(original_data):
        logging.warning(f"Length mismatch: {len(expanded)} vs {len(original_data)}")
        return False, 0, len(original_data)
    
    matched = sum(1 for a, b in zip(expanded, original_data) if a == b)
    
    return matched == len(original_data), matched, len(original_data)


def analyze_grammar_detailed(grammar, original_data):
    """
    详细分析 Grammar 对象
    """
    logging.info("=== Detailed Grammar Analysis ===")
    
    total_rules = len(grammar)
    logging.info(f"Total rules: {total_rules}")
    
    # 分析每条规则
    total_elements = 0
    terminal_count = 0
    nonterminal_count = 0
    rule_sizes = []
    
    for prod_id, elements in sorted(grammar.items()):
        elements_list = list(elements) if not isinstance(elements, list) else elements
        rule_sizes.append(len(elements_list))
        total_elements += len(elements_list)
        
        for elem in elements_list:
            elem_type_name = type(elem).__name__
            if elem_type_name == "Production" or (isinstance(elem, int) and elem_type_name != "int"):
                nonterminal_count += 1
            else:
                terminal_count += 1
    
    logging.info(f"Total elements in rules: {total_elements}")
    logging.info(f"  - Terminal symbols: {terminal_count}")
    logging.info(f"  - Nonterminal symbols (Production refs): {nonterminal_count}")
    logging.info(f"Average rule size: {total_elements / total_rules:.2f} elements/rule")
    logging.info(f"Max rule size: {max(rule_sizes)} elements")
    logging.info(f"Min rule size: {min(rule_sizes)} elements")
    
    # 打印详细规则信息
    logging.info("\n=== Rule Details ===")
    for prod_id, elements in sorted(grammar.items()):
        elements_list = list(elements) if not isinstance(elements, list) else elements
        
        # 分析此规则的元素
        elem_summary = []
        for i, elem in enumerate(elements_list):
            elem_type_name = type(elem).__name__
            if elem_type_name == "Production" or (isinstance(elem, int) and elem_type_name != "int"):
                elem_summary.append(f"P{int(elem)}")
            else:
                elem_summary.append(f"T({int(elem)})")
        
        # 显示摘要
        if len(elem_summary) <= 10:
            elem_str = " ".join(elem_summary)
        else:
            elem_str = " ".join(elem_summary[:7]) + f" ... (+{len(elem_summary)-7})"
        
        logging.info(f"Rule {int(prod_id):2d}: {elem_str} (len={len(elements_list)})")
    
    return total_elements, terminal_count, nonterminal_count


def main():
    setup_logging()
    
    workspace = Path(__file__).parent
    hash_file = workspace / "hash_value.txt"
    rules_output = workspace / "rules_detailed.bin"
    
    logging.info(f"Workspace: {workspace}\n")
    
    # Step 1: 解析
    logging.info("Step 1: Parsing hash file...")
    if not hash_file.exists():
        logging.error(f"Hash file not found: {hash_file}")
        return 1
    
    original_data = parse_infile(str(hash_file))
    logging.info(f"✓ Parsed {len(original_data):,} elements")
    logging.info(f"  Value range: [{min(original_data)}, {max(original_data)}]")
    logging.info(f"  Sample: {original_data[:10]}...\n")
    
    # Step 2: Sequitur 压缩
    logging.info("Step 2: Compressing with Sequitur...")
    grammar = compress_with_sequitur(original_data)
    
    if grammar is None:
        logging.error("Compression failed")
        return 1
    
    logging.info(f"✓ Compression completed\n")
    
    # Step 3: 详细分析
    total_elements, terminal_count, nonterminal_count = analyze_grammar_detailed(grammar, original_data)
    
    # Step 4: 验证压缩
    logging.info("\n=== Compression Verification ===")
    matches, matched_count, total_count = verify_compression(original_data, grammar)
    
    if matches:
        logging.info(f"✓ Expansion verification PASSED")
        logging.info(f"  Expanded all {total_count:,} elements correctly")
    else:
        logging.warning(f"✗ Expansion verification FAILED")
        logging.warning(f"  Matched {matched_count}/{total_count} elements")
    
    # Step 5: 二进制序列化
    logging.info("\n=== Binary Serialization ===")
    try:
        RulesPacker.serialize_grammar(grammar, str(rules_output), len(original_data))
        file_size = rules_output.stat().st_size
        logging.info(f"✓ Serialized to {rules_output}")
        logging.info(f"  File size: {file_size:,} bytes")
    except Exception as e:
        logging.error(f"Serialization failed: {e}")
        return 1
    
    # Step 6: 验证反序列化
    logging.info("\n=== Deserialization Verification ===")
    try:
        restored_grammar = RulesPacker.deserialize_grammar(str(rules_output))
        
        if len(restored_grammar) == len(grammar):
            logging.info(f"✓ Deserialization successful")
            logging.info(f"  Restored {len(restored_grammar)} rules")
        else:
            logging.warning(f"✗ Rule count mismatch: {len(restored_grammar)} vs {len(grammar)}")
            return 1
            
    except Exception as e:
        logging.error(f"Deserialization failed: {e}")
        return 1
    
    # 最终统计
    logging.info("\n=== Final Statistics ===")
    original_size = len(original_data) * 8
    binary_size = rules_output.stat().st_size
    
    logging.info(f"Original size: {original_size:,} bytes ({len(original_data):,} uint64 values)")
    logging.info(f"Compressed size: {binary_size:,} bytes")
    logging.info(f"Compression ratio: {binary_size / original_size * 100:.4f}%")
    logging.info(f"Space saved: {original_size - binary_size:,} bytes ({(1 - binary_size/original_size)*100:.2f}%)")
    logging.info(f"Compression factor: {original_size / binary_size:.1f}x")
    
    logging.info(f"\n✓ Analysis complete!")
    
    return 0


if __name__ == '__main__':
    sys.exit(main())
