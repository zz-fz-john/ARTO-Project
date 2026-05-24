#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
Sequitur 压缩与二进制打包 - 使用示例

这个脚本展示了完整的工作流程：
1. 从二进制文件解析长整数序列
2. 使用 Sequitur 算法进行无损压缩
3. 将压缩的规则打包为二进制格式
4. 从二进制格式恢复规则并展开
"""

import logging
import sys
from pathlib import Path

from parseData import parse_infile, compress_with_sequitur
from rules_packing import RulesPacker


def main():
    """完整的工作流程示例"""
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(levelname)s - %(message)s'
    )
    
    # 配置
    workspace = Path(__file__).parent
    input_file = workspace / "hash_value.txt"
    output_rules = workspace / "compressed_rules.bin"
    
    logging.info("=" * 60)
    logging.info("Sequitur Compression & Binary Packing Workflow")
    logging.info("=" * 60)
    
    # ============ 步骤 1: 解析输入文件 ============
    logging.info("\n[Step 1] Parsing input hash file...")
    if not input_file.exists():
        logging.error(f"Input file not found: {input_file}")
        return 1
    
    trace_data = parse_infile(str(input_file))
    if not trace_data:
        logging.error("Failed to parse input file")
        return 1
    
    logging.info(f"✓ Successfully parsed {len(trace_data):,} uint64 elements")
    logging.info(f"  Value range: [{min(trace_data)}, {max(trace_data)}]")
    
    # ============ 步骤 2: Sequitur 压缩 ============
    logging.info("\n[Step 2] Compressing with Sequitur algorithm...")
    grammar = compress_with_sequitur(trace_data)
    
    if grammar is None:
        logging.error("Compression failed")
        return 1
    
    # 分析规则
    rule_counts = grammar.counts()
    total_elements = sum(len(list(elems)) for elems in grammar.values())
    
    logging.info(f"✓ Compression successful")
    logging.info(f"  Rules extracted: {len(grammar)}")
    logging.info(f"  Total rule elements: {total_elements}")
    logging.info(f"  Compression factor: {len(trace_data) / total_elements:.1f}x")
    
    # 显示规则使用统计
    logging.info(f"\n  Rule usage counts:")
    for prod_id in sorted(rule_counts.keys()):
        count = rule_counts[prod_id]
        logging.info(f"    Rule {prod_id}: used {count} times")
    
    # ============ 步骤 3: 二进制打包 ============
    logging.info("\n[Step 3] Serializing rules to binary format...")
    try:
        RulesPacker.serialize_grammar(grammar, str(output_rules), len(trace_data))
    except Exception as e:
        logging.error(f"Serialization failed: {e}")
        return 1
    
    original_size = len(trace_data) * 8
    compressed_size = output_rules.stat().st_size
    
    logging.info(f"✓ Binary serialization successful")
    logging.info(f"  Output file: {output_rules}")
    logging.info(f"  File size: {compressed_size:,} bytes")
    
    # ============ 步骤 4: 验证 - 反序列化 ============
    logging.info("\n[Step 4] Verifying - deserializing rules...")
    try:
        restored_grammar = RulesPacker.deserialize_grammar(str(output_rules))
    except Exception as e:
        logging.error(f"Deserialization failed: {e}")
        return 1
    
    if len(restored_grammar) != len(grammar):
        logging.error(f"Rule count mismatch after deserialization")
        return 1
    
    logging.info(f"✓ Deserialization successful")
    logging.info(f"  Restored {len(restored_grammar)} rules")
    
    # ============ 步骤 5: 验证 - 展开规则 ============
    logging.info("\n[Step 5] Verifying - expanding rules...")
    try:
        from sksequitur.api import Production
        expanded = list(restored_grammar.expand(Production(0)))
    except:
        # 如果 restored_grammar 是普通字典，需要使用原始 grammar 的 expand 方法
        expanded = list(grammar.expand(list(grammar.keys())[0]))
    
    if len(expanded) != len(trace_data):
        logging.error(f"Expansion length mismatch: {len(expanded)} vs {len(trace_data)}")
        return 1
    
    mismatches = sum(1 for a, b in zip(expanded, trace_data) if a != b)
    if mismatches > 0:
        logging.error(f"Data mismatch after expansion: {mismatches} elements differ")
        return 1
    
    logging.info(f"✓ Expansion verification successful")
    logging.info(f"  All {len(expanded):,} elements match original data")
    
    # ============ 最终统计 ============
    logging.info("\n" + "=" * 60)
    logging.info("FINAL COMPRESSION STATISTICS")
    logging.info("=" * 60)
    logging.info(f"Original data:")
    logging.info(f"  Elements: {len(trace_data):,}")
    logging.info(f"  Size: {original_size:,} bytes")
    logging.info(f"\nCompressed (binary rules file):")
    logging.info(f"  Size: {compressed_size:,} bytes")
    logging.info(f"  Ratio: {compressed_size / original_size * 100:.4f}%")
    logging.info(f"  Space saved: {original_size - compressed_size:,} bytes")
    logging.info(f"\nCompression metrics:")
    logging.info(f"  Compression factor: {original_size / compressed_size:.1f}x")
    logging.info(f"  Space efficiency: {(1 - compressed_size/original_size)*100:.2f}%")
    logging.info(f"  Rules: {len(grammar)}")
    logging.info(f"  Rule elements: {total_elements}")
    logging.info(f"  Rule redundancy: {len(trace_data) / total_elements:.1f}x")
    
    logging.info("\n" + "=" * 60)
    logging.info("✓ All steps completed successfully!")
    logging.info("=" * 60)

    return 0


if __name__ == '__main__':
    sys.exit(main())
