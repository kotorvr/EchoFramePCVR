// Offline check of the runtime's DXIL double -> float demotion (runtime/src/efp_dxil.cpp):
//   dxil_demote <in.dxbc> <out.dxbc>
// then disassemble out.dxbc with dxc -dumpbin to see the result.
#include <stdint.h>
#include <stdio.h>
#include <string>
#include <vector>

bool EFP_DemoteDxilDoubles(const void* code, size_t size, std::vector<uint8_t>& out, std::string& why);

int main(int argc, char** argv)
{
	if (argc != 3) { fprintf(stderr, "usage: dxil_demote <in> <out>\n"); return 2; }
	FILE* f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 1; }
	std::vector<uint8_t> in;
	uint8_t buf[65536];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), f)) > 0) in.insert(in.end(), buf, buf + n);
	fclose(f);
	std::vector<uint8_t> out;
	std::string why;
	if (!EFP_DemoteDxilDoubles(in.data(), in.size(), out, why)) { printf("not demoted: %s\n", why.c_str()); return 1; }
	f = fopen(argv[2], "wb");
	fwrite(out.data(), 1, out.size(), f);
	fclose(f);
	printf("demoted\n");
	return 0;
}
