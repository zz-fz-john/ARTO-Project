set pagination off
set confirm off

# 在 mov r1, r5 执行前设置断点
break *0x24626c
set $target_bp = $bpnum

commands
silent

printf "[GDB] Reached 0x24626c: mov r1, r5\n"
printf "[GDB] Original r5:  0x%08x (%u)\n", $r5, $r5

# 将 r5 修改为原值加 1
set $r5 = $r5 + 1

printf "[GDB] Modified r5:  0x%08x (%u)\n", $r5, $r5

# 禁用该断点，保证只修改一次
disable $target_bp

continue
end
