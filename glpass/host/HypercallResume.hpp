#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace indy {

// What identifies one guest GL hypercall. A hypercall interrupted by a page
// fault is re-executed by IRIX once the page is mapped, with the same
// registers, so it arrives again with the same key.
struct HypercallKey {
    uint8_t asid = 0;    // the guest address space it was made in (EntryHi ASID)
    uint32_t word = 0;   // operation and context (GLP_OP_WORD)
    uint64_t buffer = 0; // guest address of the request
    uint32_t arg = 0;    // its length, or the operation's argument
    friend bool operator==(const HypercallKey&, const HypercallKey&) = default;
};

// One context's hypercall in progress across page faults, by stage:
//   1. the request copied in from guest memory,
//   2. for image reads, the guest's reply buffer read first (the pixel store
//      state may leave bytes of it untouched),
//   3. the reply, computed once, copied out.
// Each stage resumes where a fault stopped it; a finished stage is not run
// again, so a query never runs twice. Buffers keep their capacity between
// calls (a command stream can be megabytes, submitted many times a second).
struct HypercallResume {
    HypercallKey key;
    bool open = false; // a call is in progress

    std::vector<uint8_t> request;
    size_t requestDone = 0;
    bool requestRead = false;

    std::vector<uint8_t> preread;
    size_t prereadDone = 0;
    bool prereadRead = false;

    std::vector<uint8_t> reply;
    uint64_t replyAt = 0;
    size_t replyDone = 0;
    uint64_t result = 0;
    bool replyReady = false;

    void reset(const HypercallKey& newKey) {
        key = newKey;
        open = true;
        request.clear();
        requestDone = 0;
        requestRead = false;
        preread.clear();
        prereadDone = 0;
        prereadRead = false;
        reply.clear();
        replyAt = 0;
        replyDone = 0;
        result = 0;
        replyReady = false;
    }
};

// The hypercalls in progress, one per guest context. A context is current in
// one guest thread at most (glpass enforces GLX's rule), and that thread
// re-executes its interrupted call before it can make another, so a context
// has at most one call in progress. The key also holds the address space:
// context 0 (no context) is shared by every guest process.
class HypercallResumes {
public:
    // The record for `context`'s call `key`: the call in progress when it is
    // that call, else a fresh one. A call left unfinished (its thread killed
    // during a fault) is dropped then, so its state cannot reach another call.
    HypercallResume& enter(uint32_t context, const HypercallKey& key) {
        HypercallResume& r = calls_[context];
        if (!r.open || !(r.key == key)) r.reset(key);
        return r;
    }
    // The call completed: nothing to resume (the buffers are kept for reuse).
    void finish(uint32_t context) noexcept {
        if (auto it = calls_.find(context); it != calls_.end()) it->second.open = false;
    }
    // The context was destroyed.
    void forget(uint32_t context) { calls_.erase(context); }

    [[nodiscard]] bool inProgress(uint32_t context) const {
        const auto it = calls_.find(context);
        return it != calls_.end() && it->second.open;
    }

private:
    std::map<uint32_t, HypercallResume> calls_;
};

} // namespace indy
