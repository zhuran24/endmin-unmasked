# Endministrator Unmasked

An [Endfield Mod Loader](https://github.com/Dr-hydra/Endfield-Mod-Loader) (EML) module for *Arknights: Endfield*. It hides the Endministrator's mask and gives the female Endministrator the unmasked face and hair the game itself uses in story cutscenes.

- Download: [Nexus Mods](https://www.nexusmods.com/arknightendfield/mods/75)
- 中文说明在[下面](#管理员去面罩)。

## Features

- Hides the mask of both the female and the male Endministrator, in the field, in menus and in cutscenes.
- Swaps in the face and hair the story uses when she takes the mask off, for the female Endministrator only: face, brows, eyeliner and lashes, irises, eye-white shading, hair and the bangs shadow. Body and outfit are untouched. The story bangs follow the head; the side and back hair keep the playable model's hair physics.

Both options are on the module's page in EML and switch live in game. Turning the face swap on also turns mask hiding on; turning mask hiding off also turns the face swap off.

Tested in game on the CN client 1.6.3 through the third-party module loader of Better Endfield 3.5.4, which EML's loader is derived from. It has not been run inside EML itself yet.

## How it works

- **Mask hiding** hooks the engine implementations of `Transform.SetParent` and `GameObject.SetActive`. When an Endministrator model is attached to its parent, the mask parts (`S_actor_endmin{f,m}_cloth_03/05`, every LOD) are switched off, and later `SetActive(true)` calls on them become `SetActive(false)`. Setting changes are applied on the main thread from a frame pump on `EventSystem.Update`.
- **Face swap** loads the game's own unmasked prefab, `chr_0003_endminfunmasked_postmodel`, from the installed game. Its face and hair meshes and materials are reused unchanged and bound to the playable skeleton through helper bones (local transform = playable bindpose × story bindpose⁻¹), so expressions and blinking still come from the playable model's facial animation. The story hair shares its bones with the playable hair except for three bangs chains; those are attached to the head bone the same way and have no physics of their own.
- **Restoring**: switching an option or the module off puts back only what this module changed.
- **Crash guard**: before a face swap the module writes a marker file and removes it a few seconds after the new face has been drawn. If the game crashes in that window, the next launch only hides the mask. Turning the face swap off and on again retries it. The marker is in `%LOCALAPPDATA%\zhuran24.endmin-unmasked\`.

The source is `src/endmin_mask.c` (mask hiding, module entry points) and `src/official_head.inl` (face swap).

## Building

Requires clang, lld, wine (for its Windows headers and PE import libraries) and zip.

```bash
./build_eml.sh
```

The package is written to `dist-eml/endmin-unmasked-<version>.zip`: `module.json` at the root, the DLL in `native/windows-x64/`, the settings page in `ui/`. If wine's headers are not in `/usr/include/wine` or its PE import libraries are not in `/usr/lib/wine/x86_64-windows`, set `WINE_INCLUDE` and `WINE_PE_LIB`.

`build.sh` builds the same source as a Better Endfield 3.5.x built-in module (`local.endmin_mask`, configured through an INI section). It is used for development and is not released.

## Testing

`eml/test/run.sh` loads the DLL into a fake third-party host (`eml/test/fake_host.c`) in a throwaway wine prefix under Xvfb. It checks initialisation, configuration, status messages, shutdown and re-initialisation without the game. Run `./build_eml.sh` first; it also needs `xvfb-run`.

## License

AGPL-3.0-only, see [LICENSE](LICENSE). The headers in `third_party/eml-sdk/` are taken unchanged from Endfield Mod Loader, also AGPL-3.0-only ([NOTICE](third_party/eml-sdk/NOTICE.md)).

Written with AI assistance (Claude Code and Codex). The in-game results were checked by the author.

## Credits

- Hypergryph: the story face is the game's own official model from *Arknights: Endfield*. It is loaded from the installed game at runtime; no game assets are included here.
- Dr-hydra, for Better Endfield and Endfield Mod Loader, whose module interface this is built on.
- EFTool, whose HideEndminMask showed how to hide the mask.

---

# 管理员去面罩

《明日方舟：终末地》的 [Endfield Mod Loader](https://github.com/Dr-hydra/Endfield-Mod-Loader)（EML）模块：隐藏管理员的面罩，女管理员另换上游戏自带的无面罩的脸和头发，也就是剧情里摘下面罩时用的模型。

下载：[N 网](https://www.nexusmods.com/arknightendfield/mods/75)

## 功能

- 隐藏管理员的面罩，男女管理员都适用，大世界、角色界面和剧情里都生效。
- 换上剧情里摘下面罩时用的脸和头发，只做了女管理员：脸、眉毛、眼线睫毛、眼珠、眼白阴影、头发和刘海投影。身体和衣服不动。剧情版的刘海跟着头动，侧面和后面的头发照旧由可玩模型的头发物理带动。

两个开关都在 EML 的模块页面里，改了在游戏里马上生效。打开换脸会一并打开隐藏面罩，关掉隐藏面罩会一并关掉换脸。

在国服 1.6.3 实测过，用的是 Better Endfield 3.5.4 的第三方模块加载器，EML 的加载器就是从它拆出来的。还没在 EML 里实际跑过。

## 原理

- **隐藏面罩**：挂 `Transform.SetParent` 和 `GameObject.SetActive` 的引擎实现。管理员模型挂到父节点上时，把面罩部件（`S_actor_endmin{f,m}_cloth_03/05`，各级 LOD）关掉；之后对它们的 `SetActive(true)` 一律改成 `SetActive(false)`。开关变化在主线程处理，挂在 `EventSystem.Update` 的帧泵上。
- **换脸**：从装好的游戏里加载自带的无面罩预制体 `chr_0003_endminfunmasked_postmodel`，脸部和头发的网格、材质原样复用，用辅助骨（局部变换 = 可玩 bindpose × 剧情版 bindpose⁻¹）接到可玩骨架上，所以表情和眨眼仍由可玩模型的脸部动画带动。剧情版头发除了三串刘海骨都和可玩模型的头发共用骨骼；刘海骨同样挂在头骨下，没有自己的物理。
- **还原**：关掉开关或停用模块时，只把本模块改过的东西换回去。
- **崩溃保险**：换脸前写一个标记文件，新脸画出来几秒后删掉。游戏在这几秒里崩了的话，下次启动只隐藏面罩；把换脸关掉再打开就会重试。标记文件在 `%LOCALAPPDATA%\zhuran24.endmin-unmasked\`。

源码是 `src/endmin_mask.c`（隐藏面罩、模块入口）和 `src/official_head.inl`（换脸）。

## 构建

需要 clang、lld、wine（用它带的 Windows 头文件和 PE 导入库）和 zip。

```bash
./build_eml.sh
```

包输出到 `dist-eml/endmin-unmasked-<版本>.zip`：根目录是 `module.json`，DLL 在 `native/windows-x64/`，设置页在 `ui/`。wine 的头文件不在 `/usr/include/wine`、PE 导入库不在 `/usr/lib/wine/x86_64-windows` 时，用 `WINE_INCLUDE`、`WINE_PE_LIB` 指定。

`build.sh` 把同一份源码编成 Better Endfield 3.5.x 的内置模块（`local.endmin_mask`，配置写在 INI 段里），开发时用，不发布。

## 测试

`eml/test/run.sh` 在一次性的 wine 前缀和 Xvfb 里，用假的第三方宿主（`eml/test/fake_host.c`）加载 DLL，不进游戏，测初始化、改配置、要状态、停用和重新初始化。先跑 `./build_eml.sh`，另外需要 `xvfb-run`。

## 许可

AGPL-3.0-only，见 [LICENSE](LICENSE)。`third_party/eml-sdk/` 里的头文件原样取自 Endfield Mod Loader，同样是 AGPL-3.0-only（[来源说明](third_party/eml-sdk/NOTICE.md)）。

代码和说明由 AI（Claude Code、Codex）协助编写，游戏里的效果由作者实测确认。

## 致谢

- 鹰角网络（Hypergryph）：剧情版的脸是游戏自带的官方模型，运行时从装好的游戏里加载，本仓库不含任何游戏资源。
- Dr-hydra：Better Endfield 和 Endfield Mod Loader，这个模块用的就是它们的模块接口。
- EFTool：隐藏面罩的做法最早来自它的 HideEndminMask。
