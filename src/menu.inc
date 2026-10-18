namespace menu {

static uint8_t* M;
static bool     g_ok;

typedef void (*PlaySoundFn)(void* mgr, int a, int id, int b, short c, int d, int e, int f);
typedef void (*ObjFn)(void* obj);
typedef bool (*ExitFn)(void* obj);
typedef void* (*DtorFn)(void* obj, int flags);
typedef void (*RenderFn)(void* cont);
typedef void* (*SingletonFn)();

static PlaySoundFn PlaySound;
static ObjFn       OrigInit, OrigUpdate;
static ExitFn      OrigExit;
static DtorFn      OrigDtor;
static RenderFn    g_renderTramp;

static inline uint32_t Flags()         { return *(volatile uint32_t*)(M + 0xB6B2AC); }
static inline bool     FullLayout()    { return (Flags() >> 14) & 1; }
static inline uint64_t TrigBits()      { return *(volatile uint64_t*)(M + 0xB6AD28); }
static inline uint64_t RepBits()       { return *(volatile uint64_t*)(M + 0xB6AD60); }
static inline uint32_t FrameCtr()      { return *(volatile uint32_t*)(M + 0xB6AC20); }
static inline int&     Cursor(void* o) { return *(int*)((uint8_t*)o + 0x50); }
static inline int      State(void* o)  { return *(int*)((uint8_t*)o + 0x88); }
static inline void*    Cont(void* o)   { return (uint8_t*)o + 0x18; }
static void Sound(int id) { PlaySound(M + 0xB4DFF0, 0, id, -1, (short)-1, 0, 0, 0); }
static void* GetWindowControl() { return ((SingletonFn)(M + 0x152B80))(); }
static void** MsgTable() { return *(void***)(M + 0x79BEF8); }

enum { kMsgLabel = 0xEFF0, kMsgValue = 0xEFF1 };
static uint16_t g_label[64], g_value[64];
static bool     g_textOk;
static int      g_glyph[128];
static uint16_t g_end = 0x8000;

static uint16_t* GetMsgImpl(void** table, uint16_t id)
{
    if (id == kMsgLabel) return g_label;
    if (id == kMsgValue) return g_value;
    uint8_t* blk = (uint8_t*)table[id >> 12];
    return (uint16_t*)(blk + *(uint32_t*)(blk + (size_t)(id & 0xFFF) * 8 + 4));
}

static inline bool IsEnd(uint16_t c)  { return (c & 0xFF00) == 0x8000; }
static inline bool IsCtrl(uint16_t c) { return (c & 0x8000) != 0; }

static bool Learn(uint16_t id, const char* s)
{
    uint16_t* m = GetMsgImpl(MsgTable(), id);
    int tmp[128];
    for (int& t : tmp) t = -1;
    int j = 0;
    for (int i = 0; i < 200; ++i)
    {
        uint16_t c = m[i];
        if (IsEnd(c)) { g_end = c; break; }
        if (IsCtrl(c)) { if (s[j] == ' ') { tmp[' '] = c; ++j; } continue; }
        if (!s[j]) return false;
        tmp[(uint8_t)s[j] & 127] = c;
        ++j;
    }
    if (s[j]) return false;
    for (int k = 0; k < 128; ++k) if (tmp[k] >= 0) g_glyph[k] = tmp[k];
    return true;
}

static void Extrapolate(char lo, char hi)
{
    int off = INT32_MIN, known = 0;
    bool consistent = true;
    for (int ch = lo; ch <= hi; ++ch)
        if (g_glyph[ch] >= 0)
        {
            int o = g_glyph[ch] - ch;
            ++known;
            if (off == INT32_MIN) off = o; else if (o != off) consistent = false;
        }
    if (known >= 3 && consistent)
        for (int ch = lo; ch <= hi; ++ch) if (g_glyph[ch] < 0) g_glyph[ch] = ch + off;
}

static bool Encode(const char* s, uint16_t* out, int max)
{
    int n = 0;
    for (; *s && n < max - 1; ++s)
    {
        int g = g_glyph[(uint8_t)*s & 127];
        if (g < 0) return false;
        out[n++] = (uint16_t)g;
    }
    out[n] = g_end;
    return true;
}

static void LearnDigits()
{
    uint16_t* m = GetMsgImpl(MsgTable(), 0x2DA);
    int n = 0;
    while (n < 32 && !IsEnd(m[n])) ++n;
    if (n >= 6 && g_glyph['H'] >= 0 && m[n - 2] == g_glyph['H'])
    {
        float hz = *(float*)((uint8_t*)GetWindowControl() + 0x18);
        char txt[32];
        snprintf(txt, sizeof txt, "%4.2f", hz);
        int len = (int)strlen(txt);
        if (len == n - 2)
        {
            int off = INT32_MIN;
            bool ok = true;
            for (int i = 0; i < len; ++i)
            {
                if (txt[i] == '.') { g_glyph['.'] = m[i]; continue; }
                if (txt[i] == ' ') continue;
                int o = m[i] - txt[i];
                if (off == INT32_MIN) off = o; else if (o != off) ok = false;
            }
            if (ok && off != INT32_MIN)
                for (int d = '0'; d <= '9'; ++d) g_glyph[d] = d + off;
        }
    }
    if (g_glyph['0'] < 0)
        for (int d = '0'; d <= '9'; ++d) g_glyph[d] = d - 35;
}

static void BuildGlyphs()
{
    for (int& g : g_glyph) g = -1;
    static const struct { uint16_t id; const char* s; } known[] = {
        { 0x2C7, "Display Settings" }, { 0x2C8, "Resolution" }, { 0x2D4, "Screen Mode" },
        { 0x2D8, "V-Sync" }, { 0x2D9, "Refresh Rate" }, { 0x2DB, "Graphics Settings" },
        { 0x29A, "On" }, { 0x29B, "Off" }, { 0x291, "Default" }, { 0x292, "Exit Options" },
    };
    for (auto& k : known) Learn(k.id, k.s);
    Extrapolate('A', 'Z');
    Extrapolate('a', 'z');
    LearnDigits();
}

static void RefreshValueText()
{
    if (!g_textOk) return;
    char s[32];
    snprintf(s, sizeof s, "%d FPS", g_cfg.targetFps);
    if (Encode(s, g_value, 64)) return;
    snprintf(s, sizeof s, "%d fps", g_cfg.targetFps);
    if (Encode(s, g_value, 64)) return;
    snprintf(s, sizeof s, "%d", g_cfg.targetFps);
    Encode(s, g_value, 64);
}

static void PrepareText()
{
    if (g_textOk) return;
    BuildGlyphs();
    static const char* const labels[] = { "Frame Rate", "Framerate", "Rate" };
    for (const char* l : labels)
        if (Encode(l, g_label, 64)) { g_textOk = true; break; }
    if (!g_textOk)
    {
        Log("menu: could not build the Frame Rate label");
        g_label[0] = g_end;
        g_value[0] = g_end;
        return;
    }
    RefreshValueText();
}

enum { kElem = 0xC8, kMaxElems = 250, kMaxSet = 96 };
static const uint8_t kSrc[6] = { 0x64, 0x94, 0x48, 0x49, 0x1E, 0x1F };
static const uint8_t kRef[6] = { 0x63, 0x93, 0x46, 0x47, 0x1C, 0x1D };

static inline uint8_t* ContArr(uint8_t* c) { return *(uint8_t**)(c + 0x08); }
static inline uint8_t* ContMap(uint8_t* c) { return *(uint8_t**)(c + 0x18); }
static inline int      ContCnt(uint8_t* c) { return *(int*)(c + 0x20); }
static inline float*   F(uint8_t* e, int off) { return (float*)(e + off); }
static inline uint32_t& Fl(uint8_t* e) { return *(uint32_t*)(e + 0xA8); }

static void*    g_menuObj;
static uint8_t* g_spCont;
static int      g_spRoots[4], g_spRootRef[4], g_spNRoots;
static int      g_spPairSrc[4], g_spPairRef[4];
static int      g_spSet[kMaxSet], g_spN;
static int      g_spLabel = -1, g_spValue = -1, g_spPanel = -1;
static int      g_spArrowIdx[2] = { -1, -1 };
static int      g_arrowIdx = -1;
static float    g_arrowRowX = 16.0f;
static uint32_t g_spSavedFlags[kMaxElems];
static uint8_t  g_spSavedElem[kMaxSet][kElem];
static uint32_t g_spColor38[kMaxSet], g_spColor50[kMaxSet];
static uint8_t  g_spArrowRest[2][kElem];
static bool     g_spRestTaken;

static bool PrepareRow(void* obj)
{
    uint8_t* c = (uint8_t*)Cont(obj);
    uint8_t* arr = ContArr(c);
    uint8_t* map = ContMap(c);
    int count = ContCnt(c);
    if (!arr || !map || count <= 0 || count > kMaxElems) return false;

    auto idxOf = [&](uint8_t* p) { return (p >= arr && p < arr + (size_t)count * kElem) ? (int)((p - arr) / kElem) : -1; };
    auto parentIdx = [&](int i) { return idxOf(*(uint8_t**)(arr + (size_t)i * kElem + 0x98)); };

    int srcIdx[6], refIdx[6];
    for (int i = 0; i < 6; ++i)
    {
        srcIdx[i] = map[kSrc[i]] < count ? map[kSrc[i]] : -1;
        refIdx[i] = map[kRef[i]] < count ? map[kRef[i]] : -1;
        if (srcIdx[i] < 0 || refIdx[i] < 0) return false;
    }

    g_spNRoots = 0;
    for (int i = 0; i < 6; ++i)
    {
        int p = parentIdx(srcIdx[i]);
        bool isSrc = false;
        for (int k = 0; k < 6; ++k) if (srcIdx[k] == p) isSrc = true;
        if (isSrc || p < 0) continue;
        bool dup = false;
        for (int k = 0; k < g_spNRoots; ++k) if (g_spRoots[k] == p) dup = true;
        if (dup || g_spNRoots >= 4) continue;
        g_spRoots[g_spNRoots] = p;
        g_spRootRef[g_spNRoots] = parentIdx(refIdx[i]);
        ++g_spNRoots;
    }
    if (g_spNRoots == 0) return false;

    g_spN = 0;
    for (int r = 0; r < g_spNRoots; ++r) g_spSet[g_spN++] = g_spRoots[r];
    auto inSet = [&](int idx) { for (int k = 0; k < g_spN; ++k) if (g_spSet[k] == idx) return true; return false; };
    for (bool grew = true; grew; )
    {
        grew = false;
        for (int i = 0; i < count && g_spN < kMaxSet; ++i)
            if (!inSet(i))
            {
                int p = parentIdx(i);
                if (p >= 0 && inSet(p)) { g_spSet[g_spN++] = i; grew = true; }
            }
    }

    g_spPanel = srcIdx[0];
    g_spLabel = srcIdx[4];
    g_spValue = srcIdx[5];
    g_spArrowIdx[0] = srcIdx[2];
    g_spArrowIdx[1] = srcIdx[3];
    for (int r = 0; r < g_spNRoots; ++r)
    {
        g_spPairSrc[r] = g_spPairRef[r] = -1;
        for (int i = 0; i < 6; ++i)
            if (parentIdx(srcIdx[i]) == g_spRoots[r] && !(Fl(arr + (size_t)srcIdx[i] * kElem) & 0x28000000u))
            {
                g_spPairSrc[r] = srcIdx[i];
                g_spPairRef[r] = refIdx[i];
                break;
            }
    }
    g_arrowIdx = map[0x04] < count ? map[0x04] : -1;
    g_spRestTaken = false;
    g_spCont = c;
    return true;
}

static int RootOf(uint8_t* arr, int count, int idx)
{
    for (int i = idx, guard = 0; i >= 0 && guard < 16; ++guard)
    {
        for (int r = 0; r < g_spNRoots; ++r) if (g_spRoots[r] == i) return r;
        uint8_t* p = *(uint8_t**)(arr + (size_t)i * kElem + 0x98);
        i = (p >= arr && p < arr + (size_t)count * kElem) ? (int)((p - arr) / kElem) : -1;
    }
    return -1;
}

static void DrawRow(uint8_t* c)
{
    uint8_t* arr = ContArr(c);
    int count = ContCnt(c);
    if (count > kMaxElems) return;

    for (int i = 0; i < count; ++i) g_spSavedFlags[i] = Fl(arr + (size_t)i * kElem);
    for (int k = 0; k < g_spN; ++k) memcpy(g_spSavedElem[k], arr + (size_t)g_spSet[k] * kElem, kElem);

    if (!g_spRestTaken)
    {
        for (int k = 0; k < g_spN; ++k)
        {
            g_spColor38[k] = *(uint32_t*)(g_spSavedElem[k] + 0x38);
            g_spColor50[k] = *(uint32_t*)(g_spSavedElem[k] + 0x50);
            for (int a = 0; a < 2; ++a)
                if (g_spSet[k] == g_spArrowIdx[a]) memcpy(g_spArrowRest[a], g_spSavedElem[k], kElem);
        }
        g_spRestTaken = true;
    }

    float dLx[4], dLy[4], dWx[4], dWy[4];
    for (int r = 0; r < g_spNRoots; ++r)
    {
        uint8_t* s4 = arr + (size_t)g_spRoots[r] * kElem;
        uint8_t* s3 = arr + (size_t)g_spRootRef[r] * kElem;
        dLx[r] = *F(s4, 0x20) - *F(s3, 0x20);
        dLy[r] = *F(s4, 0x24) - *F(s3, 0x24);
        dWx[r] = *F(s4, 0x3C) - *F(s3, 0x3C);
        dWy[r] = *F(s4, 0x40) - *F(s3, 0x40);
        if (g_spPairSrc[r] >= 0)
        {
            uint8_t* a4 = arr + (size_t)g_spPairSrc[r] * kElem;
            uint8_t* a3 = arr + (size_t)g_spPairRef[r] * kElem;
            dWx[r] = *F(a4, 0x3C) - *F(a3, 0x3C);
            dWy[r] = *F(a4, 0x40) - *F(a3, 0x40);
        }
    }

    for (int i = 0; i < count; ++i)
    {
        bool keep = false;
        for (int k = 0; k < g_spN; ++k) if (g_spSet[k] == i) { keep = true; break; }
        if (!keep) Fl(arr + (size_t)i * kElem) |= 0x20000000u;
    }

    for (int a = 0; a < 2; ++a)
        if (g_spArrowIdx[a] >= 0) memcpy(arr + (size_t)g_spArrowIdx[a] * kElem, g_spArrowRest[a], kElem);

    for (int k = 0; k < g_spN; ++k)
    {
        uint8_t* e = arr + (size_t)g_spSet[k] * kElem;
        int root = RootOf(arr, count, g_spSet[k]);
        if (root < 0) continue;
        if (g_spSet[k] == g_spRoots[root]) { *F(e, 0x20) += dLx[root]; *F(e, 0x24) += dLy[root]; }
        *F(e, 0x3C) += dWx[root];
        *F(e, 0x40) += dWy[root];
    }

    for (int k = 0; k < g_spN; ++k)
    {
        uint8_t* e = arr + (size_t)g_spSet[k] * kElem;
        *(uint32_t*)(e + 0x38) = g_spColor38[k];
        *(uint32_t*)(e + 0x50) = g_spColor50[k];
    }

    *(uint16_t*)(arr + (size_t)g_spLabel * kElem + 0x78) = kMsgLabel;
    *(uint16_t*)(arr + (size_t)g_spValue * kElem + 0x78) = kMsgValue;

    uint8_t* pe = arr + (size_t)g_spPanel * kElem;
    float s = 1.0f;
    if (g_menuObj && Cursor(g_menuObj) == 6)
        s = sinf((float)((FrameCtr() * 10u) % 360u) * 0.017453292f) * 0.01f + 1.01f;
    float ratio = *F(pe, 0x2C) != 0 ? s / *F(pe, 0x2C) : 1.0f;
    *F(pe, 0x2C) = s;
    *F(pe, 0x30) = s;
    *F(pe, 0x44) *= ratio;
    *F(pe, 0x48) *= ratio;
    uint8_t* le = arr + (size_t)g_spLabel * kElem;
    *F(le, 0x44) *= ratio;
    *F(le, 0x48) *= ratio;

    g_renderTramp(c);

    for (int k = 0; k < g_spN; ++k) memcpy(arr + (size_t)g_spSet[k] * kElem, g_spSavedElem[k], kElem);
    for (int i = 0; i < count; ++i) Fl(arr + (size_t)i * kElem) = g_spSavedFlags[i];
}

static void PlaceArrow(uint8_t* c)
{
    if (g_arrowIdx < 0 || !g_menuObj || g_spNRoots < 1) return;
    uint8_t* arr = ContArr(c);
    uint8_t* a = arr + (size_t)g_arrowIdx * kElem;
    uint8_t* g4 = arr + (size_t)g_spRoots[0] * kElem;
    uint8_t* g3 = arr + (size_t)g_spRootRef[0] * kElem;
    int cur = Cursor(g_menuObj);
    static int prev = -1;
    auto put = [&](float y)
    {
        *F(a, 0x20) = g_arrowRowX; *F(a, 0x3C) = g_arrowRowX; *F(a, 0xB0) = g_arrowRowX;
        *F(a, 0x24) = y;           *F(a, 0x40) = y;
    };
    if (prev == 6 && cur == 4) put(*F(g4, 0x24));
    prev = cur;
    if (cur >= 0 && cur <= 4) { g_arrowRowX = *F(a, 0x20); return; }
    if (cur == 6) put(*F(g4, 0x24) + (*F(g4, 0x24) - *F(g3, 0x24)));
}

static void HookRender(void* contv)
{
    uint8_t* c = (uint8_t*)contv;
    if (c == g_spCont && ContArr(c))
    {
        PlaceArrow(c);
        if (g_textOk) DrawRow(c);
    }
    g_renderTramp(contv);
}

static void Deactivate() { g_spCont = nullptr; }

static void HookInit(void* obj)
{
    OrigInit(obj);
    Deactivate();
    if (!FullLayout()) return;
    PrepareText();
    g_menuObj = obj;
    if (!PrepareRow(obj)) Log("menu: could not add the Frame Rate row");
}

static bool HookExit(void* obj)
{
    Deactivate();
    return OrigExit(obj);
}

static void* HookDtor(void* obj, int flags)
{
    Deactivate();
    return OrigDtor(obj, flags);
}

static bool HandleInput(void* obj)
{
    int& cur = Cursor(obj);
    uint64_t rep = RepBits(), trig = TrigBits();
    bool up = rep & 0x1000, down = rep & 0x2000;
    bool left = trig & 0x4000, right = trig & 0x8000;

    if (cur == 6)
    {
        if (up)   { cur = 4; Sound(0x83); return true; }
        if (down) { cur = 5; *(int*)((uint8_t*)obj + 0x54) = 1; Sound(0x83); return true; }
        if (left || right)
        {
            int f = g_cfg.targetFps + (right ? 30 : -30);
            if (f > 120) f = 30;
            if (f < 30) f = 120;
            g_cfg.targetFps = f;
            g.haveHistory = false;
            SaveTargetFps();
            RefreshValueText();
            Sound(0x83);
            return true;
        }
        return false;
    }
    if (cur == 4 && down && !up) { cur = 6; Sound(0x83); return true; }
    if (cur == 5 && up && !down) { cur = 6; Sound(0x83); return true; }
    return false;
}

static void HookUpdate(void* obj)
{
    if (State(obj) == 0 && FullLayout() && g_textOk && g_spCont && HandleInput(obj))
    {
        void** vt = *(void***)obj;
        ((ObjFn)vt[5])(obj);
        return;
    }
    OrigUpdate(obj);
}

static bool Expect(uint32_t rva, const char* hex)
{
    uint8_t* p = M + rva;
    for (int i = 0; hex[i] && hex[i + 1]; i += 2)
    {
        unsigned v;
        sscanf(hex + i, "%2x", &v);
        if (p[i / 2] != (uint8_t)v) return false;
    }
    return true;
}

static void PatchPtr(void** slot, void* val)
{
    DWORD old;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old);
    *slot = val;
    VirtualProtect(slot, sizeof(void*), old, &old);
}

static void WriteAbsJump(uint8_t* at, void* to)
{
    DWORD old;
    VirtualProtect(at, 16, PAGE_EXECUTE_READWRITE, &old);
    at[0] = 0x48; at[1] = 0xB8; *(void**)(at + 2) = to; at[10] = 0xFF; at[11] = 0xE0;
    VirtualProtect(at, 16, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 16);
}

static bool Install()
{
    if (g_ok) return true;
    M = (uint8_t*)GetModuleHandleA("main.dll");
    if (!M) return false;
    void** vt = (void**)(M + 0x675180);
    bool sig = Expect(0x1C8A80, "440fb7c2418bc04181e0ff0f000048c1") &&
               Expect(0x15B460, "40574883ec3048c7442420feffffff48") &&
               Expect(0x15B940, "40534883ec2048638188000000488bd9") &&
               Expect(0x15C890, "48895c241848897c2420415441564157") &&
               Expect(0x1B8260, "40574881ece000000080792e00") &&
               vt[0] == M + 0x15A3F0 && vt[1] == M + 0x15B460 && vt[2] == M + 0x15B940 &&
               vt[3] == M + 0x15C460 && vt[5] == M + 0x15C890;
    if (!sig)
    {
        Log("menu: main.dll is not the expected Steam build, Frame Rate menu row disabled");
        return false;
    }

    PlaySound  = (PlaySoundFn)(M + 0x44E470);
    OrigDtor   = (DtorFn)vt[0];
    OrigInit   = (ObjFn)vt[1];
    OrigUpdate = (ObjFn)vt[2];
    OrigExit   = (ExitFn)vt[3];

    uint8_t* t = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!t) return false;
    uint8_t* r = M + 0x1B8260;
    memcpy(t, r, 13);
    t[13] = 0xFF; t[14] = 0x25; *(int32_t*)(t + 15) = 0; *(void**)(t + 19) = r + 13;
    g_renderTramp = (RenderFn)t;

    WriteAbsJump(M + 0x1C8A80, (void*)&GetMsgImpl);
    WriteAbsJump(r, (void*)&HookRender);
    PatchPtr(&vt[0], (void*)&HookDtor);
    PatchPtr(&vt[1], (void*)&HookInit);
    PatchPtr(&vt[2], (void*)&HookUpdate);
    PatchPtr(&vt[3], (void*)&HookExit);
    g_ok = true;
    Log("menu: Display Settings hooks installed");
    return true;
}

}
