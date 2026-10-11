# Shared compiler configuration, exposed as the interface target `indy::options`.
# Link it PRIVATE so these flags never leak into consumers or third-party code.

add_library(indy_options INTERFACE)
add_library(indy::options ALIAS indy_options)

if(MSVC)
    target_compile_options(indy_options INTERFACE
        /W4 /permissive- /Zc:__cplusplus /Zc:preprocessor /utf-8
        $<$<BOOL:${INDY_WARNINGS_AS_ERRORS}>:/WX>
        $<$<CONFIG:Release,RelWithDebInfo>:/O2 /Ob3 /Oi>
        $<$<AND:$<CONFIG:Release,RelWithDebInfo>,$<BOOL:${INDY_NATIVE_ARCH}>>:/arch:AVX2>)
    target_compile_definitions(indy_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN)
else()
    target_compile_options(indy_options INTERFACE
        -Wall -Wextra -Wpedantic
        -Wshadow -Wconversion -Wsign-conversion   # catch silent truncation of register values
        -Wnon-virtual-dtor -Woverloaded-virtual
        -Wnull-dereference -Wimplicit-fallthrough
        $<$<BOOL:${INDY_WARNINGS_AS_ERRORS}>:-Werror>
        # Profile what you ship: RelWithDebInfo gets the same -O3 as Release.
        $<$<CONFIG:Release,RelWithDebInfo>:-O3>
        $<$<AND:$<CONFIG:Release,RelWithDebInfo>,$<BOOL:${INDY_NATIVE_ARCH}>>:-march=native>
        $<$<CONFIG:Debug>:-O0 -g3 -fno-omit-frame-pointer>)

    if(INDY_SANITIZE)
        target_compile_options(indy_options INTERFACE
            $<$<CONFIG:Debug>:-fsanitize=address,undefined -fno-sanitize-recover=undefined>)
        target_link_options(indy_options INTERFACE
            $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
    endif()
endif()
