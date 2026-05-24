import subprocess
import cxxfilt
# /// get class name before brackets
# /// e.g., for `namespace::A<...::...>::f', we get `namespace::A'
def getBeforeBrackets(func_name):
    if not func_name or func_name[-1]!='>':
        return func_name
    bracket_num=1
    pos=len(func_name)-2
    while pos>=0:
        if func_name[pos]=='>':
            bracket_num+=1
        elif func_name[pos]=='<':
            bracket_num-=1
        if bracket_num==0:
            pos-=1
    return func_name[:pos]

def getBeforeParenthesis(func_name):
    last_right_paren=func_name.rfind(')')
    assert last_right_paren > 0, "No closing parenthesis found"
    paren_num=1
    pos =last_right_paren-1
    while pos>=0:
        if func_name[pos]==')':
            paren_num+=1
        if func_name[pos]=='(':
            paren_num-=1
        if paren_num==0:
            break
        pos-=1
    return func_name[:pos]

def is_oper_overload(name: str) -> bool:
    leftnum = 0
    rightnum = 0

    subname = name
    leftpos = subname.find('<')
    while leftpos != -1:  # 在 Python 中，find 找不到时返回 -1
        subname = subname[leftpos + 1:]
        leftpos = subname.find('<')
        leftnum += 1

    subname = name
    rightpos = subname.find('>')
    while rightpos != -1:
        subname = subname[rightpos + 1:]
        rightpos = subname.find('>')
        rightnum += 1

    return leftnum != rightnum

def handle_thunk_function(dname):
    """
    处理多重继承时的 thunk 函数名称。
    如果类名以 "virtual thunk to " 或 "non-virtual thunk to " 作为前缀，
    则去除该前缀以获得真实的类名。
    """
    thunk_prefixes = ["virtual thunk to ", "non-virtual thunk to "]

    for prefix in thunk_prefixes:
        if len(dname.className) > len(prefix) and dname.className.startswith(prefix):
            dname.className = dname.className[len(prefix):]
            dname.isThunkFunc = True
            return
class DemangledName:
    def __init__(self):
        self.className = ""
        self.funcName = ""
        self.isThunkFunc = False
# /*
#  * input: _ZN****
#  * after abi::__cxa_demangle:
#  * namespace::A<...::...>::f<...::...>(...)
#  *                       ^
#  *                    delimiter
#  *
#  * step1: getBeforeParenthesis
#  * namespace::A<...::...>::f<...::...>
#  *
#  * step2: getBeforeBrackets
#  * namespace::A<...::...>::f
#  *
#  * step3: find delimiter
#  * namespace::A<...::...>::
#  *                       ^
#  *
#  * className: namespace::A<...::...>
#  * functionName: f<...::...>
#  */
def demangle(name: str) -> DemangledName:##解析c++函数名，得到最后函数的类名和函数名
    # real_name=cxxfilt.demangle(func_name)
    dname=DemangledName()
    # 使用 c++filt 作为等效的 demangling 工具
    try:
        result = subprocess.run(['c++filt', '-n', name], capture_output=True, text=True)
        realname = result.stdout.strip() if result.returncode == 0 else None
    except Exception:
        realname = None
    if realname is None:
        dname.className = ""
        dname.funcName = ""
    else:
        before_parenthesis = getBeforeParenthesis(realname)
        if "::" not in before_parenthesis or is_oper_overload(before_parenthesis):
            dname.className = ""
            dname.funcName = ""
        else:
            before_bracket = getBeforeBrackets(before_parenthesis)
            colon_pos = before_bracket.rfind("::")
            if colon_pos == -1:
                dname.className = ""
                dname.funcName = ""
            else:
                dname.className = before_parenthesis[:colon_pos]
                dname.funcName = before_parenthesis[colon_pos + 2:]

    handle_thunk_function(dname)

    return dname
