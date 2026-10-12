// GLES 3.2 backend of gpu::Renderer (gl_renderer.cpp): createGlesRenderer().
// The renderer interface itself is API-neutral (gpu_renderer.h).
#pragma once

// The xr/ code still draws with GL through this include and uses the old
// names (EfbTarget); both go when it moves to the graphics split.
#include <GLES3/gl32.h>

#include "gpu_renderer.h"
