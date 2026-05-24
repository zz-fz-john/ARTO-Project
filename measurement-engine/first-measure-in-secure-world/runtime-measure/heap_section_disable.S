.section .custom_heap_data_base, "aw", %nobits
.global _custom_heap_start_data_base
.global _custom_heap_end_data_base
_custom_heap_start_data_base:
    .space 0x10000  // 64KB 堆
_custom_heap_end_data_base:

.section .custom_heap_run_time, "aw", %nobits
.global _custom_heap_start_run_time
.global _custom_heap_end_run_time
_custom_heap_start_run_time:
    .space 0x8000   // 32KB 堆
_custom_heap_end_run_time:

