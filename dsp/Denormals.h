// ENCORE. Audio Processing
// Copyright (C) 2026 Encore Music, Inc. Licensed under the GNU AGPLv3 (see LICENSE).
//
// The denormal guard: numbers too small to be normal floats (denormals) are
// slow on most processors, and a decaying tail makes plenty of them, so audio
// code turns them off while it runs. This sets the processor's own mode, the
// same bits on the same systems as JUCE's ScopedNoDenormals:
//   x86 (SSE): MXCSR flush-to-zero (bit 15) and denormals-are-zero (bit 6)
//   64-bit Arm: FPCR FZ (bit 24); 32-bit Arm with NEON: FPSCR FZ (bit 24)
// Elsewhere (the browser's WebAssembly has no such mode) it does nothing.
#pragma once

#include <cstdint>

#if defined (__SSE__) || defined (_M_X64) || (defined (_M_IX86_FP) && _M_IX86_FP >= 1)
 #include <xmmintrin.h>
 #define VS_DENORMALS_X86 1
#elif defined (_MSC_VER) && defined (_M_ARM64)
 #include <float.h>
 #define VS_DENORMALS_MSVC_ARM 1
#elif defined (__aarch64__)
 #define VS_DENORMALS_ARM64 1
#elif defined (__arm__) && defined (__ARM_NEON)
 #define VS_DENORMALS_ARM32 1
#endif

namespace vs
{
    // The bits that turn denormals off (0 where there are none).
   #if defined (VS_DENORMALS_X86)
    inline constexpr std::intptr_t noDenormalsMask = 0x8040;
   #elif defined (VS_DENORMALS_MSVC_ARM) || defined (VS_DENORMALS_ARM64) || defined (VS_DENORMALS_ARM32)
    inline constexpr std::intptr_t noDenormalsMask = (std::intptr_t) 1 << 24;
   #else
    inline constexpr std::intptr_t noDenormalsMask = 0;
   #endif

    // The processor's floating-point control register (0 where there is none).
    inline std::intptr_t fpStatus() noexcept
    {
        std::intptr_t r = 0;
       #if defined (VS_DENORMALS_X86)
        r = (std::intptr_t) _mm_getcsr();
       #elif defined (VS_DENORMALS_MSVC_ARM)
        r = (std::intptr_t) (_control87 (0, 0) & _MCW_DN);
       #elif defined (VS_DENORMALS_ARM64)
        asm volatile ("mrs %0, fpcr" : "=r" (r));
       #elif defined (VS_DENORMALS_ARM32)
        asm volatile ("vmrs %0, fpscr" : "=r" (r));
       #endif
        return r;
    }

    inline void setFpStatus ([[maybe_unused]] std::intptr_t r) noexcept
    {
       #if defined (VS_DENORMALS_X86)
        // through a volatile: AppleClang 13 optimises the plain value away
        volatile auto v = (std::uint32_t) r;
        _mm_setcsr (v);
       #elif defined (VS_DENORMALS_MSVC_ARM)
        _control87 ((unsigned int) r, _MCW_DN);
       #elif defined (VS_DENORMALS_ARM64)
        asm volatile ("msr fpcr, %0" : : "r" (r));
       #elif defined (VS_DENORMALS_ARM32)
        asm volatile ("vmsr fpscr, %0" : : "r" (r));
       #endif
    }

    // Turns denormals off (or back on) on this thread until changed again.
    inline void disableDenormals (bool disable = true) noexcept
    {
        if constexpr (noDenormalsMask != 0)
            setFpStatus ((fpStatus() & ~noDenormalsMask) | (disable ? noDenormalsMask : 0));
    }

    inline bool denormalsDisabled() noexcept
    {
        return noDenormalsMask != 0 && (fpStatus() & noDenormalsMask) == noDenormalsMask;
    }

    // Denormals off for as long as this lives (an audio callback), then the
    // thread's mode as it was.
    class ScopedNoDenormals
    {
    public:
        ScopedNoDenormals() noexcept : saved (fpStatus())
        {
            if constexpr (noDenormalsMask != 0) setFpStatus (saved | noDenormalsMask);
        }
        ~ScopedNoDenormals()
        {
            if constexpr (noDenormalsMask != 0) setFpStatus (saved);
        }
        ScopedNoDenormals (const ScopedNoDenormals&) = delete;
        ScopedNoDenormals& operator= (const ScopedNoDenormals&) = delete;

    private:
        std::intptr_t saved;
    };
}
