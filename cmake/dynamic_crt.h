#pragma once

// LTO objects hide normal CRT linker directives. Check the compiler's actual
// runtime selection in every C/C++ translation unit instead, before archiving.
#if !defined(_MT) || !defined(_DLL)
#error "Winamp Opus and its dependencies require the dynamic CRT (/MD or /MDd)."
#endif
#if defined(_DEBUG) != defined(WINAMP_OPUS_DEBUG_CRT)
#error "The CRT debug/release mode does not match the build configuration."
#endif
