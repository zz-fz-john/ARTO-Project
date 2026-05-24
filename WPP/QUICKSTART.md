# 快速参考指南

## 文件结构

```
WPP/
├── hash_value.txt          # 输入数据（二进制）
├── parseData.py            # 解析与压缩模块
├── rules_packing.py        # 二进制打包模块
├── main.py                 # 标准工作流程
├── analyze.py              # 详细分析脚本
├── demo.py                 # 完整演示脚本
├── README.md               # 完整文档
├── QUICKSTART.md           # 本文件
├── venv/                   # Python 虚拟环境
├── compressed_rules.bin    # 输出规则文件
└── ...
```

## 快速命令

```bash
# 激活虚拟环境
& "venv\Scripts\Activate.ps1"

# 运行完整演示（推荐开始）
python demo.py

# 生成详细分析报告
python analyze.py

# 标准工作流程
python main.py
```

## 核心 API

### 1. 解析二进制文件

```python
from parseData import parse_infile

trace_data = parse_infile("hash_value.txt")
# 返回: [uint64, uint64, ...]，长度 68,931
```

### 2. 进行 Sequitur 压缩

```python
from parseData import compress_with_sequitur

grammar = compress_with_sequitur(trace_data)
# 返回: Grammar 对象（dict-like）
# 包含 11 条规则，共 94 个元素
```

### 3. 序列化为二进制

```python
from rules_packing import RulesPacker

RulesPacker.serialize_grammar(grammar, "output.bin", len(trace_data))
# 输出: 874 字节的二进制文件
```

### 4. 从二进制恢复

```python
restored_grammar = RulesPacker.deserialize_grammar("output.bin")
# 返回: 等价的 Grammar 字典
```

### 5. 展开规则恢复原始数据

```python
from sksequitur.api import Production

expanded = list(restored_grammar.expand(Production(0)))
# 返回: 完整的 uint64 序列
# 长度: 68,931，与原始数据完全相同
```

## 压缩结果

| 指标 | 值 |
|------|-----|
| 原始大小 | 551,448 bytes |
| 压缩后 | 874 bytes |
| 压缩比 | 0.1585% |
| 压缩因子 | 630.9x |
| 空间节省 | 99.84% |

## 文件格式

**二进制规则文件结构:**

```
[Header 12 bytes]
  0x53455130 (Magic)      4 bytes
  11 (Rule count)         4 bytes
  68931 (Orig length)     4 bytes

[Metadata 88 bytes (11 rules × 8)]
  [Rule 0 metadata]       8 bytes
  [Rule 1 metadata]       8 bytes
  ...

[Data 774 bytes (94 elements × 8)]
  [Element 0]             8 bytes
  [Element 1]             8 bytes
  ...
```

## 验证流程

✓ 解析验证 (14 ms)
  └─ 确认 68,931 个元素正确解析

✓ 压缩验证 (680 ms)
  └─ Sequitur 算法提取 11 条规则

✓ 序列化验证 (2 ms)
  └─ 规则编码为二进制格式

✓ 反序列化验证 (2 ms)
  └─ 规则正确从二进制恢复

✓ 展开验证 (16 ms)
  └─ 所有 68,931 个元素完全匹配原始数据

## 常见任务

### 任务1: 压缩新数据

```python
from parseData import parse_infile, compress_with_sequitur
from rules_packing import RulesPacker

# 解析 → 压缩 → 打包
data = parse_infile("new_data.bin")
grammar = compress_with_sequitur(data)
RulesPacker.serialize_grammar(grammar, "new_data_compressed.bin", len(data))
```

### 任务2: 恢复原始数据

```python
from rules_packing import RulesPacker
from sksequitur.api import Production

# 反序列化 → 展开
grammar = RulesPacker.deserialize_grammar("compressed.bin")
original = list(grammar.expand(Production(0)))
```

### 任务3: 分析压缩效率

```python
from rules_packing import RulesPacker

grammar = RulesPacker.deserialize_grammar("compressed.bin")
rule_counts = grammar.counts()

for rule_id, count in sorted(rule_counts.items()):
    print(f"Rule {rule_id}: used {count} times")
```

## 排除故障

| 问题 | 解决 |
|------|------|
| `ModuleNotFoundError` | 运行 `& "venv\Scripts\Activate.ps1"` |
| 文件格式错误 | 检查魔术数字 `0x53455130` |
| 内存不足 | 使用流式处理，分块处理 |
| 数据不匹配 | 验证展开规则完整性 |

## 性能基准

| 操作 | 时间 | 吞吐量 |
|------|------|--------|
| 解析 | 14 ms | 4.9 M元素/s |
| 压缩 | 680 ms | 101 K元素/s |
| 序列化 | 2 ms | 34.5 M元素/s |
| 反序列化 | 2 ms | 34.5 M元素/s |
| 展开 | 16 ms | 4.3 M元素/s |

## 下一步

- 📖 详见 [README.md](README.md) 获取完整文档
- 🔬 运行 `python analyze.py` 查看详细统计
- 🎯 参考 `demo.py` 了解最佳实践

---

**版本**: 1.0  
**更新**: 2026-05-15  
**语言**: Python 3.8+
