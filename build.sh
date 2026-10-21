#!/usr/bin/env bash
# 把同一份源码编成 Better Endfield 3.5.x 的内置模块 local.endmin_mask（开发用，不发布）：
# 产物在 dist/，DLL 和 local.endmin_mask.module.ini 放进 BE 的 modules 目录。
# 用 clang + lld 交叉编译，Windows 头文件和 PE 导入库用 wine 自带的，不需要 mingw/MSVC；
# 不在默认位置时用 WINE_INCLUDE、WINE_PE_LIB 指定。
set -euo pipefail
cd "$(dirname "$0")"
sdk=third_party/eml-sdk/include
wineinc=${WINE_INCLUDE:-/usr/include/wine}
winelib=${WINE_PE_LIB:-/usr/lib/wine/x86_64-windows}
build=build
mkdir -p "$build" dist
clang --target=x86_64-w64-mingw32 -std=c11 -O2 -Wall -Wextra -Werror \
  -nostdinc -D_UCRT \
  -isystem "$wineinc/msvcrt" -isystem "$wineinc/windows" \
  -isystem "$(clang -print-resource-dir)/include" -I "$sdk" \
  -fno-stack-protector -mno-stack-arg-probe -ffunction-sections \
  -c src/endmin_mask.c -o "$build/endmin_mask.o"
ld.lld -flavor gnu -m i386pep --shared --gc-sections -e DllMainCRTStartup \
  -o "$build/Local.EndminMask.dll" "$build/endmin_mask.o" \
  "$winelib/libkernel32.a" "$winelib/libucrtbase.a"
cp "$build/Local.EndminMask.dll" dist/Local.EndminMask.dll
cat > dist/local.endmin_mask.module.ini <<'INI'
[Module]
id=local.endmin_mask
library=Local.EndminMask.dll
enabled=false
api=BetterEndfield_GetModuleApiV1
contract=il2cpp-metadata
contract_version=1
hook_group=endmin-mask
requires=UnityEngine.CoreModule.dll|UnityEngine.UI.dll
INI
sha256sum dist/*
