/*
 * SPDX-License-Identifier: AGPL-3.0-only
 * 离线测 EML 版模块的入口：不进游戏，用假的第三方宿主加载 DLL，依次调初始化、改配置（JSON 和
 * 内置版用的 INI 两种写法）、要状态、停用、再初始化、拿错 ID 初始化，结果打到 stderr。
 * 游戏辅助接口一直不给（get_runtime 返回 NULL），模块应停在 waiting_runtime，不碰任何游戏地址。
 * 用法见同目录 run.sh。
 */
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "BetterEndfield/ThirdPartyModule.h"

#define MODULE_ID "zhuran24.endmin-unmasked"

static HANDLE g_out;
static int g_failures;
static char g_last_reply[1024];

static void Say(const char* format, ...) {
    char buffer[1200];
    DWORD written;
    int length;
    va_list args;
    va_start(args, format);
    length = vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (length > 0) {
        WriteFile(g_out, buffer, (DWORD)length, &written, NULL);
    }
}

static void Expect(int condition, const char* what) {
    Say("%s %s\n", condition ? "PASS" : "FAIL", what);
    if (!condition) {
        ++g_failures;
    }
}

static void BE_CALL HostLog(void* context, const char* message) {
    (void)context;
    Say("  [log] %s\n", message);
}

static BE_Result BE_CALL HostReply(void* context, const char* request_id, BE_Result result,
    const char* json) {
    (void)context;
    Say("  [reply %s %d] %s\n", request_id, (int)result, json);
    lstrcpynA(g_last_reply, json, sizeof(g_last_reply));
    return BE_Result_Ok;
}

static BE_Result BE_CALL HostEmit(void* context, const char* json) {
    (void)context;
    Say("  [emit] %s\n", json);
    return BE_Result_Ok;
}

static const BE_HostApiV1* BE_CALL HostGetRuntime(void* context) {
    (void)context;
    return NULL;
}

static int ReplyHas(const char* fragment) {
    return strstr(g_last_reply, fragment) != NULL;
}

int Start(void) {
    HMODULE dll;
    BE_GetThirdPartyModuleV1Fn get;
    const BE_ThirdPartyModuleV1* module;
    BE_ThirdPartyHostV1 host;

    g_out = GetStdHandle(STD_ERROR_HANDLE);
    dll = LoadLibraryW(L"" MODULE_ID ".dll");
    Expect(dll != NULL, "LoadLibrary");
    if (!dll) {
        ExitProcess(1);
    }
    get = (BE_GetThirdPartyModuleV1Fn)(void*)GetProcAddress(dll, BETTER_ENDFIELD_THIRD_PARTY_ENTRY_V1);
    Expect(get != NULL, "entry " BETTER_ENDFIELD_THIRD_PARTY_ENTRY_V1 " exported");
    if (!get) {
        ExitProcess(1);
    }
    module = get();
    Expect(module && module->struct_size == sizeof(BE_ThirdPartyModuleV1) && module->version == 1,
        "module struct size and version");
    Expect(module && module->id && strcmp(module->id, MODULE_ID) == 0, "module id matches manifest");
    Expect(module && module->initialize && module->on_message, "required callbacks present");

    ZeroMemory(&host, sizeof(host));
    host.struct_size = sizeof(host);
    host.version = 1;
    host.context = &host;
    host.module_id = MODULE_ID;
    host.platform = "windows-x64";
    host.package_root = "C:\\fake-package";
    host.log = HostLog;
    host.reply = HostReply;
    host.emit = HostEmit;
    host.get_runtime = HostGetRuntime;

    Expect(module->initialize(&host, "{\"hide_mask\":true,\"official_face\":false}") == BE_Result_Ok,
        "initialize with JSON configuration");
    Expect(module->initialize(&host, "{}") == BE_Result_Conflict, "second initialize is rejected");
    Expect(module->on_message("r1", "{\"action\":\"status\"}") == BE_Result_Ok, "status message");
    Expect(ReplyHas("\"phase\":\"waiting_runtime\"") && ReplyHas("\"hide_mask\":true") &&
            ReplyHas("\"official_face\":false"), "status reflects initial JSON, waiting for runtime");

    Expect(module->configuration_changed("{\n  \"hide_mask\" : false,\n  \"official_face\": true\n}") ==
            BE_Result_Ok, "configuration_changed with spaced JSON");
    module->on_message("r2", "{\"action\":\"status\"}");
    Expect(ReplyHas("\"hide_mask\":false") && ReplyHas("\"official_face\":true"),
        "status reflects changed JSON");

    module->configuration_changed("enabled=true\r\nhide_mask=true\r\nofficial_face=false\r\n");
    module->on_message("r3", "{}");
    Expect(ReplyHas("\"hide_mask\":true") && ReplyHas("\"official_face\":false"),
        "INI-style lines (built-in module format) parse the same way");

    Sleep(300); /* 等待线程这时应在空转等 runtime */
    module->shutdown();
    Say("shutdown returned\n");
    Expect(module->on_message("r4", "{}") == BE_Result_NotReady, "messages refused after shutdown");

    Expect(module->initialize(&host, "{}") == BE_Result_Ok, "initialize again after shutdown");
    module->on_message("r5", "{}");
    Expect(ReplyHas("\"hide_mask\":true") && ReplyHas("\"official_face\":true"),
        "empty JSON falls back to defaults (both on)");
    module->shutdown();

    host.module_id = "someone.else";
    Expect(module->initialize(&host, "{}") == BE_Result_ContractMismatch,
        "initialize with a different module id is rejected");

    Say("%s: %d failure(s)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    ExitProcess(g_failures ? 1 : 0);
    return 0;
}
