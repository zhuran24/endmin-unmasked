/*
 * 编队界面里女管理员的眼睛。
 *
 * 编队界面的界面模型按位置放四段待机动画 A_actor_endminf_uiteam_idle_01..04，脸部骨整段是常量曲线。
 * 其中 idle_02 把头骨下的三根脸型骨 head_{dn,mid,up}_jnt 相对预制体转了约 90°、挪了 14～16 毫米，
 * 上眼皮控制骨 eye{Lf,Rt}upeyeControl_jnt 转了 178° 和 23°；另外三段不绑这五根，别的界面动画里它们都是
 * 静止值。可玩模型的脸有约 700 个顶点挂在这三根脸型骨上（眼睛一圈都在内），这样一转，眼眶周围的皮肤
 * 盖住眼白和眼珠：原来的脸只剩一条上眼睑线，换上的剧情版的头眼珠挤在眼角。戴着面罩时看不见。
 *
 * 做法：界面模型第一次挂上父节点时（SetParent 触发，动画还没跑过）记下这五根骨的局部变换；之后每帧在
 * TailGameLoop.LateUpdate 之后，把编队里的模型（根名字带 _slot）的这五根写回去，动画下一帧照样写它的，
 * 不用还原。眼睛、眉毛、嘴的表情和身体照旧跟动画走；角色总览等别的界面、大世界的模型都不动。
 * 时机是实测定的：动画在 TailGameLoop.LateUpdate 之前写完骨，在这之后写回的会显示；
 * 同样的值改在 Canvas.SendWillRenderCanvases 里写，编队画面上看不出变化。
 * 开关 squad_eyes：0 不写回，1 写回（默认）。
 * 换头开关关了又开时，记着的界面模型也重新排进换头队列（定时扫描只扫大世界和剧情）。
 * facedump=<数字>（排查用，平时不写）：数字变了就列出这五根现在和生成时不一样的（帧泵里读，是上一帧
 * 最后写下的值，包括这里的写回），再报同一帧里 TailGameLoop.LateUpdate 之后、写回之前有几根被动画改了。
 */

#define SQUAD_BONES 5
#define SQUAD_MAX_MODELS 8
#define SQUAD_NAME_CHECK_MS 1000

static const char kSquadHeadPath[] =
    "Root/Bip001/Bip001_Pelvis/Bip001_Spine/Bip001_Spine1/Bip001_Spine2/Bip001_Neck/Bip001_Head";

/* 相对 Bip001_Head 的路径。 */
static const char* const kSquadBones[SQUAD_BONES] = {
    "head_dn_jnt", "head_mid_jnt", "head_up_jnt", "eyeLfupeyeControl_jnt", "eyeRtupeyeControl_jnt",
};

typedef struct SquadModel {
    uint32_t root;               /* 界面模型根 GameObject */
    uint32_t bones[SQUAD_BONES]; /* 找不到的是 0 */
    float rest[SQUAD_BONES][10]; /* 局部位置 3、旋转 4、缩放 3 */
    int squad;                   /* 根名字带 _slot：在编队里 */
    ULONGLONG next_name_check;
} SquadModel;

static struct {
    const void* get_local_position;
    const void* get_local_rotation;
    const void* get_local_scale;
    int ok;
} g_squad_api;

static SquadModel g_squad[SQUAD_MAX_MODELS];
static int g_squad_count;
static unsigned g_squad_logs;
static volatile LONG g_squad_eyes = 1;       /* 工作线程写 */
static volatile LONG g_squad_dump_requested; /* facedump=<数字>，排查用 */
static int g_squad_dump_pending;             /* 帧泵列完了，等这一帧的 TailGameLoop.LateUpdate */
static UpdateFn g_original_tail_late;

static void ResolveSquadContract(void) {
    const char* core = "UnityEngine.CoreModule.dll";
    int ok = 1;
    ok &= ResolveOptional(core, "UnityEngine", "Transform", "get_localPosition", NULL,
        "UnityEngine.Vector3", 0, &g_squad_api.get_local_position);
    ok &= ResolveOptional(core, "UnityEngine", "Transform", "get_localRotation", NULL,
        "UnityEngine.Quaternion", 0, &g_squad_api.get_local_rotation);
    ok &= ResolveOptional(core, "UnityEngine", "Transform", "get_localScale", NULL,
        "UnityEngine.Vector3", 0, &g_squad_api.get_local_scale);
    g_squad_api.ok = ok && g_head.transform_find && g_head.set_local_position &&
        g_head.set_local_rotation && g_head.set_local_scale && g_gchandle_get_target;
}

static int SquadReadVector(const void* method, void* transform, float* out, size_t count) {
    void* boxed = Call0(method, transform);
    void* raw = boxed ? g_host->object_unbox(g_host->context, boxed) : NULL;
    if (!raw) {
        return 0;
    }
    memcpy(out, raw, sizeof(float) * count);
    return 1;
}

static int SquadReadLocal(void* transform, float out[10]) {
    return SquadReadVector(g_squad_api.get_local_position, transform, out, 3) &&
        SquadReadVector(g_squad_api.get_local_rotation, transform, out + 3, 4) &&
        SquadReadVector(g_squad_api.get_local_scale, transform, out + 7, 3) && AllFinite(out, 10);
}

static void* SquadFind(void* transform, const char* path) {
    void* managed = g_host->string_new(g_host->context, path);
    void* found = managed ? Invoke(g_head.transform_find, transform, (void*[]){managed}) : NULL;
    return Alive(found) ? found : NULL;
}

static void SquadFree(SquadModel* model) {
    int index;
    FreeHandle(&model->root);
    for (index = 0; index < SQUAD_BONES; ++index) {
        FreeHandle(&model->bones[index]);
    }
}

/* 去掉已经销毁的模型；到点的看一眼根名字，判断在不在编队里。 */
static void SquadRefresh(void) {
    ULONGLONG now = GetTickCount64();
    char name[HEAD_NAME];
    int index = 0;
    while (index < g_squad_count) {
        SquadModel* model = &g_squad[index];
        void* root = HandleTarget(model->root);
        if (!Alive(root)) {
            SquadFree(model);
            g_squad[index] = g_squad[--g_squad_count];
            continue;
        }
        if (now >= model->next_name_check) {
            model->next_name_check = now + SQUAD_NAME_CHECK_MS;
            model->squad = ObjectName(root, name, sizeof(name)) && strstr(name, "_slot") != NULL;
        }
        ++index;
    }
}

/* 女管理员的界面模型挂上父节点时调（root_transform 是模型根）。同一个模型只记第一次：之后再挂
   （进编队后改名成 ..._slotN 会再挂一次）动画已经跑过了。 */
static void SquadTrack(void* root_transform, const char* name) {
    void* root = Invoke(g_mi_get_game_object, root_transform, NULL);
    void* head;
    SquadModel* model;
    int index;
    int found = 0;
    if (!g_squad_api.ok || !Alive(root)) {
        return;
    }
    for (index = 0; index < g_squad_count; ++index) {
        if (HandleTarget(g_squad[index].root) == root) {
            return;
        }
    }
    SquadRefresh();
    if (g_squad_count >= SQUAD_MAX_MODELS) {
        return;
    }
    head = SquadFind(root_transform, kSquadHeadPath);
    if (!head) {
        if (g_squad_logs++ < 12) {
            Log("squad eyes: no Bip001_Head under %s", name);
        }
        return;
    }
    model = &g_squad[g_squad_count];
    memset(model, 0, sizeof(*model));
    for (index = 0; index < SQUAD_BONES; ++index) {
        void* bone = SquadFind(head, kSquadBones[index]);
        if (bone && SquadReadLocal(bone, model->rest[index])) {
            model->bones[index] = NewHandle(bone);
            ++found;
        }
    }
    if (!found) {
        return;
    }
    model->root = NewHandle(root);
    ++g_squad_count;
    if (g_squad_logs++ < 12) {
        const float* dn = model->rest[0];
        Log("squad eyes: tracking %s, %d/%d bone(s); head_dn_jnt rest q=(%.4f,%.4f,%.4f,%.4f)",
            name, found, SQUAD_BONES, dn[3], dn[4], dn[5], dn[6]);
    }
}

/* EML 停用前的整套还原里调：放掉记着的界面模型（骨不用还原，动画下一帧会自己写）。 */
static void SquadForget(void) {
    while (g_squad_count > 0) {
        SquadFree(&g_squad[--g_squad_count]);
    }
}

/* 和生成时比：偏了就返回 1，顺带给出位置差（米）和旋转四元数点积的绝对值（1 是没转，0 是转了 180°）。 */
static int SquadOffRest(const float* now, const float* rest, float* dp_out, float* dot_out) {
    float dp = __builtin_sqrtf((now[0] - rest[0]) * (now[0] - rest[0]) +
        (now[1] - rest[1]) * (now[1] - rest[1]) + (now[2] - rest[2]) * (now[2] - rest[2]));
    float dot = __builtin_fabsf(now[3] * rest[3] + now[4] * rest[4] + now[5] * rest[5] + now[6] * rest[6]);
    *dp_out = dp;
    *dot_out = dot;
    return !(dp < 0.0002f && dot > 0.99999f && __builtin_fabsf(now[7] - rest[7]) < 0.001f);
}

/* 每个记着的模型有几根偏离生成时；list 为真时逐根列出。 */
static void SquadReport(const char* where, int list) {
    int index, bone, off;
    char name[HEAD_NAME];
    float now[10], dp, dot;
    for (index = 0; index < g_squad_count; ++index) {
        SquadModel* model = &g_squad[index];
        void* root = HandleTarget(model->root);
        if (!Alive(root) || !ObjectName(root, name, sizeof(name))) {
            continue;
        }
        off = 0;
        for (bone = 0; bone < SQUAD_BONES; ++bone) {
            void* transform = HandleTarget(model->bones[bone]);
            if (!Alive(transform) || !SquadReadLocal(transform, now) ||
                !SquadOffRest(now, model->rest[bone], &dp, &dot)) {
                continue;
            }
            ++off;
            if (list) {
                Log("facedump: %s %s dpos=%.2fmm qdot=%.5f p=(%.5f,%.5f,%.5f) q=(%.4f,%.4f,%.4f,%.4f)", name,
                    kSquadBones[bone], dp * 1000.0f, dot, now[0], now[1], now[2], now[3], now[4], now[5], now[6]);
            }
        }
        Log("facedump %s: %s (%s): %d/%d bone(s) off their rest pose", where, name,
            model->squad ? "squad" : "not squad", off, SQUAD_BONES);
    }
}

static void DetourTailLateUpdate(void* self, const void* method) {
    LONG mode;
    g_original_tail_late(self, method);
    mode = g_squad_eyes;
    if (!g_squad_count || !g_hide_applied || g_in_pump || g_self_call) {
        return;
    }
    g_in_pump = 1;
    SquadRefresh();
    if (g_squad_dump_pending) {
        g_squad_dump_pending = 0;
        SquadReport("after TailGameLoop.LateUpdate, before write-back", 0);
    }
    if (mode == 1) {
        int index, bone;
        for (index = 0; index < g_squad_count; ++index) {
            SquadModel* model = &g_squad[index];
            if (!model->squad) {
                continue;
            }
            for (bone = 0; bone < SQUAD_BONES; ++bone) {
                void* transform = HandleTarget(model->bones[bone]);
                if (Alive(transform)) {
                    Call1(g_head.set_local_position, transform, model->rest[bone]);
                    Call1(g_head.set_local_rotation, transform, model->rest[bone] + 3);
                    Call1(g_head.set_local_scale, transform, model->rest[bone] + 7);
                }
            }
        }
    }
    g_in_pump = 0;
}

/* 换头开关关了又开：定时扫描只管大世界和剧情，界面模型在这里重新排队。 */
static void SquadRequeueHeads(void) {
    int index;
    for (index = 0; index < g_squad_count; ++index) {
        void* root = HandleTarget(g_squad[index].root);
        if (Alive(root)) {
            QueueHeadRoot(root);
        }
    }
}

/* facedump=<数字>：帧泵里调。 */
static void SquadDump(void) {
    Log("facedump: %d tracked UI model(s), squad_eyes=%ld", g_squad_count, (long)g_squad_eyes);
    SquadReport("at frame start", 1);
    g_squad_dump_pending = 1;
}
