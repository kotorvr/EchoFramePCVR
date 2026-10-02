// Stands in for a vertex shader the GPU can't run (it uses double precision): every vertex
// lands at w = 0, so the draw is clipped away and nothing it would have drawn shows.
float4 main() : SV_Position { return float4(0, 0, 0, 0); }
