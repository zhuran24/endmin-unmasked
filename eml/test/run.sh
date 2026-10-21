#!/usr/bin/env bash
# 编假宿主，在一次性的独立 wine 前缀和 Xvfb 里加载 EML 版 DLL（先跑 ../../build_eml.sh）。
# 不碰游戏的 prefix，也不上真桌面。退出码 0 = 全部通过。
set -euo pipefail
cd "$(dirname "$0")"
root=$(cd ../.. && pwd)
sdk=$root/third_party/eml-sdk/include
wineinc=${WINE_INCLUDE:-/usr/include/wine}
winelib=${WINE_PE_LIB:-/usr/lib/wine/x86_64-windows}
work=$(mktemp -d "${TMPDIR:-/tmp}/endmin-eml-test.XXXXXX")
trap 'rm -rf "$work"' EXIT

clang --target=x86_64-w64-mingw32 -std=c11 -O2 -Wall -Wextra -Werror \
  -nostdinc -D_UCRT \
  -isystem "$wineinc/msvcrt" -isystem "$wineinc/windows" \
  -isystem "$(clang -print-resource-dir)/include" -I "$sdk" \
  -fno-stack-protector -mno-stack-arg-probe \
  -c fake_host.c -o "$work/fake_host.o"
ld.lld -flavor gnu -m i386pep --subsystem windows -e Start \
  -o "$work/fake_host.exe" "$work/fake_host.o" \
  "$winelib/libkernel32.a" "$winelib/libucrtbase.a"
cp "$root/build/eml/zhuran24.endmin-unmasked.dll" "$work/"

export WINEPREFIX=$work/prefix WINEDEBUG=-all
export WINEDLLOVERRIDES='mscoree,mshtml=;winemenubuilder.exe=d'
unset WAYLAND_DISPLAY
cd "$work"
xvfb-run -a sh -c 'wineboot -i >/dev/null 2>&1; wine fake_host.exe'
