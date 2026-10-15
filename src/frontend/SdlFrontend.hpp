#pragma once

#include "indy/core/Commands.hpp"
#include "indy/core/FrameBuffer.hpp"
#include "indy/input/TextKeys.hpp"

#include <SDL3/SDL.h>

#include <deque>
#include <memory>
#include <string>

namespace indy::frontend {

struct FrontendConfig {
    std::string title = "SGI Indy";
    int  width  = 1024;
    int  height = 768;
    bool vsync  = true;
    KeyLayout keyLayout = KeyLayout::Us; // IRIX's keyboard layout, for pasted text
};

// Owns the SDL window, renderer and streaming texture. Lives on the main
// thread (SDL requires video/events there on macOS). Throws on init failure.
class SdlFrontend {
public:
    explicit SdlFrontend(const FrontendConfig& config);
    SdlFrontend(const SdlFrontend&) = delete;
    SdlFrontend& operator=(const SdlFrontend&) = delete;

    // Drain all pending SDL events, translating user actions into commands.
    // Returns false once the user has asked to quit.
    [[nodiscard]] bool pumpEvents(CommandQueue& commands);

    // Upload the newest emulated frame (if any) and present.
    void present(FrameBuffer& frame);

    void setTitle(const std::string& title);
    [[nodiscard]] bool vsyncActive() const noexcept { return vsyncActive_; }
    // Mouse capture: click in the window to grab, Ctrl+Alt+G to release.
    [[nodiscard]] bool mouseCaptured() const noexcept { return mouseCaptured_; }

private:
    // RAII for SDL_Init/SDL_Quit. Declared first so it is destroyed last,
    // after the texture, renderer and window have been released.
    struct SdlContext {
        SdlContext();
        ~SdlContext();
        SdlContext(const SdlContext&) = delete;
        SdlContext& operator=(const SdlContext&) = delete;
    };

    struct WindowDeleter   { void operator()(SDL_Window* p) const noexcept { SDL_DestroyWindow(p); } };
    struct RendererDeleter { void operator()(SDL_Renderer* p) const noexcept { SDL_DestroyRenderer(p); } };
    struct TextureDeleter  { void operator()(SDL_Texture* p) const noexcept { SDL_DestroyTexture(p); } };

    void handleEvent(const SDL_Event& event, CommandQueue& commands);
    void handleDropFile(const SDL_DropEvent& drop, CommandQueue& commands);
    void toggleFullscreen();
    void setMouseCapture(bool captured);
    void startPaste();
    void feedPaste(CommandQueue& commands);
    void flushMouse(CommandQueue& commands, bool force);

    SdlContext context_;
    std::unique_ptr<SDL_Window, WindowDeleter> window_;
    std::unique_ptr<SDL_Renderer, RendererDeleter> renderer_;
    std::unique_ptr<SDL_Texture, TextureDeleter> texture_;
    bool vsyncActive_ = false;
    bool quitRequested_ = false;
    bool mouseCaptured_ = false;
    float mouseDx_ = 0, mouseDy_ = 0; // accumulated motion in guest pixels (PS/2: +y = up)
    uint8_t mouseButtons_ = 0;
    std::deque<cmd::KeyEvent> pasteQueue_; // clipboard text being typed into the PS/2 keyboard
    KeyLayout keyLayout_;                  // the guest's layout, which picks the keys
};

} // namespace indy::frontend
