/*
 * 去面罩时换上官方无面罩的头（1.4.0 起）。
 * SPDX-License-Identifier: AGPL-3.0-only
 *
 * 可玩的女管理员模型（chr_0003_endminf）面罩下的那张脸是给面罩挡着的：眼线淡、眉间画了皱纹、
 * 虹膜贴图和材质不同、鼻子更尖，也没有刘海投影（cloth_05 是面罩自己的投影）。游戏剧情里摘下
 * 面罩时用的是另一套模型 chr_0003_endminfunmasked_postmodel（同一套衣服，单独做的头），
 * 但那个预制体没挂脚步、武器显隐、表演这些处理器，不能直接拿来当可玩模型。所以这里只借它的头：
 *
 *   脸 face_01、眉毛 eyebrow_01 ← 官方 face_01、brow_01（网格＋材质）
 *   面罩 cloth_03 这一格 ← 官方 brow_02（眼线和睫毛；原模型把它们并在 eyebrow_01 里）
 *   面罩投影 cloth_05 这一格 ← 官方 hairshadow_01（刘海投影）
 *   虹膜 iris_01、眼白阴影 eyeshadow_01 ← 官方同名部件
 *   各级 LOD 和脸的投影代理（shadowProxyDesktop）都按同样的后缀换；头发、身体、衣服不动。
 *
 * 官方网格的 bindpose 是按它自己的脸部骨架做的，两套骨架的身体骨一致、脸部骨（眉、眼、睫毛）
 * 静止位置差 2–11 mm。官方网格原样复用，不复制、不改：这个客户端对游戏自己的网格调
 * Object.Instantiate（Internal_CloneSingle）会在引擎里崩（1.4.0 第一版实测，上游 BE 的
 * CUSTOM_MODEL_BONES_PER_VERTEX_ANDROID.md 也记过）。差的那点由辅助骨补：在可玩骨下面挂一个
 * 子节点，局部变换 L = 可玩 bindpose × 官方 bindpose⁻¹，于是 (可玩骨 × L) × 官方 bindpose
 * = 可玩骨 × 可玩 bindpose，静止时官方网格原样贴在脸上，表情、眨眼由可玩骨架带动。
 * L 接近单位矩阵（身体骨）就直接用可玩骨。官方独有的骨（多出来的睫毛、脸颊、高光骨，刘海骨）
 * 沿官方骨架往上找到可玩骨架有的那根，辅助骨挂在那根下面。
 *
 * 换上的材质还要写进 EntityRenderHelper 的材质缓存（RendererInfo.sourceMaterials），
 * 否则角色切换、受击这类特效结束后会按缓存把可玩模型的旧材质换回来，和新网格的 UV 对不上。
 * 只改本机的显示：不碰 PlayerInfoSystem 的面罩开关，也不发任何消息。
 */

#define HEAD_MAX_BONES 512
#define HEAD_MAX_OFFICIAL 80
#define HEAD_MAX_SWAPS 256
#define HEAD_MAX_ROOTS 24
#define HEAD_MAX_REPURPOSED 96
#define HEAD_MAX_QUEUE 24
#define HEAD_NAME 96
#define HEAD_WATCH_INTERVAL_MS 500

/* data_npc_avatarmesh_endminfunmasked 里记的主预制体路径（剧情里的无面罩 NPC 也是从这里加载）。 */
static const char kOfficialPrefabPath[] =
    "Assets/Beyond/DynamicAssets/Gameplay/Actors/PostModels/Characters/"
    "chr_0003_endminfunmasked_postmodel.prefab";

typedef struct HeadPart {
    const char* playable; /* 可玩模型的部件名前缀，后面接 lod0..3 或 shadowProxyDesktop */
    const char* official;
    int repurposed;       /* 这一格原来是面罩（或面罩投影），换上之后不再隐藏 */
} HeadPart;

static const HeadPart kHeadParts[] = {
    {"S_actor_endminf_face_01_", "S_npc_major_endminfunmasked_face_01_", 0},
    {"S_actor_endminf_eyebrow_01_", "S_npc_major_endminfunmasked_brow_01_", 0},
    {"S_actor_endminf_cloth_03_", "S_npc_major_endminfunmasked_brow_02_", 1},
    {"S_actor_endminf_iris_01_", "S_npc_major_endminfunmasked_iris_01_", 0},
    {"S_actor_endminf_eyeshadow_01_", "S_npc_major_endminfunmasked_eyeshadow_01_", 0},
    {"S_actor_endminf_cloth_05_", "S_npc_major_endminfunmasked_hairshadow_01_", 1},
};
#define HEAD_PART_COUNT (sizeof(kHeadParts) / sizeof(kHeadParts[0]))

typedef void (*WbarrierFn)(void* object, void** target, void* value);
typedef void* (*ObjectGetClassFn)(void* object);
typedef const char* (*ClassGetNameFn)(void* klass);

static struct {
    const void* get_shared_mesh;
    const void* set_shared_mesh;
    const void* get_bones;
    const void* set_bones;
    const void* get_root_bone;
    const void* set_root_bone;
    const void* get_local_bounds;
    const void* set_local_bounds;
    const void* get_shared_materials;
    const void* set_shared_materials;
    const void* get_bindposes;
    const void* get_sub_mesh_count;
    const void* get_component;
    const void* game_object_class;  /* 新建辅助骨用 */
    const void* game_object_ctor;
    const void* transform_find;
    const void* transform_set_parent;
    const void* set_local_position;
    const void* set_local_rotation;
    const void* set_local_scale;
    const void* res_initialized;
    const void* res_async_initing;
    const void* path_loader_ctor;
    const void* path_loader_load_direct;
    const void* path_loader_class;
    void* smr_type;
    void* game_object_type;
    void* render_helper_type;
    int32_t off_controller;     /* EntityRenderHelper.m_rendererAndMaterialController */
    int32_t off_infos;          /* EntityRenderHelperMaterialController.m_rendererInfos */
    int32_t off_info_renderer;  /* RendererInfo.m_renderer */
    int32_t off_info_materials; /* RendererInfo.sourceMaterials */
    int32_t off_info_submeshes; /* RendererInfo.subMeshCount */
    WbarrierFn wbarrier;
    ObjectGetClassFn object_get_class;
    ClassGetNameFn class_get_name;
    int contract_ok; /* 契约都解析到了；ready 还要看崩溃保险、辅助骨有没有出过错 */
    int ready;
} g_head;

typedef struct OfficialRenderer {
    char name[HEAD_NAME];
    uint32_t renderer; /* 官方预制体里的 SkinnedMeshRenderer */
} OfficialRenderer;

typedef struct HeadSwap {
    uint32_t renderer;
    void* renderer_raw;
    uint32_t game_object;    /* 渲染器所在的 GameObject；渲染器单独没了也能撤销面罩那两格的豁免 */
    uint32_t root;           /* 所属模型根 GameObject */
    uint32_t orig_mesh;
    uint32_t orig_materials;
    uint32_t orig_bones;
    uint32_t orig_root_bone;
    float orig_bounds[6];
    /* 换上去的值。渲染器上某一项还是它，说明这一项还归我们管：还原时退回 orig_*，
       看门狗重换时保留第一次的备份；已经被别的系统改掉的项不动。 */
    uint32_t new_mesh;
    uint32_t new_materials;
    uint32_t new_bones;
    uint32_t new_root_bone;
    float new_bounds[6];
    int official;            /* g_official 下标 */
} HeadSwap;

typedef struct PlayableBone {
    char name[64];
    void* transform;
    float bindpose[16];
} PlayableBone;

static volatile LONG g_official_face_requested = 1;
static LONG g_official_face_applied = 1;
static int g_official_state;            /* 0 没加载，1 可用，-1 加载失败（隔一段时间重试） */
static int g_official_attempts;
static ULONGLONG g_official_retry_at;
static uint32_t g_official_prefab;
static uint32_t g_official_loader;      /* 加载器里存着资源句柄，留住它资源就不会被回收 */
static OfficialRenderer g_official[HEAD_MAX_OFFICIAL];
static int g_official_count;
static HeadSwap g_swaps[HEAD_MAX_SWAPS];
static int g_swap_count;
static uint32_t g_head_roots[HEAD_MAX_ROOTS];
static int g_head_root_count;
static uint32_t g_head_queue[HEAD_MAX_QUEUE];
static int g_head_queue_count;
static uint32_t g_repurposed[HEAD_MAX_REPURPOSED];
static void* g_repurposed_raw[HEAD_MAX_REPURPOSED];
static int g_repurposed_count;
static PlayableBone g_bones[HEAD_MAX_BONES];
static int g_bone_count;
static ULONGLONG g_next_watch;
static unsigned g_watch_reswaps;
static int g_logged_fallbacks;
static int g_helpers_created;

static void* HandleTarget(uint32_t handle) {
    return handle && g_gchandle_get_target ? g_gchandle_get_target(handle) : NULL;
}

static uint32_t NewHandle(void* object) {
    return object ? g_host->gchandle_new(g_host->context, object, 0) : 0;
}

static void FreeHandle(uint32_t* handle) {
    if (*handle) {
        g_host->gchandle_free(g_host->context, *handle);
        *handle = 0;
    }
}

static uintptr_t ArrayLength(void* array) {
    return array ? *(uintptr_t*)((char*)array + IL2CPP_ARRAY_LENGTH_OFFSET) : 0;
}

static void** ArrayItems(void* array) {
    return (void**)((char*)array + IL2CPP_ARRAY_DATA_OFFSET);
}

/* 往托管数组、字段里写引用都走 IL2CPP 的写屏障（ready 要求它存在）。 */
static void ArraySet(void* array, uintptr_t index, void* value) {
    g_head.wbarrier(array, ArrayItems(array) + index, value);
}

static void FieldSet(void* object, int32_t offset, void* value) {
    g_head.wbarrier(object, (void**)((char*)object + offset), value);
}

/* 两个托管数组逐项相同。get_bones、get_sharedMaterials 每次都返回新数组，只能比内容。 */
static int SameItems(void* a, void* b) {
    uintptr_t length = ArrayLength(a);
    if (!a || !b || length != ArrayLength(b)) {
        return 0;
    }
    return memcmp(ArrayItems(a), ArrayItems(b), length * sizeof(void*)) == 0;
}

static int ClassNameIs(void* object, const char* expected) {
    void* klass;
    const char* name;
    if (!object || !g_head.object_get_class || !g_head.class_get_name) {
        return 0;
    }
    klass = g_head.object_get_class(object);
    name = klass ? g_head.class_get_name(klass) : NULL;
    return name && strcmp(name, expected) == 0;
}

static void* Call0(const void* method, void* instance) {
    return Invoke(method, instance, NULL);
}

/* 返回值可能本来就是 NULL 的 getter（比如 rootBone）：返回 0 表示方法缺失或抛了异常。 */
static int Call0Ok(const void* method, void* instance, void** out) {
    void* exception = NULL;
    *out = NULL;
    if (!method) {
        return 0;
    }
    *out = g_host->runtime_invoke(g_host->context, method, instance, NULL, &exception);
    return exception == NULL;
}

/* 带一个参数的调用；返回 0 表示方法缺失或抛了托管异常（void 方法正常返回也是 NULL，只能看异常）。 */
static int Call1(const void* method, void* instance, void* argument) {
    void* params[1] = {argument};
    void* exception = NULL;
    if (!method) {
        return 0;
    }
    g_host->runtime_invoke(g_host->context, method, instance, params, &exception);
    return exception == NULL;
}

static int ReadBounds(void* renderer, float out[6]) {
    void* boxed = Call0(g_head.get_local_bounds, renderer);
    void* raw = boxed ? g_host->object_unbox(g_host->context, boxed) : NULL;
    if (!raw) {
        return 0;
    }
    memcpy(out, raw, sizeof(float) * 6);
    return 1;
}

static int GameObjectName(void* component, char* buffer, size_t size) {
    void* game_object = Invoke(g_mi_get_game_object, component, NULL);
    return game_object && ObjectName(game_object, buffer, size);
}

static int RepurposedIndex(void* game_object) {
    int index;
    for (index = 0; index < g_repurposed_count; ++index) {
        if (g_repurposed_raw[index] == game_object && Alive(HandleTarget(g_repurposed[index]))) {
            return index;
        }
    }
    return -1;
}

static int IsRepurposed(void* game_object) {
    return game_object && RepurposedIndex(game_object) >= 0;
}

static void DropRepurposed(int index) {
    FreeHandle(&g_repurposed[index]);
    g_repurposed[index] = g_repurposed[--g_repurposed_count];
    g_repurposed_raw[index] = g_repurposed_raw[g_repurposed_count];
    g_repurposed[g_repurposed_count] = 0;
    g_repurposed_raw[g_repurposed_count] = NULL;
}

/* 表满时先清掉已销毁的；还是满就返回 0（调用方不换这一格）。 */
static int HasRepurposedRoom(void) {
    int index = 0;
    while (g_repurposed_count >= HEAD_MAX_REPURPOSED && index < g_repurposed_count) {
        if (!Alive(HandleTarget(g_repurposed[index]))) {
            DropRepurposed(index);
            continue;
        }
        ++index;
    }
    return g_repurposed_count < HEAD_MAX_REPURPOSED;
}

static void AddRepurposed(void* game_object) {
    if (!IsRepurposed(game_object) && HasRepurposedRoom()) {
        g_repurposed[g_repurposed_count] = NewHandle(game_object);
        g_repurposed_raw[g_repurposed_count] = game_object;
        ++g_repurposed_count;
    }
}

static void ClearRepurposed(void) {
    while (g_repurposed_count > 0) {
        --g_repurposed_count;
        FreeHandle(&g_repurposed[g_repurposed_count]);
        g_repurposed_raw[g_repurposed_count] = NULL;
    }
}

/* ---------- 崩溃保险 ---------- */

/* 加载官方预制体、换头前写下这个文件；换完再过 GUARD_SETTLE_MS（引擎拿新脸渲染过几帧）才删。
   游戏在这段时间里崩了它就留下来，下次启动看到它就不换头、只隐藏面罩，免得每次一进大世界就崩。
   模块关闭时不删：崩溃退出也可能走到关闭回调。删掉文件，或把 official_face 关掉再打开，才会再试。 */
#define GUARD_SETTLE_MS 5000

static WCHAR g_guard_path[MAX_PATH];
static ULONGLONG g_guard_clear_at; /* 非 0：标记还在，到这个时刻删 */
static volatile LONG g_guard_tripped; /* 启动时看到了标记，这一局先不换头 */
static int g_guard_checked;           /* 本进程已经看过启动时的标记 */

/* 内置版放在 BE 的数据目录；EML 版放在自己的目录，文件名带版本号：换了新版就重新试。 */
#ifdef EML_MODULE
#define GUARD_DIR L"\\" MODULE_ID
#define GUARD_NAME L"\\swap-pending-" MODULE_VERSION
#else
#define GUARD_DIR L"\\BetterEndfield"
#define GUARD_NAME L"\\endmin_mask.swap-pending"
#endif

static void GuardInit(void) {
    static const WCHAR kDir[] = GUARD_DIR;
    static const WCHAR kName[] = GUARD_NAME;
    DWORD length;
    if (g_guard_path[0]) {
        return; /* EML 停用再启用时已经有了；帧泵可能正在读它 */
    }
    length = GetEnvironmentVariableW(L"LOCALAPPDATA", g_guard_path, MAX_PATH);
    if (!length || length + (sizeof(kDir) + sizeof(kName)) / sizeof(WCHAR) >= MAX_PATH) {
        g_guard_path[0] = 0;
        Log("swap guard unavailable: LOCALAPPDATA not usable");
        return;
    }
    lstrcatW(g_guard_path, kDir);
    CreateDirectoryW(g_guard_path, NULL); /* 已经有了也无妨 */
    lstrcatW(g_guard_path, kName);
}

static int GuardTripped(void) {
    DWORD attributes = g_guard_path[0] ? GetFileAttributesW(g_guard_path) : INVALID_FILE_ATTRIBUTES;
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static void GuardBegin(void) {
    HANDLE file;
    if (!g_guard_path[0]) {
        return;
    }
    file = CreateFileW(g_guard_path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
        NULL);
    if (file == INVALID_HANDLE_VALUE) {
        Log("swap guard: could not write marker (error %lu)", (unsigned long)GetLastError());
        return;
    }
    CloseHandle(file);
    g_guard_clear_at = 0;
}

static void GuardEnd(void) {
    if (g_guard_path[0]) {
        g_guard_clear_at = GetTickCount64() + GUARD_SETTLE_MS;
    }
}

static void GuardClear(void) {
    g_guard_clear_at = 0;
    if (g_guard_path[0] && !DeleteFileW(g_guard_path) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        Log("swap guard: could not delete marker (error %lu)", (unsigned long)GetLastError());
    }
}

/* Pump 每帧调：到时候就删标记。 */
static void GuardPump(void) {
    if (g_guard_clear_at && GetTickCount64() >= g_guard_clear_at) {
        GuardClear();
    }
}

/* official_face 从关变开时调（配置线程）：启动时因为标记停用的，清掉标记再试。 */
static void GuardRetry(void) {
    if (InterlockedExchange(&g_guard_tripped, 0) && g_head.contract_ok) {
        GuardClear();
        g_head.ready = 1;
        Log("official face: swap marker cleared by toggling official_face, retrying");
    }
}

/* ---------- 解析契约（Initialize 里调；缺一项就只关这一功能，不影响隐藏面罩） ---------- */

static int ResolveOptional(const char* assembly, const char* namespace_name, const char* class_name,
    const char* method_name, const char* parameter_types, const char* return_type,
    uint32_t parameter_count, const void** out) {
    BE_ResolvedMethodV1 method;
    if (!ResolveMethod(assembly, namespace_name, class_name, method_name, parameter_types,
            return_type, parameter_count, &method)) {
        return 0;
    }
    *out = method.method_info;
    return 1;
}

static int ResolveOffset(const char* assembly, const char* namespace_name, const char* class_name,
    const char* field_name, int32_t* out) {
    BE_FieldDescriptorV1 descriptor;
    BE_ResolvedFieldV1 field;
    descriptor.assembly_name = assembly;
    descriptor.namespace_name = namespace_name;
    descriptor.class_name = class_name;
    descriptor.field_name = field_name;
    descriptor.field_type = NULL;
    if (!g_host->resolve_field ||
        g_host->resolve_field(g_host->context, &descriptor, &field) != BE_Result_Ok ||
        field.offset <= 0) {
        Log("unresolved field: %s.%s", class_name, field_name);
        return 0;
    }
    *out = field.offset;
    return 1;
}

static void* ResolveType(const char* assembly, const char* namespace_name, const char* class_name,
    const void** class_info) {
    BE_ResolvedClassV1 resolved;
    if (!g_host->resolve_class ||
        g_host->resolve_class(g_host->context, assembly, namespace_name, class_name, &resolved) !=
            BE_Result_Ok ||
        !resolved.type_object) {
        Log("unresolved class: %s.%s", namespace_name, class_name);
        return NULL;
    }
    if (class_info) {
        *class_info = resolved.class_info;
    }
    return resolved.type_object;
}

static void ResolveHeadContract(HMODULE game_assembly) {
    const char* core = "UnityEngine.CoreModule.dll";
    const char* common = "Common.Beyond.dll";
    int ok = 1;

    ok &= ResolveOptional(core, "UnityEngine", "SkinnedMeshRenderer", "get_sharedMesh", NULL,
        "UnityEngine.Mesh", 0, &g_head.get_shared_mesh);
    ok &= ResolveOptional(core, "UnityEngine", "SkinnedMeshRenderer", "set_sharedMesh",
        "UnityEngine.Mesh", "System.Void", 1, &g_head.set_shared_mesh);
    ok &= ResolveOptional(core, "UnityEngine", "SkinnedMeshRenderer", "get_bones", NULL,
        "UnityEngine.Transform[]", 0, &g_head.get_bones);
    ok &= ResolveOptional(core, "UnityEngine", "SkinnedMeshRenderer", "set_bones",
        "UnityEngine.Transform[]", "System.Void", 1, &g_head.set_bones);
    ok &= ResolveOptional(core, "UnityEngine", "SkinnedMeshRenderer", "get_rootBone", NULL,
        "UnityEngine.Transform", 0, &g_head.get_root_bone);
    ok &= ResolveOptional(core, "UnityEngine", "SkinnedMeshRenderer", "set_rootBone",
        "UnityEngine.Transform", "System.Void", 1, &g_head.set_root_bone);
    ok &= ResolveOptional(core, "UnityEngine", "Renderer", "get_localBounds", NULL,
        "UnityEngine.Bounds", 0, &g_head.get_local_bounds);
    ok &= ResolveOptional(core, "UnityEngine", "Renderer", "set_localBounds", "UnityEngine.Bounds",
        "System.Void", 1, &g_head.set_local_bounds);
    ok &= ResolveOptional(core, "UnityEngine", "Renderer", "get_sharedMaterials", NULL,
        "UnityEngine.Material[]", 0, &g_head.get_shared_materials);
    ok &= ResolveOptional(core, "UnityEngine", "Renderer", "set_sharedMaterials",
        "UnityEngine.Material[]", "System.Void", 1, &g_head.set_shared_materials);
    ok &= ResolveOptional(core, "UnityEngine", "Mesh", "get_bindposes", NULL,
        "UnityEngine.Matrix4x4[]", 0, &g_head.get_bindposes);
    ok &= ResolveOptional(core, "UnityEngine", "Mesh", "get_subMeshCount", NULL, "System.Int32", 0,
        &g_head.get_sub_mesh_count);
    ok &= ResolveOptional(core, "UnityEngine", "GameObject", ".ctor", "System.String", "System.Void",
        1, &g_head.game_object_ctor);
    ok &= ResolveOptional(core, "UnityEngine", "Transform", "Find", "System.String",
        "UnityEngine.Transform", 1, &g_head.transform_find);
    ok &= ResolveOptional(core, "UnityEngine", "Transform", "SetParent",
        "UnityEngine.Transform|System.Boolean", "System.Void", 2, &g_head.transform_set_parent);
    ok &= ResolveOptional(core, "UnityEngine", "Transform", "set_localPosition",
        "UnityEngine.Vector3", "System.Void", 1, &g_head.set_local_position);
    ok &= ResolveOptional(core, "UnityEngine", "Transform", "set_localRotation",
        "UnityEngine.Quaternion", "System.Void", 1, &g_head.set_local_rotation);
    ok &= ResolveOptional(core, "UnityEngine", "Transform", "set_localScale", "UnityEngine.Vector3",
        "System.Void", 1, &g_head.set_local_scale);
    ok &= ResolveOptional(core, "UnityEngine", "GameObject", "GetComponent", "System.Type",
        "UnityEngine.Component", 1, &g_head.get_component);
    ok &= ResolveOptional(common, "Beyond.Resource", "ResourceManager", "IsInitialized", NULL,
        "System.Boolean", 0, &g_head.res_initialized);
    ok &= ResolveOptional(common, "Beyond.Resource", "ResourceManager", "IsAsyncIniting", NULL,
        "System.Boolean", 0, &g_head.res_async_initing);
    ok &= ResolveOptional(common, "Beyond.Resource", "CachedPathAssetLoader", ".ctor",
        "Beyond.Resource.RootCategory", "System.Void", 1, &g_head.path_loader_ctor);
    ok &= ResolveOptional(common, "Beyond.Resource", "CachedPathAssetLoader", "LoadDirect",
        "System.String|System.Type", "UnityEngine.Object", 2, &g_head.path_loader_load_direct);
    ok &= ResolveType(common, "Beyond.Resource", "CachedPathAssetLoader",
        &g_head.path_loader_class) != NULL;

    g_head.smr_type = ResolveType(core, "UnityEngine", "SkinnedMeshRenderer", NULL);
    g_head.game_object_type =
        ResolveType(core, "UnityEngine", "GameObject", &g_head.game_object_class);
    g_head.render_helper_type =
        ResolveType("Gameplay.Beyond.dll", "Beyond.Gameplay.View", "EntityRenderHelper", NULL);
    ok &= g_head.smr_type && g_head.game_object_type && g_head.game_object_class && g_mi_get_transform;

    /* 材质缓存的几个字段解析不到时照样换，只是特效之后可能被换回旧材质，日志会提示。 */
    if (!g_head.render_helper_type ||
        !ResolveOffset("Gameplay.Beyond.dll", "Beyond.Gameplay.View", "EntityRenderHelper",
            "m_rendererAndMaterialController", &g_head.off_controller) ||
        !ResolveOffset("Rendering.Beyond.dll", "Beyond.Rendering",
            "EntityRenderHelperMaterialController", "m_rendererInfos", &g_head.off_infos) ||
        !ResolveOffset("Rendering.Beyond.dll", "Beyond.Rendering",
            "EntityRenderHelperMaterialController.RendererInfo", "m_renderer",
            &g_head.off_info_renderer) ||
        !ResolveOffset("Rendering.Beyond.dll", "Beyond.Rendering",
            "EntityRenderHelperMaterialController.RendererInfo", "sourceMaterials",
            &g_head.off_info_materials) ||
        !ResolveOffset("Rendering.Beyond.dll", "Beyond.Rendering",
            "EntityRenderHelperMaterialController.RendererInfo", "subMeshCount",
            &g_head.off_info_submeshes)) {
        g_head.off_controller = 0;
        Log("material cache fields unresolved; effects may restore the original face materials");
    }

    g_head.wbarrier = game_assembly
        ? (WbarrierFn)(void*)GetProcAddress(game_assembly, "il2cpp_gc_wbarrier_set_field")
        : NULL;
    g_head.object_get_class = game_assembly
        ? (ObjectGetClassFn)(void*)GetProcAddress(game_assembly, "il2cpp_object_get_class")
        : NULL;
    g_head.class_get_name = game_assembly
        ? (ClassGetNameFn)(void*)GetProcAddress(game_assembly, "il2cpp_class_get_name")
        : NULL;
    if (!g_head.wbarrier || !g_head.object_get_class || !g_head.class_get_name) {
        Log("il2cpp write barrier / class exports missing");
        ok = 0;
    }

    g_head.contract_ok = ok && g_gchandle_get_target != NULL;
    GuardInit();
    /* 标记只在本进程第一次初始化时看。EML 停用再启用时还留着的标记是本进程自己的（刚换过头，或停用时
       没来得及还原），不是上次崩溃留下的；上次留下的那个照旧挡着，直到拨一下 official_face。 */
    if (!g_guard_checked) {
        g_guard_checked = 1;
        if (GuardTripped()) {
            g_guard_tripped = 1;
        }
    }
    g_head.ready = g_head.contract_ok && !g_guard_tripped;
    if (g_guard_tripped) {
        Log("official face: disabled, found the swap marker of an earlier session (it likely crashed "
            "during or right after the head swap): %ls; turn official_face off and on, or delete it, "
            "to retry", g_guard_path);
        return;
    }
    Log(g_head.ready ? "official face: contract ok" : "official face: contract incomplete, disabled");
}

/* ---------- 官方预制体 ---------- */

static int ResourcesReady(void) {
    void* boxed = Call0(g_head.res_initialized, NULL);
    void* raw = boxed ? g_host->object_unbox(g_host->context, boxed) : NULL;
    void* boxed_async;
    void* raw_async;
    if (!raw || !*(uint8_t*)raw) {
        return 0;
    }
    boxed_async = Call0(g_head.res_async_initing, NULL);
    raw_async = boxed_async ? g_host->object_unbox(g_host->context, boxed_async) : NULL;
    return raw_async && !*(uint8_t*)raw_async;
}

/* 用游戏自己的 CachedPathAssetLoader 按路径同步加载（返回的就是对象，托管异常由 Host 接住）。
   加载器里存着资源句柄；留住加载器，资源就不会被回收。 */
static void* LoadOfficialPrefab(void) {
    uint8_t category = 0; /* RootCategory.Main */
    void* load_params[2];
    void* loader;
    void* asset;
    if (!g_head.path_loader_class || !g_head.path_loader_ctor || !g_head.path_loader_load_direct ||
        !g_host->object_new) {
        return NULL;
    }
    loader = g_host->object_new(g_host->context, g_head.path_loader_class);
    if (!loader) {
        return NULL;
    }
    if (!Call1(g_head.path_loader_ctor, loader, &category)) {
        return NULL;
    }
    load_params[0] = g_host->string_new(g_host->context, kOfficialPrefabPath);
    load_params[1] = g_head.game_object_type;
    asset = load_params[0] ? Invoke(g_head.path_loader_load_direct, loader, load_params) : NULL;
    if (!Alive(asset)) {
        return NULL;
    }
    FreeHandle(&g_official_loader);
    g_official_loader = NewHandle(loader);
    return asset;
}

static int EnsureOfficial(void) {
    LARGE_INTEGER start;
    void* prefab;
    void* params[2];
    uint8_t include_inactive = 1;
    void* array;
    uintptr_t length, index;
    ULONGLONG now = GetTickCount64();

    if (g_official_state == 1) {
        if (Alive(HandleTarget(g_official_prefab))) {
            return 1;
        }
        Log("official prefab was unloaded; reloading");
        g_official_state = 0;
    }
    if (g_official_state != 1) {
        /* 上一次没成（或资源被卸了）留下的句柄先放掉 */
        FreeHandle(&g_official_prefab);
        FreeHandle(&g_official_loader);
        for (index = 0; index < (uintptr_t)g_official_count; ++index) {
            FreeHandle(&g_official[index].renderer);
        }
        g_official_count = 0;
    }
    if (!g_head.ready || (g_official_state == -1 && now < g_official_retry_at) ||
        g_official_attempts >= 5) {
        return 0;
    }
    if (!ResourcesReady()) {
        return 0;
    }
    ++g_official_attempts;
    QueryPerformanceCounter(&start);
    GuardBegin();
    prefab = LoadOfficialPrefab();
    if (!prefab) {
        GuardEnd();
        g_official_state = -1;
        g_official_retry_at = now + 30000;
        Log("official prefab not loaded (attempt %d); keeping the plain demask", g_official_attempts);
        return 0;
    }
    Log("official prefab loaded in %.1f ms", Milliseconds(&start));
    g_official_prefab = NewHandle(prefab);
    params[0] = g_head.smr_type;
    params[1] = &include_inactive;
    array = Invoke(g_mi_get_components, prefab, params);
    length = ArrayLength(array);
    g_official_count = 0;
    for (index = 0; array && index < length && g_official_count < HEAD_MAX_OFFICIAL; ++index) {
        void* renderer = ArrayItems(array)[index];
        OfficialRenderer* entry = &g_official[g_official_count];
        if (!GameObjectName(renderer, entry->name, sizeof(entry->name)) ||
            strncmp(entry->name, "S_npc_major_endminfunmasked_", 28) != 0) {
            continue;
        }
        entry->renderer = NewHandle(renderer);
        ++g_official_count;
    }
    g_official_state = g_official_count > 0 ? 1 : -1;
    g_official_retry_at = now + 30000;
    if (g_official_state != 1) {
        FreeHandle(&g_official_prefab);
        FreeHandle(&g_official_loader);
    }
    GuardEnd();
    Log("official prefab: %d renderer(s) in %.1f ms", g_official_count, Milliseconds(&start));
    return g_official_state == 1;
}

static int FindOfficial(const char* name) {
    int index;
    for (index = 0; index < g_official_count; ++index) {
        if (strcmp(g_official[index].name, name) == 0) {
            return index;
        }
    }
    return -1;
}

/* ---------- 可玩骨架：骨名 → Transform 和它的 bindpose ---------- */

static PlayableBone* FindBone(const char* name) {
    int index;
    for (index = 0; index < g_bone_count; ++index) {
        if (strcmp(g_bones[index].name, name) == 0) {
            return &g_bones[index];
        }
    }
    return NULL;
}

static HeadSwap* FindSwap(void* renderer) {
    int index;
    for (index = 0; index < g_swap_count; ++index) {
        if (g_swaps[index].renderer_raw == renderer && Alive(HandleTarget(g_swaps[index].renderer))) {
            return &g_swaps[index];
        }
    }
    return NULL;
}

/* 从可玩模型自己的 lod0 网格收集每根骨的 bindpose（已换过的部件取原来的网格）。 */
static void CollectPlayableBones(void* renderers) {
    uintptr_t count = ArrayLength(renderers);
    uintptr_t index;
    char name[HEAD_NAME];
    g_bone_count = 0;
    for (index = 0; index < count; ++index) {
        void* renderer = ArrayItems(renderers)[index];
        HeadSwap* swap = FindSwap(renderer);
        void* mesh;
        void* bones;
        void* bindposes;
        uintptr_t bone_count, slot;
        size_t name_length;
        if (!GameObjectName(renderer, name, sizeof(name)) ||
            strncmp(name, "S_actor_endminf_", 16) != 0) {
            continue;
        }
        name_length = strlen(name);
        if (name_length < 5 || strcmp(name + name_length - 5, "_lod0") != 0) {
            continue;
        }
        mesh = swap ? HandleTarget(swap->orig_mesh) : Call0(g_head.get_shared_mesh, renderer);
        bones = swap ? HandleTarget(swap->orig_bones) : Call0(g_head.get_bones, renderer);
        bindposes = Alive(mesh) ? Call0(g_head.get_bindposes, mesh) : NULL;
        bone_count = ArrayLength(bones);
        if (!bindposes || ArrayLength(bindposes) != bone_count) {
            continue;
        }
        for (slot = 0; slot < bone_count && g_bone_count < HEAD_MAX_BONES; ++slot) {
            void* transform = ArrayItems(bones)[slot];
            PlayableBone* bone;
            char bone_name[64];
            if (!ObjectName(transform, bone_name, sizeof(bone_name)) || FindBone(bone_name)) {
                continue;
            }
            bone = &g_bones[g_bone_count++];
            memcpy(bone->name, bone_name, sizeof(bone->name));
            bone->transform = transform;
            memcpy(bone->bindpose, (char*)bindposes + IL2CPP_ARRAY_DATA_OFFSET + slot * 64, 64);
        }
    }
}

/* 官方骨在可玩骨架里没有同名的，就沿官方骨架往上找。 */
static PlayableBone* MapOfficialBone(void* official_transform) {
    char name[64];
    void* transform = official_transform;
    int depth = 0;
    int climbed = 0;
    char first[64];
    first[0] = '\0';
    while (transform && depth++ < 24) {
        PlayableBone* bone;
        if (!ObjectName(transform, name, sizeof(name))) {
            return NULL;
        }
        if (!first[0]) {
            memcpy(first, name, sizeof(first));
        }
        bone = FindBone(name);
        if (bone) {
            if (climbed && g_logged_fallbacks < 32) {
                ++g_logged_fallbacks;
                Log("bone %s -> %s", first, name);
            }
            return bone;
        }
        climbed = 1;
        transform = Invoke(g_mi_get_parent, transform, NULL);
    }
    return NULL;
}

/* ---------- 辅助骨 ---------- */

/* Unity 的 Matrix4x4 按列存：第 r 行第 c 列在 [c * 4 + r]。 */
#define M44(m, r, c) ((m)[(c) * 4 + (r)])

static void MatMul(const float* a, const float* b, float* out) {
    int r, c, k;
    for (c = 0; c < 4; ++c) {
        for (r = 0; r < 4; ++r) {
            double sum = 0.0;
            for (k = 0; k < 4; ++k) {
                sum += (double)M44(a, r, k) * M44(b, k, c);
            }
            M44(out, r, c) = (float)sum;
        }
    }
}

/* 仿射矩阵（最后一行 0 0 0 1）求逆；奇异时返回 0。 */
static int AffineInverse(const float* m, float* out) {
    double a = M44(m, 0, 0), b = M44(m, 0, 1), c = M44(m, 0, 2);
    double d = M44(m, 1, 0), e = M44(m, 1, 1), f = M44(m, 1, 2);
    double g = M44(m, 2, 0), h = M44(m, 2, 1), i = M44(m, 2, 2);
    double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    double inv[9];
    int r;
    if (__builtin_fabs(det) < 1e-12) {
        return 0;
    }
    inv[0] = (e * i - f * h) / det; inv[1] = (c * h - b * i) / det; inv[2] = (b * f - c * e) / det;
    inv[3] = (f * g - d * i) / det; inv[4] = (a * i - c * g) / det; inv[5] = (c * d - a * f) / det;
    inv[6] = (d * h - e * g) / det; inv[7] = (b * g - a * h) / det; inv[8] = (a * e - b * d) / det;
    for (r = 0; r < 3; ++r) {
        M44(out, r, 0) = (float)inv[r * 3];
        M44(out, r, 1) = (float)inv[r * 3 + 1];
        M44(out, r, 2) = (float)inv[r * 3 + 2];
        M44(out, r, 3) = (float)-(inv[r * 3] * M44(m, 0, 3) + inv[r * 3 + 1] * M44(m, 1, 3) +
                                  inv[r * 3 + 2] * M44(m, 2, 3));
        M44(out, 3, r) = 0.0f;
    }
    M44(out, 3, 3) = 1.0f;
    return 1;
}

static int AllFinite(const float* values, int count) {
    int index;
    for (index = 0; index < count; ++index) {
        if (values[index] != values[index] || __builtin_fabs(values[index]) > 1e30f) {
            return 0;
        }
    }
    return 1;
}

/* 辅助骨名字里带的指纹：按 0.01 mm、1e-5 量化后的位置、旋转、缩放算 FNV-1a，
   同名的辅助骨一定是同一个偏移，按名字复用不会拿错。 */
static uint32_t OffsetFingerprint(const float position[3], const float rotation[4],
    const float scale[3]) {
    float values[10];
    uint32_t hash = 2166136261u;
    int index, byte;
    memcpy(values, position, sizeof(float) * 3);
    memcpy(values + 3, rotation, sizeof(float) * 4);
    memcpy(values + 7, scale, sizeof(float) * 3);
    for (index = 0; index < 10; ++index) {
        double scaled = values[index] * 1e5;
        int32_t quantized;
        if (scaled > 2e9) {
            scaled = 2e9;
        } else if (scaled < -2e9) {
            scaled = -2e9;
        }
        quantized = (int32_t)(scaled + (scaled >= 0 ? 0.5 : -0.5));
        for (byte = 0; byte < 4; ++byte) {
            hash = (hash ^ (uint8_t)(quantized >> (byte * 8))) * 16777619u;
        }
    }
    return hash;
}

static int NearIdentity(const float* m) {
    int r, c;
    for (c = 0; c < 4; ++c) {
        for (r = 0; r < 4; ++r) {
            if (__builtin_fabs(M44(m, r, c) - (r == c ? 1.0f : 0.0f)) > 1e-4f) {
                return 0;
            }
        }
    }
    return 1;
}

/* 拆成局部位置、旋转（四元数 x y z w）、缩放，再按 Unity 的 T·R·S 拼回去核对；
   有剪切、镜像或拼不回去时返回 0。 */
static int DecomposeTRS(const float* m, float position[3], float rotation[4], float scale[3]) {
    double rot[3][3];
    double q[4];
    double trace, s, length;
    int r, c;
    for (c = 0; c < 3; ++c) {
        length = 0.0;
        for (r = 0; r < 3; ++r) {
            length += (double)M44(m, r, c) * M44(m, r, c);
        }
        length = __builtin_sqrt(length);
        if (length < 1e-6) {
            return 0;
        }
        scale[c] = (float)length;
        for (r = 0; r < 3; ++r) {
            rot[r][c] = M44(m, r, c) / length;
        }
        position[c] = M44(m, c, 3);
    }
    trace = rot[0][0] + rot[1][1] + rot[2][2];
    if (trace > 0.0) {
        s = __builtin_sqrt(trace + 1.0) * 2.0;
        q[3] = 0.25 * s;
        q[0] = (rot[2][1] - rot[1][2]) / s;
        q[1] = (rot[0][2] - rot[2][0]) / s;
        q[2] = (rot[1][0] - rot[0][1]) / s;
    } else if (rot[0][0] > rot[1][1] && rot[0][0] > rot[2][2]) {
        s = __builtin_sqrt(1.0 + rot[0][0] - rot[1][1] - rot[2][2]) * 2.0;
        q[3] = (rot[2][1] - rot[1][2]) / s;
        q[0] = 0.25 * s;
        q[1] = (rot[0][1] + rot[1][0]) / s;
        q[2] = (rot[0][2] + rot[2][0]) / s;
    } else if (rot[1][1] > rot[2][2]) {
        s = __builtin_sqrt(1.0 + rot[1][1] - rot[0][0] - rot[2][2]) * 2.0;
        q[3] = (rot[0][2] - rot[2][0]) / s;
        q[0] = (rot[0][1] + rot[1][0]) / s;
        q[1] = 0.25 * s;
        q[2] = (rot[1][2] + rot[2][1]) / s;
    } else {
        s = __builtin_sqrt(1.0 + rot[2][2] - rot[0][0] - rot[1][1]) * 2.0;
        q[3] = (rot[1][0] - rot[0][1]) / s;
        q[0] = (rot[0][2] + rot[2][0]) / s;
        q[1] = (rot[1][2] + rot[2][1]) / s;
        q[2] = 0.25 * s;
    }
    length = __builtin_sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (length < 1e-6) {
        return 0;
    }
    for (r = 0; r < 4; ++r) {
        rotation[r] = (float)(q[r] / length);
    }
    {
        double x = rotation[0], y = rotation[1], z = rotation[2], w = rotation[3];
        double back[3][3] = {
            {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
            {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
            {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
        };
        for (r = 0; r < 3; ++r) {
            for (c = 0; c < 3; ++c) {
                if (__builtin_fabs(back[r][c] * scale[c] - M44(m, r, c)) > 1e-3) {
                    return 0;
                }
            }
        }
    }
    return 1;
}

static int SetParentLocal(void* transform, void* parent) {
    uint8_t stays = 0;
    void* params[2] = {parent, &stays};
    void* exception = NULL;
    g_host->runtime_invoke(g_host->context, g_head.transform_set_parent, transform, params,
        &exception);
    return exception == NULL;
}

/* 新建辅助骨出错：这一局停用换头（已经换上的由 SwapHead/WatchHeads 换回），免得反复建出孤立节点。 */
static void HelperBroken(const char* what, const char* official_name) {
    g_head.ready = 0;
    Log("official face: disabled for this session, helper bone for %s failed at %s", official_name,
        what);
}

/* 让按官方 bindpose 做的网格贴在可玩骨 bone 上：返回 bone 本身（两边 bindpose 一样时），
   或 bone 下面的辅助骨（已有就复用，按名字找）。失败返回 NULL。 */
static void* HelperFor(PlayableBone* bone, const char* official_name, const float* official_bindpose) {
    float inverse[16];
    float local[16];
    float position[3], rotation[4], scale[3];
    char name[HEAD_NAME];
    void* managed_name;
    void* helper;

    if (!AllFinite(official_bindpose, 16) || !AllFinite(bone->bindpose, 16) ||
        !AffineInverse(official_bindpose, inverse)) {
        Log("helper for %s: bindpose unusable", official_name);
        return NULL;
    }
    MatMul(bone->bindpose, inverse, local);
    if (!AllFinite(local, 16)) {
        Log("helper for %s: offset not finite", official_name);
        return NULL;
    }
    if (NearIdentity(local)) {
        return bone->transform;
    }
    if (!DecomposeTRS(local, position, rotation, scale) || !AllFinite(position, 3) ||
        !AllFinite(rotation, 4) || !AllFinite(scale, 3)) {
        Log("helper for %s under %s: offset is not a plain transform", official_name, bone->name);
        return NULL;
    }
    snprintf(name, sizeof(name), "endmin_face_%s_%08x", official_name,
        (unsigned)OffsetFingerprint(position, rotation, scale));
    managed_name = g_host->string_new(g_host->context, name);
    if (!managed_name) {
        return NULL;
    }
    helper = Invoke(g_head.transform_find, bone->transform, (void*[]){managed_name});
    if (!Alive(helper)) {
        void* game_object = g_host->object_new(g_host->context, g_head.game_object_class);
        if (!game_object || !Call1(g_head.game_object_ctor, game_object, managed_name) ||
            !Alive(game_object)) {
            HelperBroken("construct", official_name);
            return NULL;
        }
        helper = Invoke(g_mi_get_transform, game_object, NULL);
        if (!Alive(helper) || !SetParentLocal(helper, bone->transform)) {
            HelperBroken("parent", official_name);
            return NULL;
        }
        ++g_helpers_created;
    }
    if (!Call1(g_head.set_local_position, helper, position) ||
        !Call1(g_head.set_local_rotation, helper, rotation) ||
        !Call1(g_head.set_local_scale, helper, scale)) {
        return NULL;
    }
    return helper;
}

/* 官方网格原样复用；给它的每个骨槽配上可玩骨架里的骨（或辅助骨），返回网格和换上用的骨骼数组。 */
static int PrepareOfficial(int official, void** mesh_out, void** bones_out, void** root_out) {
    OfficialRenderer* entry = &g_official[official];
    void* source = HandleTarget(entry->renderer);
    void* bones;
    void* mesh;
    void* bindposes;
    void* root;
    uintptr_t count, index;
    PlayableBone* mapped_root;

    if (!g_head.ready || !Alive(source)) {
        return 0;
    }
    bones = Call0(g_head.get_bones, source); /* 拿到的是新数组，可以就地改 */
    count = ArrayLength(bones);
    mesh = Call0(g_head.get_shared_mesh, source);
    bindposes = Alive(mesh) ? Call0(g_head.get_bindposes, mesh) : NULL;
    if (!count || ArrayLength(bindposes) != count) {
        Log("official mesh %s: bindposes unavailable", entry->name);
        return 0;
    }
    for (index = 0; index < count; ++index) {
        void* official_bone = ArrayItems(bones)[index];
        PlayableBone* bone = MapOfficialBone(official_bone);
        char bone_name[64];
        void* target;
        if (!bone || !ObjectName(official_bone, bone_name, sizeof(bone_name))) {
            Log("official mesh %s: bone slot %u has no playable match", entry->name,
                (unsigned)index);
            return 0;
        }
        target = HelperFor(bone, bone_name,
            (const float*)((char*)bindposes + IL2CPP_ARRAY_DATA_OFFSET + index * 64));
        if (!target) {
            Log("official mesh %s: no bone for slot %s", entry->name, bone_name);
            return 0;
        }
        ArraySet(bones, index, target);
    }
    root = Call0(g_head.get_root_bone, source);
    mapped_root = root ? MapOfficialBone(root) : NULL;
    *mesh_out = mesh;
    *bones_out = bones;
    *root_out = mapped_root ? mapped_root->transform : NULL;
    return 1;
}

/* ---------- 材质缓存 ---------- */

/* 模型根和往上几层的 EntityRenderHelper 里，记着这个渲染器的 RendererInfo。 */
static int FindRendererInfos(void* root, void* renderer, void** out, int max) {
    void* transform;
    int depth = 0;
    int found = 0;
    if (!g_head.off_controller || !g_head.render_helper_type || !root) {
        return 0;
    }
    transform = Invoke(g_mi_get_transform, root, NULL);
    while (transform && depth++ < 6) {
        void* game_object = Invoke(g_mi_get_game_object, transform, NULL);
        void* helper = game_object ? Invoke(g_head.get_component, game_object,
                                         (void*[]){g_head.render_helper_type})
                                   : NULL;
        if (Alive(helper)) {
            void* controller = *(void**)((char*)helper + g_head.off_controller);
            void* list = controller ? *(void**)((char*)controller + g_head.off_infos) : NULL;
            void* items = list ? *(void**)((char*)list + 0x10) : NULL;
            int32_t size = list ? *(int32_t*)((char*)list + 0x18) : 0;
            int32_t index;
            for (index = 0; items && index < size && (uintptr_t)index < ArrayLength(items);
                 ++index) {
                void* info = ArrayItems(items)[index];
                if (!info || *(void**)((char*)info + g_head.off_info_renderer) != renderer ||
                    !ClassNameIs(info, "RendererInfo")) {
                    continue;
                }
                if (found < max) {
                    out[found++] = info;
                }
            }
        }
        transform = Invoke(g_mi_get_parent, transform, NULL);
    }
    return found;
}

static void PatchMaterialCache(void* root, void* renderer, void* materials, int sub_meshes) {
    void* infos[8];
    int count = FindRendererInfos(root, renderer, infos, 8);
    int index;
    for (index = 0; index < count; ++index) {
        FieldSet(infos[index], g_head.off_info_materials, materials);
        if (sub_meshes > 0) {
            *(int32_t*)((char*)infos[index] + g_head.off_info_submeshes) = sub_meshes;
        }
    }
}

/* 缓存里的原材质。切换、受击这类特效期间渲染器上挂的是特效材质，原样备份以缓存为准。 */
static void* CachedMaterials(void* root, void* renderer) {
    void* info;
    return FindRendererInfos(root, renderer, &info, 1) ?
        *(void**)((char*)info + g_head.off_info_materials) : NULL;
}

static int MeshSubMeshes(void* mesh) {
    void* boxed = Alive(mesh) ? Call0(g_head.get_sub_mesh_count, mesh) : NULL;
    void* raw = boxed ? g_host->object_unbox(g_host->context, boxed) : NULL;
    return raw ? *(int32_t*)raw : 0;
}

/* ---------- 换头 ---------- */

/* 部件名 → kHeadParts 下标和后缀（lod0..3、shadowProxyDesktop）。 */
static int ClassifyPart(const char* name, const char** suffix) {
    size_t index;
    for (index = 0; index < HEAD_PART_COUNT; ++index) {
        size_t length = strlen(kHeadParts[index].playable);
        if (strncmp(name, kHeadParts[index].playable, length) == 0) {
            *suffix = name + length;
            return (int)index;
        }
    }
    return -1;
}

/* 可玩部件名对应的官方渲染器下标；没有对应件返回 -1。 */
static int OfficialFor(const char* playable_name, int* part_out) {
    const char* suffix = NULL;
    char official_name[HEAD_NAME];
    int part = ClassifyPart(playable_name, &suffix);
    if (part < 0) {
        return -1;
    }
    snprintf(official_name, sizeof(official_name), "%s%s", kHeadParts[part].official, suffix);
    if (part_out) {
        *part_out = part;
    }
    return FindOfficial(official_name);
}

static void Retain(uint32_t* handle, void* object) {
    FreeHandle(handle);
    *handle = NewHandle(object);
}

/* 换上官方部件。existing 非空是看门狗重换：网格是游戏刚换上的可玩网格，记下它；
   其余各项若还是上次换上去的值，就保留第一次的原样备份。任一步失败都退回调用前的样子。 */
static int ApplyOfficial(void* root, void* renderer, int official, HeadSwap* existing) {
    void* bones = NULL;
    void* root_bone = NULL;
    void* source = HandleTarget(g_official[official].renderer);
    void* materials;
    void* mesh;
    float bounds[6];
    void* cur_mesh;
    void* cur_materials;
    void* cur_bones;
    void* cur_root;
    float cur_bounds[6];
    HeadSwap* swap = existing;
    int ok;

    if (!swap && g_swap_count >= HEAD_MAX_SWAPS) {
        return 0;
    }
    if (!PrepareOfficial(official, &mesh, &bones, &root_bone)) {
        return 0;
    }
    materials = Call0(g_head.get_shared_materials, source);
    if (!Alive(mesh) || !materials || !ReadBounds(source, bounds)) {
        return 0;
    }
    cur_mesh = Call0(g_head.get_shared_mesh, renderer);
    cur_materials = Call0(g_head.get_shared_materials, renderer);
    cur_bones = Call0(g_head.get_bones, renderer);
    if (!Alive(cur_mesh) || !cur_materials || !cur_bones ||
        !Call0Ok(g_head.get_root_bone, renderer, &cur_root) || !ReadBounds(renderer, cur_bounds)) {
        return 0;
    }

    if (!Call1(g_head.set_shared_mesh, renderer, mesh)) {
        return 0;
    }
    ok = Call1(g_head.set_bones, renderer, bones) &&
        (!root_bone || Call1(g_head.set_root_bone, renderer, root_bone)) &&
        Call1(g_head.set_local_bounds, renderer, bounds) &&
        Call1(g_head.set_shared_materials, renderer, materials);
    if (!ok) {
        int restored = Call1(g_head.set_shared_mesh, renderer, cur_mesh) &
            Call1(g_head.set_bones, renderer, cur_bones) &
            (!root_bone || Call1(g_head.set_root_bone, renderer, cur_root)) &
            Call1(g_head.set_local_bounds, renderer, cur_bounds) &
            Call1(g_head.set_shared_materials, renderer, cur_materials);
        if (!restored) {
            Log("head swap rollback incomplete on %s", g_official[official].name);
        }
        return 0;
    }

    if (!swap) {
        void* cached = CachedMaterials(root, renderer);
        swap = &g_swaps[g_swap_count++];
        memset(swap, 0, sizeof(*swap));
        swap->renderer = NewHandle(renderer);
        swap->renderer_raw = renderer;
        swap->game_object = NewHandle(Invoke(g_mi_get_game_object, renderer, NULL));
        swap->root = NewHandle(root);
        swap->orig_mesh = NewHandle(cur_mesh);
        swap->orig_materials = NewHandle(ArrayLength(cached) ? cached : cur_materials);
        swap->orig_bones = NewHandle(cur_bones);
        swap->orig_root_bone = NewHandle(cur_root);
        memcpy(swap->orig_bounds, cur_bounds, sizeof(cur_bounds));
    } else {
        Retain(&swap->orig_mesh, cur_mesh);
        /* 渲染器上不是我们换上的材质：缓存还是我们写的就是在放特效，保留原备份；
           缓存被游戏重写过以缓存为准；没有缓存才取渲染器上的。 */
        if (!SameItems(cur_materials, HandleTarget(swap->new_materials))) {
            void* cached = CachedMaterials(root, renderer);
            if (!ArrayLength(cached)) {
                Retain(&swap->orig_materials, cur_materials);
            } else if (!SameItems(cached, HandleTarget(swap->new_materials))) {
                Retain(&swap->orig_materials, cached);
            }
        }
        if (!SameItems(cur_bones, HandleTarget(swap->new_bones))) {
            Retain(&swap->orig_bones, cur_bones);
        }
        if (cur_root != HandleTarget(swap->new_root_bone)) {
            Retain(&swap->orig_root_bone, cur_root);
        }
        if (memcmp(cur_bounds, swap->new_bounds, sizeof(cur_bounds)) != 0) {
            memcpy(swap->orig_bounds, cur_bounds, sizeof(cur_bounds));
        }
    }
    swap->official = official;
    Retain(&swap->new_mesh, mesh);
    Retain(&swap->new_materials, materials);
    Retain(&swap->new_bones, bones);
    Retain(&swap->new_root_bone, root_bone ? root_bone : cur_root);
    memcpy(swap->new_bounds, bounds, sizeof(bounds));
    PatchMaterialCache(root, renderer, materials, MeshSubMeshes(mesh));
    return 1;
}

static void FreeSwap(HeadSwap* swap) {
    FreeHandle(&swap->renderer);
    FreeHandle(&swap->game_object);
    FreeHandle(&swap->root);
    FreeHandle(&swap->orig_mesh);
    FreeHandle(&swap->orig_materials);
    FreeHandle(&swap->orig_bones);
    FreeHandle(&swap->orig_root_bone);
    FreeHandle(&swap->new_mesh);
    FreeHandle(&swap->new_materials);
    FreeHandle(&swap->new_bones);
    FreeHandle(&swap->new_root_bone);
    swap->renderer_raw = NULL;
}

/* 还原一件：渲染器上还是我们换上去的项退回原样，已经被别的系统改掉的项不动（包围盒跟着网格走）。
   不释放记录。返回 0 表示有一步没退成。 */
static int RestoreSwap(HeadSwap* swap) {
    void* renderer = HandleTarget(swap->renderer);
    void* root = HandleTarget(swap->root);
    void* current;
    void* materials;
    float bounds[6];
    int mesh_restored = 0;
    int materials_restored = 0;
    int ok = 1;
    if (!Alive(renderer)) {
        return 1;
    }
    current = Call0(g_head.get_shared_mesh, renderer);
    if (current && current == HandleTarget(swap->new_mesh)) {
        mesh_restored = Call1(g_head.set_shared_mesh, renderer, HandleTarget(swap->orig_mesh));
        ok &= mesh_restored;
    }
    if (SameItems(Call0(g_head.get_bones, renderer), HandleTarget(swap->new_bones))) {
        ok &= Call1(g_head.set_bones, renderer, HandleTarget(swap->orig_bones));
    }
    if (Call0Ok(g_head.get_root_bone, renderer, &current) &&
        current == HandleTarget(swap->new_root_bone)) {
        ok &= Call1(g_head.set_root_bone, renderer, HandleTarget(swap->orig_root_bone));
    }
    if (mesh_restored ||
        (ReadBounds(renderer, bounds) && memcmp(bounds, swap->new_bounds, sizeof(bounds)) == 0)) {
        ok &= Call1(g_head.set_local_bounds, renderer, swap->orig_bounds);
    }
    materials = HandleTarget(swap->orig_materials);
    if (SameItems(Call0(g_head.get_shared_materials, renderer), HandleTarget(swap->new_materials))) {
        materials_restored = materials && Call1(g_head.set_shared_materials, renderer, materials);
        ok &= materials_restored;
    }
    /* 缓存还是我们写的官方材质（比如正在放特效、渲染器上是特效材质）也改回原材质，
       特效结束时游戏按缓存换回。 */
    if (materials && (mesh_restored || materials_restored ||
                         SameItems(CachedMaterials(root, renderer), HandleTarget(swap->new_materials)))) {
        PatchMaterialCache(root, renderer, materials,
            MeshSubMeshes(Call0(g_head.get_shared_mesh, renderer)));
    }
    return ok;
}

static int HeadRootIndex(void* root) {
    int index = 0;
    while (index < g_head_root_count) {
        void* target = HandleTarget(g_head_roots[index]);
        if (!Alive(target)) { /* 模型销毁了，顺手清掉 */
            FreeHandle(&g_head_roots[index]);
            g_head_roots[index] = g_head_roots[--g_head_root_count];
            g_head_roots[g_head_root_count] = 0;
            continue;
        }
        if (target == root) {
            return index;
        }
        ++index;
    }
    return -1;
}

/* 取模型根下全部 SkinnedMeshRenderer，并把 g_bones 换成这个模型的骨架；失败返回 NULL。 */
static void* PrepareRootBones(void* root) {
    uint8_t include_inactive = 1;
    void* params[2] = {g_head.smr_type, &include_inactive};
    void* renderers = Alive(root) ? Invoke(g_mi_get_components, root, params) : NULL;
    if (!ArrayLength(renderers)) {
        return NULL;
    }
    CollectPlayableBones(renderers);
    if (!FindBone("Bip001_Head")) {
        Log("head swap skipped: no playable head bone (%d bones)", g_bone_count);
        return NULL;
    }
    return renderers;
}

static void UnswapAllHeads(void);
static void HideRepurposed(void);

/* 换这个模型里还没换的头部部件，返回这次换上的件数。已经换过的跳过，所以同一个根可以反复排队
   （后来才出现的 LOD 部件也能补上）。 */
static int SwapHead(void* root) {
    uint8_t include_inactive = 1;
    void* params[2] = {g_head.smr_type, &include_inactive};
    void* renderers;
    uintptr_t count, index;
    int swapped = 0;
    int pending = 0;
    char name[HEAD_NAME];
    char root_name[HEAD_NAME];
    LARGE_INTEGER start;

    if (!Alive(root) || !EnsureOfficial()) {
        return 0;
    }
    /* 先粗看有没有要换的，没有就不收骨架（这一步要读几百个名字）。 */
    renderers = Invoke(g_mi_get_components, root, params);
    count = ArrayLength(renderers);
    for (index = 0; index < count && !pending; ++index) {
        void* renderer = ArrayItems(renderers)[index];
        pending = GameObjectName(renderer, name, sizeof(name)) && !FindSwap(renderer) &&
            OfficialFor(name, NULL) >= 0;
    }
    if (!pending) {
        return 0;
    }
    QueryPerformanceCounter(&start);
    GuardBegin();
    renderers = PrepareRootBones(root);
    count = ArrayLength(renderers);
    for (index = 0; index < count; ++index) {
        void* renderer = ArrayItems(renderers)[index];
        void* game_object;
        int part = -1;
        int official;
        if (!GameObjectName(renderer, name, sizeof(name)) || FindSwap(renderer)) {
            continue;
        }
        official = OfficialFor(name, &part);
        if (official < 0) {
            continue; /* 比如面罩的投影代理：官方没有对应件，照旧隐藏 */
        }
        game_object = Invoke(g_mi_get_game_object, renderer, NULL);
        if (kHeadParts[part].repurposed && (!game_object || !HasRepurposedRoom())) {
            continue; /* 放不进豁免表就不换这一格，面罩照旧隐藏 */
        }
        if (!ApplyOfficial(root, renderer, official, NULL)) {
            Log("head swap failed on %s", name);
            if (!g_head.ready) {
                break;
            }
            continue;
        }
        ++swapped;
        if (kHeadParts[part].repurposed) {
            AddRepurposed(game_object);
            /* 本模块先前替游戏压下去的，现在放出来。 */
            if (TrackedIndex(game_object) >= 0) {
                Untrack(game_object);
                SetActive(game_object, 1);
            }
        }
    }
    GuardEnd();
    if (!g_head.ready) { /* 辅助骨出错，这一局停用：已经换上的都换回去，面罩那两格重新藏起来 */
        UnswapAllHeads();
        HideRepurposed();
        return 0;
    }
    if (swapped) {
        if (HeadRootIndex(root) < 0 && g_head_root_count < HEAD_MAX_ROOTS) {
            g_head_roots[g_head_root_count++] = NewHandle(root);
        }
        if (!ObjectName(root, root_name, sizeof(root_name))) {
            strcpy(root_name, "?");
        }
        Log("official face on %s: %d part(s), %d bone(s), %d helper(s) so far, %.1f ms", root_name,
            swapped, g_bone_count, g_helpers_created, Milliseconds(&start));
    }
    return swapped;
}

static void UnswapAllHeads(void) {
    int restored = 0;
    int failed = 0;
    while (g_swap_count > 0) {
        HeadSwap* swap = &g_swaps[--g_swap_count];
        if (Alive(HandleTarget(swap->renderer))) {
            ++restored;
        }
        if (!RestoreSwap(swap)) {
            ++failed;
        }
        FreeSwap(swap);
    }
    while (g_head_root_count > 0) {
        FreeHandle(&g_head_roots[--g_head_root_count]);
    }
    if (restored) {
        Log("official face removed from %d part(s), %d incomplete", restored, failed);
    }
}

/* 面罩那两格换回原样后要重新藏起来（开关还开着时）。 */
static void HideRepurposed(void) {
    int index;
    for (index = 0; index < g_repurposed_count; ++index) {
        void* game_object = HandleTarget(g_repurposed[index]);
        if (Alive(game_object) && ActiveSelf(game_object)) {
            Track(game_object);
            SetActive(game_object, 0);
        }
    }
    ClearRepurposed();
}

/* 同一个 GameObject 上还有别的活着的换头记录（渲染器被换过一个）。 */
static int GameObjectSwapped(void* game_object, const HeadSwap* except) {
    int index;
    for (index = 0; index < g_swap_count; ++index) {
        if (&g_swaps[index] != except && HandleTarget(g_swaps[index].game_object) == game_object &&
            Alive(HandleTarget(g_swaps[index].renderer))) {
            return 1;
        }
    }
    return 0;
}

/* 看门狗放弃 leaving 这一件时：若它是面罩那两格之一、且同一对象上没有别的换头记录，取消豁免；
   开关还开着就照旧藏起来。 */
static void ReleaseRepurposed(void* game_object, const HeadSwap* leaving) {
    int index = Alive(game_object) && !GameObjectSwapped(game_object, leaving)
        ? RepurposedIndex(game_object) : -1;
    if (index < 0) {
        return;
    }
    DropRepurposed(index);
    if (g_hide_requested && ActiveSelf(game_object)) {
        Track(game_object);
        SetActive(game_object, 0);
    }
}

/* 找到 S_actor_endminf_face_01_lod0 这类部件时，往上找模型根排进队列，下一帧换。 */
static void QueueHeadRoot(void* root) {
    int index;
    if (!root) {
        return;
    }
    for (index = 0; index < g_head_queue_count; ++index) {
        if (HandleTarget(g_head_queue[index]) == root) {
            return;
        }
    }
    if (g_head_queue_count < HEAD_MAX_QUEUE) {
        g_head_queue[g_head_queue_count++] = NewHandle(root);
    }
}

static void* ModelRootOf(void* transform) {
    char name[HEAD_NAME];
    int depth = 0;
    while (transform && depth++ < 10) {
        if (ObjectName(transform, name, sizeof(name)) && StartsWith(name, "chr_0003_endminf") &&
            !StartsWith(name, "chr_0003_endminfunmasked") &&
            !StartsWith(name, "chr_0003_endminfink")) {
            return Invoke(g_mi_get_game_object, transform, NULL);
        }
        transform = Invoke(g_mi_get_parent, transform, NULL);
    }
    return NULL;
}

static void DropHeadQueue(void) {
    while (g_head_queue_count > 0) {
        FreeHandle(&g_head_queue[--g_head_queue_count]);
    }
}

/* face 是 Pump 这一帧取的开关快照（不看线上值，免得同一帧里前后不一致）。 */
static void ProcessHeadQueue(LONG face) {
    if (!g_head_queue_count) {
        return;
    }
    if (!face || !g_head.ready) {
        DropHeadQueue();
        return;
    }
    if (!EnsureOfficial()) {
        /* 资源系统还没好、或者等下次重试：队列留着；试满 5 次就放弃，只隐藏面罩。 */
        if (g_official_attempts >= 5 && g_official_state != 1) {
            DropHeadQueue();
        }
        return;
    }
    while (g_head_queue_count > 0) {
        uint32_t handle = g_head_queue[--g_head_queue_count];
        void* root = HandleTarget(handle);
        g_head_queue[g_head_queue_count] = 0;
        if (Alive(root)) {
            SwapHead(root);
        }
        g_host->gchandle_free(g_host->context, handle);
    }
}

/* 有的系统会换掉部件的网格（比如 NPC 的 LOD），换回可玩网格时按新网格名再换一次。 */
static void WatchHeads(void) {
    ULONGLONG now = GetTickCount64();
    int index = 0;
    char name[HEAD_NAME];
    void* prepared_root = NULL; /* g_bones 当前对应的模型；准备别的模型前清空，成功后再记 */
    if (now < g_next_watch) {
        return;
    }
    g_next_watch = now + HEAD_WATCH_INTERVAL_MS;
    if (!g_head.ready) { /* 这一局停用了（辅助骨出过错）：换上的都换回去 */
        if (g_swap_count) {
            UnswapAllHeads();
            HideRepurposed();
        }
        return;
    }
    while (index < g_swap_count) {
        HeadSwap* swap = &g_swaps[index];
        void* renderer = HandleTarget(swap->renderer);
        void* current;
        if (!Alive(renderer)) {
            ReleaseRepurposed(HandleTarget(swap->game_object), swap);
            FreeSwap(swap);
            g_swaps[index] = g_swaps[--g_swap_count];
            continue;
        }
        current = Call0(g_head.get_shared_mesh, renderer);
        if (current == HandleTarget(swap->new_mesh)) {
            ++index;
            continue;
        }
        if (Alive(current) && ObjectName(current, name, sizeof(name))) {
            int official = OfficialFor(name, NULL);
            void* root = HandleTarget(swap->root);
            if (official >= 0 && root != prepared_root) {
                prepared_root = NULL;
                if (PrepareRootBones(root)) {
                    prepared_root = root;
                }
            }
            if (official >= 0 && prepared_root && root == prepared_root) {
                int reapplied;
                GuardBegin();
                reapplied = ApplyOfficial(root, renderer, official, swap);
                GuardEnd();
                if (reapplied) {
                    if (++g_watch_reswaps <= 16) {
                        Log("re-applied official face on %s", name);
                    }
                    ++index;
                    continue;
                }
            }
        }
        /* 不认识的网格（别的模组换的）或重换失败：不再管这一件。网格留给换它的人，
           骨骼、材质这些还是我们换上去的就退回原样，免得新网格配着官方头的骨骼和材质。 */
        Log("stopped managing a face part (mesh changed elsewhere)%s",
            RestoreSwap(swap) ? "" : "; restore incomplete");
        ReleaseRepurposed(HandleTarget(swap->game_object), swap);
        FreeSwap(swap);
        g_swaps[index] = g_swaps[--g_swap_count];
    }
}
