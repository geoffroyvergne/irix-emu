// GL pass-through on macOS: legacy (2.1) CGL contexts without a window, one
// per guest context, rendering each guest drawable into a framebuffer object
// with a back (attachment 0) and a front (attachment 1) colour buffer.
#include "GlBridge.hpp"

#include "indy/core/Log.hpp"
#include "indy/cpu/R5000.hpp"

#define GL_SILENCE_DEPRECATION 1
#include <OpenGL/OpenGL.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <span>
#include <type_traits>
#include <vector>

extern "C" {
#include "../guest/glpass_protocol.h"
#include "../guest/glpass_fn.h"
}

namespace indy {

namespace {

// Big-endian 32-bit words, as the guest wrote them.
class Words {
public:
    explicit Words(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    [[nodiscard]] size_t size() const noexcept { return bytes_.size() / 4; }
    [[nodiscard]] uint32_t u(size_t i) const noexcept {
        const uint8_t* p = &bytes_[i * 4];
        return uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3];
    }
    [[nodiscard]] int32_t i(size_t k) const noexcept { return static_cast<int32_t>(u(k)); }
    [[nodiscard]] float f(size_t k) const noexcept { return std::bit_cast<float>(u(k)); }
    [[nodiscard]] double d(size_t k) const noexcept {
        return std::bit_cast<double>(uint64_t{u(k)} << 32 | u(k + 1));
    }
    // Raw bytes starting at word k (images, array data).
    [[nodiscard]] const uint8_t* bytes(size_t k) const noexcept { return bytes_.data() + k * 4; }

private:
    std::span<const uint8_t> bytes_;
};

// A reply for the guest: big-endian words, or raw bytes.
class Reply {
public:
    // A computed reply being written into guest memory.
    struct Pending {
        uint32_t fn = 0;
        VirtAddr at = 0;
        uint32_t size = 0;
        uint64_t result = 0;
        std::vector<uint8_t> data;
        size_t done = 0;
        bool active = false;
    };

    void u(uint32_t v) {
        bytes_.insert(bytes_.end(), {static_cast<uint8_t>(v >> 24), static_cast<uint8_t>(v >> 16),
                                     static_cast<uint8_t>(v >> 8), static_cast<uint8_t>(v)});
    }
    void f(float v) { u(std::bit_cast<uint32_t>(v)); }
    void d(double v) {
        const auto bits = std::bit_cast<uint64_t>(v);
        u(static_cast<uint32_t>(bits >> 32));
        u(static_cast<uint32_t>(bits));
    }
    void byte(uint8_t v) { bytes_.push_back(v); }
    [[nodiscard]] std::vector<uint8_t>& bytes() noexcept { return bytes_; }

private:
    std::vector<uint8_t> bytes_;
};

constexpr GLenum kBack = GL_COLOR_ATTACHMENT0_EXT;
constexpr GLenum kFront = GL_COLOR_ATTACHMENT1_EXT;

// A guest drawable's buffers. GLX shares them among all contexts drawing to
// it, so they are renderbuffers of the contexts' share group (Maya clears a
// view with one context and draws it with another).
struct Surface {
    GLuint back = 0, front = 0, depth = 0;
    int32_t width = 0, height = 0;
    std::vector<uint32_t> palette; // colour index drawables: index -> 0x00RRGGBB (empty: RGBA)
    bool overlay = false;          // overlay planes: index 0 is transparent
};

// A context's framebuffer object for a drawable (framebuffer objects are not
// shared), with the surface's renderbuffers attached.
struct Framebuffer {
    GLuint fbo = 0;
    Surface* surface = nullptr;
};

struct Context {
    CGLContextObj cgl = nullptr;
    uint32_t drawable = 0;                     // bound to (GLP_OP_MAKE_CURRENT); 0: none
    std::map<uint32_t, Framebuffer> drawables; // by guest X drawable
    GLenum drawBuffer = GL_BACK;               // as the guest named them
    GLenum readBuffer = GL_BACK;
    std::vector<GLuint> select;
    std::vector<GLfloat> feedback;
};

// One client array of a GLP_FN_CLIENT_DRAW record.
struct ClientArray {
    uint32_t which; // 0 vertex, 1 normal, 2 colour, 3 index, 4 texture coordinates, 5 edge flag
    GLint size;
    GLenum type;
};

size_t typeBytes(GLenum type) {
    switch (type) {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    case GL_DOUBLE: return 8;
    default: return 4;
    }
}

// Value of a component in host order, normalised for colours and normals
// (OpenGL 1.1, table 2.6).
double component(const uint8_t* p, GLenum type, bool normalize) {
    switch (type) {
    case GL_UNSIGNED_BYTE: return normalize ? p[0] / 255.0 : p[0];
    case GL_BYTE: {
        const auto v = static_cast<int8_t>(p[0]);
        return normalize ? (2.0 * v + 1.0) / 255.0 : v;
    }
    case GL_UNSIGNED_SHORT: { uint16_t v; std::memcpy(&v, p, 2); return normalize ? v / 65535.0 : v; }
    case GL_SHORT: { int16_t v; std::memcpy(&v, p, 2); return normalize ? (2.0 * v + 1.0) / 65535.0 : v; }
    case GL_UNSIGNED_INT: { uint32_t v; std::memcpy(&v, p, 4); return normalize ? v / 4294967295.0 : v; }
    case GL_INT: { int32_t v; std::memcpy(&v, p, 4); return normalize ? (2.0 * v + 1.0) / 4294967295.0 : v; }
    case GL_FLOAT: { float v; std::memcpy(&v, p, 4); return v; }
    default: { double v; std::memcpy(&v, p, 8); return v; }
    }
}

// Guest buffer names -> attachments of the drawable's framebuffer.
void applyDrawBuffer(GLenum mode) {
    switch (mode) {
    case GL_BACK: case GL_BACK_LEFT: glDrawBuffer(kBack); break;
    case GL_FRONT: case GL_FRONT_LEFT: glDrawBuffer(kFront); break;
    case GL_FRONT_AND_BACK: case GL_LEFT: {
        const GLenum both[] = {kBack, kFront};
        glDrawBuffers(2, both);
        break;
    }
    default: glDrawBuffer(GL_NONE); break; // right, aux and none: no such buffer here
    }
}

bool isFront(GLenum mode) {
    return mode == GL_FRONT || mode == GL_FRONT_LEFT || mode == GL_LEFT || mode == GL_FRONT_AND_BACK;
}

void applyReadBuffer(GLenum mode) { glReadBuffer(isFront(mode) ? kFront : kBack); }

// Pixel store state for the bridge's own reads, whatever the guest set.
class DefaultPackState {
public:
    DefaultPackState() {
        glPushClientAttrib(GL_CLIENT_PIXEL_STORE_BIT);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_FALSE);
        glPixelStorei(GL_PACK_LSB_FIRST, GL_FALSE);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
    }
    ~DefaultPackState() { glPopClientAttrib(); }
    DefaultPackState(const DefaultPackState&) = delete;
    DefaultPackState& operator=(const DefaultPackState&) = delete;
};

class MacGlBridge final : public GlBridge {
public:
    explicit MacGlBridge(OverlaySink sink) : sink_(std::move(sink)) {
        const CGLPixelFormatAttribute attrs[] = {
            kCGLPFAAccelerated, kCGLPFAOpenGLProfile,
            static_cast<CGLPixelFormatAttribute>(kCGLOGLPVersion_Legacy), static_cast<CGLPixelFormatAttribute>(0)};
        GLint count = 0;
        if (CGLChoosePixelFormat(attrs, &pixelFormat_, &count) != kCGLNoError || pixelFormat_ == nullptr) {
            log::warn("GL bridge: no host OpenGL pixel format; guest OpenGL stays in software");
        }
    }

    ~MacGlBridge() override {
        for (auto& [id, ctx] : contexts_) destroy(ctx);
        if (pixelFormat_ != nullptr) CGLReleasePixelFormat(pixelFormat_);
    }

    [[nodiscard]] bool available() const noexcept override { return pixelFormat_ != nullptr; }

    void hypercall(R5000& cpu) override {
        // The operation word names the guest context the operation is for:
        // guest threads each have their own current context (sprocs and
        // POSIX threads alike), and their calls interleave.
        const auto word = static_cast<uint32_t>(cpu.gpr(5));
        const uint32_t op = GLP_OP_OF(word);
        const uint32_t context = GLP_CONTEXT_OF(word);
        const VirtAddr buffer = cpu.gpr(6);
        const auto arg = static_cast<uint32_t>(cpu.gpr(7));
        if (!available()) {
            cpu.setHypercallResult(static_cast<uint64_t>(-1));
            return;
        }
        // Copies interrupted by a page fault are kept per context: another
        // guest thread may call in before the faulting one re-executes.
        Transfers& io = transfers_[context];
        std::vector<uint8_t>& data = io.in.data;
        // Reads the request buffer. Large buffers span more pages than the TLB
        // maps at once, so the copy resumes where a page fault stopped it when
        // the hypercall re-executes (same operation, buffer and length).
        auto read = [&](size_t bytes) {
            if (!io.in.active || io.in.op != word || io.in.buffer != buffer || io.in.arg != arg) {
                io.in = Transfer{word, buffer, arg, 0, {}, true};
                io.in.data.resize(bytes);
            }
            if (!cpu.guestCopyIn(buffer, io.in.data, io.in.done)) return false; // page fault: retried
            io.in.active = false;
            return true;
        };
        switch (op) {
        case GLP_OP_HELLO:
            if (arg != GLP_FN_TABLE_ID) { // a guest library built from other sources: refuse it
                if (!tableMismatchLogged_) {
                    tableMismatchLogged_ = true;
                    log::warn("GL bridge: the guest glpass library ({:#010x}) was built from other sources than "
                              "this emulator ({:#010x}); rebuild it in IRIX, see glpass/README.md",
                              arg, GLP_FN_TABLE_ID);
                }
                cpu.setHypercallResult(static_cast<uint64_t>(int64_t{GLP_ERR_TABLE}));
                return;
            }
            cpu.setHypercallResult(GLP_VERSION);
            return;
        case GLP_OP_CREATE_CONTEXT:
            cpu.setHypercallResult(createContext(arg));
            return;
        case GLP_OP_DESTROY_CONTEXT:
            if (auto it = contexts_.find(arg); it != contexts_.end()) {
                destroy(it->second);
                contexts_.erase(it);
                if (active_ == arg) active_ = 0;
                if (arg != context) transfers_.erase(arg);
            }
            transfers_.erase(context);
            cpu.setHypercallResult(0);
            return;
        case GLP_OP_MAKE_CURRENT:
            if (!read(16)) return;
            {
                const Words w(data);
                // The payload repeats the context: both must name the same one.
                const bool ok = w.u(0) == context && makeCurrent(w.u(0), w.u(1), w.i(2), w.i(3));
                cpu.setHypercallResult(ok ? 0 : static_cast<uint64_t>(-1));
            }
            return;
        case GLP_OP_SUBMIT:
            if (arg > kMaxRequest) { // a corrupt length: refuse rather than read gigabytes
                cpu.setHypercallResult(static_cast<uint64_t>(-1));
                return;
            }
            if (!read(arg)) return;
            if (Context* ctx = activate(context)) execute(*ctx, Words(data));
            cpu.setHypercallResult(0);
            return;
        case GLP_OP_SWAP:
            if (!read(28)) return;
            {
                const Words w(data);
                static const bool trace = std::getenv("INDY_TRACE_GL") != nullptr;
                const auto start = std::chrono::steady_clock::now();
                swap(context, w.u(0), w.i(1), w.i(2), w.i(3), w.i(4), static_cast<uint8_t>(w.u(5)), w.u(6));
                if (trace) {
                    log::info("GL swap {:#x} flags {}: {} us", w.u(0), w.u(6),
                              std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - start).count());
                }
            }
            cpu.setHypercallResult(0);
            return;
        case GLP_OP_FINISH:
            if (activate(context) != nullptr) glFinish();
            cpu.setHypercallResult(0);
            return;
        case GLP_OP_QUERY:
            if (arg < 12 || arg > kMaxRequest) {
                cpu.setHypercallResult(static_cast<uint64_t>(-1));
                return;
            }
            if (!read(arg)) return;
            query(cpu, context, io, Words(data));
            return;
        default:
            cpu.setHypercallResult(static_cast<uint64_t>(-1));
            return;
        }
    }

private:
    static constexpr uint32_t kMaxRequest = 64u << 20;

    // Hypercall transfers in progress across page faults.
    struct Transfer {
        uint32_t op = 0;
        VirtAddr buffer = 0;
        uint32_t arg = 0;
        size_t done = 0;
        std::vector<uint8_t> data;
        bool active = false;
    };
    struct Preread {
        VirtAddr at = 0;
        size_t done = 0;
        std::vector<uint8_t> data;
        bool active = false;
    };
    struct Transfers {
        Transfer in;
        Preread preread;
        Reply::Pending out;
    };

    uint64_t createContext(uint32_t share) {
        CGLContextObj shareWith = nullptr;
        if (auto it = contexts_.find(share); it != contexts_.end()) shareWith = it->second.cgl;
        CGLContextObj cgl = nullptr;
        if (CGLCreateContext(pixelFormat_, shareWith, &cgl) != kCGLNoError || cgl == nullptr) {
            log::warn("GL bridge: cannot create a host context");
            return static_cast<uint64_t>(-1);
        }
        // Guest images are big-endian: the host swaps what the guest does not.
        CGLContextObj previous = CGLGetCurrentContext();
        CGLSetCurrentContext(cgl);
        glPixelStorei(GL_UNPACK_SWAP_BYTES, GL_TRUE);
        glPixelStorei(GL_PACK_SWAP_BYTES, GL_TRUE);
        CGLSetCurrentContext(previous);
        const uint32_t id = nextId_++;
        contexts_[id].cgl = cgl;
        log::info("GL bridge: guest context {} created", id);
        return id;
    }

    void destroy(Context& ctx) {
        CGLSetCurrentContext(ctx.cgl);
        for (auto& [drawable, fb] : ctx.drawables) glDeleteFramebuffersEXT(1, &fb.fbo);
        // The last context of a share group takes the group's surfaces along.
        CGLShareGroupObj group = CGLGetShareGroup(ctx.cgl);
        const bool last = std::ranges::none_of(contexts_, [&](const auto& entry) {
            return &entry.second != &ctx && CGLGetShareGroup(entry.second.cgl) == group;
        });
        if (last) {
            for (auto it = surfaces_.begin(); it != surfaces_.end();) {
                if (it->first.first != group) {
                    ++it;
                    continue;
                }
                glDeleteRenderbuffersEXT(1, &it->second.back);
                glDeleteRenderbuffersEXT(1, &it->second.front);
                glDeleteRenderbuffersEXT(1, &it->second.depth);
                if (sink_) sink_(it->first.second, nullptr);
                it = surfaces_.erase(it);
            }
        }
        CGLSetCurrentContext(nullptr);
        CGLDestroyContext(ctx.cgl);
        active_ = 0; // no host context is current now
    }

    // Makes guest context `id` the host's current one, with the framebuffer
    // of the drawable it is bound to and its draw and read buffers; nullptr
    // when it does not exist or is bound to no drawable. Host state changes
    // only when another context ran since.
    Context* activate(uint32_t id) {
        auto it = contexts_.find(id);
        if (it == contexts_.end() || it->second.drawable == 0) return nullptr;
        Context& ctx = it->second;
        if (active_ != id) {
            auto fb = ctx.drawables.find(ctx.drawable);
            if (fb == ctx.drawables.end()) return nullptr;
            CGLSetCurrentContext(ctx.cgl);
            glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb->second.fbo);
            applyDrawBuffer(ctx.drawBuffer);
            applyReadBuffer(ctx.readBuffer);
            active_ = id;
        }
        return &ctx;
    }

    // Binds `context` to `drawable` (sized width x height); drawable 0
    // unbinds it. Other contexts keep their bindings: each guest thread has
    // its own current context.
    bool makeCurrent(uint32_t context, uint32_t drawable, int32_t width, int32_t height) {
        auto it = contexts_.find(context);
        if (it == contexts_.end()) return context == 0;
        Context& ctx = it->second;
        if (drawable == 0) {
            ctx.drawable = 0;
            if (active_ == context) {
                CGLSetCurrentContext(nullptr);
                active_ = 0;
            }
            return true;
        }
        CGLSetCurrentContext(ctx.cgl);
        active_ = context;
        ctx.drawable = drawable;
        width = std::max(1, width);
        height = std::max(1, height);
        Surface& surface = surfaces_[{CGLGetShareGroup(ctx.cgl), drawable}];
        if (surface.back == 0) {
            glGenRenderbuffersEXT(1, &surface.back);
            glGenRenderbuffersEXT(1, &surface.front);
            glGenRenderbuffersEXT(1, &surface.depth);
        }
        if (surface.width != width || surface.height != height) {
            surface.width = width;
            surface.height = height;
            glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, surface.back);
            glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, width, height);
            glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, surface.front);
            glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, width, height);
            glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, surface.depth);
            glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_DEPTH24_STENCIL8_EXT, width, height);
        }
        Framebuffer& fb = ctx.drawables[drawable];
        const bool fresh = fb.fbo == 0;
        if (fresh) glGenFramebuffersEXT(1, &fb.fbo);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb.fbo);
        if (fb.surface != &surface) {
            fb.surface = &surface;
            glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, kBack, GL_RENDERBUFFER_EXT, surface.back);
            glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, kFront, GL_RENDERBUFFER_EXT, surface.front);
            glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_DEPTH_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT,
                                         surface.depth);
            glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_STENCIL_ATTACHMENT_EXT, GL_RENDERBUFFER_EXT,
                                         surface.depth);
        }
        if (glCheckFramebufferStatusEXT(GL_FRAMEBUFFER_EXT) != GL_FRAMEBUFFER_COMPLETE_EXT) {
            log::warn("GL bridge: incomplete framebuffer for drawable {:#x} ({}x{})", drawable, width, height);
        }
        // Draw and read buffers are framebuffer state: set them for this one.
        applyDrawBuffer(ctx.drawBuffer);
        applyReadBuffer(ctx.readBuffer);
        if (fresh) {
            glViewport(0, 0, width, height); // GL: the first binding sets the viewport to the window
        }
        return true;
    }

    // Copies big-endian elements of `size` bytes into host order.
    static void toHost(std::vector<uint8_t>& out, const uint8_t* src, size_t bytes, size_t size) {
        out.assign(src, src + bytes);
        if (size == 1) return;
        for (size_t at = 0; at + size <= bytes; at += size) std::reverse(&out[at], &out[at + size]);
    }

    // GLP_FN_CLIENT_DRAW: kind, mode, count, narrays, arrays..., [indices].
    void clientDraw(const Words& w, size_t p, size_t n) {
        const uint32_t kind = w.u(p);
        const GLenum mode = w.u(p + 1);
        const auto count = static_cast<GLsizei>(w.u(p + 2));
        const uint32_t narrays = w.u(p + 3);
        const size_t end = p + n;
        size_t at = p + 4;
        std::array<ClientArray, 6> arrays{};
        size_t used = 0;
        for (uint32_t k = 0; k < narrays && used < arrays.size(); ++k) {
            if (at + 4 > end) return;
            const ClientArray a{w.u(at), static_cast<GLint>(w.u(at + 1)), w.u(at + 2)};
            const uint32_t bytes = w.u(at + 3);
            if (a.which >= arrays.size() || at + 4 + (bytes + 3) / 4 > end) return;
            toHost(arrayData_[a.which], w.bytes(at + 4), bytes, typeBytes(a.type));
            arrays[used++] = a;
            at += 4 + (bytes + 3) / 4;
        }
        const std::span<const ClientArray> list(arrays.data(), used);
        if (kind == GLP_DRAW_ELEMENT) {
            emitElement(list);
            return;
        }
        for (const ClientArray& a : list) {
            const void* data = arrayData_[a.which].data();
            switch (a.which) {
            case 0: glVertexPointer(a.size, a.type, 0, data); glEnableClientState(GL_VERTEX_ARRAY); break;
            case 1: glNormalPointer(a.type, 0, data); glEnableClientState(GL_NORMAL_ARRAY); break;
            case 2: glColorPointer(a.size, a.type, 0, data); glEnableClientState(GL_COLOR_ARRAY); break;
            case 3: glIndexPointer(a.type, 0, data); glEnableClientState(GL_INDEX_ARRAY); break;
            case 4: glTexCoordPointer(a.size, a.type, 0, data); glEnableClientState(GL_TEXTURE_COORD_ARRAY); break;
            default: glEdgeFlagPointer(0, data); glEnableClientState(GL_EDGE_FLAG_ARRAY); break;
            }
        }
        if (kind == GLP_DRAW_ARRAYS) {
            glDrawArrays(mode, 0, count);
        } else if (at < end && count > 0) {
            const auto nindices = static_cast<uint32_t>(std::min<size_t>(w.u(at), end - at - 1));
            indices_.resize(nindices);
            for (uint32_t k = 0; k < nindices; ++k) {
                indices_[k] = std::min<GLuint>(w.u(at + 1 + k), static_cast<GLuint>(count - 1));
            }
            glDrawElements(mode, static_cast<GLsizei>(nindices), GL_UNSIGNED_INT, indices_.data());
        }
        static constexpr std::array<GLenum, 6> kArrays{GL_VERTEX_ARRAY, GL_NORMAL_ARRAY, GL_COLOR_ARRAY,
                                                       GL_INDEX_ARRAY, GL_TEXTURE_COORD_ARRAY, GL_EDGE_FLAG_ARRAY};
        for (const GLenum array : kArrays) glDisableClientState(array);
    }

    // glArrayElement as immediate-mode calls: array pointers may not change
    // between glBegin and glEnd. The vertex goes last, as it emits the element.
    void emitElement(std::span<const ClientArray> arrays) {
        const ClientArray* vertex = nullptr;
        for (const ClientArray& a : arrays) {
            const uint8_t* d = arrayData_[a.which].data();
            const size_t step = typeBytes(a.type);
            const auto n = static_cast<size_t>(std::clamp(a.size, 1, 4));
            std::array<double, 4> v{0.0, 0.0, 0.0, 1.0};
            switch (a.which) {
            case 0:
                vertex = &a;
                break;
            case 1:
                for (size_t k = 0; k < 3; ++k) v[k] = component(d + k * step, a.type, true);
                glNormal3dv(v.data());
                break;
            case 2:
                for (size_t k = 0; k < n; ++k) v[k] = component(d + k * step, a.type, true);
                glColor4dv(v.data());
                break;
            case 3:
                glIndexd(component(d, a.type, false));
                break;
            case 4:
                for (size_t k = 0; k < n; ++k) v[k] = component(d + k * step, a.type, false);
                glTexCoord4dv(v.data());
                break;
            default:
                glEdgeFlag(d[0] != 0);
                break;
            }
        }
        if (vertex != nullptr) {
            const uint8_t* d = arrayData_[0].data();
            const size_t step = typeBytes(vertex->type);
            const auto n = static_cast<size_t>(std::clamp(vertex->size, 2, 4));
            std::array<double, 4> v{0.0, 0.0, 0.0, 1.0};
            for (size_t k = 0; k < n; ++k) v[k] = component(d + k * step, vertex->type, false);
            glVertex4dv(v.data());
        }
    }

    // Pixel transfer records end with { has data, bytes, data... }.
    static const void* imageData(const Words& w, size_t at, size_t end) {
        if (at + 2 > end || w.u(at) == 0) return nullptr;
        const uint32_t bytes = w.u(at + 1);
        if (at + 2 + (bytes + 3) / 4 > end) return nullptr;
        return w.bytes(at + 2);
    }

    // Runs a command stream in `ctx`, which activate() made current.
    void execute(Context& ctx, const Words& w) {
        for (size_t at = 0; at < w.size();) {
            const uint32_t header = w.u(at);
            const uint32_t cmd = header >> 16;
            size_t n = header & 0xFFFF;
            size_t p = at + 1;
            if (n == GLP_LONG_RECORD) {
                if (p >= w.size()) break;
                n = w.u(p);
                ++p;
            }
            if (p + n > w.size()) break; // truncated batch
            const size_t end = p + n;
            switch (cmd) {
            case GLP_FN_CLIENT_DRAW:
                if (n >= 4) clientDraw(w, p, n);
                break;
            case GLP_FN_glPixelStorei: {
                const GLenum pname = w.u(p);
                const GLint value = w.i(p + 1);
                // The guest's byte order is the opposite of the host's.
                if (pname == GL_UNPACK_SWAP_BYTES || pname == GL_PACK_SWAP_BYTES) {
                    glPixelStorei(pname, value != 0 ? GL_FALSE : GL_TRUE);
                } else {
                    glPixelStorei(pname, value);
                }
                break;
            }
            case GLP_FN_glTexImage1D:
                glTexImage1D(w.u(p), w.i(p + 1), w.i(p + 2), w.i(p + 3), w.i(p + 4), w.u(p + 5), w.u(p + 6),
                             imageData(w, p + 7, end));
                break;
            case GLP_FN_glTexImage2D:
                glTexImage2D(w.u(p), w.i(p + 1), w.i(p + 2), w.i(p + 3), w.i(p + 4), w.i(p + 5), w.u(p + 6),
                             w.u(p + 7), imageData(w, p + 8, end));
                break;
            case GLP_FN_glTexImage3D:
            case GLP_FN_glTexImage3DEXT: // the core command of the legacy profile (GL 1.2)
                glTexImage3D(w.u(p), w.i(p + 1), w.i(p + 2), w.i(p + 3), w.i(p + 4), w.i(p + 5), w.i(p + 6),
                             w.u(p + 7), w.u(p + 8), imageData(w, p + 9, end));
                break;
            case GLP_FN_glTexSubImage3D:
            case GLP_FN_glTexSubImage3DEXT:
                if (const void* data = imageData(w, p + 10, end)) {
                    glTexSubImage3D(w.u(p), w.i(p + 1), w.i(p + 2), w.i(p + 3), w.i(p + 4), w.i(p + 5), w.i(p + 6),
                                    w.i(p + 7), w.u(p + 8), w.u(p + 9), data);
                }
                break;
            case GLP_FN_glTexSubImage1D:
                if (const void* data = imageData(w, p + 6, end)) {
                    glTexSubImage1D(w.u(p), w.i(p + 1), w.i(p + 2), w.i(p + 3), w.u(p + 4), w.u(p + 5), data);
                }
                break;
            case GLP_FN_glTexSubImage2D:
                if (const void* data = imageData(w, p + 8, end)) {
                    glTexSubImage2D(w.u(p), w.i(p + 1), w.i(p + 2), w.i(p + 3), w.i(p + 4), w.i(p + 5), w.u(p + 6),
                                    w.u(p + 7), data);
                }
                break;
            case GLP_FN_glDrawPixels:
                if (const void* data = imageData(w, p + 4, end)) {
                    glDrawPixels(w.i(p), w.i(p + 1), w.u(p + 2), w.u(p + 3), data);
                }
                break;
            case GLP_FN_glBitmap:
                glBitmap(w.i(p), w.i(p + 1), w.f(p + 2), w.f(p + 3), w.f(p + 4), w.f(p + 5),
                         static_cast<const GLubyte*>(imageData(w, p + 6, end)));
                break;
            case GLP_FN_glPolygonStipple:
                if (const void* data = imageData(w, p, end)) glPolygonStipple(static_cast<const GLubyte*>(data));
                break;
            case GLP_FN_glCallLists: {
                const auto count = static_cast<uint32_t>(std::min<size_t>(w.u(p), n - 1));
                lists_.resize(count);
                for (uint32_t k = 0; k < count; ++k) lists_[k] = w.i(p + 1 + k);
                glCallLists(static_cast<GLsizei>(count), GL_INT, lists_.data());
                break;
            }
            case GLP_FN_glDeleteTextures: {
                const auto count = static_cast<uint32_t>(std::min<size_t>(w.u(p), n - 1));
                std::vector<GLuint> names(count);
                for (uint32_t k = 0; k < count; ++k) names[k] = w.u(p + 1 + k);
                glDeleteTextures(static_cast<GLsizei>(count), names.data());
                break;
            }
            case GLP_FN_glPrioritizeTextures: {
                const auto count = static_cast<uint32_t>(std::min<size_t>(w.u(p), (n - 1) / 2));
                std::vector<GLuint> names(count);
                std::vector<GLclampf> priorities(count);
                for (uint32_t k = 0; k < count; ++k) {
                    names[k] = w.u(p + 1 + k);
                    priorities[k] = w.f(p + 1 + count + k);
                }
                glPrioritizeTextures(static_cast<GLsizei>(count), names.data(), priorities.data());
                break;
            }
            case GLP_FN_glMap1f: case GLP_FN_glMap1d: case GLP_FN_glMap2f: case GLP_FN_glMap2d:
                map(w, cmd, p, n);
                break;
            case GLP_FN_glPixelMapfv: case GLP_FN_glPixelMapuiv: case GLP_FN_glPixelMapusv: {
                if (n < 2) break;
                const GLenum which = w.u(p);
                const auto size = static_cast<uint32_t>(std::min<size_t>(w.u(p + 1), n - 2));
                if (cmd == GLP_FN_glPixelMapfv) {
                    std::vector<GLfloat> v(size);
                    for (uint32_t k = 0; k < size; ++k) v[k] = w.f(p + 2 + k);
                    glPixelMapfv(which, static_cast<GLint>(size), v.data());
                } else if (cmd == GLP_FN_glPixelMapuiv) {
                    std::vector<GLuint> v(size);
                    for (uint32_t k = 0; k < size; ++k) v[k] = w.u(p + 2 + k);
                    glPixelMapuiv(which, static_cast<GLint>(size), v.data());
                } else {
                    std::vector<GLushort> v(size);
                    for (uint32_t k = 0; k < size; ++k) v[k] = static_cast<GLushort>(w.u(p + 2 + k));
                    glPixelMapusv(which, static_cast<GLint>(size), v.data());
                }
                break;
            }
            case GLP_FN_glDrawBuffer:
                ctx.drawBuffer = w.u(p);
                applyDrawBuffer(ctx.drawBuffer);
                break;
            case GLP_FN_glReadBuffer:
                ctx.readBuffer = w.u(p);
                applyReadBuffer(ctx.readBuffer);
                break;
            case GLP_FN_glSelectBuffer:
                ctx.select.assign(std::min<uint32_t>(w.u(p), 1u << 20), 0);
                glSelectBuffer(static_cast<GLsizei>(ctx.select.size()), ctx.select.data());
                break;
            case GLP_FN_glFeedbackBuffer:
                ctx.feedback.assign(std::min<uint32_t>(w.u(p), 1u << 20), 0.0f);
                glFeedbackBuffer(static_cast<GLsizei>(ctx.feedback.size()), w.u(p + 1), ctx.feedback.data());
                break;
            case GLP_FN_glEnable:
                glEnable(w.u(p));
                break;
            case GLP_FN_glDisable:
                glDisable(w.u(p));
                break;
            case GLP_FN_glBegin:
                glBegin(w.u(p));
                break;
            case GLP_FN_glEnd:
                glEnd();
                break;
            case GLP_FN_PALETTE:
                if (n >= 2 && ctx.drawable != 0) {
                    Surface& surface = surfaces_[{CGLGetShareGroup(ctx.cgl), ctx.drawable}];
                    const auto count = static_cast<uint32_t>(std::min<size_t>(w.u(p + 1), n - 2));
                    surface.overlay = (w.u(p) & GLP_PALETTE_OVERLAY) != 0;
                    surface.palette.resize(count);
                    for (uint32_t k = 0; k < count; ++k) surface.palette[k] = w.u(p + 2 + k) & 0x00FF'FFFFu;
                }
                break;
            case GLP_FN_glPushClientAttrib:
                glPushClientAttrib(w.u(p));
                break;
            case GLP_FN_glPopClientAttrib:
                glPopClientAttrib();
                break;
#include "GlDispatchGen.inc"
            default:
                if (unknownLogged_++ < 16) log::warn("GL bridge: unknown command {:#x}", cmd);
                break;
            }
            at = end;
        }
    }

    // Record: target, u1, u2, v1, v2 (doubles), uorder, vorder, packed points.
    static void map(const Words& w, uint32_t cmd, size_t p, size_t n) {
        if (n < 11) return;
        const GLenum target = w.u(p);
        const double u1 = w.d(p + 1), u2 = w.d(p + 3), v1 = w.d(p + 5), v2 = w.d(p + 7);
        const auto uorder = static_cast<GLint>(w.u(p + 9));
        const auto vorder = static_cast<GLint>(w.u(p + 10));
        const bool isDouble = cmd == GLP_FN_glMap1d || cmd == GLP_FN_glMap2d;
        if (uorder <= 0 || vorder <= 0) return;
        const size_t points = static_cast<size_t>(uorder) * static_cast<size_t>(vorder);
        const size_t values = (n - 11) / (isDouble ? 2 : 1);
        const auto k = static_cast<GLint>(values / points);
        if (k < 1 || k > 4) return;
        std::vector<GLdouble> v(values);
        for (size_t i = 0; i < values; ++i) v[i] = isDouble ? w.d(p + 11 + 2 * i) : w.f(p + 11 + i);
        if (cmd == GLP_FN_glMap1f || cmd == GLP_FN_glMap1d) {
            glMap1d(target, u1, u2, k, uorder, v.data());
        } else {
            glMap2d(target, u1, u2, k, uorder, v1, v2, k * uorder, vorder, v.data());
        }
    }

    // GLP_OP_QUERY: { command, reply address, reply bytes, arguments... }.
    void query(R5000& cpu, uint32_t context, Transfers& io, const Words& w) {
        const uint32_t fn = w.u(0);
        const auto replyAt = static_cast<VirtAddr>(static_cast<int64_t>(static_cast<int32_t>(w.u(1))));
        const uint32_t replyBytes = std::min<uint32_t>(w.u(2), kMaxRequest);
        const size_t nargs = w.size() - 3;
        auto a = [&](size_t k) { return k < nargs ? w.u(3 + k) : 0u; };
        // A reply larger than the TLB maps is written page by page; when a
        // fault interrupts it, the re-executed hypercall carries on writing
        // the same, already computed reply: the query does not run twice.
        if (io.out.active && io.out.fn == fn && io.out.at == replyAt && io.out.size == replyBytes) {
            finishReply(cpu, io.out);
            return;
        }
        Context* active = activate(context);
        if (active == nullptr) {
            cpu.setHypercallResult(0);
            return;
        }
        const bool readsBack = fn == GLP_FN_glReadPixels || fn == GLP_FN_glGetTexImage || fn == GLP_FN_glGetPolygonStipple;
        if (readsBack) { // image reads start from the guest buffer: the pack state may skip bytes
            if (!io.preread.active || io.preread.at != replyAt || io.preread.data.size() != replyBytes) {
                io.preread = Preread{replyAt, 0, std::vector<uint8_t>(replyBytes), true};
            }
            if (!cpu.guestCopyIn(replyAt, io.preread.data, io.preread.done)) return;
            io.preread.active = false;
        }
        Context& ctx = *active;
        Reply reply;
        uint64_t result = 0;
        auto values = [&](const auto* data, size_t count) {
            using T = std::remove_cvref_t<decltype(*data)>;
            for (size_t k = 0; k < count; ++k) {
                if constexpr (std::is_same_v<T, GLboolean>) reply.byte(data[k]);
                else if constexpr (std::is_same_v<T, GLfloat>) reply.f(data[k]);
                else if constexpr (std::is_same_v<T, GLdouble>) reply.d(data[k]);
                else reply.u(static_cast<uint32_t>(data[k]));
            }
        };
        auto imageReply = [&](auto&& readInto) {
            reply.bytes() = std::move(io.preread.data);
            reply.bytes().resize(replyBytes);
            readInto(reply.bytes().data());
        };
        switch (fn) {
        case GLP_FN_glGetError:
            result = glGetError();
            break;
        case GLP_FN_glGetString: {
            const auto* s = reinterpret_cast<const char*>(glGetString(a(0)));
            if (s != nullptr && replyBytes > 0) {
                const size_t len = std::min<size_t>(std::strlen(s), replyBytes - 1);
                reply.bytes().assign(s, s + len);
                reply.byte(0);
            }
            break;
        }
        case GLP_FN_glGetBooleanv: {
            std::array<GLboolean, 32> v{};
            glGetBooleanv(a(0), v.data());
            values(v.data(), std::min<size_t>(replyBytes, v.size()));
            break;
        }
        case GLP_FN_glGetIntegerv: {
            std::array<GLint, 32> v{};
            glGetIntegerv(a(0), v.data());
            values(v.data(), std::min<size_t>(replyBytes / 4, v.size()));
            break;
        }
        case GLP_FN_glGetFloatv: {
            std::array<GLfloat, 32> v{};
            glGetFloatv(a(0), v.data());
            values(v.data(), std::min<size_t>(replyBytes / 4, v.size()));
            break;
        }
        case GLP_FN_glGetDoublev: {
            std::array<GLdouble, 32> v{};
            glGetDoublev(a(0), v.data());
            values(v.data(), std::min<size_t>(replyBytes / 8, v.size()));
            break;
        }
        case GLP_FN_glIsEnabled: result = glIsEnabled(a(0)); break;
        case GLP_FN_glIsList: result = glIsList(a(0)); break;
        case GLP_FN_glIsTexture: result = glIsTexture(a(0)); break;
        case GLP_FN_glGenLists: result = glGenLists(static_cast<GLsizei>(a(0))); break;
        case GLP_FN_glGenTextures: {
            std::vector<GLuint> names(std::min<uint32_t>(a(0), replyBytes / 4));
            glGenTextures(static_cast<GLsizei>(names.size()), names.data());
            values(names.data(), names.size());
            break;
        }
        case GLP_FN_glRenderMode: {
            GLint previous = GL_RENDER;
            glGetIntegerv(GL_RENDER_MODE, &previous);
            result = static_cast<uint64_t>(static_cast<int64_t>(glRenderMode(a(0))));
            // Leaving selection or feedback: the guest gets the buffer's contents.
            if (previous == GL_SELECT) values(ctx.select.data(), std::min<size_t>(ctx.select.size(), replyBytes / 4));
            if (previous == GL_FEEDBACK) {
                values(ctx.feedback.data(), std::min<size_t>(ctx.feedback.size(), replyBytes / 4));
            }
            break;
        }
#define GLP_GET2(name, T)                                                   \
    case GLP_FN_##name: {                                                   \
        std::array<T, 16> v{};                                              \
        name(a(0), a(1), v.data());                                         \
        values(v.data(), std::min<size_t>(replyBytes / sizeof(T), v.size())); \
        break;                                                              \
    }
        GLP_GET2(glGetLightfv, GLfloat)
        GLP_GET2(glGetLightiv, GLint)
        GLP_GET2(glGetMaterialfv, GLfloat)
        GLP_GET2(glGetMaterialiv, GLint)
        GLP_GET2(glGetTexParameterfv, GLfloat)
        GLP_GET2(glGetTexParameteriv, GLint)
        GLP_GET2(glGetTexEnvfv, GLfloat)
        GLP_GET2(glGetTexEnviv, GLint)
        GLP_GET2(glGetTexGenfv, GLfloat)
        GLP_GET2(glGetTexGeniv, GLint)
        GLP_GET2(glGetTexGendv, GLdouble)
#undef GLP_GET2
        case GLP_FN_glGetTexLevelParameterfv: {
            GLfloat v = 0;
            glGetTexLevelParameterfv(a(0), static_cast<GLint>(a(1)), a(2), &v);
            values(&v, 1);
            break;
        }
        case GLP_FN_glGetTexLevelParameteriv: {
            GLint v = 0;
            glGetTexLevelParameteriv(a(0), static_cast<GLint>(a(1)), a(2), &v);
            values(&v, 1);
            break;
        }
        case GLP_FN_glGetClipPlane: {
            std::array<GLdouble, 4> v{};
            glGetClipPlane(a(0), v.data());
            values(v.data(), v.size());
            break;
        }
        case GLP_FN_glReadPixels:
            imageReply([&](uint8_t* out) {
                glReadPixels(static_cast<GLint>(a(0)), static_cast<GLint>(a(1)), static_cast<GLsizei>(a(2)),
                             static_cast<GLsizei>(a(3)), a(4), a(5), out);
            });
            break;
        case GLP_FN_glGetTexImage:
            imageReply([&](uint8_t* out) { glGetTexImage(a(0), static_cast<GLint>(a(1)), a(2), a(3), out); });
            break;
        case GLP_FN_glGetPolygonStipple:
            imageReply([&](uint8_t* out) { glGetPolygonStipple(out); });
            break;
        case GLP_FN_glAreTexturesResident: {
            std::vector<GLuint> names(std::min<size_t>(nargs, replyBytes));
            for (size_t k = 0; k < names.size(); ++k) names[k] = a(k);
            std::vector<GLboolean> resident(names.size(), GL_TRUE);
            result = glAreTexturesResident(static_cast<GLsizei>(names.size()), names.data(), resident.data());
            if (result == GL_TRUE) std::fill(resident.begin(), resident.end(), GL_TRUE); // GL leaves them unset
            values(resident.data(), resident.size());
            break;
        }
        case GLP_FN_READ_DRAWABLE: {
            // The buffer the guest draws to, as RGBA8 (pixmap copy-out).
            const auto width = static_cast<GLsizei>(a(0));
            const auto height = static_cast<GLsizei>(a(1));
            if (width <= 0 || height <= 0 ||
                static_cast<uint64_t>(width) * static_cast<uint64_t>(height) * 4 > replyBytes) {
                break;
            }
            reply.bytes().resize(replyBytes);
            const DefaultPackState pack;
            glReadBuffer(isFront(ctx.drawBuffer) ? kFront : kBack);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, reply.bytes().data());
            applyReadBuffer(ctx.readBuffer);
            break;
        }
        default:
            if (unknownLogged_++ < 16) log::warn("GL bridge: unknown query {:#x}", fn);
            break;
        }
        std::vector<uint8_t>& bytes = reply.bytes();
        if (bytes.size() > replyBytes) bytes.resize(replyBytes);
        io.out = Reply::Pending{fn, replyAt, replyBytes, result, std::move(bytes), 0, true};
        finishReply(cpu, io.out);
    }

    static void finishReply(R5000& cpu, Reply::Pending& out) {
        if (!cpu.guestCopyOut(out.at, out.data, out.done)) return; // page fault: the hypercall resumes
        out.active = false;
        cpu.setHypercallResult(out.result);
    }

    // The framebuffer of `drawable`: the calling context's, else another's.
    std::pair<Context*, Framebuffer*> locate(uint32_t context, uint32_t drawable) {
        if (auto own = contexts_.find(context); own != contexts_.end()) {
            Context& ctx = own->second;
            if (auto it = ctx.drawables.find(drawable); it != ctx.drawables.end()) return {&ctx, &it->second};
        }
        for (auto& [id, ctx] : contexts_) {
            if (auto it = ctx.drawables.find(drawable); it != ctx.drawables.end()) return {&ctx, &it->second};
        }
        return {nullptr, nullptr};
    }

    void swap(uint32_t context, uint32_t drawable, int32_t x, int32_t y, int32_t width, int32_t height, uint8_t key,
              uint32_t flags) {
        if ((flags & GLP_SWAP_HIDE) != 0) { // unmapped or destroyed: no image any more
            if (sink_) sink_(drawable, nullptr);
            return;
        }
        auto [ctx, fb] = locate(context, drawable);
        if (fb == nullptr || fb->surface == nullptr) return;
        // The copy and read-back below change the host context, framebuffer
        // and buffers: the next operation's activate() sets its own again.
        active_ = 0;
        CGLSetCurrentContext(ctx->cgl);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fb->fbo);
        if ((flags & GLP_SWAP_BACK_TO_FRONT) != 0) {
            const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST); // a blit is scissored
            if (scissor) glDisable(GL_SCISSOR_TEST);
            glReadBuffer(kBack);
            glDrawBuffer(kFront);
            glBlitFramebufferEXT(0, 0, fb->surface->width, fb->surface->height, 0, 0, fb->surface->width, fb->surface->height, GL_COLOR_BUFFER_BIT,
                                 GL_NEAREST);
            if (scissor) glEnable(GL_SCISSOR_TEST);
        }
        if ((flags & GLP_SWAP_NO_DISPLAY) == 0 && sink_) {
            DisplayOverlay overlay;
            overlay.x = x;
            overlay.y = y;
            overlay.width = static_cast<uint32_t>(std::max(0, std::min(width, fb->surface->width)));
            overlay.height = static_cast<uint32_t>(std::max(0, std::min(height, fb->surface->height)));
            overlay.key = key;
            overlay.layer = fb->surface->overlay ? 1 : 0;
            std::vector<uint32_t> rows(static_cast<size_t>(fb->surface->width) * static_cast<size_t>(fb->surface->height));
            glReadBuffer(kFront);
            {
                const DefaultPackState pack;
                glReadPixels(0, 0, fb->surface->width, fb->surface->height, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, rows.data());
            }
            overlay.pixels.resize(size_t{overlay.width} * overlay.height);
            for (uint32_t row = 0; row < overlay.height; ++row) { // GL rows are bottom-up
                const uint32_t* src = &rows[static_cast<size_t>(fb->surface->height - 1 - static_cast<int32_t>(row)) *
                                            static_cast<size_t>(fb->surface->width)];
                uint32_t* dst = &overlay.pixels[size_t{row} * overlay.width];
                const std::vector<uint32_t>& palette = fb->surface->palette;
                if (palette.empty()) {
                    for (uint32_t col = 0; col < overlay.width; ++col) {
                        dst[col] = DisplayOverlay::kOpaque | (src[col] & 0x00FF'FFFFu);
                    }
                    continue;
                }
                for (uint32_t col = 0; col < overlay.width; ++col) { // stored index -> colour
                    const uint32_t index =
                        GLP_INDEX_FROM_RGBA(src[col] >> 16 & 0xFFu, src[col] >> 8 & 0xFFu, src[col] & 0xFFu, src[col] >> 24);
                    if (index == 0 && fb->surface->overlay) {
                        dst[col] = 0; // transparent
                    } else {
                        dst[col] = DisplayOverlay::kOpaque | (index < palette.size() ? palette[index] : 0u);
                    }
                }
            }
            sink_(drawable, &overlay);
        }
        // The framebuffer keeps the guest's draw and read buffers.
        applyDrawBuffer(ctx->drawBuffer);
        applyReadBuffer(ctx->readBuffer);
    }

    OverlaySink sink_;
    CGLPixelFormatObj pixelFormat_ = nullptr;
    std::map<uint32_t, Context> contexts_;
    std::map<std::pair<CGLShareGroupObj, uint32_t>, Surface> surfaces_; // by share group and drawable
    uint32_t nextId_ = 1;
    uint32_t active_ = 0; // the guest context whose state the host GL holds (activate)
    unsigned unknownLogged_ = 0;
    bool tableMismatchLogged_ = false;
    std::map<uint32_t, Transfers> transfers_; // by guest context (0: none)
    std::array<std::vector<uint8_t>, 6> arrayData_;
    std::vector<GLuint> indices_;
    std::vector<GLint> lists_;
};

} // namespace

std::unique_ptr<GlBridge> GlBridge::create(OverlaySink sink) {
    return std::make_unique<MacGlBridge>(std::move(sink));
}

} // namespace indy
