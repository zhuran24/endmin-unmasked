/*
 * 管理员去面罩（EML 模块 zhuran24.endmin-unmasked / BE 内置模块 local.endmin_mask）。
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * 隐藏面罩沿用 EFTool 的 HideEndminMask 的做法：
 *   - 面罩是四个 GameObject：S_actor_endmin{f,m}_cloth_{03,05}，按名字子串匹配；
 *   - 挂 Transform.SetParent：管理员模型（chr_0002/0003_endmin…postmodel/uimodel）挂上父节点、
 *     或有对象挂到 CutsceneRoot 下时，从顶层根把整棵树扫一遍，关掉面罩（各级 LOD 都关）；
 *   - 拦截 GameObject.SetActive：开关打开时，对这四个对象的 SetActive(true) 改成 false
 *     （游戏切 LOD 时会这样重新打开 lod0 的面罩）；
 *   - 开关变化时扫描 CharContainer / CutsceneRoot / 登录桌面角色，主动隐藏或恢复；
 *   - 另有每 5 秒的指纹检查兜底。
 * 实机确认过角色生成时游戏确实走 SetParent。
 * 与 EFTool 不同的一处：恢复时只把"被本模块压下去、而游戏原本要显示"的对象重新打开，
 * 不把四个部件一律设成显示（03/05 可能是同一件衣服的不同变体，由游戏决定显示哪个）。
 * 1.4.0 起女管理员去面罩时另换上官方无面罩的头（official_head.inl），面罩那两格改放眼线和
 * 刘海投影，不再隐藏；男管理员仍只隐藏面罩。
 *
 * 线程：initialize / configuration_changed 在 Host 工作线程；Unity 调用一律放在
 * 主线程（EventSystem.Update 帧泵、SetActive 拦截）里做。会抛托管异常的调用都走
 * Host 的 runtime_invoke，异常在 Host 里接住，不穿过本模块的栈帧。
 *
 * 同一份源码编两种：默认是 BE 3.5.x 的内置模块（*.module.ini、配置是 INI 段，build.sh）；
 * 定义 EML_MODULE 时是第三方模块（Endfield Mod Loader 与 BE 3.5.x 的 format 1 / ABI 1 包，
 * 配置是 JSON，游戏接口在 initialize 之后才就绪，build_eml.sh），入口在文件末尾。
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "BetterEndfield/ModuleApi.h"

#ifdef EML_MODULE
#include "BetterEndfield/ThirdPartyModule.h"
#define MODULE_ID "zhuran24.endmin-unmasked"
#else
#define MODULE_ID "local.endmin_mask"
#endif
#define MODULE_VERSION "1.4.1"

static const char* const kMaskNames[] = {
    "S_actor_endminf_cloth_03",
    "S_actor_endminf_cloth_05",
    "S_actor_endminm_cloth_03",
    "S_actor_endminm_cloth_05",
};

/* IL2CPP 托管数组：klass, monitor, bounds, max_length，元素从 0x20 开始。 */
#define IL2CPP_ARRAY_LENGTH_OFFSET 0x18
#define IL2CPP_ARRAY_DATA_OFFSET 0x20

#define MAX_TRACKED 128

typedef void (*SetActiveFn)(void* self, uint8_t value, const void* method);
typedef void (*UpdateFn)(void* self, const void* method);
typedef void (*SetParentFn)(void* self, void* parent, uint8_t world_position_stays, const void* method);
typedef void* (*ResolveIcallFn)(const char* name);
typedef void* (*GcHandleGetTargetFn)(uint32_t handle);

static const BE_HostApiV1* g_host; /* 游戏辅助接口；EML 版要等它就绪才有 */
#ifdef EML_MODULE
static const BE_ThirdPartyHostV1* volatile g_bridge; /* 生命周期上下文，停用后不再用 */
static volatile LONG g_bootstrap_stop;               /* 开始停用了：等待线程和 Hook 都看它 */
static SRWLOCK g_hook_gate = SRWLOCK_INIT;           /* 「看停用标志＋装钩子」与「置停用标志」互斥 */
#endif

static const void* g_mi_get_name;
static const void* g_mi_find;
static const void* g_mi_get_transform;
static const void* g_mi_get_game_object;
static const void* g_mi_get_parent;
static const void* g_mi_get_components;
static const void* g_mi_get_active_self;
static const void* g_mi_set_active;
static const void* g_mi_find_objects;
static const void* g_mi_active_in_hierarchy;
static void* g_transform_type;
static int32_t g_cached_ptr_offset = -1;
static GcHandleGetTargetFn g_gchandle_get_target;

static SetActiveFn g_original_set_active;
static UpdateFn g_original_event_update;
static SetParentFn g_original_set_parent;
static volatile LONG g_parent_triggers;
static int g_pump_in_set_active; /* EventSystem.Update 挂不上时退回 SetActive 里泵 */

/* 工作线程写、主线程读。 */
static volatile LONG g_hide_requested;
static volatile LONG g_rescan_requested;
static volatile LONG g_dump_requested;
/* EML 版停用时工作线程请求整套还原，主线程帧泵做完后回执同一个序号（内置版一直是 0）。 */
static volatile LONG g_restore_request;
static volatile LONG g_restore_done;

/* 以下只在主线程访问。 */
static LONG g_hide_applied;
static int g_in_pump;
static int g_self_call;
static uint32_t g_tracked[MAX_TRACKED];
static int g_tracked_count;
static volatile LONG g_intercepts;

static void Log(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (g_host && g_host->log) {
        g_host->log(g_host->context, MODULE_ID, buffer);
        return;
    }
#ifdef EML_MODULE
    { /* 游戏接口就绪前（等待线程、生命周期回调里）用宿主给本模块的日志 */
        const BE_ThirdPartyHostV1* bridge = g_bridge;
        if (bridge && bridge->log) {
            bridge->log(bridge->context, buffer);
        }
    }
#endif
}

static void* Invoke(const void* method, void* instance, void** params) {
    void* exception = NULL;
    void* result;
    if (!method || !g_host || !g_host->runtime_invoke) {
        return NULL;
    }
    result = g_host->runtime_invoke(g_host->context, method, instance, params, &exception);
    return exception ? NULL : result;
}

static int Alive(void* object) {
    if (!object) {
        return 0;
    }
    if (g_cached_ptr_offset < 0) {
        return 1;
    }
    return *(void* volatile*)((char*)object + g_cached_ptr_offset) != NULL;
}

static int ObjectName(void* object, char* buffer, size_t size) {
    void* managed;
    if (!Alive(object) || !g_host->copy_managed_string) {
        return 0;
    }
    managed = Invoke(g_mi_get_name, object, NULL);
    if (!managed) {
        return 0;
    }
    buffer[0] = '\0';
    return g_host->copy_managed_string(g_host->context, managed, buffer, size) > 0;
}

static int IsMaskName(const char* name) {
    size_t index;
    if (strncmp(name, "S_actor_endmin", 14) != 0 && !strstr(name, "S_actor_endmin")) {
        return 0;
    }
    for (index = 0; index < sizeof(kMaskNames) / sizeof(kMaskNames[0]); ++index) {
        if (strstr(name, kMaskNames[index])) {
            return 1;
        }
    }
    return 0;
}

static void* TrackedTarget(int index) {
    return g_gchandle_get_target ? g_gchandle_get_target(g_tracked[index]) : NULL;
}

static void DropTracked(int index) {
    g_host->gchandle_free(g_host->context, g_tracked[index]);
    g_tracked[index] = g_tracked[--g_tracked_count];
}

static void PruneTracked(void) {
    int index = 0;
    while (index < g_tracked_count) {
        if (!Alive(TrackedTarget(index))) {
            DropTracked(index);
        } else {
            ++index;
        }
    }
}

/* 记下"游戏要显示、被本模块压下去"的对象，关闭功能时只恢复这些。 */
static void Track(void* object) {
    int index;
    if (!g_gchandle_get_target || !g_host->gchandle_new) {
        return;
    }
    for (index = 0; index < g_tracked_count; ++index) {
        if (TrackedTarget(index) == object) {
            return;
        }
    }
    if (g_tracked_count == MAX_TRACKED) {
        PruneTracked();
    }
    if (g_tracked_count < MAX_TRACKED) {
        g_tracked[g_tracked_count++] = g_host->gchandle_new(g_host->context, object, 0);
    }
}

static void Untrack(void* object) {
    int index;
    for (index = 0; index < g_tracked_count; ++index) {
        if (TrackedTarget(index) == object) {
            DropTracked(index);
            return;
        }
    }
}

static int TrackedIndex(void* object) {
    int index;
    for (index = 0; index < g_tracked_count; ++index) {
        if (TrackedTarget(index) == object) {
            return index;
        }
    }
    return -1;
}

static void SetActive(void* game_object, uint8_t value) {
    void* params[1] = {&value};
    g_self_call = 1;
    Invoke(g_mi_set_active, game_object, params);
    g_self_call = 0;
}

static int ActiveSelf(void* game_object) {
    void* boxed = Invoke(g_mi_get_active_self, game_object, NULL);
    void* raw = boxed ? g_host->object_unbox(g_host->context, boxed) : NULL;
    return raw ? *(uint8_t*)raw != 0 : 0;
}

static void* Find(const char* path) {
    void* params[1];
    params[0] = g_host->string_new(g_host->context, path);
    return params[0] ? Invoke(g_mi_find, NULL, params) : NULL;
}

static void* TopGameObject(void* game_object) {
    void* transform = Invoke(g_mi_get_transform, game_object, NULL);
    void* parent;
    int depth = 0;
    while (transform && depth++ < 64) {
        parent = Invoke(g_mi_get_parent, transform, NULL);
        if (!parent) {
            break;
        }
        transform = parent;
    }
    return transform ? Invoke(g_mi_get_game_object, transform, NULL) : NULL;
}

static double Milliseconds(const LARGE_INTEGER* start) {
    LARGE_INTEGER end, frequency;
    QueryPerformanceCounter(&end);
    QueryPerformanceFrequency(&frequency);
    return (double)(end.QuadPart - start->QuadPart) * 1000.0 / (double)frequency.QuadPart;
}

static int StartsWith(const char* text, const char* prefix) {
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

static int ResolveMethod(const char* assembly, const char* namespace_name, const char* class_name,
    const char* method_name, const char* parameter_types, const char* return_type,
    uint32_t parameter_count, BE_ResolvedMethodV1* out);

#include "official_head.inl"

/* 兜底的定时检查：新角色的面罩在预制体里本来就是激活的，游戏不会对它调 SetActive(true)。
   主力是下面的 SetParent 触发（模型挂上父节点那一刻就扫）；这里每 5 秒对每个根取一次
   全部 Transform（引擎内部收集，很便宜），用"数量 + 指针和"做指纹，变了才逐个查名字，
   防的是不经 SetParent 出现的模型。 */
typedef struct ScanRoot {
    const char* path;
    int climb_to_top; /* 登录桌面：从身体对象上溯到顶层再扫 */
    uint32_t handle;
    uintptr_t length;
    uintptr_t checksum;
    ULONGLONG next_find;
} ScanRoot;

static ScanRoot g_roots[] = {
    {"CharContainer", 0, 0, 0, 0, 0},
    {"CutsceneRoot", 0, 0, 0, 0, 0},
    {"SP_Desktop/S_actor_endminf_body_01_shadowProxyDesktop", 1, 0, 0, 0, 0},
    {"SP_Desktop/S_actor_endminm_body_01_shadowProxyDesktop", 1, 0, 0, 0, 0},
};
#define ROOT_COUNT (sizeof(g_roots) / sizeof(g_roots[0]))
#define CHECK_INTERVAL_MS 5000
#define FIND_INTERVAL_MS 3000

static ULONGLONG g_next_check;
static unsigned g_stat_checks;
static unsigned g_stat_scans;
static double g_stat_worst_ms;
static ULONGLONG g_stat_since;

static void ForgetRoot(ScanRoot* root) {
    if (root->handle) {
        g_host->gchandle_free(g_host->context, root->handle);
    }
    root->handle = 0;
    root->length = 0;
    root->checksum = 0;
}

/* GameObject.Find 只返回激活的对象，且同名时只给一个；切场景时可能出现新的同名根。
   所以不长期信任缓存：强制扫描时、以及每 3 秒，都重新 Find 一次，结果变了就换根。 */
static void* RootObject(ScanRoot* root, ULONGLONG now, int force_find) {
    void* cached = root->handle && g_gchandle_get_target ? g_gchandle_get_target(root->handle) : NULL;
    void* found;
    if (!Alive(cached)) {
        cached = NULL;
        ForgetRoot(root);
    }
    if (!force_find && now < root->next_find) {
        return cached;
    }
    root->next_find = now + FIND_INTERVAL_MS;
    found = Find(root->path);
    if (found && root->climb_to_top) {
        found = TopGameObject(found);
    }
    if (!Alive(found)) {
        ForgetRoot(root);
        return NULL;
    }
    if (found != cached) {
        if (cached) {
            Log("root %s changed", root->path);
        }
        ForgetRoot(root);
        if (g_gchandle_get_target) {
            root->handle = g_host->gchandle_new(g_host->context, found, 0);
        }
    }
    return found;
}

/* 返回隐藏的数量；指纹没变且不强制时返回 -1（没扫）。 */
static int HideUnder(ScanRoot* root, void* root_game_object, int force) {
    uint8_t include_inactive = 1;
    void* params[2] = {g_transform_type, &include_inactive};
    void* array;
    void** items;
    uintptr_t length;
    uintptr_t checksum = 0;
    uintptr_t index;
    int hidden = 0;
    char name[256];

    array = Invoke(g_mi_get_components, root_game_object, params);
    if (!array) {
        return -1;
    }
    length = *(uintptr_t*)((char*)array + IL2CPP_ARRAY_LENGTH_OFFSET);
    items = (void**)((char*)array + IL2CPP_ARRAY_DATA_OFFSET);
    for (index = 0; index < length; ++index) {
        uintptr_t value = (uintptr_t)items[index];
        checksum += (value ^ (value >> 7)) * 0x9E3779B97F4A7C15ull;
    }
    if (root) {
        if (!force && length == root->length && checksum == root->checksum) {
            return -1;
        }
        root->length = length;
        root->checksum = checksum;
    }
    for (index = 0; index < length; ++index) {
        void* transform = items[index];
        void* game_object;
        if (!ObjectName(transform, name, sizeof(name))) {
            continue;
        }
        /* 看到女管理员的脸，就把它所在的模型排进换头队列；换不换由 Pump 按那一帧的开关定。 */
        if (strcmp(name, "S_actor_endminf_face_01_lod0") == 0 && g_head.ready) {
            QueueHeadRoot(ModelRootOf(transform));
        }
        if (!IsMaskName(name)) {
            continue;
        }
        game_object = Invoke(g_mi_get_game_object, transform, NULL);
        if (!Alive(game_object) || !ActiveSelf(game_object) || IsRepurposed(game_object)) {
            continue;
        }
        Track(game_object);
        SetActive(game_object, 0);
        if (++hidden <= 8) {
            Log("hid %s", name);
        }
    }
    return hidden;
}

static void ScanRoots(int force) {
    LARGE_INTEGER start;
    ULONGLONG now = GetTickCount64();
    int hidden = 0;
    int scanned = 0;
    size_t index;
    double elapsed;

    QueryPerformanceCounter(&start);
    for (index = 0; index < ROOT_COUNT; ++index) {
        void* object = RootObject(&g_roots[index], now, force);
        int result;
        if (!object) {
            continue;
        }
        result = HideUnder(&g_roots[index], object, force);
        if (result >= 0) {
            ++scanned;
            hidden += result;
        }
    }
    elapsed = Milliseconds(&start);

    if (!g_stat_since) {
        g_stat_since = now;
    }
    ++g_stat_checks;
    if (scanned) {
        ++g_stat_scans;
    }
    if (elapsed > g_stat_worst_ms) {
        g_stat_worst_ms = elapsed;
    }
    if (force || hidden) {
        Log("scan of %d root(s) hid %d object(s) in %.1f ms; tracked=%d",
            scanned, hidden, elapsed, g_tracked_count);
    }
    if (now - g_stat_since >= 300000) {
        Log("last 5 min: %u checks, %u rescans, worst %.1f ms", g_stat_checks, g_stat_scans,
            g_stat_worst_ms);
        g_stat_since = now;
        g_stat_checks = g_stat_scans = 0;
        g_stat_worst_ms = 0;
    }
}

static void PathOf(void* transform, char* out, size_t size) {
    char parts[8][96];
    int count = 0;
    int index;
    size_t used = 0;
    while (transform && count < 8) {
        if (!ObjectName(transform, parts[count], sizeof(parts[count]))) {
            break;
        }
        ++count;
        transform = Invoke(g_mi_get_parent, transform, NULL);
    }
    out[0] = '\0';
    for (index = count - 1; index >= 0 && used + 2 < size; --index) {
        int written = snprintf(out + used, size - used, "%s%s", parts[index], index ? "/" : "");
        if (written < 0) {
            break;
        }
        used += (size_t)written;
    }
    (void)transform; /* 超过 8 层的祖先不记 */
}

static void DumpScene(void) {
    uint8_t include_inactive = 1;
    void* params[2] = {g_transform_type, &include_inactive};
    void* array;
    uintptr_t length, index;
    int listed = 0;
    char name[256];
    char path[512];
    if (!g_mi_find_objects) {
        Log("dump: FindObjectsOfType unavailable");
        return;
    }
    array = Invoke(g_mi_find_objects, NULL, params);
    if (!array) {
        Log("dump: FindObjectsOfType failed");
        return;
    }
    length = *(uintptr_t*)((char*)array + IL2CPP_ARRAY_LENGTH_OFFSET);
    for (index = 0; index < length && listed < 40; ++index) {
        void* transform = ((void**)((char*)array + IL2CPP_ARRAY_DATA_OFFSET))[index];
        void* game_object;
        void* boxed;
        void* raw;
        int in_hierarchy = -1;
        if (!ObjectName(transform, name, sizeof(name)) || !strstr(name, "endmin") ||
            (!strstr(name, "cloth") && !strstr(name, "mask") && !strstr(name, "face"))) {
            continue;
        }
        game_object = Invoke(g_mi_get_game_object, transform, NULL);
        boxed = g_mi_active_in_hierarchy ? Invoke(g_mi_active_in_hierarchy, game_object, NULL) : NULL;
        raw = boxed ? g_host->object_unbox(g_host->context, boxed) : NULL;
        if (raw) {
            in_hierarchy = *(uint8_t*)raw != 0;
        }
        PathOf(transform, path, sizeof(path));
        Log("dump: self=%d hierarchy=%d %s", ActiveSelf(game_object), in_hierarchy, path);
        ++listed;
    }
    Log("dump: %d match(es) among %u transforms", listed, (unsigned)length);
}

static void RestoreAll(void) {
    int restored = 0;
    while (g_tracked_count > 0) {
        void* object = TrackedTarget(g_tracked_count - 1);
        if (Alive(object)) {
            SetActive(object, 1);
            ++restored;
        }
        g_host->gchandle_free(g_host->context, g_tracked[--g_tracked_count]);
    }
    Log("restored %d object(s)", restored);
}

/* EFTool 的生成时机：游戏把角色模型挂到父节点时，子对象名以 chr_0002_endmin / chr_0003_endmin
   开头且含 postmodel（大世界模型）或 uimodel（界面模型），或者父节点就叫 CutsceneRoot，
   就在挂完后从顶层根把整棵树扫一遍。 */
static int IsEndminModel(const char* name) {
    return (StartsWith(name, "chr_0002_endmin") || StartsWith(name, "chr_0003_endmin")) &&
        (strstr(name, "postmodel") || strstr(name, "uimodel"));
}

static void DetourSetParent(void* self, void* parent, uint8_t stays, const void* method) {
    char name[256];
    char parent_name[64];
    void* top;
    int reason = 0;
    int hidden;
    LONG count;

    g_original_set_parent(self, parent, stays, method);
    if (g_self_call || g_in_pump || !g_hide_requested || !self) {
        return;
    }
    if (ObjectName(self, name, sizeof(name)) && IsEndminModel(name)) {
        reason = 1;
    } else if (parent && ObjectName(parent, parent_name, sizeof(parent_name)) &&
               strcmp(parent_name, "CutsceneRoot") == 0) {
        reason = 2;
    }
    if (!reason) {
        return;
    }
    g_in_pump = 1;
    top = TopGameObject(Invoke(g_mi_get_game_object, self, NULL));
    hidden = top ? HideUnder(NULL, top, 1) : 0;
    g_in_pump = 0;
    count = InterlockedIncrement(&g_parent_triggers);
    if (count <= 24) {
        Log("SetParent trigger (%s): %s; hid %d object(s)",
            reason == 1 ? "endmin model" : "CutsceneRoot child", reason == 1 ? name : "-", hidden);
    }
}

/* 停用前的整套还原（EmlShutdown 在等回执）。之后钩子就直通了，没人再去删还没到时间的崩溃保险标记，
   这里一并删掉，否则下一局会当成换头时崩过。 */
static void RestoreForShutdown(void) {
    LONG request = g_restore_request;
    g_in_pump = 1;
    DropHeadQueue();
    UnswapAllHeads();
    ClearRepurposed();
    RestoreAll();
    g_hide_applied = 0;
    if (g_guard_clear_at) {
        GuardClear();
    }
    g_in_pump = 0;
    InterlockedExchange(&g_restore_done, request);
}

static void Pump(void) {
    LONG want;
    LONG face;
    ULONGLONG now;
    if (g_in_pump || g_self_call) {
        return;
    }
    GuardPump();
    if (g_restore_request != g_restore_done) {
        RestoreForShutdown();
        return;
    }
    if (g_dump_requested) {
        InterlockedExchange(&g_dump_requested, 0);
        g_in_pump = 1;
        DumpScene();
        g_in_pump = 0;
    }
    want = g_hide_requested;
    face = g_official_face_requested; /* 这一帧（含换头队列）都按这个值做；中途变了下一帧再补 */
    if (want != g_hide_applied || g_rescan_requested) {
        g_in_pump = 1;
        InterlockedExchange(&g_rescan_requested, 0);
        if (want) {
            if (!face && (g_swap_count || g_repurposed_count)) {
                DropHeadQueue();
                UnswapAllHeads();
                HideRepurposed();
            }
            ScanRoots(1);
            ProcessHeadQueue(face);
        } else {
            /* 先把头换回原样（面罩那两格就又是面罩了），再放出被压下去的面罩。 */
            DropHeadQueue();
            UnswapAllHeads();
            ClearRepurposed();
            RestoreAll();
        }
        g_hide_applied = want;
        g_official_face_applied = face;
        g_next_check = GetTickCount64() + CHECK_INTERVAL_MS;
        g_in_pump = 0;
        return;
    }
    if (!want) {
        return;
    }
    if (face != g_official_face_applied) {
        g_in_pump = 1;
        g_official_face_applied = face;
        if (face) {
            ScanRoots(1);
        } else {
            DropHeadQueue();
            UnswapAllHeads();
            HideRepurposed();
        }
        g_in_pump = 0;
    }
    if (g_head_queue_count || g_swap_count) {
        g_in_pump = 1;
        ProcessHeadQueue(face);
        WatchHeads();
        g_in_pump = 0;
    }
    now = GetTickCount64();
    if (now < g_next_check) {
        return;
    }
    g_next_check = now + CHECK_INTERVAL_MS;
    g_in_pump = 1;
    ScanRoots(0);
    g_in_pump = 0;
}

static void DetourSetActive(void* self, uint8_t value, const void* method) {
    if (!g_self_call && self && g_hide_requested) {
        if (value) {
            char name[256];
            /* 换上官方头以后，面罩那两格放的是眼线和刘海投影，游戏开它们不拦。 */
            if (ObjectName(self, name, sizeof(name)) && IsMaskName(name) && !IsRepurposed(self)) {
                Track(self);
                value = 0;
                if (InterlockedIncrement(&g_intercepts) <= 16) {
                    Log("blocked SetActive(true) on %s", name);
                }
            }
        } else if (g_tracked_count > 0) {
            /* 游戏自己要隐藏它：以后关闭功能时不该再把它打开。 */
            Untrack(self);
        }
    }
    g_original_set_active(self, value, method);
    if (g_pump_in_set_active) {
        Pump();
    }
}

static void DetourEventSystemUpdate(void* self, const void* method) {
    g_original_event_update(self, method);
    Pump();
}

static int ResolveMethod(const char* assembly, const char* namespace_name, const char* class_name,
    const char* method_name, const char* parameter_types, const char* return_type,
    uint32_t parameter_count, BE_ResolvedMethodV1* out) {
    BE_MethodDescriptorV1 descriptor;
    descriptor.assembly_name = assembly;
    descriptor.namespace_name = namespace_name;
    descriptor.class_name = class_name;
    descriptor.method_name = method_name;
    descriptor.parameter_types = parameter_types;
    descriptor.return_type = return_type;
    descriptor.parameter_count = parameter_count;
    memset(out, 0, sizeof(*out));
    if (g_host->resolve_method(g_host->context, &descriptor, out) != BE_Result_Ok ||
        !out->method_info) {
        Log("unresolved: %s.%s.%s", namespace_name, class_name, method_name);
        return 0;
    }
    return 1;
}

/* 挂一个钩子。宿主在启用钩子之前就把原函数写进 *original（detour 一进来就要用），所以第一次直接传
   全局变量。EML 停用再启用会重新跑 Setup：宿主把停用的节点原地恢复、原函数不变，这时先写进局部变量，
   免得注册失败时宿主把输出清零，而还没退出的 detour 正要用它；节点还活着就返回 Conflict，原函数照旧。 */
static int HookNow(const BE_HostApiV1* host, void* target, void* detour, void** original) {
    void* fresh = NULL;
    BE_Result result;
    if (!*original) {
        result = host->create_hook(host->context, MODULE_ID, target, detour, original);
        return result == BE_Result_Ok && *original != NULL;
    }
    result = host->create_hook(host->context, MODULE_ID, target, detour, &fresh);
    if (result == BE_Result_Ok && fresh) {
        *original = fresh;
        return 1;
    }
    return result == BE_Result_Conflict;
}

/* EML 版停用一开始就不再装新钩子；正在装的装完了停用才往下走，宿主随后停用钩子时它们都已在链上。 */
static int Hook(const BE_HostApiV1* host, void* target, void* detour, void** original) {
#ifdef EML_MODULE
    int ok;
    AcquireSRWLockExclusive(&g_hook_gate);
    ok = !g_bootstrap_stop && HookNow(host, target, detour, original);
    ReleaseSRWLockExclusive(&g_hook_gate);
    return ok;
#else
    return HookNow(host, target, detour, original);
#endif
}

/* 解析契约、挂钩子。要在挂到 IL2CPP 的线程上调（内置模块是 Host 工作线程，EML 版是等待线程）。 */
static BE_Result Setup(const BE_HostApiV1* host) {
    BE_ResolvedMethodV1 method;
    BE_ResolvedFieldV1 field;
    BE_FieldDescriptorV1 field_descriptor;
    BE_ResolvedClassV1 transform_class;
    HMODULE game_assembly;
    ResolveIcallFn resolve_icall;
    void* set_active_target = NULL;
    const char* core = "UnityEngine.CoreModule.dll";

    if (!host || host->abi_version != BETTER_ENDFIELD_MODULE_ABI_V1 || !host->resolve_method ||
        !host->create_hook || !host->runtime_invoke || !host->string_new ||
        !host->object_unbox || !host->gchandle_new || !host->gchandle_free) {
        return BE_Result_ContractMismatch;
    }
    g_host = host;

    if (!ResolveMethod(core, "UnityEngine", "Object", "get_name", NULL, "System.String", 0, &method))
        return BE_Result_ContractMismatch;
    g_mi_get_name = method.method_info;
    if (!ResolveMethod(core, "UnityEngine", "GameObject", "Find", "System.String",
            "UnityEngine.GameObject", 1, &method))
        return BE_Result_ContractMismatch;
    g_mi_find = method.method_info;
    if (!ResolveMethod(core, "UnityEngine", "GameObject", "get_transform", NULL,
            "UnityEngine.Transform", 0, &method))
        return BE_Result_ContractMismatch;
    g_mi_get_transform = method.method_info;
    if (!ResolveMethod(core, "UnityEngine", "Component", "get_gameObject", NULL,
            "UnityEngine.GameObject", 0, &method))
        return BE_Result_ContractMismatch;
    g_mi_get_game_object = method.method_info;
    if (!ResolveMethod(core, "UnityEngine", "Transform", "get_parent", NULL,
            "UnityEngine.Transform", 0, &method))
        return BE_Result_ContractMismatch;
    g_mi_get_parent = method.method_info;
    if (!ResolveMethod(core, "UnityEngine", "GameObject", "GetComponentsInChildren",
            "System.Type|System.Boolean", "UnityEngine.Component[]", 2, &method))
        return BE_Result_ContractMismatch;
    g_mi_get_components = method.method_info;
    if (!ResolveMethod(core, "UnityEngine", "GameObject", "get_activeSelf", NULL,
            "System.Boolean", 0, &method))
        return BE_Result_ContractMismatch;
    g_mi_get_active_self = method.method_info;
    if (!ResolveMethod(core, "UnityEngine", "GameObject", "SetActive", "System.Boolean",
            "System.Void", 1, &method))
        return BE_Result_ContractMismatch;
    g_mi_set_active = method.method_info;
    set_active_target = method.method_pointer;
    /* 以下两个只给排查用的 dump，解析不到不影响功能。 */
    if (ResolveMethod(core, "UnityEngine", "Object", "FindObjectsOfType",
            "System.Type|System.Boolean", "UnityEngine.Object[]", 2, &method))
        g_mi_find_objects = method.method_info;
    if (ResolveMethod(core, "UnityEngine", "GameObject", "get_activeInHierarchy", NULL,
            "System.Boolean", 0, &method))
        g_mi_active_in_hierarchy = method.method_info;

    if (!host->resolve_class ||
        host->resolve_class(host->context, core, "UnityEngine", "Transform", &transform_class) !=
            BE_Result_Ok ||
        !transform_class.type_object) {
        Log("unresolved: typeof(UnityEngine.Transform)");
        return BE_Result_ContractMismatch;
    }
    g_transform_type = transform_class.type_object;

    field_descriptor.assembly_name = core;
    field_descriptor.namespace_name = "UnityEngine";
    field_descriptor.class_name = "Object";
    field_descriptor.field_name = "m_CachedPtr";
    field_descriptor.field_type = "System.IntPtr";
    if (host->resolve_field &&
        host->resolve_field(host->context, &field_descriptor, &field) == BE_Result_Ok &&
        field.offset > 0) {
        g_cached_ptr_offset = field.offset;
    } else {
        Log("m_CachedPtr not resolved; destroyed-object check disabled");
    }

    game_assembly = GetModuleHandleW(L"GameAssembly.dll");
    g_gchandle_get_target = game_assembly
        ? (GcHandleGetTargetFn)(void*)GetProcAddress(game_assembly, "il2cpp_gchandle_get_target")
        : NULL;
    if (!g_gchandle_get_target) {
        Log("il2cpp_gchandle_get_target missing; restore-on-disable will be skipped");
    }
    ResolveHeadContract(game_assembly);

    /* 优先挂 icall（Unity 引擎实现，所有托管调用都要经过，包括被内联的包装方法）；
       解析不到再退回 IL2CPP 包装方法入口。两者参数都是 (self, bool[, MethodInfo*])。 */
    resolve_icall = game_assembly
        ? (ResolveIcallFn)(void*)GetProcAddress(game_assembly, "il2cpp_resolve_icall")
        : NULL;
    if (resolve_icall) {
        void* icall = resolve_icall("UnityEngine.GameObject::SetActive(System.Boolean)");
        if (!icall) {
            icall = resolve_icall("UnityEngine.GameObject::SetActive");
        }
        if (icall) {
            set_active_target = icall;
            Log("hooking SetActive icall");
        }
    }
    if (!set_active_target ||
        !Hook(host, set_active_target, (void*)DetourSetActive, (void**)&g_original_set_active)) {
        Log("SetActive hook failed");
        return BE_Result_Failed;
    }

    {
        void* set_parent_target = NULL;
        if (ResolveMethod(core, "UnityEngine", "Transform", "SetParent",
                "UnityEngine.Transform|System.Boolean", "System.Void", 2, &method)) {
            set_parent_target = method.method_pointer;
        }
        if (resolve_icall) {
            void* icall = resolve_icall(
                "UnityEngine.Transform::SetParent(UnityEngine.Transform,System.Boolean)");
            if (!icall) {
                icall = resolve_icall("UnityEngine.Transform::SetParent");
            }
            if (icall) {
                set_parent_target = icall;
            }
        }
        if (!set_parent_target ||
            !Hook(host, set_parent_target, (void*)DetourSetParent,
                (void**)&g_original_set_parent)) {
            Log("SetParent hook unavailable; relying on periodic checks");
        }
    }

    if (ResolveMethod("UnityEngine.UI.dll", "UnityEngine.EventSystems", "EventSystem", "Update",
            NULL, "System.Void", 0, &method) &&
        method.method_pointer &&
        Hook(host, method.method_pointer, (void*)DetourEventSystemUpdate,
            (void**)&g_original_event_update)) {
        g_pump_in_set_active = 0;
    } else {
        g_pump_in_set_active = 1;
        Log("EventSystem.Update hook unavailable; pumping from SetActive");
    }

    Log("initialized " MODULE_VERSION);
    return BE_Result_Ok;
}

/* 配置的值：内置模块是 BetterEndfield.ini 里本模块段的 "key=value" 行，EML 版是 JSON 的 "key": value。
   找不到返回 NULL。 */
static const char* FindValue(const char* configuration, const char* key) {
    size_t key_length = strlen(key);
    const char* at = configuration;
    while (at && (at = strstr(at, key)) != NULL) {
        const char* after = at + key_length;
        if ((at == configuration || at[-1] == '\n' || at[-1] == '\r') && *after == '=') {
            return after + 1;
        }
        if (at > configuration && at[-1] == '"' && *after == '"') {
            const char* value = after + 1;
            while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
                ++value;
            }
            if (*value == ':') {
                ++value;
                while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n' ||
                       *value == '"') {
                    ++value;
                }
                return value;
            }
        }
        at = after;
    }
    return NULL;
}

static int ReadBool(const char* configuration, const char* key, int fallback) {
    const char* value = FindValue(configuration, key);
    if (!value) {
        return fallback;
    }
    return _strnicmp(value, "true", 4) == 0 || value[0] == '1' || _strnicmp(value, "yes", 3) == 0;
}

static long ReadLong(const char* configuration, const char* key, long fallback) {
    const char* value = FindValue(configuration, key);
    return value ? strtol(value, NULL, 10) : fallback;
}

static BE_Result BE_CALL ConfigurationChanged(const char* configuration) {
    static long last_rescan;
    const char* text = configuration ? configuration : "";
    LONG hide = ReadBool(text, "hide_mask", 1);
    LONG previous = InterlockedExchange(&g_hide_requested, hide);
    /* official_face：去面罩时给女管理员换上官方无面罩的头，热生效。 */
    LONG face = ReadBool(text, "official_face", 1);
    LONG previous_face = InterlockedExchange(&g_official_face_requested, face);
    /* rescan=<数字>：数字变了就在下一帧重扫一次（排查用，平时不写）。 */
    long rescan = ReadLong(text, "rescan", 0);
    static long last_dump;
    long dump = ReadLong(text, "dump", 0);
    if (previous != hide) {
        Log("hide_mask=%s", hide ? "true" : "false");
    }
    if (previous_face != face) {
        Log("official_face=%s", face ? "true" : "false");
        if (face) {
            GuardRetry(); /* 关了再开：启动时因为崩溃保险停用的，清掉标记再试 */
        }
    }
    if (rescan != last_rescan) {
        last_rescan = rescan;
        InterlockedExchange(&g_rescan_requested, 1);
        Log("rescan requested");
    }
    if (dump != last_dump) {
        last_dump = dump;
        InterlockedExchange(&g_dump_requested, 1);
    }
    return BE_Result_Ok;
}

static void BE_CALL Shutdown(void) {
    /* 工作线程上不能碰 Unity；Host 会把本模块的 Hook 节点变成直通。 */
    InterlockedExchange(&g_hide_requested, 0);
}

#ifdef EML_MODULE
/* ---------- 第三方模块入口（EML / BE 3.5.x 的 format 1 / ABI 1 包） ---------- */

typedef void* (*Il2CppDomainGetFn)(void);
typedef void* (*Il2CppThreadCurrentFn)(void);
typedef void* (*Il2CppThreadAttachFn)(void* domain);
typedef void (*Il2CppThreadDetachFn)(void* thread);

static HANDLE g_bootstrap; /* 停用时没等到它退出就留着，下次初始化前再确认 */
static volatile LONG g_phase; /* 0 等游戏接口，1 就绪，2 解析或挂钩失败 */
static int g_initialized;     /* initialize 到 shutdown 之间；只在宿主线程上读写 */
static const char* const kPhaseNames[] = {"waiting_runtime", "ready", "failed"};

/* 等宿主把游戏辅助接口准备好，再把本线程挂到 IL2CPP 上解析、挂钩（解析元数据会建托管对象）。 */
static DWORD WINAPI Bootstrap(LPVOID unused) {
    (void)unused;
    while (!g_bootstrap_stop) {
        const BE_ThirdPartyHostV1* bridge = g_bridge;
        const BE_HostApiV1* runtime = !bridge ? NULL
            : bridge->get_runtime ? bridge->get_runtime(bridge->context) : bridge->runtime;
        HMODULE game_assembly = GetModuleHandleW(L"GameAssembly.dll");
        Il2CppDomainGetFn domain_get;
        Il2CppThreadCurrentFn thread_current;
        Il2CppThreadAttachFn thread_attach;
        Il2CppThreadDetachFn thread_detach;
        void* domain;
        void* attached;
        BE_Result result;
        if (!runtime || !game_assembly) {
            Sleep(50);
            continue;
        }
        domain_get = (Il2CppDomainGetFn)(void*)GetProcAddress(game_assembly, "il2cpp_domain_get");
        thread_current =
            (Il2CppThreadCurrentFn)(void*)GetProcAddress(game_assembly, "il2cpp_thread_current");
        thread_attach =
            (Il2CppThreadAttachFn)(void*)GetProcAddress(game_assembly, "il2cpp_thread_attach");
        thread_detach =
            (Il2CppThreadDetachFn)(void*)GetProcAddress(game_assembly, "il2cpp_thread_detach");
        if (!domain_get || !thread_current || !thread_attach || !thread_detach) {
            InterlockedExchange(&g_phase, 2);
            Log("IL2CPP thread exports unavailable; the mask is left as the game shows it");
            return 0;
        }
        domain = domain_get();
        if (!domain) {
            Sleep(50);
            continue;
        }
        attached = thread_current() ? NULL : thread_attach(domain);
        if (!thread_current()) {
            Sleep(50);
            continue;
        }
        result = Setup(runtime);
        if (attached) {
            thread_detach(attached);
        }
        if (g_bootstrap_stop) {
            return 0; /* 停用赶在 Setup 走完之前：之后的钩子没装，装上的由宿主随后停用 */
        }
        InterlockedExchange(&g_phase, result == BE_Result_Ok ? 1 : 2);
        if (result != BE_Result_Ok) {
            Log("setup failed (%d); the mask is left as the game shows it", (int)result);
        }
        return 0;
    }
    return 0;
}

static BE_Result BE_CALL EmlInitialize(const BE_ThirdPartyHostV1* bridge, const char* configuration) {
    if (!bridge || bridge->struct_size < sizeof(*bridge) || bridge->version != 1 || !bridge->reply ||
        !bridge->log || !bridge->module_id || strcmp(bridge->module_id, MODULE_ID) != 0) {
        return BE_Result_ContractMismatch;
    }
    if (g_initialized) {
        return BE_Result_Conflict;
    }
    if (g_bootstrap) {
        if (WaitForSingleObject(g_bootstrap, 0) != WAIT_OBJECT_0) {
            return BE_Result_NotReady; /* 上次停用时还在装的等待线程没退出 */
        }
        CloseHandle(g_bootstrap);
        g_bootstrap = NULL;
    }
    g_bridge = bridge;
    ConfigurationChanged(configuration ? configuration : "{}");
    InterlockedExchange(&g_bootstrap_stop, 0);
    InterlockedExchange(&g_phase, 0);
    g_bootstrap = CreateThread(NULL, 0, Bootstrap, NULL, 0, NULL);
    if (!g_bootstrap) {
        g_bridge = NULL;
        return BE_Result_Failed;
    }
    g_initialized = 1;
    bridge->log(bridge->context, "Endministrator Unmasked " MODULE_VERSION " waiting for game helpers");
    return BE_Result_Ok;
}

static BE_Result BE_CALL EmlConfigure(const char* configuration) {
    return ConfigurationChanged(configuration ? configuration : "{}");
}

/* 配置页面发 {"action":"status"} 之类的消息；一律回当前状态。 */
static BE_Result BE_CALL EmlMessage(const char* request_id, const char* body) {
    char reply[512];
    const BE_ThirdPartyHostV1* bridge = g_bridge;
    LONG phase = g_phase;
    (void)body;
    if (!bridge || !request_id) {
        return BE_Result_NotReady;
    }
    snprintf(reply, sizeof(reply),
        "{\"version\":\"%s\",\"phase\":\"%s\",\"hide_mask\":%s,\"official_face\":%s,"
        "\"official_face_available\":%s,\"parts_swapped\":%d,\"masks_hidden\":%d}",
        MODULE_VERSION, kPhaseNames[phase >= 0 && phase <= 2 ? phase : 2],
        g_hide_requested ? "true" : "false", g_official_face_requested ? "true" : "false",
        g_head.ready ? "true" : "false", g_swap_count, g_tracked_count);
    return bridge->reply(bridge->context, request_id, BE_Result_Ok, reply);
}

/* 停用：宿主随后把本模块的钩子变成直通，帧泵就不再跑了。所以先关掉开关（等待线程这时才装好钩子的话，
   帧泵也不会再去藏面罩、换头），再请主线程的帧泵把面罩和头整套换回原样、回执（一般一帧）。
   钩子装上过就可能藏过面罩，Setup 没走完也一样，所以一律请求；就绪了才等，最多 1 秒，游戏在读图、
   没有帧泵时就保持现状。没做掉的请求留到重新启用后的第一帧。 */
static void BE_CALL EmlShutdown(void) {
    LONG request;
    Shutdown();
    AcquireSRWLockExclusive(&g_hook_gate);
    g_bootstrap_stop = 1;
    ReleaseSRWLockExclusive(&g_hook_gate);
    if (g_bootstrap && WaitForSingleObject(g_bootstrap, 2000) == WAIT_OBJECT_0) {
        CloseHandle(g_bootstrap);
        g_bootstrap = NULL;
    }
    request = InterlockedIncrement(&g_restore_request);
    if (g_phase == 1) {
        ULONGLONG deadline = GetTickCount64() + 1000;
        while (g_restore_done != request && GetTickCount64() < deadline) {
            Sleep(10);
        }
        if (g_restore_done != request) {
            Log("disabled before the game could restore the mask; it returns when the model reloads");
        }
    }
    g_initialized = 0;
    g_bridge = NULL;
}

static const BE_ThirdPartyModuleV1 kThirdPartyApi = {
    sizeof(BE_ThirdPartyModuleV1), 1, MODULE_ID, EmlInitialize, EmlConfigure, EmlMessage, EmlShutdown,
};

__declspec(dllexport) const BE_ThirdPartyModuleV1* BE_CALL BetterEndfield_GetThirdPartyModuleV1(void) {
    return &kThirdPartyApi;
}
#else
static BE_Result BE_CALL Initialize(const BE_HostApiV1* host) {
    return Setup(host);
}

static const BE_ModuleApiV1 kModuleApi = {
    {MODULE_ID, "Hide Endministrator Mask", MODULE_VERSION, BETTER_ENDFIELD_MODULE_ABI_V1},
    Initialize,
    ConfigurationChanged,
    Shutdown,
};

__declspec(dllexport) const BE_ModuleApiV1* BE_CALL BetterEndfield_GetModuleApiV1(void) {
    return &kModuleApi;
}
#endif

BOOL WINAPI DllMainCRTStartup(HINSTANCE instance, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
