#include "SdlFrontend.hpp"

#include "indy/core/Log.hpp"
#include "indy/input/TextKeys.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <system_error>

namespace indy::frontend {

namespace {

[[noreturn]] void throwSdlError(std::string_view what) {
    throw std::runtime_error(std::string(what) + ": " + SDL_GetError());
}

bool looksLikeCdImage(const std::filesystem::path& path) {
    static constexpr std::array<std::string_view, 4> kExtensions = {".iso", ".img", ".cdr", ".toast"};
    std::string ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return std::ranges::find(kExtensions, ext) != kExtensions.end();
}

} // namespace

// ---------------------------------------------------------------------------

SdlFrontend::SdlContext::SdlContext() {
    SDL_SetAppMetadata("SGI Indy Emulator", INDY_VERSION, "org.irixemu.indy");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        throwSdlError("SDL_Init failed");
    }
}

SdlFrontend::SdlContext::~SdlContext() {
    SDL_Quit();
}

SdlFrontend::SdlFrontend(const FrontendConfig& config) {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    if (!SDL_CreateWindowAndRenderer(config.title.c_str(), config.width, config.height,
                                     SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY,
                                     &window, &renderer)) {
        throwSdlError("SDL_CreateWindowAndRenderer failed");
    }
    window_.reset(window);
    renderer_.reset(renderer);

    // Keep the guest resolution fixed and letterbox it into whatever size the
    // window is, so resizing / fullscreen / HiDPI never distort the image.
    SDL_SetRenderLogicalPresentation(renderer, config.width, config.height,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    vsyncActive_ = config.vsync && SDL_SetRenderVSync(renderer, 1);
    if (config.vsync && !vsyncActive_) {
        log::warn("VSync unavailable ({}); falling back to timed frame pacing", SDL_GetError());
    }

    // Streaming texture: CPU-side pixels uploaded once per new frame.
    texture_.reset(SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888,
                                     SDL_TEXTUREACCESS_STREAMING, config.width, config.height));
    if (!texture_) {
        throwSdlError("SDL_CreateTexture failed");
    }
    SDL_SetTextureScaleMode(texture_.get(), SDL_SCALEMODE_NEAREST); // crisp pixels

    log::info("SDL {}.{}.{} video driver '{}', renderer '{}'",
              SDL_VERSIONNUM_MAJOR(SDL_GetVersion()), SDL_VERSIONNUM_MINOR(SDL_GetVersion()),
              SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver(),
              SDL_GetRendererName(renderer));
}

bool SdlFrontend::pumpEvents(CommandQueue& commands) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        handleEvent(event, commands);
    }
    flushMouse(commands, /*force=*/false); // at most one motion packet per frame
    feedPaste(commands);
    return !quitRequested_;
}

void SdlFrontend::startPaste() {
    if (!pasteQueue_.empty()) {
        pasteQueue_.clear(); // the hotkey again while pasting cancels
        log::info("Paste cancelled");
        return;
    }
    if (!SDL_HasClipboardText()) {
        log::info("Paste: the clipboard holds no text");
        return;
    }
    char* text = SDL_GetClipboardText();
    const TypedText typed = keyEventsForText(text != nullptr ? text : "");
    SDL_free(text);
    pasteQueue_.assign(typed.events.begin(), typed.events.end());
    log::info("Pasting {} key presses into the PS/2 keyboard{}", typed.events.size() / 2,
              typed.skipped != 0 ? std::format(" ({} characters with no US-keyboard key skipped)", typed.skipped)
                                 : std::string{});
}

void SdlFrontend::feedPaste(CommandQueue& commands) {
    // Wait until the host modifiers of the hotkey are released, or the guest
    // would see Ctrl/Alt/Cmd held during the paste. Then type at a steady pace
    // (about 1,000 characters per second at 60 frames per second).
    constexpr size_t kEventsPerFrame = 64;
    if (pasteQueue_.empty() ||
        (SDL_GetModState() & (SDL_KMOD_CTRL | SDL_KMOD_ALT | SDL_KMOD_GUI | SDL_KMOD_SHIFT)) != 0) {
        return;
    }
    for (size_t i = 0; i < kEventsPerFrame && !pasteQueue_.empty(); ++i) {
        commands.push(pasteQueue_.front());
        pasteQueue_.pop_front();
    }
}

void SdlFrontend::setMouseCapture(bool captured) {
    if (captured == mouseCaptured_) return;
    mouseCaptured_ = captured;
    SDL_SetWindowRelativeMouseMode(window_.get(), captured); // hides the host cursor while captured
    mouseDx_ = mouseDy_ = 0;
    mouseButtons_ = 0;
}

void SdlFrontend::flushMouse(CommandQueue& commands, bool force) {
    const auto dx = static_cast<int>(mouseDx_);
    const auto dy = static_cast<int>(mouseDy_);
    if (!force && dx == 0 && dy == 0) return;
    mouseDx_ -= static_cast<float>(dx); // keep the sub-pixel remainder
    mouseDy_ -= static_cast<float>(dy);
    commands.push(cmd::MouseEvent{static_cast<int16_t>(dx), static_cast<int16_t>(dy), mouseButtons_});
}

void SdlFrontend::handleEvent(const SDL_Event& event, CommandQueue& commands) {
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        quitRequested_ = true;
        break;

    // ---- Drag & drop: hot-swap the virtual SCSI CD-ROM -------------------
    case SDL_EVENT_DROP_BEGIN:
        log::debug("Drop begin");
        break;
    case SDL_EVENT_DROP_FILE:
        handleDropFile(event.drop, commands);
        break;
    case SDL_EVENT_DROP_TEXT:
        log::info("Ignoring dropped text (drop a file to insert a CD image)");
        break;
    case SDL_EVENT_DROP_COMPLETE:
        log::debug("Drop complete");
        break;

    // ---- Mouse: PS/2 mouse on the 8042 (only while captured) ---------------
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!mouseCaptured_) {
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN) setMouseCapture(true); // this click only grabs
            break;
        }
        const uint8_t bit = event.button.button == SDL_BUTTON_LEFT     ? 0x01
                            : event.button.button == SDL_BUTTON_RIGHT  ? 0x02
                            : event.button.button == SDL_BUTTON_MIDDLE ? 0x04
                                                                       : 0;
        if (bit == 0) break;
        mouseButtons_ = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? static_cast<uint8_t>(mouseButtons_ | bit)
                                                                   : static_cast<uint8_t>(mouseButtons_ & ~bit);
        flushMouse(commands, /*force=*/true); // button changes go out at once
        break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!mouseCaptured_) break;
        // Scale window motion to guest pixels (the image is letterboxed/scaled).
        SDL_Event scaled = event;
        SDL_ConvertEventToRenderCoordinates(renderer_.get(), &scaled);
        mouseDx_ += scaled.motion.xrel;
        mouseDy_ -= scaled.motion.yrel; // SDL: +y down; PS/2: +y up
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        setMouseCapture(false);
        break;

    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        // Host hotkey: Ctrl+Alt+V (or Cmd+V on macOS) types the clipboard.
        const bool ctrlAlt = (event.key.mod & SDL_KMOD_CTRL) != 0 && (event.key.mod & SDL_KMOD_ALT) != 0;
        if (event.key.key == SDLK_V && (ctrlAlt || (event.key.mod & SDL_KMOD_GUI) != 0)) {
            if (down && !event.key.repeat) startPaste();
            break;
        }
        // Host hotkey: Ctrl+Alt+G releases the captured mouse.
        if (event.key.key == SDLK_G && (event.key.mod & SDL_KMOD_CTRL) != 0 && (event.key.mod & SDL_KMOD_ALT) != 0) {
            if (down) setMouseCapture(false);
            break;
        }
        // Host hotkey: Alt+Enter toggles fullscreen and is not forwarded.
        if (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT) != 0) {
            if (down && !event.key.repeat) {
                toggleFullscreen();
            }
            break;
        }
        // Everything else goes to the Indy's PS/2 keyboard. SDL scancodes are
        // USB HID usages; auto-repeat arrives as repeated key-downs, like the
        // keyboard's own typematic repeat.
        commands.push(cmd::KeyEvent{static_cast<uint16_t>(event.key.scancode), down});
        break;
    }

    default:
        break;
    }
}

void SdlFrontend::handleDropFile(const SDL_DropEvent& drop, CommandQueue& commands) {
    // SDL3 owns drop.data (UTF-8) and frees it after the event; copy what we need.
    if (drop.data == nullptr) {
        log::warn("Drop event without a file path");
        return;
    }
    const std::filesystem::path path{
        std::u8string_view(reinterpret_cast<const char8_t*>(drop.data))};

    log::info("Dropped file: {}", drop.data);

    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        log::warn("Ignoring drop: '{}' is not a regular file", drop.data);
        return;
    }
    if (!looksLikeCdImage(path)) {
        log::warn("'{}' has an unusual extension for a CD image; trying anyway", drop.data);
    }

    // Placeholder for the full SCSI swap: the emulation thread validates the
    // image and loads it into the CD-ROM target (raising UNIT ATTENTION later).
    commands.push(cmd::InsertMedia{path});
}

void SdlFrontend::toggleFullscreen() {
    const bool fullscreen = (SDL_GetWindowFlags(window_.get()) & SDL_WINDOW_FULLSCREEN) != 0;
    SDL_SetWindowFullscreen(window_.get(), !fullscreen);
}

void SdlFrontend::present(FrameBuffer& frame) {
    if (frame.acquire()) {
        const auto pixels = frame.frontBuffer();
        SDL_UpdateTexture(texture_.get(), nullptr, pixels.data(), static_cast<int>(frame.pitchBytes()));
    }
    SDL_SetRenderDrawColor(renderer_.get(), 0, 0, 0, SDL_ALPHA_OPAQUE);
    SDL_RenderClear(renderer_.get());
    SDL_RenderTexture(renderer_.get(), texture_.get(), nullptr, nullptr);
    SDL_RenderPresent(renderer_.get());
}

void SdlFrontend::setTitle(const std::string& title) {
    SDL_SetWindowTitle(window_.get(), title.c_str());
}

} // namespace indy::frontend
