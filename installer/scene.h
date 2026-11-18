// The installer's backdrop: a snowy mountainside seen from above, rendered on
// the CPU the way the game draws its piste map.
#pragma once
#include <cstdint>

// Renders the scene at w x h pixels into `out` (w * h pixels, 0xAARRGGBB,
// opaque). Takes a fraction of a second; uses every core.
void RenderScene(int w, int h, uint32_t* out);
// The app icon's picture: a snowy peak against the sky, size x size pixels.
void RenderIconScene(int size, uint32_t* out);
