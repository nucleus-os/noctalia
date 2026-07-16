#pragma once

class GraphicsDevice;

// Runs an opt-in, synchronous pixel correctness check through the production
// Graphite offscreen rendering path. Intended for validation builds only.
void runGraphiteOffscreenGolden(GraphicsDevice& graphics);
