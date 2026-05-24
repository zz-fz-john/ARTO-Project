.section .custom_heap_data_base, "aw", %nobits
.global _custom_heap_start_data_base
.global _custom_heap_end_data_base
_custom_heap_start_data_base:
    .space 0x20000  // 64KB 堆
_custom_heap_end_data_base:


.section .custom_ro_data, "a",  %progbits
.global _custom_ro_data_start
.global _custom_ro_data_end
_custom_ro_data_start:
    .byte 0x00
    .space 0xfffff // 填充 0x00
_custom_ro_data_end:
