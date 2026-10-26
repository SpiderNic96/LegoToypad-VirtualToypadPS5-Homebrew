/* ======================================================================================
 * Virtual Toy Pad overlay engine (SpiderNic96, 2026; GPL-3.0-or-later).
 *
 * A retained-mode compositor driven by the injector over TCP (see ovl_proto.h):
 * widgets (ImageBox / Panel / Label) are created once from a DEFINE list, textures are
 * uploaded once, and FRAME deltas only touch the properties that changed. Everything that
 * touches PUI runs on the UI thread (Application.Update hook) - or on our own render thread
 * when only the legacy main-thread guard could be installed, the same split the HUD overlay
 * uses. The receiver thread only parses messages into the desired state under g_state_mx.
 * ====================================================================================== */

struct MonoMethodSignature;
struct MonoType;
struct MonoArray;
extern "C" {
MonoClass* mono_class_get_parent(MonoClass*);
MonoMethod* mono_class_get_methods(MonoClass*, void**);
const char* mono_method_get_name(MonoMethod*);
MonoMethodSignature* mono_method_signature(MonoMethod*);
std::uint32_t mono_signature_get_param_count(MonoMethodSignature*);
MonoType* mono_signature_get_params(MonoMethodSignature*, void**);
char* mono_type_get_name(MonoType*);
void mono_free(void*);
MonoClass* mono_get_byte_class();
MonoArray* mono_array_new(MonoDomain*, MonoClass*, std::uintptr_t);
char* mono_array_addr_with_size(MonoArray*, int, std::uintptr_t);
MonoClass* mono_object_get_class(MonoObject*);
}

constexpr float kCanvasW = 1920.0f;

struct WDesired {
    std::uint8_t type = 0;
    float x = 0, y = 0, w = 0, h = 0;
    std::uint32_t vis = 0;
    float alpha = 1.0f;
    std::uint8_t c[4] = {255, 255, 255, 255};
    std::uint32_t tex = 0;
    std::string text;
    std::uint16_t font = 24;
    std::uint8_t align = 0, lines = 1;
    std::uint32_t dirty = 0;
};

struct WLive {
    std::uint8_t type = 0;
    std::uint32_t handle = 0;
    std::uint32_t font_handle = 0;  /* boxed UIFont kept alive */
    std::uint32_t tex = 0;          /* texture currently shown */
    std::uint16_t font = 0;
};

struct Tex {
    std::vector<std::uint8_t> data;   /* pending upload (RGBA or PNG) */
    std::uint16_t w = 0, h = 0;
    bool png = false;
    bool pending = false;
    std::uint32_t texture = 0;        /* Texture2D handle (pixels mode) */
    std::uint32_t asset = 0;          /* ImageAsset handle */
    std::string uri;                  /* file mode: file:///Temp/vtpN.png */
    int failures = 0;
};

pthread_mutex_t g_state_mx = PTHREAD_MUTEX_INITIALIZER;
std::vector<WDesired> g_desired;           /* guarded by g_state_mx */
std::uint32_t g_define_gen = 0;            /* guarded */
std::map<std::uint32_t, Tex> g_tex;        /* map guarded; handles only touched on the UI thread */
std::vector<std::uint32_t> g_tex_free;     /* guarded */
std::uint32_t g_tex_mode_pref = 0;         /* 0 auto, 1 pixels, 2 files */
std::atomic<int> g_client_connected{0};

std::vector<WLive> g_live;                 /* UI thread */
std::uint32_t g_live_gen = 0;
std::uint32_t g_container = 0;             /* Panel holding our widgets */
MonoObject* g_live_root = nullptr;
float g_scale = 1.0f;
std::atomic<float> g_scene_w{0.0f}, g_scene_h{0.0f};
std::atomic<int> g_scene_up{0};

/* ------------------------------------------------------------- bindings */

MonoClass *g_widget_class{}, *g_panel_class{}, *g_image_class{}, *g_label_class{}, *g_font_class{},
    *g_color_class{}, *g_asset_class{}, *g_texture_class{};
MonoMethod *g_append{}, *g_remove{}, *g_font_ctor{}, *g_asset_uri_ctor{}, *g_asset_tex_ctor{},
    *g_texture_ctor{}, *g_set_pixels{}, *g_load_async{}, *g_dispose_tex{}, *g_dispose_asset{};
std::uint32_t g_caps = 0;
const char* g_temp_dir = nullptr;

MonoMethod* method_exact(MonoClass* type, const char* name, std::initializer_list<const char*> params) {
    for (MonoClass* c = type; c; c = mono_class_get_parent(c)) {
        void* iter = nullptr;
        while (MonoMethod* m = mono_class_get_methods(c, &iter)) {
            if (std::strcmp(mono_method_get_name(m), name)) continue;
            MonoMethodSignature* sig = mono_method_signature(m);
            if (!sig || mono_signature_get_param_count(sig) != params.size()) continue;
            void* pi = nullptr;
            bool match = true;
            for (const char* expected : params) {
                char* actual = mono_type_get_name(mono_signature_get_params(sig, &pi));
                match = match && actual && !std::strcmp(actual, expected);
                if (actual) mono_free(actual);
            }
            if (match) return m;
        }
    }
    return nullptr;
}

MonoMethod* setter_of(MonoClass* c, const char* name) {
    for (; c; c = mono_class_get_parent(c)) {
        MonoProperty* p = mono_class_get_property_from_name_(c, name);
        if (p) return mono_property_get_set_method_(p);
    }
    return nullptr;
}

MonoMethod* getter_of(MonoClass* c, const char* name) {
    for (; c; c = mono_class_get_parent(c)) {
        MonoProperty* p = mono_class_get_property_from_name_(c, name);
        if (p) return mono_property_get_get_method_(p);
    }
    return nullptr;
}

/* invoke, logging (rate limited) any managed exception */
Tex* tex_get(std::uint32_t id) {
    pthread_mutex_lock(&g_state_mx);
    auto it = g_tex.find(id);
    Tex* t = it == g_tex.end() ? nullptr : &it->second;
    pthread_mutex_unlock(&g_state_mx);
    return t;
}

bool call(MonoMethod* m, void* obj, void** args, MonoObject** out = nullptr) {
    if (!m) return false;
    MonoObject* exc = nullptr;
    MonoObject* r = mono_runtime_invoke_(m, obj, args, &exc);
    if (exc) {
        static int logged = 0;
        if (logged++ < 20) log_line("managed exception in %s", mono_method_get_name(m));
        return false;
    }
    if (out) *out = r;
    return true;
}

/* property cache: (class, name) -> setter */
struct PropKey { MonoClass* c; const char* n; bool operator<(const PropKey& o) const { return c != o.c ? c < o.c : std::strcmp(n, o.n) < 0; } };
std::map<PropKey, MonoMethod*> g_setters;

MonoMethod* cached_setter(MonoClass* c, const char* name) {
    auto it = g_setters.find({c, name});
    if (it != g_setters.end()) return it->second;
    MonoMethod* m = setter_of(c, name);
    g_setters[{c, name}] = m;
    if (!m) log_line("no setter %s", name);
    return m;
}

bool set_f(MonoClass* c, MonoObject* o, const char* n, float v) { void* a[] = {&v}; return call(cached_setter(c, n), o, a); }
bool set_i(MonoClass* c, MonoObject* o, const char* n, int v) { void* a[] = {&v}; return call(cached_setter(c, n), o, a); }
bool set_b(MonoClass* c, MonoObject* o, const char* n, bool v) { void* a[] = {&v}; return call(cached_setter(c, n), o, a); }
bool set_o(MonoClass* c, MonoObject* o, const char* n, void* v) { void* a[] = {v}; return call(cached_setter(c, n), o, a); }
bool set_color(MonoClass* c, MonoObject* o, const char* n, const std::uint8_t rgba[4]) {
    float col[4] = {rgba[0] / 255.0f, rgba[1] / 255.0f, rgba[2] / 255.0f, rgba[3] / 255.0f};
    void* a[] = {col};   /* UIColor is a struct of 4 floats, passed by pointer */
    return call(cached_setter(c, n), o, a);
}

MonoObject* target(std::uint32_t h) { return h ? mono_gchandle_get_target_(h) : nullptr; }
void release(std::uint32_t& h) { if (h) mono_gchandle_free_(h); h = 0; }

void bind_engine() {
    g_widget_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI.UI2", "Widget");
    g_panel_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI.UI2", "Panel");
    g_image_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI.UI2", "ImageBox");
    g_label_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI.UI2", "Label");
    g_font_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI.UI2", "UIFont");
    g_color_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI", "UIColor");
    g_asset_class = mono_class_from_name_(g_pui_image, "Sce.PlayStation.PUI", "ImageAsset");
    MonoImage* core = open_image(kCoreDll);
    g_texture_class = core ? mono_class_from_name_(core, "Sce.PlayStation.Core.Graphics", "Texture2D") : nullptr;

    g_append = g_widget_class ? mono_class_get_method_from_name_(g_widget_class, "AppendChild", 1) : nullptr;
    g_remove = g_widget_class ? mono_class_get_method_from_name_(g_widget_class, "RemoveChild", 1) : nullptr;
    g_font_ctor = g_font_class ? mono_class_get_method_from_name_(g_font_class, ".ctor", 3) : nullptr;
    if (g_asset_class) {
        g_asset_uri_ctor = method_exact(g_asset_class, ".ctor", {"System.String"});
        g_asset_tex_ctor = method_exact(g_asset_class, ".ctor", {"Sce.PlayStation.Core.Graphics.Texture"});
        g_dispose_asset = mono_class_get_method_from_name_(g_asset_class, "Dispose", 0);
    }
    if (g_texture_class) {
        g_texture_ctor = method_exact(g_texture_class, ".ctor",
            {"System.Int32", "System.Int32", "System.Boolean", "Sce.PlayStation.Core.Graphics.PixelFormat"});
        g_set_pixels = method_exact(g_texture_class, "SetPixels", {"System.Int32", "System.Array"});
        g_dispose_tex = mono_class_get_method_from_name_(g_texture_class, "Dispose", 0);
    }
    g_load_async = g_image_class ? mono_class_get_method_from_name_(g_image_class, "LoadAsync", 1) : nullptr;

    g_caps = 0;
    if (g_texture_ctor && g_set_pixels && g_asset_tex_ctor && setter_of(g_image_class, "Image")) g_caps |= OVL_CAP_PIXELS;
    if (g_load_async || (g_asset_uri_ctor && setter_of(g_image_class, "Image"))) g_caps |= OVL_CAP_FILES;
    if (setter_of(g_image_class, "FilterColor")) g_caps |= OVL_CAP_FILTER;
    if (g_panel_class && setter_of(g_panel_class, "BackgroundColor")) g_caps |= OVL_CAP_PANEL;
    if (setter_of(g_label_class, "MaxLineCount") && setter_of(g_label_class, "TextTrimming")) g_caps |= OVL_CAP_TRIM;
    log_line("engine bindings widget=%d panel=%d image=%d label=%d font=%d asset=%d texture=%d caps=0x%x",
             g_widget_class != nullptr, g_panel_class != nullptr, g_image_class != nullptr,
             g_label_class != nullptr, g_font_ctor != nullptr, g_asset_class != nullptr,
             g_texture_class != nullptr, g_caps);
}

/* ------------------------------------------------------------- textures (UI thread) */

bool ensure_temp_dir() {
    if (g_temp_dir) return true;
    static const char* cands[] = {"/Temp", "/download", "/system_tmp", nullptr};
    for (int i = 0; cands[i]; ++i) {
        char p[64];
        std::snprintf(p, sizeof p, "%s/vtpovl.probe", cands[i]);
        FILE* f = std::fopen(p, "wb");
        if (!f) continue;
        bool ok = std::fwrite("ok", 1, 2, f) == 2;
        std::fclose(f);
        unlink(p);
        if (ok) { g_temp_dir = cands[i]; log_line("file textures in %s", g_temp_dir); return true; }
    }
    return false;
}

bool use_pixels() {
    if (g_tex_mode_pref == 2) return false;
    if (g_tex_mode_pref == 1) return (g_caps & OVL_CAP_PIXELS) != 0;
    return (g_caps & OVL_CAP_PIXELS) != 0;
}

void tex_dispose(Tex& t) {
    if (MonoObject* a = target(t.asset)) call(g_dispose_asset, a, nullptr);
    if (MonoObject* x = target(t.texture)) call(g_dispose_tex, x, nullptr);
    release(t.asset);
    release(t.texture);
    if (!t.uri.empty()) {
        unlink(t.uri.c_str() + 7);   /* strip file:// */
        t.uri.clear();
    }
}

/* creates the managed texture for one pending entry; data is moved out under the lock */
bool tex_upload(std::uint32_t id, Tex& t, std::vector<std::uint8_t>& data, bool png) {
    tex_dispose(t);
    if (!png && use_pixels()) {
        int w = t.w, h = t.h, fmt = 1;   /* PixelFormat.Rgba */
        bool mip = false;
        void* targs[] = {&w, &h, &mip, &fmt};
        MonoObject* obj = mono_object_new_(g_domain, g_texture_class);
        if (!obj) return false;
        std::uint32_t th = mono_gchandle_new_(obj, 1);
        if (!call(g_texture_ctor, target(th), targs)) { release(th); return false; }
        MonoArray* arr = mono_array_new(g_domain, mono_get_byte_class(), data.size());
        std::uint32_t ah = arr ? mono_gchandle_new_(reinterpret_cast<MonoObject*>(arr), 1) : 0;
        if (!ah) { release(th); return false; }
        std::memcpy(mono_array_addr_with_size(reinterpret_cast<MonoArray*>(target(ah)), 1, 0), data.data(), data.size());
        int level = 0;
        void* pargs[] = {&level, target(ah)};
        bool ok = call(g_set_pixels, target(th), pargs);
        release(ah);
        if (!ok) { release(th); return false; }
        MonoObject* asset = mono_object_new_(g_domain, g_asset_class);
        std::uint32_t sh = asset ? mono_gchandle_new_(asset, 1) : 0;
        void* aargs[] = {target(th)};
        if (!sh || !call(g_asset_tex_ctor, target(sh), aargs)) { release(sh); release(th); return false; }
        t.texture = th;
        t.asset = sh;
        return true;
    }
    /* file mode: write the PNG where SceShellUI can read it */
    if (!png || !ensure_temp_dir()) return false;
    char path[96];
    const bool jpeg = data.size() > 2 && data[0] == 0xFF && data[1] == 0xD8;   /* wallpapers */
    std::snprintf(path, sizeof path, "%s/vtp%u.%s", g_temp_dir, id, jpeg ? "jpg" : "png");
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = std::fclose(f) == 0 && ok;
    if (!ok) { unlink(path); return false; }
    t.uri = std::string("file://") + path;
    if (g_asset_uri_ctor && setter_of(g_image_class, "Image")) {
        MonoString* s = mono_string_new_(g_domain, t.uri.c_str());
        MonoObject* asset = mono_object_new_(g_domain, g_asset_class);
        std::uint32_t sh = asset ? mono_gchandle_new_(asset, 1) : 0;
        void* aargs[] = {s};
        if (sh && call(g_asset_uri_ctor, target(sh), aargs)) t.asset = sh;
        else release(sh);
    }
    return true;
}

/* ------------------------------------------------------------- widgets (UI thread) */

MonoClass* class_of_type(std::uint8_t type) {
    return type == OVL_W_IMAGE ? g_image_class : type == OVL_W_PANEL ? g_panel_class : g_label_class;
}

std::uint32_t new_widget(std::uint8_t type, int index) {
    MonoClass* c = class_of_type(type);
    if (!c) return 0;
    MonoObject* o = mono_object_new_(g_domain, c);
    if (!o) return 0;
    std::uint32_t h = mono_gchandle_new_(o, 1);
    mono_runtime_object_init_(target(h));
    MonoObject* w = target(h);
    char name[32];
    std::snprintf(name, sizeof name, "id_vtpovl_%d", index);
    set_o(c, w, "Name", mono_string_new_(g_domain, name));
    set_b(c, w, "Visible", false);
    if (setter_of(c, "Focusable")) set_b(c, w, "Focusable", false);
    if (type == OVL_W_IMAGE) {
        if (setter_of(c, "ImageScaleType")) set_i(c, w, "ImageScaleType", 1);          /* stretch */
        if (setter_of(c, "DisposeImageOutOfScreen")) set_b(c, w, "DisposeImageOutOfScreen", false);
    } else if (type == OVL_W_PANEL) {
        std::uint8_t clear[4] = {0, 0, 0, 0};
        set_color(c, w, "BackgroundColor", clear);
    } else {
        set_i(c, w, "VerticalAlignment", 0);
        set_b(c, w, "FitHeightToText", true);
        if (setter_of(c, "EnableThemedColor")) set_b(c, w, "EnableThemedColor", false);
    }
    return h;
}

void destroy_live(MonoObject* parent) {
    for (auto& l : g_live) {
        if (MonoObject* w = target(l.handle)) {
            if (parent) { void* a[] = {w}; call(g_remove, parent, a); }
        }
        release(l.handle);
        release(l.font_handle);
    }
    g_live.clear();
}

void drop_container(MonoObject* root) {
    MonoObject* cont = target(g_container);
    destroy_live(cont ? cont : root);
    if (cont && root) { void* a[] = {cont}; call(g_remove, root, a); }
    release(g_container);
}

bool build_live(MonoObject* root, const std::vector<std::uint8_t>& types) {
    MonoObject* parent = root;
    if (g_panel_class && !g_container) {
        std::uint32_t h = new_widget(OVL_W_PANEL, 9999);
        if (h) {
            MonoObject* p = target(h);
            set_f(g_panel_class, p, "X", 0.0f);
            set_f(g_panel_class, p, "Y", 0.0f);
            set_f(g_panel_class, p, "Width", g_scene_w.load());
            set_f(g_panel_class, p, "Height", g_scene_h.load());
            set_b(g_panel_class, p, "Visible", true);
            void* a[] = {p};
            if (call(g_append, root, a)) g_container = h;
            else release(h);
        }
    }
    if (MonoObject* c = target(g_container)) parent = c;
    g_live.assign(types.size(), WLive{});
    int made = 0;
    for (std::size_t i = 0; i < types.size(); ++i) {
        g_live[i].type = types[i];
        std::uint32_t h = new_widget(types[i], (int)i);
        if (!h) continue;
        void* a[] = {target(h)};
        if (!call(g_append, parent, a)) { release(h); continue; }
        g_live[i].handle = h;
        ++made;
    }
    log_line("widgets built %d/%zu container=%d scale=%.3f", made, types.size(), g_container != 0, (double)g_scale);
    return made > 0;
}

void apply_one(std::size_t i, const WDesired& d, std::uint32_t mask, std::uint32_t& retry) {
    WLive& l = g_live[i];
    MonoObject* w = target(l.handle);
    if (!w) return;
    MonoClass* c = class_of_type(l.type);
    const float s = g_scale;
    if (mask & OVL_F_GEOM) {
        set_f(c, w, "X", d.x * s);
        set_f(c, w, "Y", d.y * s);
        if (l.type != OVL_W_LABEL) {
            set_f(c, w, "Width", d.w * s);
            set_f(c, w, "Height", d.h * s);
        } else if (d.w > 0) {
            set_b(c, w, "FitWidthToText", false);
            set_f(c, w, "Width", d.w * s);
        } else {
            set_b(c, w, "FitWidthToText", true);
        }
    }
    if (mask & OVL_F_ALPHA) set_f(c, w, "Opacity", d.alpha);
    if (mask & OVL_F_COLOR) {
        if (l.type == OVL_W_IMAGE) { if (g_caps & OVL_CAP_FILTER) set_color(c, w, "FilterColor", d.c); }
        else if (l.type == OVL_W_PANEL) set_color(c, w, "BackgroundColor", d.c);
        else set_color(c, w, "TextColor", d.c);
    }
    if ((mask & OVL_F_TEX) && l.type == OVL_W_IMAGE) {
        Tex* t = d.tex ? tex_get(d.tex) : nullptr;
        if (d.tex == 0) {
            if (cached_setter(c, "Image")) set_o(c, w, "Image", nullptr);
            l.tex = 0;
        } else if (!t || t->pending || (!t->asset && t->uri.empty())) {
            if (t && t->failures > 2) {}   /* give up quietly */
            else retry |= OVL_F_TEX;       /* not uploaded yet: try next frame */
        } else if (t->asset) {
            set_o(c, w, "Image", target(t->asset));
            l.tex = d.tex;
        } else {
            void* a[] = {mono_string_new_(g_domain, t->uri.c_str())};
            call(g_load_async, w, a);
            l.tex = d.tex;
        }
    }
    if ((mask & OVL_F_TEXT) && l.type == OVL_W_LABEL) {
        int px = (int)(d.font * s + 0.5f);
        if (px < 6) px = 6;
        if (px != l.font || !l.font_handle) {
            MonoObject* boxed = mono_object_new_(g_domain, g_font_class);
            std::uint32_t fh = boxed ? mono_gchandle_new_(boxed, 1) : 0;
            if (fh) {
                int style = 0, weight = 0;
                void* fa[] = {&px, &style, &weight};
                void* real = mono_object_unbox_(target(fh));
                if (call(g_font_ctor, real, fa) && set_o(c, w, "Font", real)) {
                    release(l.font_handle);
                    l.font_handle = fh;
                    l.font = (std::uint16_t)px;
                } else release(fh);
            }
        }
        set_i(c, w, "HorizontalAlignment", d.align);
        if (g_caps & OVL_CAP_TRIM) {
            set_i(c, w, "MaxLineCount", d.lines ? d.lines : 1);
            set_i(c, w, "TextTrimming", 1);
        }
        set_o(c, w, "Text", mono_string_new_(g_domain, d.text.c_str()));
    }
    if (mask & OVL_F_VIS) set_b(c, w, "Visible", d.vis != 0);
}

/* one UI-thread pass: scene tracking, texture uploads, (re)build and property deltas */
void ui_tick() {
    if (!g_runtime_ready.load()) return;
    if (!refresh_game_scene()) {
        if (!g_live.empty() || g_container) {
            g_live.clear();      /* the scene and its widgets are gone with the game */
            release(g_container);
            g_live_root = nullptr;
            log_line("game scene closed; overlay idle");
        }
        g_scene_up.store(0);
        return;
    }
    MonoObject* root = get_root_widget();
    if (!root) { g_scene_up.store(0); return; }
    if (root != g_live_root) {
        if (g_live_root) { g_live.clear(); release(g_container); }
        g_live_root = root;
        MonoMethod* gw = getter_of(g_widget_class, "Width");
        MonoMethod* gh = getter_of(g_widget_class, "Height");
        MonoObject* rw = nullptr; MonoObject* rh = nullptr;
        call(gw, root, nullptr, &rw);
        call(gh, root, nullptr, &rh);
        float sw = rw ? *static_cast<float*>(mono_object_unbox_(rw)) : kCanvasW;
        float sh = rh ? *static_cast<float*>(mono_object_unbox_(rh)) : 1080.0f;
        if (sw < 320.0f || sw > 8192.0f) sw = kCanvasW;
        if (sh < 240.0f || sh > 8192.0f) sh = 1080.0f;
        g_scene_w.store(sw);
        g_scene_h.store(sh);
        g_scale = sw / kCanvasW;
        g_live_gen = 0;   /* rebuild on this scene */
        log_line("game scene %.0fx%.0f scale %.3f", (double)sw, (double)sh, (double)g_scale);
    }
    g_scene_up.store(1);

    /* texture frees + uploads (bounded per frame so a burst never stalls the UI) */
    std::vector<std::uint32_t> frees;
    std::vector<std::pair<std::uint32_t, bool>> todo;
    std::vector<std::vector<std::uint8_t>> payloads;
    pthread_mutex_lock(&g_state_mx);
    frees.swap(g_tex_free);
    std::size_t budget = 3u << 20;
    for (auto& kv : g_tex) {
        if (!kv.second.pending || budget == 0) continue;
        if (todo.size() >= 8) break;
        budget = kv.second.data.size() > budget ? 0 : budget - kv.second.data.size();
        todo.push_back({kv.first, kv.second.png});
        payloads.emplace_back(std::move(kv.second.data));
        kv.second.data.clear();
        kv.second.pending = false;
    }
    pthread_mutex_unlock(&g_state_mx);
    for (std::uint32_t id : frees) {
        Tex* tf = tex_get(id);
        if (!tf) continue;
        for (std::size_t i = 0; i < g_live.size(); ++i)
            if (g_live[i].tex == id)
                if (MonoObject* w = target(g_live[i].handle)) { set_o(g_image_class, w, "Image", nullptr); g_live[i].tex = 0; }
        tex_dispose(*tf);
        pthread_mutex_lock(&g_state_mx);
        g_tex.erase(id);
        pthread_mutex_unlock(&g_state_mx);
    }
    for (std::size_t k = 0; k < todo.size(); ++k) {
        Tex* tu = tex_get(todo[k].first);
        if (!tu) continue;
        if (!tex_upload(todo[k].first, *tu, payloads[k], todo[k].second)) {
            static int logged = 0;
            if (logged++ < 10) log_line("texture %u upload failed (%s)", todo[k].first, todo[k].second ? "png" : "rgba");
            tu->failures++;
        } else {
            /* widgets already showing this id get the new image */
            pthread_mutex_lock(&g_state_mx);
            for (std::size_t i = 0; i < g_desired.size(); ++i)
                if (g_desired[i].tex == todo[k].first) g_desired[i].dirty |= OVL_F_TEX;
            pthread_mutex_unlock(&g_state_mx);
        }
    }

    /* (re)build widgets for the current DEFINE */
    pthread_mutex_lock(&g_state_mx);
    std::uint32_t gen = g_define_gen;
    std::vector<std::uint8_t> types;
    if (gen != g_live_gen) {
        types.reserve(g_desired.size());
        for (auto& d : g_desired) { types.push_back(d.type); d.dirty = 0xFFFF; }
    }
    pthread_mutex_unlock(&g_state_mx);
    if (gen != g_live_gen) {
        MonoObject* cont = target(g_container);
        destroy_live(cont ? cont : root);
        if (!types.empty() && !build_live(root, types)) return;
        g_live_gen = gen;
    }

    /* property deltas */
    std::vector<std::pair<std::size_t, WDesired>> work;
    pthread_mutex_lock(&g_state_mx);
    if (g_desired.size() == g_live.size()) {
        for (std::size_t i = 0; i < g_desired.size(); ++i) {
            if (!g_desired[i].dirty) continue;
            work.push_back({i, g_desired[i]});
            g_desired[i].dirty = 0;
        }
    }
    pthread_mutex_unlock(&g_state_mx);
    std::vector<std::pair<std::size_t, std::uint32_t>> retry;
    for (auto& wk : work) {
        std::uint32_t again = 0;
        apply_one(wk.first, wk.second, wk.second.dirty, again);
        if (again) retry.push_back({wk.first, again});
    }
    if (!retry.empty()) {
        pthread_mutex_lock(&g_state_mx);
        for (auto& r : retry)
            if (r.first < g_desired.size()) g_desired[r.first].dirty |= r.second;
        pthread_mutex_unlock(&g_state_mx);
    }
}

/* ------------------------------------------------------------- receiver (own thread) */

int g_listen_fd = -1;

bool read_full(int fd, void* buf, std::size_t n) {
    auto* p = static_cast<std::uint8_t*>(buf);
    while (n) {
        ssize_t k = recv(fd, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= (std::size_t)k;
    }
    return true;
}

bool send_msg(int fd, std::uint16_t type, const void* payload, std::uint32_t len) {
    ovl_msg h{OVL_MAGIC, type, OVL_VERSION, len};
    if (send(fd, &h, sizeof h, 0) != (ssize_t)sizeof h) return false;
    const auto* p = static_cast<const std::uint8_t*>(payload);
    while (len) {
        ssize_t k = send(fd, p, len, 0);
        if (k <= 0) return false;
        p += k;
        len -= (std::uint32_t)k;
    }
    return true;
}

template <typename T> bool take(const std::uint8_t*& p, const std::uint8_t* end, T& v) {
    if ((std::size_t)(end - p) < sizeof(T)) return false;
    std::memcpy(&v, p, sizeof(T));
    p += sizeof(T);
    return true;
}

void handle_frame(const std::uint8_t* p, const std::uint8_t* end) {
    std::uint16_t count = 0;
    if (!take(p, end, count)) return;
    pthread_mutex_lock(&g_state_mx);
    for (std::uint16_t n = 0; n < count; ++n) {
        std::uint16_t id = 0, mask = 0;
        if (!take(p, end, id) || !take(p, end, mask)) break;
        WDesired dummy;
        WDesired& d = id < g_desired.size() ? g_desired[id] : dummy;
        bool ok = true;
        if (mask & OVL_F_GEOM) ok = take(p, end, d.x) && take(p, end, d.y) && take(p, end, d.w) && take(p, end, d.h);
        if (ok && (mask & OVL_F_VIS)) ok = take(p, end, d.vis);
        if (ok && (mask & OVL_F_ALPHA)) ok = take(p, end, d.alpha);
        if (ok && (mask & OVL_F_COLOR)) ok = take(p, end, d.c);
        if (ok && (mask & OVL_F_TEX)) ok = take(p, end, d.tex);
        if (ok && (mask & OVL_F_TEXT)) {
            std::uint16_t len = 0;
            ok = take(p, end, d.font) && take(p, end, d.align) && take(p, end, d.lines) && take(p, end, len) &&
                 (std::size_t)(end - p) >= len;
            if (ok) {
                d.text.assign(reinterpret_cast<const char*>(p), len);
                p += len;
                std::size_t pad = (4 - (len & 3)) & 3;
                p = (std::size_t)(end - p) >= pad ? p + pad : end;
            }
        }
        if (!ok) break;
        d.dirty |= mask;
    }
    pthread_mutex_unlock(&g_state_mx);
}

void handle_message(int fd, const ovl_msg& h, std::vector<std::uint8_t>& body) {
    const std::uint8_t* p = body.data();
    const std::uint8_t* end = p + body.size();
    switch (h.type) {
    case OVL_TEX:
    case OVL_TEXPNG: {
        std::uint32_t id = 0;
        std::uint16_t w = 0, hh = 0;
        if (!take(p, end, id) || !take(p, end, w) || !take(p, end, hh)) return;
        if (h.type == OVL_TEX && (std::size_t)(end - p) != (std::size_t)w * hh * 4) return;
        pthread_mutex_lock(&g_state_mx);
        Tex& t = g_tex[id];
        t.data.assign(p, end);
        t.w = w; t.h = hh;
        t.png = h.type == OVL_TEXPNG;
        t.pending = true;
        t.failures = 0;
        pthread_mutex_unlock(&g_state_mx);
        break;
    }
    case OVL_TEXFREE: {
        std::uint32_t id = 0;
        if (!take(p, end, id)) return;
        pthread_mutex_lock(&g_state_mx);
        g_tex_free.push_back(id);
        pthread_mutex_unlock(&g_state_mx);
        break;
    }
    case OVL_DEFINE: {
        std::uint16_t count = 0;
        if (!take(p, end, count) || count > OVL_MAX_WIDGETS || (std::size_t)(end - p) < count) return;
        pthread_mutex_lock(&g_state_mx);
        g_desired.assign(count, WDesired{});
        for (std::uint16_t i = 0; i < count; ++i) { g_desired[i].type = p[i]; g_desired[i].dirty = 0xFFFF; }
        ++g_define_gen;
        pthread_mutex_unlock(&g_state_mx);
        log_line("define %u widgets", count);
        break;
    }
    case OVL_FRAME:
        handle_frame(p, end);
        break;
    case OVL_CONFIG: {
        ovl_config c{};
        if (take(p, end, c)) g_tex_mode_pref = c.tex_mode;
        break;
    }
    case OVL_PING:
        send_msg(fd, OVL_PING, nullptr, 0);
        break;
    default:
        break;
    }
}

void hide_everything() {
    pthread_mutex_lock(&g_state_mx);
    for (auto& d : g_desired) { d.vis = 0; d.dirty |= OVL_F_VIS; }
    pthread_mutex_unlock(&g_state_mx);
}

void serve_client(int fd) {
    ovl_hello hello{};
    hello.caps = g_caps | (g_scene_up.load() ? OVL_CAP_SCENE : 0);
    hello.scene_w = g_scene_w.load();
    hello.scene_h = g_scene_h.load();
    hello.pid = getpid();
    pthread_mutex_lock(&g_state_mx);
    hello.textures = (std::uint32_t)g_tex.size();
    hello.widgets = (std::uint32_t)g_desired.size();
    pthread_mutex_unlock(&g_state_mx);
    if (!send_msg(fd, OVL_HELLO, &hello, sizeof hello)) return;
    g_client_connected.store(1);
    std::vector<std::uint8_t> body;
    for (;;) {
        ovl_msg h{};
        if (!read_full(fd, &h, sizeof h)) break;
        if (h.magic != OVL_MAGIC || h.version != OVL_VERSION || h.len > OVL_MAX_MSG) {
            log_line("bad message magic=%08x version=%u len=%u", h.magic, h.version, h.len);
            break;
        }
        body.resize(h.len);
        if (h.len && !read_full(fd, body.data(), h.len)) break;
        handle_message(fd, h, body);
        /* surface queued renderer log lines to the injector's log */
        pthread_mutex_lock(&g_log_mx);
        std::vector<std::string> lines;
        lines.swap(g_log_out);
        pthread_mutex_unlock(&g_log_mx);
        for (auto& ln : lines) send_msg(fd, OVL_STATUS, ln.data(), (std::uint32_t)ln.size());
    }
    g_client_connected.store(0);
    hide_everything();   /* the injector went away: never leave a stale overlay on screen */
}

bool initialize_server() {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { log_line("server socket failed"); return false; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(OVL_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) < 0 || listen(fd, 2) < 0) {
        log_line("server bind/listen failed port=%u", OVL_PORT);
        close(fd);
        return false;
    }
    g_listen_fd = fd;
    log_line("server listening 127.0.0.1:%u", OVL_PORT);
    return true;
}

void* background_render_thread(void*) {
    mono_thread_attach_(g_domain);
    for (;;) {
        ui_tick();
        usleep(16000);
    }
    return nullptr;
}

[[noreturn]] void run_server_loop() {
    if (g_background_render_mode) {
        pthread_t t;
        pthread_create(&t, nullptr, background_render_thread, nullptr);
    }
    for (;;) {
        int c = accept(g_listen_fd, nullptr, nullptr);
        if (c < 0) { usleep(100000); continue; }
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        serve_client(c);
        close(c);
    }
}
