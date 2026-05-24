import logging
import struct
from sksequitur import parse as sequitur_parse
import xxhash


def parse_infile(infile_path):
    """
    解析 prover 侧生成的 hash-trace 文件 (Binary Format)
    每个元素为 8 字节 uint64，小端序。
    """
    trace_data = []
    try:
        with open(infile_path, 'rb') as f:
            while True:
                # Read 8 bytes (uint64)
                chunk = f.read(8)
                if not chunk:
                    break
                if len(chunk) < 8:
                    logging.warning(f"Incomplete chunk of {len(chunk)} bytes at end of file")
                    break

                # Unpack as unsigned long long (8 bytes)
                # Assuming little-endian ('<Q') as is common on ARM/x86
                val = struct.unpack('<Q', chunk)[0]
                trace_data.append(val)

        return trace_data
    except Exception as e:
        logging.error(f"Error parsing infile: {e}")
        return []


def parse_branch_trace(infile_path):
    """
    解析 branch_trace 文件 (Raw Byte Format)
    每个字节为一个元素，取值范围: 0(not taken), 1(taken), 2(operation start marker)

    Args:
        infile_path: 输入文件路径

    Returns:
        trace_data: int 类型列表，每个元素取值 0/1/2
    """
    trace_data = []
    try:
        with open(infile_path, 'rb') as f:
            data = f.read()
            trace_data = [b for b in data]  # 每个 byte 转为 int
        return trace_data
    except Exception as e:
        logging.error(f"Error parsing branch trace: {e}")
        return []


def parse_branch_paths_xxhash(infile_path):
    """
    Parse a raw branch trace into a WPP-style path-id sequence.

    Input bytes:
      0x00: branch not taken
      0x01: branch taken
      0x02: event/path boundary marker

    For each segment between boundary markers, compute xxh64 over the raw
    0/1 bytes and emit the 64-bit digest as the path ID. Empty segments are
    ignored.

    Returns:
        (path_ids, stats)
    """
    path_ids = []
    boundary_count = 0
    decision_count = 0
    ignored_count = 0
    current_len = 0
    hasher = xxhash.xxh64(seed=0)

    def finish_segment():
        nonlocal current_len, hasher
        if current_len == 0:
            return
        # RulesPacker reserves the high bit for nonterminals, so keep path IDs
        # in the 63-bit terminal range.
        path_ids.append(hasher.intdigest() & 0x7FFFFFFFFFFFFFFF)
        hasher = xxhash.xxh64(seed=0)
        current_len = 0

    try:
        with open(infile_path, 'rb') as f:
            while True:
                chunk = f.read(1024 * 1024)
                if not chunk:
                    break
                for b in chunk:
                    if b == 0 or b == 1:
                        hasher.update(bytes((b,)))
                        current_len += 1
                        decision_count += 1
                    elif b == 2:
                        boundary_count += 1
                        finish_segment()
                    else:
                        ignored_count += 1

        finish_segment()

        stats = {
            "branch_decisions": decision_count,
            "boundaries": boundary_count,
            "paths": len(path_ids),
            "unique_paths": len(set(path_ids)),
            "ignored_bytes": ignored_count,
        }
        return path_ids, stats
    except Exception as e:
        logging.error(f"Error parsing branch paths: {e}")
        return [], {
            "branch_decisions": 0,
            "boundaries": 0,
            "paths": 0,
            "unique_paths": 0,
            "ignored_bytes": 0,
        }


def parse_branch_paths(infile_path, abstraction="xxhash", window_size=256):
    """
    Parse branch trace into path IDs using one abstraction method.

    abstraction:
      - xxhash: hash each raw path with xxh64
      - dict: assign an exact dictionary ID to each raw 0/1 path
      - rle-dict: run-length encode each path, then assign dictionary ID
      - window-dict: split each 0x02-delimited path into fixed-size branch
        windows, then assign a dictionary ID to each window

    Returns:
        (path_ids, stats, path_dict)
        path_dict is empty for xxhash.
    """
    if abstraction == "xxhash":
        path_ids, stats = parse_branch_paths_xxhash(infile_path)
        return path_ids, stats, {}
    if abstraction not in ("dict", "rle-dict", "window-dict"):
        raise ValueError(f"Unknown branch abstraction: {abstraction}")
    if window_size < 1:
        raise ValueError("window_size must be at least 1")

    path_ids = []
    path_dict = {}
    boundary_count = 0
    decision_count = 0
    ignored_count = 0

    current = bytearray()
    current_rle = []
    last_bit = None
    run_len = 0

    def append_bit(bit):
        nonlocal last_bit, run_len
        if abstraction in ("dict", "window-dict"):
            current.append(bit)
            if abstraction == "window-dict" and len(current) >= window_size:
                finish_segment()
            return
        if last_bit is None:
            last_bit = bit
            run_len = 1
        elif bit == last_bit:
            run_len += 1
        else:
            current_rle.append((last_bit, run_len))
            last_bit = bit
            run_len = 1

    def finish_segment():
        nonlocal current, current_rle, last_bit, run_len
        if abstraction in ("dict", "window-dict"):
            if not current:
                return
            key = bytes(current)
            current = bytearray()
        else:
            if last_bit is not None:
                current_rle.append((last_bit, run_len))
            if not current_rle:
                return
            key = tuple(current_rle)
            current_rle = []
            last_bit = None
            run_len = 0

        path_id = path_dict.get(key)
        if path_id is None:
            path_id = len(path_dict) + 1
            path_dict[key] = path_id
        path_ids.append(path_id)

    try:
        with open(infile_path, 'rb') as f:
            while True:
                chunk = f.read(1024 * 1024)
                if not chunk:
                    break
                for b in chunk:
                    if b == 0 or b == 1:
                        append_bit(b)
                        decision_count += 1
                    elif b == 2:
                        boundary_count += 1
                        finish_segment()
                    else:
                        ignored_count += 1

        finish_segment()

        stats = {
            "branch_decisions": decision_count,
            "boundaries": boundary_count,
            "paths": len(path_ids),
            "unique_paths": len(path_dict),
            "ignored_bytes": ignored_count,
            "window_size": window_size if abstraction == "window-dict" else 0,
        }
        return path_ids, stats, path_dict
    except Exception as e:
        logging.error(f"Error parsing branch paths with {abstraction}: {e}")
        return [], {
            "branch_decisions": 0,
            "boundaries": 0,
            "paths": 0,
            "unique_paths": 0,
            "ignored_bytes": 0,
        }, {}


def serialize_path_dictionary(path_dict, output_path, abstraction):
    """Serialize exact path dictionary for dict/rle-dict abstractions."""
    if abstraction == "xxhash" or not path_dict:
        return None

    id_to_key = {path_id: key for key, path_id in path_dict.items()}
    with open(output_path, "wb") as f:
        f.write(b"WPDT")
        f.write(struct.pack("<I", 1))
        type_id = 1 if abstraction in ("dict", "window-dict") else 2
        f.write(struct.pack("<I", type_id))
        f.write(struct.pack("<I", len(id_to_key)))

        for path_id in sorted(id_to_key):
            key = id_to_key[path_id]
            f.write(struct.pack("<Q", path_id))
            if abstraction in ("dict", "window-dict"):
                f.write(struct.pack("<I", len(key)))
                f.write(key)
            else:
                f.write(struct.pack("<I", len(key)))
                for bit, count in key:
                    f.write(struct.pack("<BI", bit, count))
    return output_path


def dedup_consecutive(seq):
    """Remove consecutive duplicate values. Returns new list."""
    if not seq:
        return seq
    result = [seq[0]]
    for v in seq[1:]:
        if v != result[-1]:
            result.append(v)
    return result


def build_hash_dict(trace_data):
    """Map sparse hash values to dense sequential IDs for compact32 packing.

    Returns:
        (mapped_trace, hash_dict)
        - mapped_trace: list of int with values in [0, N)
        - hash_dict: {original_hash_value: mapped_id}
    """
    hash_dict = {}
    mapped = []
    for v in trace_data:
        idx = hash_dict.get(v)
        if idx is None:
            idx = len(hash_dict)
            hash_dict[v] = idx
        mapped.append(idx)
    return mapped, hash_dict


def serialize_hash_dictionary(hash_dict, output_path):
    """Serialize hash dictionary for compact32 reconstruction.

    Binary format:
        magic "HSHD" (4B)
        version (4B, =1)
        entry_count (4B)
        for each entry: original_hash (8B) + mapped_id (4B)
    """
    id_to_hash = {idx: h for h, idx in hash_dict.items()}
    with open(output_path, "wb") as f:
        f.write(b"HSHD")
        f.write(struct.pack("<I", 1))
        f.write(struct.pack("<I", len(id_to_hash)))
        for idx in sorted(id_to_hash):
            f.write(struct.pack("<Q", id_to_hash[idx]))
            f.write(struct.pack("<I", idx))
    return output_path


def compress_with_sequitur(trace_data):
    """
    使用 Sequitur 算法压缩 trace_data 序列

    Args:
        trace_data: uint64 值的列表

    Returns:
        Grammar 对象（字典形式）
        {Production_ID: [element1, element2, ...], ...}
    """
    try:
        grammar = sequitur_parse(trace_data)
        logging.info(f"Sequitur compression completed. Rules extracted: {len(grammar)}")
        return grammar
    except Exception as e:
        logging.error(f"Error compressing with Sequitur: {e}")
        return None
