#!/usr/bin/env bash
# 构建 SysMonitor：部件 + 卸载程序 + 安装程序
# 需要 LLVM-MinGW 或 MinGW-w64 工具链（clang/gcc + windres）
set -e
cd "$(dirname "$0")"

# ---- 探测工具链 ----
pick() {
  for c in "$@"; do
    if command -v "$c" >/dev/null 2>&1; then echo "$c"; return 0; fi
  done
  return 1
}

CC=$(pick x86_64-w64-mingw32-clang x86_64-w64-mingw32-gcc clang gcc) || {
  echo "找不到 C 编译器，请先安装 mingw-w64 / LLVM-MinGW 并加入 PATH" >&2; exit 1; }
RC=$(pick x86_64-w64-mingw32-windres windres llvm-windres llvm-rc) || {
  echo "找不到资源编译器 windres" >&2; exit 1; }

TARGET="--target=x86_64-w64-mingw32"
case "$CC" in
  *x86_64-w64-mingw32*) TARGET="" ;;
esac

# windres 需要能找到 windows.h
INC=""
for d in /usr/x86_64-w64-mingw32 /usr/lib/mingw-w64 /opt/homebrew/opt/mingw-w64 /usr/local/mingw-w64; do
  [ -d "$d/include" ] && INC="$d/include" && break
done

echo "编译器 : $CC"
echo "资源   : $RC"

echo "[1/5] 生成图标"
if command -v python3 >/dev/null 2>&1; then
  python3 res/gen_icon.py
else
  echo "  (无 python3，跳过图标生成，使用仓库内已有 app.ico)"
fi

echo "[2/5] 编译资源"
"$RC" -I "${INC:-.}" res/app.rc   -O coff -o app_res.o
"$RC" -I "${INC:-.}" res/unins.rc -O coff -o unins_res.o

echo "[3/5] 编译部件 SysMonitor.exe"
$CC $TARGET -mwindows -O2 -s -o SysMonitor.exe src/sysmon.c app_res.o \
  -lpdh -lmsimg32 -lkernel32 -luser32 -lgdi32 -lshell32 -ladvapi32

echo "[4/5] 编译卸载程序 unins.exe"
$CC $TARGET -mwindows -O2 -s -o unins.exe src/unins.c unins_res.o \
  -ladvapi32 -lshell32 -lole32 -luser32 -lkernel32

echo "[5/5] 编译安装程序 SysMonitorSetup.exe（内嵌上面两个 exe）"
"$RC" -I "${INC:-.}" res/setup.rc -O coff -o setup_res.o
$CC $TARGET -mwindows -O2 -s -o SysMonitorSetup.exe src/setup.c setup_res.o \
  -lcomctl32 -lole32 -luuid -lshell32 -ladvapi32 -luser32 -lgdi32 -lkernel32

echo
echo "=== 产物 ==="
ls -lh SysMonitor.exe unins.exe SysMonitorSetup.exe
