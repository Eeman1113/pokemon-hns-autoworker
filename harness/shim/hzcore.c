// hzcore — a flat C surface over libmgba for headless automation.
//
// Why a shim at all: struct mCore is a large struct whose function pointers sit
// behind several variable-size members (mDirectorySet, mInputMap, mCoreConfig,
// mCoreOptions, mRTCGenericSource). Reproducing those offsets by hand in ctypes
// is possible and fragile -- it breaks silently on any mGBA point release, and
// a wrong offset reads garbage rather than failing loudly. Letting the C
// compiler resolve them against the installed headers is correct by
// construction.
#include <mgba/core/core.h>
#include <mgba/core/serialize.h>
#include <mgba/core/blip_buf.h>
#include <mgba-util/vfs.h>
#include <mgba/core/log.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

struct hz {
    struct mCore* core;
    void* vbuf;
};

// mGBA logs every DMA, SWI and RTC poke to stderr by default. At the speeds a
// harness runs that is tens of megabytes of noise per scenario and it buries
// the assertion output. Swallow it unless explicitly enabled.
static void hz_null_log(struct mLogger* l, int cat, enum mLogLevel lvl,
                        const char* fmt, va_list args) {
    (void)l; (void)cat; (void)lvl; (void)fmt; (void)args;
}
static struct mLogger hz_logger = { .log = hz_null_log };

__attribute__((visibility("default")))
void hz_silence_logs(void) { mLogSetDefaultLogger(&hz_logger); }

__attribute__((visibility("default")))
struct hz* hz_open(const char* rom) {
    struct mCore* core = mCoreFind(rom);
    if (!core) return NULL;
    if (!core->init(core)) return NULL;

    // A video buffer must exist even headless: the GBA core writes scanlines
    // unconditionally and will fault on a NULL target.
    unsigned w, h;
    core->desiredVideoDimensions(core, &w, &h);
    void* vbuf = malloc((size_t)w * h * sizeof(color_t));
    core->setVideoBuffer(core, (color_t*)vbuf, w);

    // No audio consumer; a zero-size buffer keeps blip from accumulating.
    core->setAudioBufferSize(core, 1024);

    mCoreInitConfig(core, NULL);
    if (!mCoreLoadFile(core, rom)) { free(vbuf); return NULL; }
    core->reset(core);

    struct hz* h2 = (struct hz*)calloc(1, sizeof(struct hz));
    h2->core = core; h2->vbuf = vbuf;
    return h2;
}

__attribute__((visibility("default")))
void hz_close(struct hz* h) {
    if (!h) return;
    if (h->core) {
        mCoreConfigDeinit(&h->core->config);
        h->core->deinit(h->core);
    }
    free(h->vbuf); free(h);
}

__attribute__((visibility("default")))
void hz_reset(struct hz* h) { h->core->reset(h->core); }

__attribute__((visibility("default")))
void hz_set_keys(struct hz* h, uint32_t keys) { h->core->setKeys(h->core, keys); }

__attribute__((visibility("default")))
uint32_t hz_get_keys(struct hz* h) { return h->core->getKeys(h->core); }

// Runs n frames. Keys are held for the whole span; the caller owns press/release
// timing, which is what lets a scenario express "hold A for 3 frames".
__attribute__((visibility("default")))
void hz_run_frames(struct hz* h, int n) {
    for (int i = 0; i < n; ++i) h->core->runFrame(h->core);
}

__attribute__((visibility("default")))
uint32_t hz_frame_counter(struct hz* h) { return h->core->frameCounter(h->core); }

// busRead goes through the memory map exactly as the CPU sees it, so a bad
// address returns open-bus rather than crashing the harness.
__attribute__((visibility("default")))
uint32_t hz_read8(struct hz* h, uint32_t a)  { return h->core->busRead8(h->core, a); }
__attribute__((visibility("default")))
uint32_t hz_read16(struct hz* h, uint32_t a) { return h->core->busRead16(h->core, a); }
__attribute__((visibility("default")))
uint32_t hz_read32(struct hz* h, uint32_t a) { return h->core->busRead32(h->core, a); }

__attribute__((visibility("default")))
void hz_write8(struct hz* h, uint32_t a, uint8_t v)  { h->core->busWrite8(h->core, a, v); }
__attribute__((visibility("default")))
void hz_write16(struct hz* h, uint32_t a, uint16_t v){ h->core->busWrite16(h->core, a, v); }
__attribute__((visibility("default")))
void hz_write32(struct hz* h, uint32_t a, uint32_t v){ h->core->busWrite32(h->core, a, v); }

// Bulk read: one FFI call per region instead of per byte. Reading a 100-byte
// party struct a byte at a time costs ~100 ctypes round trips per frame and
// dominates runtime at the speeds we need.
__attribute__((visibility("default")))
void hz_read_block(struct hz* h, uint32_t addr, uint8_t* out, uint32_t len) {
    for (uint32_t i = 0; i < len; ++i) out[i] = (uint8_t)h->core->busRead8(h->core, addr + i);
}

__attribute__((visibility("default")))
int hz_save_state(struct hz* h, const char* path) {
    struct VFile* vf = VFileOpen(path, O_CREAT | O_TRUNC | O_RDWR);
    if (!vf) return 0;
    int ok = mCoreSaveStateNamed(h->core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
    vf->close(vf);
    return ok;
}

__attribute__((visibility("default")))
int hz_load_state(struct hz* h, const char* path) {
    struct VFile* vf = VFileOpen(path, O_RDONLY);
    if (!vf) return 0;
    int ok = mCoreLoadStateNamed(h->core, vf, SAVESTATE_SAVEDATA | SAVESTATE_RTC);
    vf->close(vf);
    return ok;
}

// color_t is uint16 or uint32 depending on how libmgba was configured, so the
// shim reports its real size rather than letting Python guess and decode noise.
__attribute__((visibility("default")))
void hz_video_info(struct hz* h, unsigned* w, unsigned* hgt, unsigned* bpp) {
    h->core->desiredVideoDimensions(h->core, w, hgt);
    *bpp = (unsigned)sizeof(color_t);
}

__attribute__((visibility("default")))
void hz_video_copy(struct hz* h, uint8_t* out) {
    unsigned w, hgt;
    h->core->desiredVideoDimensions(h->core, &w, &hgt);
    memcpy(out, h->vbuf, (size_t)w * hgt * sizeof(color_t));
}

__attribute__((visibility("default")))
void hz_game_code(struct hz* h, char* out) { h->core->getGameCode(h->core, out); }

__attribute__((visibility("default")))
void hz_game_title(struct hz* h, char* out) { h->core->getGameTitle(h->core, out); }
