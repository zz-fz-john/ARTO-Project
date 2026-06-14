set pagination off
set confirm off
set disassemble-next-line on

# 0：尚未处理；1：已经完成一次修改
set $redirect_done = 0

# 0：不是来自目标调用点；1：刚从 0x246370 调用了目标函数
set $redirect_pending = 0

# 目标调用点：
# 0x246370: bl 0x2d5488 <_ZN7Vector3IfE2xyEv>
break *0x2464fc
commands
silent

if $redirect_done == 0
    printf "[GDB] Target BL reached at 0x%08x\n", $pc
    set $redirect_pending = 1
end

continue
end

# 在下面这条指令执行之前暂停：
# 0x2d54d0: pop {r8}
break *0x2d54d0
commands
silent

# 只有从 0x246370 进入此次函数调用时才进行修改
if $redirect_pending == 1 && $redirect_done == 0
    set $saved_return = *(unsigned int *)$sp

    printf "[GDB] Reached 0x2d54d0 from call site 0x246370\n"
    printf "[GDB] SP:                      0x%08x\n", $sp
    printf "[GDB] Original saved return:   0x%08x\n", $saved_return

    # 0x246370 的正常返回地址应为 0x246374
    if $saved_return == 0x246500
        set {unsigned int}$sp = 0x2464ec

        printf "[GDB] Modified saved return:   0x%08x\n", \
               *(unsigned int *)$sp

        # 保证整个调试过程中只修改一次
        set $redirect_done = 1
    else
        printf "[GDB] Warning: unexpected saved return address; not modified.\n"
    end

    set $redirect_pending = 0
end

continue
end
