// Checks the exact FMA replacements of KytyPS5's jaguarCompat.cpp against the CPU's own FMA, bit
// for bit, on inputs chosen to be hard: sums that cancel, sums that land near a rounding midpoint,
// products that overflow or underflow, subnormals, infinities and NaNs with payloads. Every register
// form of every FMA instruction runs, a random MXCSR (rounding mode, FTZ, DAZ) in half the cases.
// jaguarTest checks every form, memory operands too, from random states; this one checks the
// arithmetic, many more times.
//   fmaStress [cases-per-form]

#include "loader/jaguarCompat.h"

#include <Zydis/Zydis.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <random>
#include <xbyak/xbyak.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

using namespace Xbyak::util;
using G = Xbyak::CodeGenerator;

namespace {

std::mt19937_64 rng(12345);

struct Data {
	uint64_t in[3][4]; // ymm0, ymm1, ymm2.
	uint64_t out[4];   // ymm0 afterwards.
	uint32_t mxcsr;    // While the instruction runs.
	uint32_t host;
};

using Runner = void (*)(Data*);

uint8_t* g_code;
size_t   g_used = 4096; // The first page: where Build assembles the instruction alone.

uint8_t* MapCode(size_t size) {
#ifdef _WIN32
	return static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
	void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

// A function that loads ymm0-2 and MXCSR from Data, runs the instruction (or its replacement)
// and stores ymm0.
Runner Build(void (*emit)(G&), bool native) {
	uint8_t bytes[16];
	{
		G g(4096, g_code);
		emit(g);
		memcpy(bytes, g.getCode(), g.getSize());
	}
	ZydisDecoder decoder;
	ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
	ZydisDecodedInstruction in;
	ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT];
	if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes, sizeof(bytes), &in, ops))) {
		return nullptr;
	}
#ifdef _WIN32
	const Xbyak::Reg64 arg = rcx;
#else
	const Xbyak::Reg64 arg = rdi;
#endif
	G g(1 << 16, g_code + g_used);
	g.stmxcsr(ptr[arg + offsetof(Data, host)]);
	g.ldmxcsr(ptr[arg + offsetof(Data, mxcsr)]);
	g.vmovupd(ymm0, ptr[arg]);
	g.vmovupd(ymm1, ptr[arg + 32]);
	g.vmovupd(ymm2, ptr[arg + 64]);
	if (native) {
		emit(g);
	} else if (!Loader::JaguarCompat::Generate(in, ops, 0x10000, g, false)) {
		return nullptr;
	}
	g.vmovupd(ptr[arg + offsetof(Data, out)], ymm0);
	g.ldmxcsr(ptr[arg + offsetof(Data, host)]);
	g.vzeroupper();
	g.ret();
	g_used += (g.getSize() + 63) & ~size_t {63};
	return reinterpret_cast<Runner>(const_cast<uint8_t*>(g.getCode()));
}

template <typename To, typename From>
To Cast(From from) {
	To to;
	memcpy(&to, &from, sizeof(to));
	return to;
}

// A double: significands with few bits, extreme exponents, special values.
double RandomDouble() {
	const double sign   = rng() % 2 != 0 ? -1.0 : 1.0;
	const auto   sparse = [&](int lo, int hi) {
		const int      bits = 1 + static_cast<int>(rng() % 53);
		const uint64_t m    = (rng() & ((uint64_t {1} << bits) - 1)) | 1;
		return sign * std::ldexp(static_cast<double>(m), lo + static_cast<int>(rng() % (hi - lo + 1)) - bits);
	};
	switch (rng() % 16) {
		case 0: return Cast<double>(rng());
		case 1: {
			static const uint64_t special[] = {0,
			                                   0x7ff0000000000000,
			                                   0x7ff8000000000000,
			                                   0x7ff0000000000123,
			                                   0x7ff8000000abcdef,
			                                   1,
			                                   0x000fffffffffffff,
			                                   0x0010000000000000,
			                                   0x7fefffffffffffff,
			                                   0x3ff0000000000000};
			return Cast<double>(special[rng() % 10] | (rng() % 2 != 0 ? uint64_t {1} << 63 : 0));
		}
		case 2: return sparse(-1074, -900);
		case 3: return sparse(900, 1023);
		case 4: return sparse(-1100, -1000);
		default: return sparse(-40, 40);
	}
}

float RandomFloat() {
	const float sign   = rng() % 2 != 0 ? -1.0f : 1.0f;
	const auto  sparse = [&](int lo, int hi) {
		const int      bits = 1 + static_cast<int>(rng() % 24);
		const uint32_t m    = (static_cast<uint32_t>(rng()) & ((1u << bits) - 1)) | 1;
		return sign * std::ldexp(static_cast<float>(m), lo + static_cast<int>(rng() % (hi - lo + 1)) - bits);
	};
	switch (rng() % 16) {
		case 0: return Cast<float>(static_cast<uint32_t>(rng()));
		case 1: {
			static const uint32_t special[] = {0, 0x7f800000, 0x7fc00000, 0x7f800123, 0x7fc0abcd,
			                                   1, 0x007fffff, 0x00800000, 0x7f7fffff, 0x3f800000};
			return Cast<float>(special[rng() % 10] | (rng() % 2 != 0 ? 0x80000000u : 0));
		}
		case 2: return sparse(-149, -110);
		case 3: return sparse(110, 127);
		default: return sparse(-20, 20);
	}
}

// c for a and b: within a few ulps of -a*b or a*b (the sum cancels), or a value with few bits far
// above a*b (the sum lands on or near a midpoint), or anything.
template <typename T, typename Bits>
T RelatedAddend(T a, T b, T (*random)(), int max_exponent) {
	const T p = a * b;
	switch (rng() % 4) {
		case 0: return Cast<T>(static_cast<Bits>(Cast<Bits>(T(-p)) + rng() % 9 - 4));
		case 1: return Cast<T>(static_cast<Bits>(Cast<Bits>(p) + rng() % 9 - 4));
		case 2:
			return std::ldexp(static_cast<T>((rng() % 1024) | 1), static_cast<int>(rng() % max_exponent)) *
			       (rng() % 2 != 0 ? T(-1) : T(1));
		default: return random();
	}
}

struct Form {
	const char* name;
	void (*emit)(G&);
};

#define PACKED(op) {#op " xmm", [](G& g) { g.op(xmm0, xmm1, xmm2); }}, {#op " ymm", [](G& g) { g.op(ymm0, ymm1, ymm2); }}
#define SCALAR(op) {#op, [](G& g) { g.op(xmm0, xmm1, xmm2); }}
#define ORDERS(m) m(132), m(213), m(231)
#define ALL_PACKED(n)                                                                                   \
	PACKED(vfmadd##n##ps), PACKED(vfmadd##n##pd), PACKED(vfmsub##n##ps), PACKED(vfmsub##n##pd),        \
	    PACKED(vfnmadd##n##ps), PACKED(vfnmadd##n##pd), PACKED(vfnmsub##n##ps), PACKED(vfnmsub##n##pd), \
	    PACKED(vfmaddsub##n##ps), PACKED(vfmaddsub##n##pd), PACKED(vfmsubadd##n##ps), PACKED(vfmsubadd##n##pd)
#define ALL_SCALAR(n)                                                                                   \
	SCALAR(vfmadd##n##ss), SCALAR(vfmadd##n##sd), SCALAR(vfmsub##n##ss), SCALAR(vfmsub##n##sd),        \
	    SCALAR(vfnmadd##n##ss), SCALAR(vfnmadd##n##sd), SCALAR(vfnmsub##n##ss), SCALAR(vfnmsub##n##sd)

const Form g_forms[] = {ORDERS(ALL_PACKED), ORDERS(ALL_SCALAR)};

} // namespace

int main(int argc, char** argv) {
	const long cases = argc > 1 ? atol(argv[1]) : 200000;
	g_code           = MapCode(1 << 24);
	if (g_code == nullptr) {
		fprintf(stderr, "no executable memory\n");
		return 2;
	}
	long failed_forms = 0;
	for (const Form& form: g_forms) {
		const Runner native   = Build(form.emit, true);
		const Runner replaced = Build(form.emit, false);
		if (native == nullptr || replaced == nullptr) {
			printf("%-20s not supported\n", form.name);
			++failed_forms;
			continue;
		}
		const char* digits = form.name + strcspn(form.name, "0123456789");
		const int   order  = atoi(digits);
		const bool  single = digits[4] == 's'; // ps or ss.
		// Which register is a, b and c (a*b + c).
		const int a = order == 132 ? 0 : 1;
		const int b = order == 213 ? 0 : 2;
		const int c = order == 231 ? 0 : order == 213 ? 2 : 1;

		long differ = 0;
		for (long i = 0; i < cases; ++i) {
			Data x {};
			for (auto& reg: x.in) {
				for (auto& lane: reg) {
					lane = rng(); // What a scalar form keeps.
				}
			}
			const bool related = rng() % 2 != 0;
			for (int lane = 0; lane < 4; ++lane) {
				if (single) {
					for (int half = 0; half < 2; ++half) {
						const float fa = RandomFloat(), fb = RandomFloat();
						const float fc = related ? RelatedAddend<float, uint32_t>(fa, fb, RandomFloat, 30)
						                         : RandomFloat();
						const auto put = [&](int reg, float v) {
							uint64_t& q = x.in[reg][lane];
							q = (q & ~(uint64_t {0xffffffff} << (32 * half))) |
							    static_cast<uint64_t>(Cast<uint32_t>(v)) << (32 * half);
						};
						put(a, fa);
						put(b, fb);
						put(c, fc);
					}
				} else {
					const double da = RandomDouble(), db = RandomDouble();
					const double dc = related ? RelatedAddend<double, uint64_t>(da, db, RandomDouble, 60)
					                          : RandomDouble();
					x.in[a][lane] = Cast<uint64_t>(da);
					x.in[b][lane] = Cast<uint64_t>(db);
					x.in[c][lane] = Cast<uint64_t>(dc);
				}
			}
			x.mxcsr = 0x1f80;
			if (rng() % 2 != 0) {
				x.mxcsr |= static_cast<uint32_t>(rng() % 4) << 13;
				x.mxcsr |= rng() % 2 != 0 ? 0x8000 : 0;
				x.mxcsr |= rng() % 2 != 0 ? 0x40 : 0;
			}
			Data y = x;
			native(&x);
			replaced(&y);
			if (memcmp(x.out, y.out, sizeof(x.out)) == 0) {
				continue;
			}
			if (differ++ < 3) {
				printf("  %s, mxcsr %04x:\n", form.name, x.mxcsr);
				for (int lane = 0; lane < 4; ++lane) {
					printf("    a %016llx b %016llx c %016llx  native %016llx  replaced %016llx\n",
					       static_cast<unsigned long long>(x.in[a][lane]),
					       static_cast<unsigned long long>(x.in[b][lane]),
					       static_cast<unsigned long long>(x.in[c][lane]),
					       static_cast<unsigned long long>(x.out[lane]),
					       static_cast<unsigned long long>(y.out[lane]));
				}
			}
		}
		if (differ != 0) {
			++failed_forms;
		}
		printf("%-20s %ld of %ld cases differ\n", form.name, differ, cases);
	}
	printf("forms: %zu (%ld fail)\n", std::size(g_forms), failed_forms);
	return failed_forms != 0;
}
