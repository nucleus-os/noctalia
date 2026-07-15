#pragma once

// Keep profiling call sites readable while making non-Tracy builds completely
// independent from Tracy headers and symbols. The Meson profiling option
// defines both NOCTALIA_TRACY_ENABLE and Tracy's own TRACY_ENABLE switch.
#ifdef NOCTALIA_TRACY_ENABLE
#include <tracy/Tracy.hpp>

#define NOCTALIA_TRACE_ZONE(name) ZoneScopedN(name)
#define NOCTALIA_TRACE_FRAME(name) FrameMarkNamed(name)
#define NOCTALIA_TRACE_PLOT(name, value) TracyPlot(name, value)
#define NOCTALIA_TRACE_MESSAGE(text, size) TracyMessage(text, size)
#else
#define NOCTALIA_TRACE_ZONE(name) ((void)0)
#define NOCTALIA_TRACE_FRAME(name) ((void)0)
#define NOCTALIA_TRACE_PLOT(name, value) ((void)0)
#define NOCTALIA_TRACE_MESSAGE(text, size) ((void)0)
#endif
