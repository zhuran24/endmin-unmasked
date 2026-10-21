#!/usr/bin/env bash
# 编 Endfield Mod Loader 用的第三方模块包（format 1 / ABI 1，BE 3.5.x 也能导入）。
# 同一份源码，定义 EML_MODULE；产物是 dist-eml/endmin-unmasked-<版本>.zip，ZIP 根目录直接是 module.json。
# 用 clang + lld 交叉编译，Windows 头文件和 PE 导入库用 wine 自带的；不在默认位置时用 WINE_INCLUDE、WINE_PE_LIB 指定。
set -euo pipefail
cd "$(dirname "$0")"
sdk=third_party/eml-sdk/include
wineinc=${WINE_INCLUDE:-/usr/include/wine}
winelib=${WINE_PE_LIB:-/usr/lib/wine/x86_64-windows}
build=build/eml
id=zhuran24.endmin-unmasked
version=$(sed -n 's/^#define MODULE_VERSION "\(.*\)"$/\1/p' src/endmin_mask.c)
[[ -n "$version" ]] || { echo "读不到 MODULE_VERSION" >&2; exit 1; }
mkdir -p "$build" dist-eml
clang --target=x86_64-w64-mingw32 -std=c11 -O2 -Wall -Wextra -Werror -DEML_MODULE \
  -nostdinc -D_UCRT \
  -isystem "$wineinc/msvcrt" -isystem "$wineinc/windows" \
  -isystem "$(clang -print-resource-dir)/include" -I "$sdk" \
  -fno-stack-protector -mno-stack-arg-probe -ffunction-sections \
  -c src/endmin_mask.c -o "$build/endmin_mask.o"
ld.lld -flavor gnu -m i386pep --shared --gc-sections -e DllMainCRTStartup \
  -o "$build/$id.dll" "$build/endmin_mask.o" \
  "$winelib/libkernel32.a" "$winelib/libucrtbase.a"

package=$build/package
rm -rf "$package"
mkdir -p "$package/native/windows-x64" "$package/ui"
sed "s/@VERSION@/$version/" eml/module.json > "$package/module.json"
cp "$build/$id.dll" "$package/native/windows-x64/"
cp eml/ui/index.html eml/ui/style.css eml/ui/app.js "$package/ui/"
zip_path=$PWD/dist-eml/endmin-unmasked-$version.zip
rm -f "$zip_path"
(cd "$package" && zip -X -q -r -D "$zip_path" module.json native ui)
unzip -l "$zip_path"
sha256sum "$zip_path" "$build/$id.dll"
