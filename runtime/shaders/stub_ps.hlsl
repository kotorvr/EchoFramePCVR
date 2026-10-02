// Stands in for a pixel shader the GPU can't run (it uses double precision): writes zero.
float4 main() : SV_Target0 { return float4(0, 0, 0, 0); }
