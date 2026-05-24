#!/usr/bin/env python
# -*- coding: utf-8 -*-
"""
主程序：集成 hash 文件解析、Sequitur 压缩和二进制规则打包
"""

import argparse
import logging
import sys
from pathlib import Path

import gzip
import shutil

from parseData import (
    parse_infile,
    parse_branch_paths,
    serialize_path_dictionary,
    serialize_hash_dictionary,
    build_hash_dict,
    dedup_consecutive,
    compress_with_sequitur,
)
from rules_packing import RulesPacker


def setup_logging():
    """配置日志"""
    logging.basicConfig(
        level=logging.INFO,
        format='%(asctime)s - %(levelname)s - %(message)s'
    )


def analyze_grammar(grammar):
    """
    分析 Grammar 对象结构，输出统计信息
    
    Args:
        grammar: sksequitur Grammar 对象
    """
    if not grammar:
        logging.error("Grammar is None or empty")
        return
    
    logging.info("=== Grammar Structure Analysis ===")
    logging.info(f"Total rules: {len(grammar)}")
    
    total_elements = 0
    total_nonterminals = 0
    
    for prod_id, elements in sorted(grammar.items()):
        elements_list = list(elements) if not isinstance(elements, list) else elements
        total_elements += len(elements_list)
        
        nonterminal_count = sum(1 for e in elements_list if isinstance(e, type(prod_id)))
        total_nonterminals += nonterminal_count
        
        # 打印前几条规则作为样本
        if int(prod_id) < 5:
            elem_types = [
                f"NT(P{int(e)})" if isinstance(e, type(prod_id)) else f"T({e})"
                for e in elements_list[:5]
            ]
            suffix = "..." if len(elements_list) > 5 else ""
            logging.info(
                f"  Rule {prod_id}: [{', '.join(elem_types)}{suffix}] "
                f"(len={len(elements_list)}, {nonterminal_count} nonterminals)"
            )
    
    logging.info(f"Total elements: {total_elements}")
    logging.info(f"Total nonterminals: {total_nonterminals}")
    logging.info(f"Compression ratio: {total_elements / len(grammar):.2f} elements/rule")


def main():
    """主流程"""
    setup_logging()
    
    parser = argparse.ArgumentParser(description="Trace compression and binary packing using Sequitur")
    parser.add_argument("--input", default="hash_value.txt",
                        help="Input trace file (default: hash_value.txt)")
    parser.add_argument("--output", default="rules.bin",
                        help="Output packed binary file (default: rules.bin)")
    parser.add_argument("--mode", default="hash", choices=["hash", "branch"],
                        help="Input format: 'hash' for uint64 hash trace (default), "
                             "'branch' for raw byte branch trace (0/1/2)")
    parser.add_argument("--branch-abstraction", default="dict",
                        choices=["xxhash", "dict", "rle-dict", "window-dict"],
                        help="Path abstraction for --mode branch: xxhash, dict, "
                             "rle-dict, or window-dict (default: dict)")
    parser.add_argument("--branch-window-size", type=int, default=256,
                        help="Branch decisions per sub-path for "
                             "--branch-abstraction window-dict (default: 256)")
    parser.add_argument("--dedup", action="store_true", default=False,
                        help="Merge consecutive identical path IDs before "
                             "Sequitur compression (reduces sequence length)")
    parser.add_argument("--gzip", action="store_true", default=False,
                        help="Apply gzip post-compression to the output binary")
    parser.add_argument("--pack", default="u64", choices=["u64", "compact32"],
                        help="Grammar element packing format: u64 or compact32 "
                             "(default: u64)")
    args = parser.parse_args()
    if args.branch_window_size < 1:
        logging.error("--branch-window-size must be at least 1")
        return 1

    # 配置路径
    workspace = Path(__file__).parent
    input_file = workspace / args.input
    rules_output = workspace / args.output

    logging.info(f"Workspace: {workspace}")
    logging.info(f"Mode: {args.mode}")
    logging.info(f"Packing: {args.pack}")

    # Step 1: 解析输入文件
    logging.info("Step 1: Parsing input file...")
    if not input_file.exists():
        logging.error(f"Input file not found: {input_file}")
        return 1

    if args.mode == "branch":
        trace_data, branch_stats, path_dict = parse_branch_paths(
            str(input_file),
            abstraction=args.branch_abstraction,
            window_size=args.branch_window_size,
        )
        elem_size = 8  # 每个 path ID 为 xxh64 uint64
        raw_input_size = input_file.stat().st_size
        logging.info(
            f"Parsed {branch_stats['branch_decisions']:,} branch decisions "
            f"from {input_file.name}"
        )
        logging.info(
            f"Operation boundaries (value=2): {branch_stats['boundaries']:,}"
        )
        logging.info(
            f"Path abstraction with {args.branch_abstraction}: "
            f"{branch_stats['paths']:,} path IDs, "
            f"{branch_stats['unique_paths']:,} unique"
        )
        if args.branch_abstraction == "window-dict":
            logging.info(
                f"Window size: {args.branch_window_size} branch decisions"
            )
        if branch_stats["ignored_bytes"]:
            logging.info(f"Ignored bytes: {branch_stats['ignored_bytes']:,}")
        if trace_data:
            logging.info(f"First 5 path IDs: {trace_data[:5]}")
    else:
        trace_data = parse_infile(str(input_file))
        elem_size = 8  # 每个元素 8 字节 (uint64)
        raw_input_size = len(trace_data) * elem_size
        logging.info(f"Parsed {len(trace_data)} hash elements from {input_file.name}")
        if trace_data:
            logging.info(f"First 5 elements: {trace_data[:5]}")
            logging.info(f"Sample range: [{min(trace_data)}, {max(trace_data)}]")

    # Optional dedup: merge consecutive identical path IDs
    if args.dedup and trace_data:
        orig_len = len(trace_data)
        trace_data = dedup_consecutive(trace_data)
        saved = orig_len - len(trace_data)
        if saved:
            logging.info(f"Dedup removed {saved:,} consecutive duplicates "
                         f"({orig_len:,} → {len(trace_data):,}, "
                         f"{saved/orig_len*100:.1f}% reduction)")

    # Optional hash dict mapping for compact32 packing
    hash_dict = {}
    if args.pack == "compact32":
        logging.info("\nStep 1.5: Building hash dictionary for compact32...")
        hash_dict = None
        if args.mode == "branch" and args.branch_abstraction in ("dict", "rle-dict", "window-dict"):
            # path_dict already maps paths to small IDs — reuse it, no remap needed
            if all(v < 2**31 for v in trace_data):
                logging.info("Path IDs already fit compact32 range — skipping remap")
            else:
                logging.warning("Path IDs exceed 31-bit range, compact32 may fail")
        else:
            trace_data, hash_dict = build_hash_dict(trace_data)
            logging.info(
                f"Mapped {len(hash_dict):,} unique hashes → [0..{len(hash_dict)-1}] "
                f"for compact32 packing"
            )
            if trace_data:
                logging.info(f"First 5 mapped: {trace_data[:5]}")

    # Step 2: 使用 Sequitur 压缩
    logging.info("\nStep 2: Compressing with Sequitur...")
    grammar = compress_with_sequitur(trace_data)
    
    if grammar is None:
        logging.error("Sequitur compression failed")
        return 1
    
    # 分析 Grammar 结构
    analyze_grammar(grammar)
    
    # Step 3: 序列化规则为二进制
    logging.info("\nStep 3: Serializing rules to binary...")
    try:
        if args.pack == "compact32":
            RulesPacker.serialize_grammar_compact32(
                grammar,
                str(rules_output),
                len(trace_data),
            )
        else:
            RulesPacker.serialize_grammar(grammar, str(rules_output), len(trace_data))
        logging.info(f"Rules serialized to: {rules_output}")
    except Exception as e:
        logging.error(f"Serialization failed: {e}")
        return 1

    path_dict_output = None
    if args.mode == "branch" and args.branch_abstraction in ("dict", "rle-dict", "window-dict"):
        dict_path = rules_output.with_suffix(rules_output.suffix + ".paths.bin")
        try:
            path_dict_output = serialize_path_dictionary(
                path_dict,
                str(dict_path),
                args.branch_abstraction,
            )
            if path_dict_output:
                logging.info(f"Path dictionary serialized to: {path_dict_output}")
        except Exception as e:
            logging.error(f"Path dictionary serialization failed: {e}")
            return 1

    hash_dict_output = None
    if hash_dict:
        hash_dict_path = rules_output.with_suffix(rules_output.suffix + ".hashdict.bin")
        try:
            hash_dict_output = serialize_hash_dictionary(hash_dict, str(hash_dict_path))
            logging.info(f"Hash dictionary serialized to: {hash_dict_output}")
        except Exception as e:
            logging.error(f"Hash dictionary serialization failed: {e}")
            return 1

    # Optional gzip post-compression
    if args.gzip:
        gz_path = Path(str(rules_output) + ".gz")
        try:
            with open(rules_output, "rb") as f_in:
                with gzip.open(gz_path, "wb") as f_out:
                    shutil.copyfileobj(f_in, f_out)
            logging.info(f"Gzip compressed to: {gz_path}")
        except Exception as e:
            logging.error(f"Gzip compression failed: {e}")
            return 1
        if path_dict_output:
            dict_gz_path = Path(str(path_dict_output) + ".gz")
            try:
                with open(path_dict_output, "rb") as f_in:
                    with gzip.open(dict_gz_path, "wb") as f_out:
                        shutil.copyfileobj(f_in, f_out)
                logging.info(f"Path dictionary gzip compressed to: {dict_gz_path}")
            except Exception as e:
                logging.error(f"Path dictionary gzip compression failed: {e}")
                return 1
        if hash_dict_output:
            hd_gz_path = Path(str(hash_dict_output) + ".gz")
            try:
                with open(hash_dict_output, "rb") as f_in:
                    with gzip.open(hd_gz_path, "wb") as f_out:
                        shutil.copyfileobj(f_in, f_out)
                logging.info(f"Hash dictionary gzip compressed to: {hd_gz_path}")
            except Exception as e:
                logging.error(f"Hash dictionary gzip compression failed: {e}")
                return 1

    # Step 4: 验证 - 反序列化
    logging.info("\nStep 4: Verifying deserialization...")
    try:
        restored_grammar = RulesPacker.deserialize_grammar(str(rules_output))
        
        if len(restored_grammar) != len(grammar):
            logging.error(f"Rule count mismatch: {len(restored_grammar)} != {len(grammar)}")
            return 1
        
        logging.info("✓ Deserialization successful")
        logging.info(f"✓ Restored {len(restored_grammar)} rules")
        
    except Exception as e:
        logging.error(f"Deserialization failed: {e}")
        return 1
    
    # 文件大小统计
    logging.info("\n=== Compression Statistics ===")
    original_size = len(trace_data) * elem_size
    compressed_size = rules_output.stat().st_size
    dict_size = Path(path_dict_output).stat().st_size if path_dict_output else 0
    hash_dict_size = Path(hash_dict_output).stat().st_size if hash_dict_output else 0
    total_compressed_size = compressed_size + dict_size + hash_dict_size
    if args.mode == "branch":
        logging.info(f"Raw branch log size: {raw_input_size:,} bytes")
        logging.info(f"Path-id sequence size: {original_size:,} bytes")
        if dict_size:
            logging.info(f"Path dictionary size: {dict_size:,} bytes")
        if hash_dict_size:
            logging.info(f"Hash dictionary size: {hash_dict_size:,} bytes")
    else:
        logging.info(f"Original size: {original_size:,} bytes")
        if hash_dict_size:
            logging.info(f"Hash dictionary size: {hash_dict_size:,} bytes")
    logging.info(f"Compressed size: {compressed_size:,} bytes")
    if args.pack == "compact32":
        logging.info(f"Compact32 packing: 4B per element (was 8B)")
    if dict_size or hash_dict_size:
        logging.info(f"Compressed size + dictionaries: {total_compressed_size:,} bytes")
    if original_size > 0 and args.mode == "branch":
        logging.info(f"Compression ratio vs path sequence: {compressed_size / original_size * 100:.2f}%")
        logging.info(f"Space saved vs path sequence: {original_size - compressed_size:,} bytes ({(1 - compressed_size/original_size)*100:.2f}%)")
    elif original_size > 0:
        logging.info(f"Compression ratio: {compressed_size / original_size * 100:.2f}%")
        logging.info(f"Space saved: {original_size - compressed_size:,} bytes ({(1 - compressed_size/original_size)*100:.2f}%)")
    if args.mode == "branch" and raw_input_size > 0:
        logging.info(f"Compression ratio vs raw branch log: {compressed_size / raw_input_size * 100:.4f}%")
        logging.info(f"Space saved vs raw branch log: {raw_input_size - compressed_size:,} bytes ({(1 - compressed_size/raw_input_size)*100:.2f}%)")
        if dict_size:
            logging.info(f"Total ratio vs raw branch log: {total_compressed_size / raw_input_size * 100:.4f}%")
            logging.info(f"Total space saved vs raw branch log: {raw_input_size - total_compressed_size:,} bytes ({(1 - total_compressed_size/raw_input_size)*100:.2f}%)")

    # Gzip statistics
    if args.gzip:
        gz_path = Path(str(rules_output) + ".gz")
        if gz_path.exists():
            gz_size = gz_path.stat().st_size
            logging.info(f"Gzip compressed size: {gz_size:,} bytes")
            logging.info(f"Gzip compression ratio vs packed: {gz_size / compressed_size * 100:.2f}%")
            logging.info(f"Overall ratio vs path sequence: {gz_size / original_size * 100:.2f}%")
            if args.mode == "branch" and raw_input_size > 0:
                logging.info(f"Overall ratio vs raw branch log: {gz_size / raw_input_size * 100:.4f}%")
            total_gz = gz_size
            if path_dict_output:
                dict_gz_path = Path(str(path_dict_output) + ".gz")
                if dict_gz_path.exists():
                    dict_gz_size = dict_gz_path.stat().st_size
                    total_gz += dict_gz_size
                    logging.info(f"Path dictionary gzip size: {dict_gz_size:,} bytes")
            if hash_dict_output:
                hd_gz_path = Path(str(hash_dict_output) + ".gz")
                if hd_gz_path.exists():
                    hd_gz_size = hd_gz_path.stat().st_size
                    total_gz += hd_gz_size
                    logging.info(f"Hash dictionary gzip size: {hd_gz_size:,} bytes")
            if total_gz > gz_size:
                logging.info(f"Total with gzip: {total_gz:,} bytes")
                if args.mode == "branch" and raw_input_size > 0:
                    logging.info(f"Total gzip ratio vs raw branch log: {total_gz / raw_input_size * 100:.4f}%")

    logging.info("\n✓ All steps completed successfully!")
    return 0


if __name__ == '__main__':
    sys.exit(main())
