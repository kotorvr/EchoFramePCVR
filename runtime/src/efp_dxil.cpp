// Demoting a DXIL shader's double-precision type to single precision, for GPUs without
// double-precision shaders (the Steam Frame's Adreno 750).
//
// vkd3d-proton translates DXIL with dxil-spirv, which reads the LLVM bitcode with its own
// parser and emits SPIR-V from the types it finds. Echo's shaders that declare doubles use
// them for a compiler artifact (`uitofp i1 -> double`, `fabs`, `fcmp une 0.0`, i.e. the bool
// itself). If the bitcode's type table says FLOAT where it said DOUBLE, every such value and
// operation comes out as 32-bit float, and the shader no longer needs Float64.
//
// A DOUBLE type record has no operands, so the change is its record code, 4 -> 3, rewritten
// in place in the same bits. This walks the LLVM bitstream (blocks, abbreviations, BLOCKINFO)
// to find those records, and refuses (the caller falls back to a stub shader) when the module
// does anything the demotion would get wrong:
//   - a double constant other than 0.0 (its 64-bit pattern would be read as a float);
//   - fpext to double, fptrunc to float (would become float -> float conversions), or a
//     bitcast to double;
//   - a DOUBLE record whose code is a literal in its abbreviation (can't change one record).
#include <stddef.h>

#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>

namespace {

struct AbbrevOp
{
	enum Kind { Literal, Fixed, VBR, Array, Char6, Blob } kind;
	uint64_t value;   // the literal, or the width
};
typedef std::vector<AbbrevOp> Abbrev;

struct CodeField   // where a record's code sits, to rewrite it
{
	size_t pos = 0;
	unsigned width = 0;
	bool vbr = false;
	bool literal = false;
};

struct Reader
{
	const uint8_t* data;
	size_t bits;
	size_t pos = 0;
	bool bad = false;

	uint64_t read(unsigned n)
	{
		if (pos + n > bits) { bad = true; pos = bits; return 0; }
		uint64_t v = 0;
		for (unsigned i = 0; i < n; i++, pos++)
			v |= (uint64_t)((data[pos >> 3] >> (pos & 7)) & 1) << i;
		return v;
	}
	uint64_t vbr(unsigned n)
	{
		uint64_t v = 0;
		for (unsigned shift = 0; !bad; shift += n - 1) {
			uint64_t chunk = read(n);
			v |= (chunk & ((1ull << (n - 1)) - 1)) << shift;
			if (!(chunk & (1ull << (n - 1)))) break;
			if (shift > 64) { bad = true; break; }
		}
		return v;
	}
	void align32() { pos = (pos + 31) & ~(size_t)31; if (pos > bits) bad = true; }
};

enum { TYPE_BLOCK = 17, CONSTANTS_BLOCK = 11, FUNCTION_BLOCK = 12, BLOCKINFO_BLOCK = 0 };
enum { TYPE_NUMENTRY = 1, TYPE_FLOAT = 3, TYPE_DOUBLE = 4, TYPE_ARRAY = 11, TYPE_VECTOR = 12, TYPE_STRUCT_NAME = 19 };
enum { CST_SETTYPE = 1, CST_FLOAT = 6, CST_DATA = 22 };
enum { INST_CAST = 3 };
enum { CAST_FPTRUNC = 7, CAST_FPEXT = 8, CAST_BITCAST = 11 };

struct Walker
{
	Reader r;
	std::vector<std::vector<Abbrev>> blockInfo = std::vector<std::vector<Abbrev>>(64);
	struct Type { uint64_t code; uint64_t element; };
	std::vector<Type> types;
	std::vector<CodeField> doubles;
	std::string why;
	uint64_t constType = 0;

	bool IsDouble(uint64_t t) const { return t < types.size() && types[t].code == TYPE_DOUBLE; }
	bool IsFloat(uint64_t t) const { return t < types.size() && types[t].code == TYPE_FLOAT; }

	bool ReadAbbrev(Abbrev& a)
	{
		uint64_t n = r.vbr(5);
		for (uint64_t i = 0; i < n && !r.bad; i++) {
			if (r.read(1)) { a.push_back({ AbbrevOp::Literal, r.vbr(8) }); continue; }
			switch (r.read(3)) {
			case 1: a.push_back({ AbbrevOp::Fixed, r.vbr(5) }); break;
			case 2: a.push_back({ AbbrevOp::VBR, r.vbr(5) }); break;
			case 3: a.push_back({ AbbrevOp::Array, 0 }); break;
			case 4: a.push_back({ AbbrevOp::Char6, 0 }); break;
			case 5: a.push_back({ AbbrevOp::Blob, 0 }); break;
			default: r.bad = true;
			}
		}
		return !r.bad;
	}

	uint64_t ReadScalar(const AbbrevOp& op, CodeField* where)
	{
		if (where) { where->pos = r.pos; where->width = (unsigned)op.value; }
		switch (op.kind) {
		case AbbrevOp::Literal: if (where) where->literal = true; return op.value;
		case AbbrevOp::Fixed: return r.read((unsigned)op.value);
		case AbbrevOp::VBR: if (where) where->vbr = true; return r.vbr((unsigned)op.value);
		case AbbrevOp::Char6: return r.read(6);
		default: r.bad = true; return 0;
		}
	}

	// One record, abbreviated (id >= 4) or not (id 3): its code, operands and code position.
	bool ReadRecord(uint64_t id, const std::vector<Abbrev>& abbrevs, uint64_t& code, std::vector<uint64_t>& ops, CodeField& field)
	{
		ops.clear();
		if (id == 3) {
			field.pos = r.pos; field.width = 6; field.vbr = true;
			code = r.vbr(6);
			uint64_t n = r.vbr(6);
			for (uint64_t i = 0; i < n && !r.bad; i++) ops.push_back(r.vbr(6));
			return !r.bad;
		}
		if (id - 4 >= abbrevs.size()) { r.bad = true; return false; }
		const Abbrev& a = abbrevs[id - 4];
		if (a.empty()) { r.bad = true; return false; }
		code = ReadScalar(a[0], &field);
		for (size_t i = 1; i < a.size() && !r.bad; i++) {
			if (a[i].kind == AbbrevOp::Array) {
				if (i + 1 >= a.size()) { r.bad = true; break; }
				uint64_t n = r.vbr(6);
				for (uint64_t k = 0; k < n && !r.bad; k++) ops.push_back(ReadScalar(a[i + 1], nullptr));
				break;
			}
			if (a[i].kind == AbbrevOp::Blob) {
				uint64_t n = r.vbr(6);
				r.align32();
				r.pos += n * 8;
				r.align32();
				break;
			}
			ops.push_back(ReadScalar(a[i], nullptr));
		}
		return !r.bad;
	}

	void Record(uint64_t block, uint64_t code, const std::vector<uint64_t>& ops, const CodeField& field)
	{
		if (block == TYPE_BLOCK) {
			if (code == TYPE_NUMENTRY || code == TYPE_STRUCT_NAME) return;
			types.push_back({ code, (code == TYPE_ARRAY || code == TYPE_VECTOR) && ops.size() >= 2 ? ops[1] : ~0ull });
			if (code == TYPE_DOUBLE) {
				if (field.literal) why = "the DOUBLE type record's code is an abbreviation literal";
				else if (field.width < 4) why = "the DOUBLE type record's code field is too narrow";
				doubles.push_back(field);
			}
		}
		else if (block == CONSTANTS_BLOCK) {
			if (code == CST_SETTYPE && !ops.empty()) constType = ops[0];
			else if (code == CST_FLOAT && IsDouble(constType) && !ops.empty() && ops[0] != 0)
				why = "a double constant that isn't 0.0";
			else if (code == CST_DATA && constType < types.size() && IsDouble(types[constType].element))
				for (uint64_t v : ops) if (v) { why = "a double array constant that isn't 0.0"; break; }
		}
		else if (block == FUNCTION_BLOCK && code == INST_CAST && ops.size() >= 3) {
			uint64_t dest = ops[ops.size() - 2], opcode = ops[ops.size() - 1];
			if ((opcode == CAST_FPEXT || opcode == CAST_BITCAST) && IsDouble(dest)) why = "an fpext or bitcast to double";
			else if (opcode == CAST_FPTRUNC && IsFloat(dest)) why = "an fptrunc from double to float";
		}
	}

	bool Block(uint64_t block, unsigned width)
	{
		std::vector<Abbrev> abbrevs = block < blockInfo.size() ? blockInfo[block] : std::vector<Abbrev>();
		uint64_t infoTarget = 0;
		std::vector<uint64_t> ops;
		while (!r.bad && why.empty()) {
			uint64_t id = r.read(width);
			if (id == 0) { r.align32(); return !r.bad; }                         // END_BLOCK
			if (id == 1) {                                                       // ENTER_SUBBLOCK
				uint64_t sub = r.vbr(8);
				unsigned subWidth = (unsigned)r.vbr(4);
				r.align32();
				r.read(32);                                                      // length in words
				if (!Block(sub, subWidth)) return false;
				continue;
			}
			if (id == 2) {                                                       // DEFINE_ABBREV
				Abbrev a;
				if (!ReadAbbrev(a)) return false;
				if (block == BLOCKINFO_BLOCK) { if (infoTarget < blockInfo.size()) blockInfo[infoTarget].push_back(a); }
				else abbrevs.push_back(a);
				continue;
			}
			uint64_t code;
			CodeField field;
			if (!ReadRecord(id, abbrevs, code, ops, field)) return false;
			if (block == BLOCKINFO_BLOCK) { if (code == 1 && !ops.empty()) infoTarget = ops[0]; }   // SETBID
			else Record(block, code, ops, field);
		}
		return !r.bad && why.empty();
	}
};

void WriteBits(uint8_t* data, size_t pos, unsigned n, uint64_t v)
{
	for (unsigned i = 0; i < n; i++, pos++) {
		uint8_t bit = (uint8_t)((v >> i) & 1);
		data[pos >> 3] = (uint8_t)((data[pos >> 3] & ~(1u << (pos & 7))) | (bit << (pos & 7)));
	}
}

}  // namespace

bool EFP_DemoteDxilDoubles(const void* code, size_t size, std::vector<uint8_t>& out, std::string& why)
{
	const uint8_t* p = (const uint8_t*)code;
	if (!p || size < 32 || memcmp(p, "DXBC", 4)) { why = "not a DXBC/DXIL container"; return false; }
	uint32_t count;
	memcpy(&count, p + 28, 4);
	if (32 + (size_t)count * 4 > size) { why = "bad container"; return false; }
	out.assign(p, p + size);
	bool found = false;
	for (uint32_t i = 0; i < count; i++) {
		uint32_t offset, chunkSize;
		memcpy(&offset, p + 32 + i * 4, 4);
		if ((size_t)offset + 8 > size) { why = "bad chunk"; return false; }
		memcpy(&chunkSize, p + offset + 4, 4);
		if ((size_t)offset + 8 + chunkSize > size) { why = "bad chunk size"; return false; }
		if (!memcmp(p + offset, "SFI0", 4) && chunkSize >= 8) {
			uint64_t flags;
			memcpy(&flags, &out[offset + 8], 8);
			flags &= ~(uint64_t)(0x1 | 0x20);                    // no longer needs doubles
			memcpy(&out[offset + 8], &flags, 8);
			continue;
		}
		if (memcmp(p + offset, "DXIL", 4)) continue;
		// DxilProgramHeader: version, size in dwords, then DxilBitcodeHeader: 'DXIL', version,
		// bitcode offset (from the bitcode header) and size
		const uint8_t* chunk = p + offset + 8;
		uint32_t bcOffset, bcSize;
		if (chunkSize < 24 || memcmp(chunk + 8, "DXIL", 4)) { why = "bad DXIL header"; return false; }
		memcpy(&bcOffset, chunk + 16, 4);
		memcpy(&bcSize, chunk + 20, 4);
		size_t bc = offset + 8 + 8 + bcOffset;
		if (bc + bcSize > size || bcSize < 8 || p[bc] != 'B' || p[bc + 1] != 'C') { why = "no LLVM bitcode"; return false; }

		Walker w{ Reader{ p + bc + 4, (size_t)(bcSize - 4) * 8 } };
		while (!w.r.bad && w.why.empty() && w.r.pos + 2 <= w.r.bits) {
			uint64_t id = w.r.read(2);
			if (id != 1) break;                                  // top level: blocks only
			uint64_t block = w.r.vbr(8);
			unsigned width = (unsigned)w.r.vbr(4);
			w.r.align32();
			w.r.read(32);
			if (!w.Block(block, width)) break;
		}
		if (!w.why.empty()) { why = w.why; return false; }
		if (w.r.bad) { why = "couldn't read the bitcode"; return false; }
		if (w.doubles.empty()) { why = "no DOUBLE type in the module"; return false; }
		for (const CodeField& f : w.doubles)
			WriteBits(&out[bc + 4], f.pos, f.width, TYPE_FLOAT);   // one chunk, continuation bit clear
		found = true;
	}
	if (!found && why.empty()) why = "no DXIL chunk";
	return found;
}
