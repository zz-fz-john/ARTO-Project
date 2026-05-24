import struct
import logging
from typing import Dict, List, Tuple, Union
from pathlib import Path


class RulesPacker:
    """
    处理规则的二进制打包和解包
    
    二进制格式设计 v2：
    - 文件头 (12 bytes):
      * 4 bytes: 魔术数字 "SEQ0" (0x53455130)
      * 4 bytes: 规则总数 (uint32, little-endian)
      * 4 bytes: 原始序列长度 (uint32, little-endian)
    
    - 规则元数据表 (variable):
      * 每条规则: rule_id (4B) + element_count (4B) + usage_count (4B) + data_offset (4B)
    
    - 规则数据块 (variable):
      * 每条规则数据: [element1 (8B), element2 (8B), ...]
      * 元素编码:
        - 终端元素(uint64): 高位=0, 低63位=数值
        - 非终端元素(Production): 高位=1, 低31位=production_id
    """
    
    MAGIC_NUMBER = 0x53455130  # "SEQ0"
    MAGIC_NUMBER_32 = 0x32335153  # "SQ32"
    
    @staticmethod
    def _encode_element(element: Union[int, 'Production']) -> int:
        """
        编码单个元素为 8 字节整数
        
        Args:
            element: 终端符号(uint64)或非终端符号(Production/int)
            
        Returns:
            编码后的 uint64 值
        """
        # 检查是否为 Production 对象（通常是 int 的子类）
        # 如果 element 看起来像 Production（整数且 < 2^31），标记为非终端
        if isinstance(element, int):
            # 假设元素是 uint64 终端符号
            # 高位设为 0（终端标记）
            return element & 0x7FFFFFFFFFFFFFFF  # 确保高位为 0
        else:
            # 非法元素
            raise ValueError(f"Unknown element type: {type(element)}")
    
    @staticmethod
    def _encode_production_element(prod_id: int) -> int:
        """
        编码非终端元素（Production 引用）
        
        Args:
            prod_id: 生产规则 ID (0-based)
            
        Returns:
            编码后的 uint64 值（高位=1）
        """
        if not isinstance(prod_id, int) or prod_id < 0 or prod_id >= 2**31:
            raise ValueError(f"Invalid production ID: {prod_id}")
        return 0x8000000000000000 | prod_id  # 设置高位为 1
    
    @staticmethod
    def _decode_element(encoded: int) -> Tuple[bool, Union[int, int]]:
        """
        解码单个元素
        
        Args:
            encoded: 编码后的 uint64 值
            
        Returns:
            (is_nonterminal, value) 元组
            - is_nonterminal: True 表示非终端(Production)，False 表示终端
            - value: 如果是终端返回数值，如果是非终端返回 production_id
        """
        is_nonterminal = (encoded & 0x8000000000000000) != 0
        if is_nonterminal:
            prod_id = encoded & 0x7FFFFFFF
            return True, prod_id
        else:
            value = encoded & 0x7FFFFFFFFFFFFFFF
            return False, value

    @staticmethod
    def _encode_element_32(element: Union[int, 'Production']) -> int:
        elem_type_name = type(element).__name__
        if elem_type_name == "Production" or (isinstance(element, int) and type(element).__name__ != "int"):
            prod_id = int(element)
            if prod_id < 0 or prod_id >= 2**31:
                raise ValueError(f"Production ID does not fit compact32: {prod_id}")
            return 0x80000000 | prod_id

        value = int(element)
        if value < 0 or value >= 2**31:
            raise ValueError(f"Terminal value does not fit compact32: {value}")
        return value

    @staticmethod
    def _decode_element_32(encoded: int) -> Tuple[bool, int]:
        is_nonterminal = (encoded & 0x80000000) != 0
        value = encoded & 0x7FFFFFFF
        return is_nonterminal, value
    
    @classmethod
    def serialize_grammar(cls, grammar: Dict, output_path: str, original_length: int = 0) -> None:
        """
        将 Grammar 对象序列化为二进制文件
        
        Args:
            grammar: sksequitur Grammar 对象（继承自 dict）
            output_path: 输出文件路径
            original_length: 原始序列长度（用于元数据，可选）
        """
        try:
            element_data = []
            index_table = []
            
            current_offset = 0
            
            # 获取规则使用计数
            try:
                rule_counts = grammar.counts()
            except:
                rule_counts = {}
            
            # 遍历所有生产规则，保持原始顺序
            for prod_id, elements in sorted(grammar.items()):
                prod_id_int = int(prod_id)
                
                if not isinstance(elements, list):
                    elements = list(elements)
                
                # 编码此规则的所有元素
                encoded_elements = []
                for elem in elements:
                    # 检查元素的类型
                    # Production 对象通常是 int 的子类，名字为 "Production"
                    elem_type_name = type(elem).__name__
                    
                    if elem_type_name == "Production" or (isinstance(elem, int) and type(elem).__name__ != "int"):
                        # 非终端符号（Production 对象）
                        encoded_elem = cls._encode_production_element(int(elem))
                    else:
                        # 终端符号（普通 uint64 整数）
                        encoded_elem = cls._encode_element(int(elem))
                    
                    encoded_elements.append(encoded_elem)
                
                # 记录此规则的元数据
                element_count = len(encoded_elements)
                data_size = element_count * 8  # 每个元素 8 字节
                usage_count = rule_counts.get(prod_id, 1)
                
                index_table.append((prod_id_int, element_count, usage_count, current_offset))
                
                element_data.extend(encoded_elements)
                current_offset += data_size
            
            # 写入文件
            with open(output_path, 'wb') as f:
                # 写入文件头
                f.write(struct.pack('<I', cls.MAGIC_NUMBER))
                f.write(struct.pack('<I', len(index_table)))
                f.write(struct.pack('<I', original_length))
                
                # 写入元数据表
                for prod_id, element_count, usage_count, offset in index_table:
                    f.write(struct.pack('<I', prod_id))
                    f.write(struct.pack('<I', element_count))
                    f.write(struct.pack('<I', int(usage_count)))
                    f.write(struct.pack('<I', offset))
                
                # 写入元素数据
                for encoded_elem in element_data:
                    f.write(struct.pack('<Q', encoded_elem))
            
            # 计算压缩统计
            file_size = Path(output_path).stat().st_size
            
            logging.info(f"Grammar serialized successfully:")
            logging.info(f"  Output file: {output_path}")
            logging.info(f"  Rules count: {len(index_table)}")
            logging.info(f"  Total elements: {len(element_data)}")
            logging.info(f"  Original length: {original_length}")
            logging.info(f"  File size: {file_size} bytes")
            
        except Exception as e:
            logging.error(f"Error serializing grammar: {e}")
            raise

    @classmethod
    def serialize_grammar_compact32(cls, grammar: Dict, output_path: str,
                                    original_length: int = 0) -> None:
        """Serialize grammar with 32-bit terminal/nonterminal elements."""
        try:
            element_data = []
            index_table = []
            current_offset = 0

            try:
                rule_counts = grammar.counts()
            except:
                rule_counts = {}

            for prod_id, elements in sorted(grammar.items()):
                prod_id_int = int(prod_id)
                if not isinstance(elements, list):
                    elements = list(elements)

                encoded_elements = [cls._encode_element_32(elem) for elem in elements]
                element_count = len(encoded_elements)
                data_size = element_count * 4
                usage_count = rule_counts.get(prod_id, 1)

                index_table.append((prod_id_int, element_count, usage_count, current_offset))
                element_data.extend(encoded_elements)
                current_offset += data_size

            with open(output_path, 'wb') as f:
                f.write(struct.pack('<I', cls.MAGIC_NUMBER_32))
                f.write(struct.pack('<I', len(index_table)))
                f.write(struct.pack('<I', original_length))

                for prod_id, element_count, usage_count, offset in index_table:
                    f.write(struct.pack('<I', prod_id))
                    f.write(struct.pack('<I', element_count))
                    f.write(struct.pack('<I', int(usage_count)))
                    f.write(struct.pack('<I', offset))

                for encoded_elem in element_data:
                    f.write(struct.pack('<I', encoded_elem))

            file_size = Path(output_path).stat().st_size
            logging.info(f"Grammar serialized successfully (compact32):")
            logging.info(f"  Output file: {output_path}")
            logging.info(f"  Rules count: {len(index_table)}")
            logging.info(f"  Total elements: {len(element_data)}")
            logging.info(f"  Original length: {original_length}")
            logging.info(f"  File size: {file_size} bytes")
        except Exception as e:
            logging.error(f"Error serializing compact32 grammar: {e}")
            raise
    
    @classmethod
    def deserialize_grammar(cls, input_path: str) -> Dict:
        """
        从二进制文件反序列化 Grammar 对象
        
        Args:
            input_path: 输入文件路径
            
        Returns:
            恢复的 Grammar 字典 {prod_id: [elements], ...}
        """
        try:
            grammar = {}
            
            with open(input_path, 'rb') as f:
                # 读取文件头
                magic = struct.unpack('<I', f.read(4))[0]
                compact32 = False
                if magic == cls.MAGIC_NUMBER_32:
                    compact32 = True
                elif magic != cls.MAGIC_NUMBER:
                    raise ValueError(f"Invalid magic number: {magic:#x}")
                
                rule_count = struct.unpack('<I', f.read(4))[0]
                original_length = struct.unpack('<I', f.read(4))[0]
                
                # 读取元数据表
                metadata_table = []
                for _ in range(rule_count):
                    prod_id = struct.unpack('<I', f.read(4))[0]
                    element_count = struct.unpack('<I', f.read(4))[0]
                    usage_count = struct.unpack('<I', f.read(4))[0]
                    offset = struct.unpack('<I', f.read(4))[0]
                    metadata_table.append((prod_id, element_count, usage_count, offset))
                
                # 读取所有元素数据
                all_elements = []
                while True:
                    chunk = f.read(4 if compact32 else 8)
                    if not chunk:
                        break
                    if len(chunk) < (4 if compact32 else 8):
                        logging.warning(f"Incomplete element at end of file")
                        break
                    encoded_elem = struct.unpack('<I' if compact32 else '<Q', chunk)[0]
                    all_elements.append(encoded_elem)
                
                # 重建 Grammar
                for prod_id, element_count, usage_count, offset in metadata_table:
                    element_byte_offset = offset // (4 if compact32 else 8)
                    elements = []
                    for i in range(element_count):
                        encoded_elem = all_elements[element_byte_offset + i]
                        if compact32:
                            is_nonterminal, value = cls._decode_element_32(encoded_elem)
                        else:
                            is_nonterminal, value = cls._decode_element(encoded_elem)
                        elements.append(value)
                    
                    grammar[prod_id] = elements
            
            logging.info(f"Grammar deserialized successfully:")
            logging.info(f"  Input file: {input_path}")
            logging.info(f"  Rules count: {len(grammar)}")
            logging.info(f"  Total elements: {len(all_elements)}")
            logging.info(f"  Original length: {original_length}")
            
            return grammar
            
        except Exception as e:
            logging.error(f"Error deserializing grammar: {e}")
            raise


if __name__ == '__main__':
    logging.basicConfig(level=logging.INFO)
    print("Rules packing module loaded successfully")
