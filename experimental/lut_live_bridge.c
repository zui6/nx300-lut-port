/* RAM-only liveview LUT bridge: one stock SmartFilter frame slot hosts the
 * caller's table through the stock IPCS YCC->YCC 3D-LUT operation.
 *
 * Ported from smartfilter_lut_bridge.c (per-frame state machine) and
 * smartfilter_ipcs.c (IPCS transaction) of the NX300M 1.15 mod, keeping their
 * safety properties: a bounded context table, no allocation, no file access, no
 * polling, one atomic frame gate, and "on any failure restore the six ADE
 * switches and forward to stock", so a frame is never reported as filtered
 * while its output buffer is unfilled.
 *
 * The image is freestanding (no libc, no libgcc helper, no floating point) and
 * keeps no address in any initialiser, so it needs no runtime relocation: every
 * code address arrives through the loader's backend array at run time.
 * Install order: fill the array, call nx_lut_ram_bridge_init(), then patch the
 * two GOT slots. The hooks cannot forward to stock before they know where it
 * is, and they never claim a frame they did not fill.
 */
typedef unsigned u32;
typedef unsigned short u16;

/* Backend slots in the loader's order, BACKEND_WORDS (28) words in total.
 * 0..25 are the resolved library entry points; 26..27 are appended by this port
 * because the hooks must forward to the two functions they displace and neither
 * appears in the 0..25 list. Slot 25 is a plain mode value, not an address. */
enum {
    B_THREAD_ID, B_SEM_TRYWAIT, B_SEM_POST, B_GET_SEMAPHORE, B_B2Y_INSTANCE,
    B_IPCS_OPEN, B_IPCS_CLOSE, B_IPCS_INIT, B_IPCS_3D_LUT, B_ADE_PARAM_INIT,
    B_ADE_YCC_SET, B_IQ_INITIALIZE, B_IQ_PROCESS,
    B_IMAGE_WIDTH, B_IMAGE_HEIGHT, B_IMAGE_STRIDE, B_IMAGE_SAM_TYPE,
    B_IMAGE_ADDRESS, B_IMAGE_FLUSH, B_ADE_SWITCH,
    B_SLOT_MODE = B_ADE_SWITCH + 6,
    B_STOCK_PREVIEW, B_STOCK_EFFECT, BACKEND_WORDS
};

enum {
    SLOTS = 4,              /* frame contexts: one per preview thread, bounded */
    SEMAPHORES = 4,         /* the four B2Y/Resize wrappers, ascending order */
    ADE_SWITCHES = 6,       /* pcse_GSHC, pcse_PSHC, pcse_SE, R3D, YCNR, YLUT */
    RGB3D_INDEX = 3,        /* the only switch this operation turns on */
    CHANNEL_WORDS = 730,    /* 729 nodes + one zero pad word per channel */
    CHANNELS = 3,
    IDLE = 0, FRAME = 1,    /* global frame gate: one transaction at a time */
    OK = 0, BUSY = 1, INVALID = -1,
    LAYOUT = -41            /* the upstream adapter's layout failure code */
};

typedef unsigned (*tid_fn)(void);
typedef int (*sem_fn)(void *);
typedef void *(*instance_fn)(void);
typedef void *(*semaphore_fn)(void *,unsigned);
typedef int (*int0_fn)(void);
typedef int (*int1_fn)(void *);
typedef void (*void0_fn)(void);
typedef void (*void1_fn)(int);
typedef void (*lut_fn)(unsigned,const u16 *);
typedef unsigned (*image_fn)(void *);
typedef unsigned (*address_fn)(void *,unsigned);
typedef int (*flush_fn)(void *,unsigned,unsigned);
typedef void (*preview_fn)(void *,void *,void *,unsigned);
typedef int (*effect_fn)(unsigned,void *);

/* Integrator diagnostic block, parked at diag_words[28..]. Never file I/O. */
static const unsigned *diag_words;
#define DIAG(i,v) do{ if(diag_words) ((volatile unsigned *)diag_words)[28+(i)]=(unsigned)(v); }while(0)

struct backend {
    u32 thread_id, sem_trywait, sem_post, get_semaphore, b2y_instance;
    u32 ipcs_open, ipcs_close, ipcs_init, write_lut, ade_param_init, ade_ycc_set;
    u32 iq_initialize, iq_process;
    u32 width, height, stride, sam_type, address, flush;
    u32 ade_switch[ADE_SWITCHES], slot_mode;
    /* Appended by the integrator: the two stock entry points the hooks must
     * forward to. Required - init refuses to arm without them. */
    u32 stock_preview, stock_effect;
};
/* One frame per thread. A slot owns everything its transaction touched, so the
 * gate - not a lock - is what serialises frames. */
struct context {
    volatile u32 tid;
    u32 admitted, effect_seen, stock_depth;
    u32 held, opened, touched;
    unsigned char saved_switch[ADE_SWITCHES];
};
struct frame { u32 y, uv, oy, ouv, bytes, chroma; };
/* Private bring-up counters: the export list stays at exactly three symbols. */
struct stats { u32 frames, rendered, passthroughs, stock_effects, busy, errors; int last_error; };

static struct backend targets;
static struct context contexts[SLOTS];
static u16 nodes[CHANNELS][CHANNEL_WORDS];
static void *semaphores[SEMAPHORES];
static struct stats stats;
static volatile u32 gate, enabled, have_table, initialized, semaphores_ready;

static u32 get(const volatile u32 *p) { return __atomic_load_n(p,__ATOMIC_ACQUIRE); }
static void put(volatile u32 *p,u32 v) { __atomic_store_n(p,v,__ATOMIC_RELEASE); }
static void count(u32 *p) { __sync_fetch_and_add(p,1); }
static void copy(void *to,const void *from,u32 n) {
    unsigned char *d=to; const unsigned char *s=from; while(n--) *d++=*s++;
}
static void zero(void *to,u32 n) { unsigned char *d=to; while(n--) *d++=0; }
/* The same inline ARM EABI boundary the rest of the blob uses: no libc call and
 * no libgcc helper, so gettid stays available even with an empty backend slot. */
static int sys3(int n,int a,int b,int c) {
    register int r0 __asm__("r0")=a;
    register int r1 __asm__("r1")=b;
    register int r2 __asm__("r2")=c;
    register int r7 __asm__("r7")=n;
    __asm__ volatile("svc 0" : "+r"(r0) : "r"(r1),"r"(r2),"r"(r7) : "memory","cc");
    return r0;
}
static u32 tid(void) {
    if(!targets.thread_id) return (u32)sys3(224,0,0,0);
    return ((tid_fn)targets.thread_id)();
}
/* A whole-struct assignment would emit an unavailable libc memcpy on ARM, so
 * the array is copied field by field in its documented order. */
static void take_backend(const unsigned *a) {
    diag_words=a;
    targets.thread_id=a[B_THREAD_ID];
    targets.sem_trywait=a[B_SEM_TRYWAIT];
    targets.sem_post=a[B_SEM_POST];
    targets.get_semaphore=a[B_GET_SEMAPHORE];
    targets.b2y_instance=a[B_B2Y_INSTANCE];
    targets.ipcs_open=a[B_IPCS_OPEN];
    targets.ipcs_close=a[B_IPCS_CLOSE];
    targets.ipcs_init=a[B_IPCS_INIT];
    targets.write_lut=a[B_IPCS_3D_LUT];
    targets.ade_param_init=a[B_ADE_PARAM_INIT];
    targets.ade_ycc_set=a[B_ADE_YCC_SET];
    targets.iq_initialize=a[B_IQ_INITIALIZE];
    targets.iq_process=a[B_IQ_PROCESS];
    targets.width=a[B_IMAGE_WIDTH];
    targets.height=a[B_IMAGE_HEIGHT];
    targets.stride=a[B_IMAGE_STRIDE];
    targets.sam_type=a[B_IMAGE_SAM_TYPE];
    targets.address=a[B_IMAGE_ADDRESS];
    targets.flush=a[B_IMAGE_FLUSH];
    for(u32 i=0;i<ADE_SWITCHES;i++) targets.ade_switch[i]=a[B_ADE_SWITCH+i];
    targets.slot_mode=a[B_SLOT_MODE];
    targets.stock_preview=a[B_STOCK_PREVIEW];
    targets.stock_effect=a[B_STOCK_EFFECT];
}
/* The four B2Y semaphores exist only once the IPCS singleton is up, long after
 * init, so they are resolved once on the first admitted frame and published
 * behind one release store: no thread ever reads a partly filled table. */
static int resolve_semaphores(void) {
    void *instance, *found[SEMAPHORES];
    if(get(&semaphores_ready)) return OK;
    instance=((instance_fn)targets.b2y_instance)();
    if(!instance) return BUSY;
    for(u32 i=0;i<SEMAPHORES;i++) {
        found[i]=((semaphore_fn)targets.get_semaphore)(instance,i);
        if(!found[i]) return BUSY;
    }
    for(u32 i=0;i<SEMAPHORES;i++) semaphores[i]=found[i];
    put(&semaphores_ready,1);
    return OK;
}
static int admit(void) { return __sync_bool_compare_and_swap(&gate,IDLE,FRAME); }
static void surrender(void) { put(&gate,IDLE); }
static struct context *context(u32 id) {
    for(u32 i=0;i<SLOTS;i++) if(get(&contexts[i].tid)==id) return &contexts[i];
    return (void *)0;
}
static struct context *claim(u32 id) {
    for(u32 i=0;i<SLOTS;i++)
        if(__sync_bool_compare_and_swap(&contexts[i].tid,0,id)) return &contexts[i];
    return (void *)0;
}
/* Geometry is read once per frame and every value is checked before any plane
 * is touched: the stock buffers are the only thing this blob trusts. */
static int geometry(void *image,struct frame *f) {
    u32 w=((image_fn)targets.width)(image);
    u32 h=((image_fn)targets.height)(image);
    u32 stride=((image_fn)targets.stride)(image);
    DIAG(4,w);DIAG(5,h);DIAG(6,stride);
    if(!w||!h||(w&1U)||(h&1U)||stride<w||stride>8192U||h>8192U) { DIAG(0,0x11); return LAYOUT; }
    /* SmartFilter builds sADEPixelType {0,1,0}: GetSamType reads the first word
     * and ADE maps zero to IPCS_YCC420, the only layout the 3D-LUT op takes. */
    { u32 sm=((image_fn)targets.sam_type)(image); DIAG(7,sm); if(sm!=0U) { DIAG(0,0x12); return LAYOUT; } }
    f->bytes=stride*h;
    f->chroma=f->bytes/2U;
    f->y=((address_fn)targets.address)(image,1);
    f->uv=((address_fn)targets.address)(image,3);
    f->oy=((address_fn)targets.address)(image,6);
    f->ouv=((address_fn)targets.address)(image,8);
    DIAG(8,f->y);DIAG(9,f->uv);DIAG(10,f->oy);DIAG(11,f->ouv);DIAG(12,f->bytes);DIAG(13,f->chroma);
    if(!f->y||!f->uv||!f->oy||!f->ouv||
       f->y>~0U-f->bytes||f->oy>~0U-f->bytes||
       f->uv>~0U-f->chroma||f->ouv>~0U-f->chroma) { DIAG(0,0x13); return LAYOUT; }
    return OK;
}
/* Adjacent planes are flushed once, exactly as the native wrapper does;
 * otherwise each plane is flushed on its own. */
static int flush_planes(void *image,const struct frame *f,u32 y,u32 uv) {
    int rc;
    if(uv==y+f->bytes) return ((flush_fn)targets.flush)(image,y,f->bytes+f->chroma);
    rc=((flush_fn)targets.flush)(image,y,f->bytes);
    if(rc) return rc;
    return ((flush_fn)targets.flush)(image,uv,f->chroma);
}
static void release_locks(struct context *c) {
    while(c->held) {
        u32 i=--c->held;
        ((sem_fn)targets.sem_post)(semaphores[i]);
    }
}
/* Acquire the whole transaction without waiting, ascending like the native
 * wrappers. A failed begin owns nothing and changes no hardware state. */
static int begin(struct context *c) {
    if(c->held||c->opened||c->touched) return BUSY;
    if(resolve_semaphores()) return BUSY;
    for(u32 i=0;i<SEMAPHORES;i++) {
        if(((sem_fn)targets.sem_trywait)(semaphores[i])) { release_locks(c); return BUSY; }
        c->held++;
    }
    for(u32 i=0;i<ADE_SWITCHES;i++)
        c->saved_switch[i]=*(volatile unsigned char *)targets.ade_switch[i];
    return OK;
}
/* A failed operation still leaves through restore: the switches it changed and
 * the handle it opened are owned by the context, not by the caller. */
static int render(struct context *c,void *image) {
    struct frame f;
    int rc;
    if(geometry(image,&f)) return LAYOUT;
    DIAG(0,1);
    rc=((int0_fn)targets.ipcs_open)();
    DIAG(1,rc);
    if(rc<0) return rc;
    c->opened=1; c->touched=1;
    DIAG(0,2);
    ((void0_fn)targets.ade_param_init)();
    DIAG(0,3);
    /* The stock adapter writes all six switches and leaves only the 3D LUT
     * enabled while the operation runs; restore puts the saved bytes back. */
    for(u32 i=0;i<ADE_SWITCHES;i++)
        *(volatile unsigned char *)targets.ade_switch[i]=(unsigned char)(i==RGB3D_INDEX);
    DIAG(0,4);
    rc=flush_planes(image,&f,f.y,f.uv);
    DIAG(2,rc);
    if(rc) return rc;
    DIAG(0,5);
    /* Without a table this is a pure transaction probe: open, init, flush and
     * restore only.  The 3D-LUT write and the YCC operation would otherwise run
     * with an empty node table and recolour the frame before stock repaints it. */
    if(!get(&have_table)) return OK;
    for(u32 i=0;i<CHANNELS;i++) ((lut_fn)targets.write_lut)(i,nodes[i]);
    rc=((int1_fn)targets.ade_ycc_set)(image);
    DIAG(3,rc);
    if(rc) return rc;
    DIAG(0,6);
    rc=flush_planes(image,&f,f.oy,f.ouv);
    DIAG(0,7);
    return rc;
}
/* The native reset also runs after a failed operation, before any fallback:
 * Initialize(0) drops the still parameter-node indices, Process(0) forwards
 * fresh colour settings through the existing JPEG hook. The switch restore and
 * the close must happen even when the reset itself failed. */
/* IQ_IPC_Initialize/Process re-run the whole IQ parameter setup and forward the
 * still colour settings into the JPEG hook.  The stock firmware only does that
 * when settings change; paying it on every liveview frame is what held this
 * port at 2.5 fps.  ipcs_init (which is what actually latches the ADE switches)
 * still runs every frame; the IQ sync runs once, and again after a failure. */
static volatile u32 iq_synced;
static int restore(struct context *c) {
    int result=OK,rc;
    if(c->touched) {
        result=((int0_fn)targets.ipcs_init)();
        if(!result) {
            if(!get(&iq_synced)) {
                ((void1_fn)targets.iq_initialize)(0);
                ((void1_fn)targets.iq_process)(0);
                put(&iq_synced,1U);
            }
        }
    }
    for(u32 i=0;i<ADE_SWITCHES;i++)
        *(volatile unsigned char *)targets.ade_switch[i]=c->saved_switch[i];
    if(c->opened) {
        rc=((int0_fn)targets.ipcs_close)();
        if(!result) result=rc;
        c->opened=0;
    }
    c->touched=0;
    return result;
}
static void end(struct context *c) { release_locks(c); }
/* "No colour change": the byte copy the stock composition step produces for an
 * unfiltered slot, so a passthrough frame stays pixel-identical to stock. */
/* If the copy cannot be validated, keep the original effect's error semantics:
 * claiming success with an unfilled output buffer would break the pipeline. */
static int fallback(unsigned effect,void *image) {
    /* The passthrough shortcut used to claim the frame whenever its copy
     * succeeded; on this target that left the display showing a stale buffer
     * while the process stayed fully responsive.  Losing a frame to the stock
     * effect is always cheaper than owning a frame we did not really paint. */
    if(!targets.stock_effect) return LAYOUT;
    count(&stats.stock_effects);
    return ((effect_fn)targets.stock_effect)(effect,image);
}

/* Hook for CLiveviewNodeSmartFilter::smartFilter(CCapImage*, void*,
 * E_LIVEVIEW_SMARTFILTER): bind or tear down this thread's frame context and
 * always forward to the function this slot used to hold. */
__attribute__((visibility("hidden")))
void nx_sf_hook_preview(void *self,void *image,void *data,unsigned mode) {
    u32 id=tid();
    /* Diagnostics: record what the caller actually passes, and how far we get. */
    DIAG(17,mode);DIAG(18,id);DIAG(19,get(&enabled));DIAG(20,targets.slot_mode);
    DIAG(21,stats.frames);
    struct context *c;
    if(!targets.stock_preview) return;  /* unconfigured: nothing to forward to */
    c=id?context(id):(void *)0;
    if(c) {
        /* Re-entered on this thread: the nested stock call must not observe our
         * transaction, so only the depth counter moves. */
        c->stock_depth++;
        ((preview_fn)targets.stock_preview)(self,image,data,mode);
        c->stock_depth--;
        return;
    }
    if(!get(&enabled)||mode!=targets.slot_mode||!id) {
        ((preview_fn)targets.stock_preview)(self,image,data,mode);
        return;
    }
    c=claim(id);
    if(!c) {
        count(&stats.busy);
        ((preview_fn)targets.stock_preview)(self,image,data,mode);
        return;
    }
    DIAG(24,1);
    c->effect_seen=0; c->stock_depth=0;
    c->held=0; c->opened=0; c->touched=0;
    c->admitted=(u32)admit();
    if(!c->admitted) count(&stats.busy);
    count(&stats.frames);
    ((preview_fn)targets.stock_preview)(self,image,data,mode);
    if(c->admitted) { c->admitted=0; surrender(); }
    put(&c->tid,0);
}

/* Hook for ADE_SmartFilter_YccProcess(eADE_MODE, ADE_Image*): run our
 * transaction for the frame this thread opened, otherwise forward to stock. */
__attribute__((visibility("hidden")))
int nx_sf_hook_effect(unsigned effect,void *ade_image) {
    u32 id=tid();
    DIAG(22,effect);DIAG(23,id);
    struct context *c=id?context(id):(void *)0;
    int rc,restored;
    if(!c||c->effect_seen||c->stock_depth) {
        if(!targets.stock_effect) return LAYOUT;
        count(&stats.stock_effects);
        return ((effect_fn)targets.stock_effect)(effect,ade_image);
    }
    c->effect_seen=1;
    if(!c->admitted) return fallback(effect,ade_image);
    /* DIAGNOSTIC ROUTING: run our whole transaction for the measurements in the
     * diag block, restore the hardware, then ALWAYS let the stock effect paint
     * the frame.  Replacing stock output before the transaction is proven is
     * exactly what froze the preview: fallback()'s passthrough claimed the frame
     * while the stock composition never ran. */
    DIAG(0,0x20);
    rc=begin(c);
    DIAG(14,rc);
    if(!rc) {
        rc=render(c,ade_image);
        DIAG(15,rc);
        restored=restore(c);
        DIAG(16,restored);
        end(c);
    }
    /* Success: our transaction already wrote the output planes, so returning OK
     * is what lets the frame reach the display with our colour. Failure of any
     * stage hands the frame back to the stock effect - never claim a frame we
     * did not fill. */
    if(rc||restored) {
        count(&stats.errors);
        stats.last_error=rc?rc:restored;
        DIAG(17,rc);DIAG(18,restored);
        put(&iq_synced,0U);          /* next frame re-runs the full IQ sync */
        return fallback(effect,ade_image);
    }
    count(&stats.rendered);
    return OK;
}

/* The one configuration entry: validate the whole array before touching any
 * state, then publish the table and arm the bridge with release stores. The
 * array is read as exactly BACKEND_WORDS words - there is no length argument, so
 * a caller that passes fewer hands this blob unmapped memory. It must run with
 * the target's threads stopped and before the GOT slots are patched. */
__attribute__((visibility("hidden")))
unsigned nx_sf_init(const unsigned *backend,const unsigned short *table) {
    u32 i;
    if(!backend) return (unsigned)INVALID;
    /* Slot 0 (gettid) may stay zero: the blob has its own svc. Slot 25 is a
     * plain value, so only 1..24 and the two appended slots are addresses. */
    for(i=0;i<B_SLOT_MODE;i++) if(!backend[i]&&i!=B_THREAD_ID) return (unsigned)INVALID;
    if(!backend[B_STOCK_PREVIEW]||!backend[B_STOCK_EFFECT]) return (unsigned)INVALID;
    if(table) {
        for(i=0;i<CHANNELS;i++) {
            for(u32 n=0;n<CHANNEL_WORDS-1;n++)
                if(table[i*CHANNEL_WORDS+n]>1023) return (unsigned)INVALID;
            if(table[i*CHANNEL_WORDS+CHANNEL_WORDS-1]) return (unsigned)INVALID;
        }
    }
    if(!__sync_bool_compare_and_swap(&initialized,0,1)) return (unsigned)BUSY;
    zero(&targets,sizeof targets);
    take_backend(backend);
    /* sizeof the array, never the pointer the caller passed. */
    if(table) copy(nodes,table,sizeof nodes);
    put(&have_table,table?1U:0U);
    put(&enabled,1U);
    return (unsigned)OK;
}
