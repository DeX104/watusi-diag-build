/*
 * WatusiDump.c
 * ARM64 iOS dylib – no SDK required, pure ObjC-runtime FFI.
 *
 * Behaviour:
 *   At load:   swizzle UIViewController -viewDidAppear: on every VC.
 *   On appear: if the class name contains "theme" (case-insensitive),
 *              attach a 3-tap UITapGestureRecognizer.
 *   On 3 taps: write NSHomeDirectory()/Documents/watusi_dump.txt
 *
 * Compile:
 *   clang -target aarch64-apple-ios14.0 -ffreestanding -fno-builtin
 *         -c WatusiDump.c -o WatusiDump.o
 * Link:
 *   ld.lld -flavor darwin -dylib -arch arm64
 *          -platform_version ios 14.0 14.0
 *          -install_name @rpath/WatusiDump.dylib
 *          -undefined dynamic_lookup
 *          WatusiDump.o -o WatusiDump.dylib
 */

/* ── basic types ─────────────────────────────────────────────────────────── */
typedef void*          id;
typedef void*          Class;
typedef void*          SEL;
typedef void*          Method;
typedef void*          IMP;
typedef signed char    BOOL;
typedef unsigned long  NSUInteger;
typedef long           NSInteger;
typedef unsigned long  uintptr_t;
typedef unsigned char  uint8_t;

/* ── C runtime (libSystem) ───────────────────────────────────────────────── */
typedef struct _FILE FILE;
extern FILE  *fopen(const char *path, const char *mode);
extern int    fprintf(FILE *f, const char *fmt, ...);
extern int    fclose(FILE *f);
extern int    snprintf(char *buf, unsigned long n, const char *fmt, ...);
extern void  *malloc(unsigned long n);
extern void   free(void *p);
extern int    strcmp(const char *a, const char *b);
extern int    strncmp(const char *a, const char *b, unsigned long n);
extern char  *strstr(const char *h, const char *needle);
extern int    toupper(int c);
extern void  *memset(void *s, int c, unsigned long n);
/* simple strcasestr – not in all libc headers on bare targets */
static char *my_strcasestr(const char *h, const char *n) {
    if (!*n) return (char*)h;
    for (; *h; h++) {
        const char *p = h, *q = n;
        while (*p && *q && (toupper((unsigned char)*p) == toupper((unsigned char)*q)))
            { p++; q++; }
        if (!*q) return (char*)h;
    }
    return 0;
}

/* ── ObjC runtime ─────────────────────────────────────────────────────────── */
extern Class  objc_getClass(const char *name);
extern SEL    sel_registerName(const char *str);
extern Class  object_getClass(id obj);
extern const char *class_getName(Class cls);
extern Method class_getInstanceMethod(Class cls, SEL sel);
extern void   method_exchangeImplementations(Method m1, Method m2);
extern IMP    method_getImplementation(Method m);
extern BOOL   class_addMethod(Class cls, SEL name, IMP imp, const char *types);
extern int    objc_getClassList(Class *buf, int n);
extern id     objc_getAssociatedObject(id obj, const void *key);
extern void   objc_setAssociatedObject(id obj, const void *key, id val, NSUInteger pol);
#define OBJC_ASSOCIATION_ASSIGN 0

/* Foundation */
extern id NSHomeDirectory(void);     /* returns NSString* */

/* ── objc_msgSend call helpers ───────────────────────────────────────────── */
/* Always cast to the exact signature – avoids variadic ABI issues on arm64 */
typedef id       (*MsgSend0)(id, SEL);
typedef id       (*MsgSend1)(id, SEL, id);
typedef id       (*MsgSend1i)(id, SEL, NSInteger);
typedef id       (*MsgSend2)(id, SEL, id, SEL);
typedef void     (*MsgSendV0)(id, SEL);
typedef void     (*MsgSendV1)(id, SEL, id);
typedef void     (*MsgSendV1u)(id, SEL, NSUInteger);
typedef void     (*MsgSendVB)(id, SEL, BOOL);
typedef BOOL     (*MsgSendB0)(id, SEL);
typedef const char* (*MsgSendCStr)(id, SEL);
typedef NSUInteger  (*MsgSendU0)(id, SEL);

extern id objc_msgSend(id, SEL, ...);  /* only used as a pointer, never called directly */
#define SEND     ((MsgSend0)   objc_msgSend)
#define SEND1    ((MsgSend1)   objc_msgSend)
#define SEND1i   ((MsgSend1i)  objc_msgSend)
#define SEND2    ((MsgSend2)   objc_msgSend)
#define SENDV0   ((MsgSendV0)  objc_msgSend)
#define SENDV1   ((MsgSendV1)  objc_msgSend)
#define SENDV1u  ((MsgSendV1u) objc_msgSend)
#define SENDVB   ((MsgSendVB)  objc_msgSend)
#define SENDB0   ((MsgSendB0)  objc_msgSend)
#define SENDCS   ((MsgSendCStr)objc_msgSend)
#define SENDU0   ((MsgSendU0)  objc_msgSend)

/* ── statics ──────────────────────────────────────────────────────────────── */
static IMP         s_orig_viewDidAppear = 0;
static const char  ASSOC_KEY[1] = {0};   /* pointer used as associated-object key */

/* ── helpers ──────────────────────────────────────────────────────────────── */
static const char *ns2c(id str) {
    if (!str) return "(nil)";
    static SEL s = 0;
    if (!s) s = sel_registerName("UTF8String");
    return SENDCS(str, s);
}

static const char *describe(id obj) {
    if (!obj) return "(nil)";
    static SEL s = 0;
    if (!s) s = sel_registerName("description");
    id d = SEND(obj, s);
    return ns2c(d);
}

/* ── dump ─────────────────────────────────────────────────────────────────── */
static void perform_dump(id vc) {
    /* build file path: NSHomeDirectory()/Documents/watusi_dump.txt */
    id home = NSHomeDirectory();
    const char *home_c = ns2c(home);
    char path[1024];
    snprintf(path, sizeof(path), "%s/Documents/watusi_dump.txt", home_c);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        fp = fopen("/tmp/watusi_dump.txt", "w");
        if (!fp) return;
    }

    fprintf(fp, "=== WatusiDump v1.0 ===\n");
    fprintf(fp, "HomePath: %s\n", home_c);

    /* ── VC class hierarchy ── */
    fprintf(fp, "\n=== ViewController class ===\n");
    Class c = object_getClass(vc);
    for (int i = 0; i < 8 && c; i++) {
        fprintf(fp, "  [%d] %s\n", i, class_getName(c));
        static SEL sup_sel = 0;
        if (!sup_sel) sup_sel = sel_registerName("superclass");
        c = (Class)SEND((id)c, sup_sel);
    }

    /* ── NSUserDefaults (filtered) ── */
    fprintf(fp, "\n=== NSUserDefaults (theme/watusi/lock/vault/record keys) ===\n");
    static SEL std_sel = 0, drep_sel = 0, keys_sel = 0,
               obj_sel = 0, val_sel = 0, cnt_sel = 0;
    if (!std_sel)  std_sel  = sel_registerName("standardUserDefaults");
    if (!drep_sel) drep_sel = sel_registerName("dictionaryRepresentation");
    if (!keys_sel) keys_sel = sel_registerName("allKeys");
    if (!obj_sel)  obj_sel  = sel_registerName("objectAtIndex:");
    if (!val_sel)  val_sel  = sel_registerName("objectForKey:");
    if (!cnt_sel)  cnt_sel  = sel_registerName("count");

    id ud = SEND((id)objc_getClass("NSUserDefaults"), std_sel);
    id dd = SEND(ud, drep_sel);
    id ks = SEND(dd, keys_sel);
    NSUInteger n = SENDU0(ks, cnt_sel);
    int found_keys = 0;
    for (NSUInteger i = 0; i < n; i++) {
        id k_obj = ((id (*)(id,SEL,NSUInteger))objc_msgSend)(ks, obj_sel, i);
        const char *k = ns2c(k_obj);
        if (!k) continue;
        if (my_strcasestr(k,"theme") || my_strcasestr(k,"watusi") ||
            my_strcasestr(k,"vault") || my_strcasestr(k,"lock")  ||
            my_strcasestr(k,"record") || strncmp(k,"WS",2)==0) {
            id v = SEND1(dd, val_sel, k_obj);
            fprintf(fp, "  %s = %s\n", k, describe(v));
            found_keys++;
        }
    }
    if (!found_keys) fprintf(fp, "  (none found)\n");

    /* ── All NSUserDefaults keys dump (full list) ── */
    fprintf(fp, "\n=== NSUserDefaults (all keys) ===\n");
    for (NSUInteger i = 0; i < n && i < 300; i++) {
        id k_obj = ((id (*)(id,SEL,NSUInteger))objc_msgSend)(ks, obj_sel, i);
        const char *k = ns2c(k_obj);
        id v = SEND1(dd, val_sel, k_obj);
        fprintf(fp, "  %s = %s\n", k ? k : "?", describe(v));
    }

    /* ── ObjC WS* classes with resolvedThemeIDForSelection: ── */
    fprintf(fp, "\n=== WS* classes responding to resolvedThemeIDForSelection: ===\n");
    static SEL rts_sel = 0;
    if (!rts_sel) rts_sel = sel_registerName("resolvedThemeIDForSelection:");
    int cls_n = objc_getClassList(0, 0);
    Class *cls_buf = (Class*)malloc((unsigned long)cls_n * sizeof(Class));
    if (cls_buf) {
        objc_getClassList(cls_buf, cls_n);
        for (int i = 0; i < cls_n; i++) {
            const char *nm = class_getName(cls_buf[i]);
            if (!nm || nm[0]!='W' || nm[1]!='S') continue;
            if (class_getInstanceMethod(cls_buf[i], rts_sel)) {
                fprintf(fp, "  %s\n", nm);
            }
        }
        free(cls_buf);
    }

    /* ── resolvedThemeIDForSelection: on VC self ── */
    fprintf(fp, "\n=== [vc resolvedThemeIDForSelection:sel] ===\n");
    Class vc_cls = object_getClass(vc);
    if (class_getInstanceMethod(vc_cls, rts_sel)) {
        for (NSInteger sel_v = 0; sel_v <= 5; sel_v++) {
            id res = SEND1i(vc, rts_sel, sel_v);
            fprintf(fp, "  sel=%ld -> %s\n", (long)sel_v, ns2c(res));
        }
    } else {
        fprintf(fp, "  VC does not respond\n");
    }

    /* ── isLicensed / settingsHasValidLicense ── */
    fprintf(fp, "\n=== License methods on VC ===\n");
    static SEL lic_sel = 0, svl_sel = 0, feat_sel = 0;
    if (!lic_sel)  lic_sel  = sel_registerName("isLicensed");
    if (!svl_sel)  svl_sel  = sel_registerName("settingsHasValidLicense");
    if (!feat_sel) feat_sel = sel_registerName("featureEnabled");
    if (class_getInstanceMethod(vc_cls, lic_sel))
        fprintf(fp, "  isLicensed = %d\n", (int)SENDB0(vc, lic_sel));
    if (class_getInstanceMethod(vc_cls, svl_sel))
        fprintf(fp, "  settingsHasValidLicense = %d\n", (int)SENDB0(vc, svl_sel));
    if (class_getInstanceMethod(vc_cls, feat_sel))
        fprintf(fp, "  featureEnabled = %d\n", (int)SENDB0(vc, feat_sel));

    fprintf(fp, "\n=== Done. Send this file. ===\n");
    fclose(fp);

    /* Show UIAlertController with confirmation */
    static SEL alert_sel = 0, pref_sel = 0, addact_sel = 0, pvc_sel = 0;
    if (!alert_sel) alert_sel = sel_registerName("alertControllerWithTitle:message:preferredStyle:");
    if (!pref_sel)  pref_sel  = sel_registerName("alertActionWithTitle:style:handler:");
    if (!addact_sel) addact_sel = sel_registerName("addAction:");
    if (!pvc_sel)   pvc_sel   = sel_registerName("presentViewController:animated:completion:");

    Class alert_cls = objc_getClass("UIAlertController");
    if (alert_cls && alert_sel) {
        /* Build NSString via stringWithUTF8String: */
        static SEL swu_sel = 0;
        if (!swu_sel) swu_sel = sel_registerName("stringWithUTF8String:");
        typedef id (*Fn)(id, SEL, const char *);
        id title_ns = ((Fn)objc_msgSend)((id)objc_getClass("NSString"), swu_sel,
                                          "WatusiDump");
        id msg_ns   = ((Fn)objc_msgSend)((id)objc_getClass("NSString"), swu_sel,
                                          "Dump scritto in Documents/watusi_dump.txt");

        typedef id (*AlertFn)(id, SEL, id, id, NSInteger);
        id alert = ((AlertFn)objc_msgSend)((id)alert_cls, alert_sel,
                                            title_ns, msg_ns, (NSInteger)1 /* UIAlertControllerStyleAlert */);

        typedef id (*ActionFn)(id, SEL, id, NSInteger, void*);
        static SEL act_sel = 0;
        if (!act_sel) act_sel = sel_registerName("alertActionWithTitle:style:handler:");
        id action_title = ((Fn)objc_msgSend)((id)objc_getClass("NSString"), swu_sel, "OK");
        id action = ((ActionFn)objc_msgSend)((id)objc_getClass("UIAlertAction"),
                                              act_sel, action_title, (NSInteger)0, (void*)0);
        SENDV1(alert, addact_sel, action);

        typedef void (*PresentFn)(id, SEL, id, BOOL, void*);
        ((PresentFn)objc_msgSend)(vc, pvc_sel, alert, (BOOL)1, (void*)0);
    }
}

/* ── hooked viewDidAppear: ───────────────────────────────────────────────── */
static void hooked_viewDidAppear(id self, SEL cmd, BOOL animated) {
    /* call original (now installed under "watusiOrigVDA:") */
    if (s_orig_viewDidAppear) {
        ((void (*)(id,SEL,BOOL))s_orig_viewDidAppear)(self, cmd, animated);
    }

    /* check for "theme" in class name */
    const char *nm = class_getName(object_getClass(self));
    if (!nm) return;
    if (!my_strcasestr(nm, "theme")) return;

    /* avoid adding gesture twice */
    if (objc_getAssociatedObject(self, ASSOC_KEY)) return;
    objc_setAssociatedObject(self, ASSOC_KEY, (id)(uintptr_t)1, OBJC_ASSOCIATION_ASSIGN);

    /* self.view */
    static SEL view_sel = 0;
    if (!view_sel) view_sel = sel_registerName("view");
    id view = SEND(self, view_sel);
    if (!view) return;

    /* [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(watusiDumpAction:)] */
    Class gr_cls = objc_getClass("UITapGestureRecognizer");
    if (!gr_cls) return;
    static SEL alloc_sel = 0, init_sel = 0, ntaps_sel = 0, addgr_sel = 0, dump_sel = 0;
    if (!alloc_sel)  alloc_sel  = sel_registerName("alloc");
    if (!init_sel)   init_sel   = sel_registerName("initWithTarget:action:");
    if (!dump_sel)   dump_sel   = sel_registerName("watusiDumpAction:");
    if (!ntaps_sel)  ntaps_sel  = sel_registerName("setNumberOfTapsRequired:");
    if (!addgr_sel)  addgr_sel  = sel_registerName("addGestureRecognizer:");

    id gr = SEND((id)gr_cls, alloc_sel);
    gr = SEND2(gr, init_sel, self, (id)dump_sel);   /* SEL fits in an id register */
    SENDV1u(gr, ntaps_sel, (NSUInteger)3);
    SENDV1(view, addgr_sel, gr);
    static SEL rel_sel = 0;
    if (!rel_sel) rel_sel = sel_registerName("release");
    SENDV0(gr, rel_sel);
}

/* ── dump action ─────────────────────────────────────────────────────────── */
static void dump_action(id self, SEL cmd, id sender) {
    (void)cmd; (void)sender;
    perform_dump(self);
}

/* ── constructor ─────────────────────────────────────────────────────────── */
__attribute__((constructor))
static void watusi_dump_init(void) {
    Class vc_cls = objc_getClass("UIViewController");
    if (!vc_cls) return;

    /* add watusiDumpAction: */
    SEL da_sel = sel_registerName("watusiDumpAction:");
    class_addMethod(vc_cls, da_sel, (IMP)dump_action, "v@:@");

    /* swizzle viewDidAppear: ↔ watusiOrigVDA: */
    SEL vda_sel  = sel_registerName("viewDidAppear:");
    SEL orig_sel = sel_registerName("watusiOrigVDA:");
    class_addMethod(vc_cls, orig_sel, (IMP)hooked_viewDidAppear, "v@:B");

    Method m_vda  = class_getInstanceMethod(vc_cls, vda_sel);
    Method m_orig = class_getInstanceMethod(vc_cls, orig_sel);
    if (!m_vda || !m_orig) return;

    /* save original IMP before exchange */
    s_orig_viewDidAppear = method_getImplementation(m_vda);
    method_exchangeImplementations(m_vda, m_orig);
    /*
     * After exchange:
     *  -viewDidAppear:  → hooked_viewDidAppear  (our hook)
     *  -watusiOrigVDA:  → original IMP
     * We call s_orig_viewDidAppear (original) directly from the hook.
     */
}
