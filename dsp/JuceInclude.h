#pragma once

// The DSP core builds two ways: with JUCE (desktop plugins) or with the small
// stand-ins in lite/JuceLite.h (browser / WebAssembly), selected by VS_NO_JUCE.
#if VS_NO_JUCE
 #include "lite/JuceLite.h"
#else
 #include <juce_dsp/juce_dsp.h>
#endif
