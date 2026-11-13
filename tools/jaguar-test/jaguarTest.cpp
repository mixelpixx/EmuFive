// Copyright (C) 2026 Chokri Hammedi (blue0x1) <https://github.com/blue0x1>
// Checks KytyPS5's Jaguar replacements (src/loader/jaguarCompat.cpp) on a CPU that has AVX2,
// FMA and BMI2: every AVX2/FMA/BMI2 instruction form found in the given code files runs
// natively and as its replacement from the same random state, and all registers, flags and
// memory must match. It also checks that no replacement uses AVX2, FMA or BMI2 itself.
//   jaguarTest [-n instances-per-form] [-r runs] code.bin...
// With KYTY_JAGUAR_FMA=fast, FMA destinations are checked against a multiply then an add (each
// rounded) instead of the fused native result.

#include "loader/jaguarCompat.h"

#include <Zydis/Zydis.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <map>
#include <random>
#include <string>
#include <vector>
#include <xbyak/xbyak.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <asm/prctl.h>
#include <csetjmp>
#include <csignal>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

using namespace Xbyak::util;

struct State {
	uint64_t gpr[16];
	uint64_t flags;
	uint64_t host_rsp;
	uint64_t pad[6];
	uint8_t  ymm[16][32];
};

constexpr size_t AreaSize  = 64 * 1024;
constexpr size_t CodeSize  = 1024 * 1024;
constexpr uint64_t Status  = 0x8d5; // CF PF AF ZF SF OF

// Everything is mapped below 2 GB: the runners address the state with 32-bit displacements.
static uint8_t* Map(size_t size) {
#ifdef _WIN32
	static uintptr_t next = 0x10000000;
	for (; next < 0x7ff00000; next += 0x100000) {
		void* p = VirtualAlloc(reinterpret_cast<void*>(next), size, MEM_RESERVE | MEM_COMMIT,
		                       PAGE_EXECUTE_READWRITE);
		if (p != nullptr) {
			next += (size + 0xfffff) & ~uintptr_t {0xfffff};
			return static_cast<uint8_t*>(p);
		}
	}
	fprintf(stderr, "VirtualAlloc: no room below 2 GB\n");
	exit(1);
#else
	void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
	               MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
	if (p == MAP_FAILED) {
		perror("mmap");
		exit(1);
	}
	return static_cast<uint8_t*>(p);
#endif
}

static State*   g_state;
static uint8_t* g_data;
static uint8_t* g_stack;
static uint8_t* g_native_code;
static uint8_t* g_repl_code;

static volatile int g_signal;
#ifdef _WIN32
// A fault in the code under test resumes at g_recover with the runner's own stack, which
// restores the host registers and returns as the runner would.
static uint8_t* g_recover;
static LONG CALLBACK OnException(PEXCEPTION_POINTERS info) {
	const auto rip = static_cast<uintptr_t>(info->ContextRecord->Rip);
	const auto in  = [rip](const uint8_t* code) {
		return rip >= reinterpret_cast<uintptr_t>(code) &&
		       rip < reinterpret_cast<uintptr_t>(code) + CodeSize;
	};
	if (!in(g_native_code) && !in(g_repl_code)) {
		return EXCEPTION_CONTINUE_SEARCH;
	}
	g_signal                   = static_cast<int>(info->ExceptionRecord->ExceptionCode);
	info->ContextRecord->Rsp   = g_state->host_rsp;
	info->ContextRecord->Rip   = reinterpret_cast<DWORD64>(g_recover);
	return EXCEPTION_CONTINUE_EXECUTION;
}
// Win64 also keeps RSI, RDI and XMM6-XMM15 for the caller.
static const Xbyak::Reg64 g_host_saved[] = {rbx, rbp, rsi, rdi, r12, r13, r14, r15};
constexpr int XmmSaveSize = 10 * 16;
#else
static sigjmp_buf g_jump;
static void OnSignal(int sig, siginfo_t*, void*) {
	g_signal = sig;
	siglongjmp(g_jump, 1);
}
static const Xbyak::Reg64 g_host_saved[] = {rbx, rbp, r12, r13, r14, r15};
#endif

static Xbyak::Address At(const void* p) {
	return Xbyak::util::ptr[Xbyak::RegExp(reinterpret_cast<size_t>(p))];
}

static void SaveHost(Xbyak::CodeGenerator& g) {
	for (auto r: g_host_saved) {
		g.push(r);
	}
#ifdef _WIN32
	g.sub(rsp, XmmSaveSize);
	for (int i = 0; i < 10; ++i) {
		g.movdqu(ptr[rsp + i * 16], Xbyak::Xmm(6 + i));
	}
#endif
}

// Expects RSP as SaveHost left it.
static void RestoreHost(Xbyak::CodeGenerator& g) {
#ifdef _WIN32
	for (int i = 0; i < 10; ++i) {
		g.movdqu(Xbyak::Xmm(6 + i), ptr[rsp + i * 16]);
	}
	g.add(rsp, XmmSaveSize);
#endif
	for (int i = static_cast<int>(std::size(g_host_saved)) - 1; i >= 0; --i) {
		g.pop(g_host_saved[i]);
	}
	g.vzeroupper();
	g.ret();
}

// Loads the state, runs `body`, saves the state.
template <typename Body>
static void BuildRunner(Xbyak::CodeGenerator& g, Body body) {
	const Xbyak::Reg64 regs[16] = {rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi,
	                               r8,  r9,  r10, r11, r12, r13, r14, r15};
	g.reset();
	SaveHost(g);
	g.mov(At(&g_state->host_rsp), rsp);
	for (int i = 0; i < 16; ++i) {
		g.vmovdqu(Xbyak::Ymm(i), At(g_state->ymm[i]));
	}
	g.push(Xbyak::util::qword[Xbyak::RegExp(reinterpret_cast<size_t>(&g_state->flags))]);
	g.popfq();
	g.mov(rsp, At(&g_state->gpr[4]));
	for (int i = 0; i < 16; ++i) {
		if (i != 4) {
			g.mov(regs[i], At(&g_state->gpr[i]));
		}
	}
	body();
	for (int i = 0; i < 16; ++i) {
		g.mov(At(&g_state->gpr[i]), regs[i]);
	}
	g.mov(rsp, At(&g_state->host_rsp));
	g.pushfq();
	g.pop(rax);
	g.mov(At(&g_state->flags), rax);
	for (int i = 0; i < 16; ++i) {
		g.vmovdqu(At(g_state->ymm[i]), Xbyak::Ymm(i));
	}
	RestoreHost(g);
}

// KYTY_JAGUAR_FMA=fast: what an FMA form must leave in its destination, from the inputs of
// operands 0, 1 and 2: per element, the product rounded, then the sum or difference rounded, in
// the element's precision. Scalar forms keep the destination's other elements; the upper half is
// zero unless the form is 256-bit.
static bool FastFmaExpected(const ZydisDecodedInstruction& in, const uint8_t (*op)[32], bool wide,
                            uint8_t* out, int* element_size) {
	const std::string m = ZydisMnemonicGetString(in.mnemonic); // e.g. vfnmadd231ps
	if (m.size() < 9 || m.compare(0, 2, "vf") != 0) {
		return false;
	}
	const std::string type   = m.substr(m.size() - 2);
	const int         order  = std::stoi(m.substr(m.size() - 5, 3));
	const std::string name   = m.substr(2, m.size() - 7);
	const bool        single = type[1] == 's';
	const bool        scalar = type[0] == 's';
	const bool        negate = name == "nmadd" || name == "nmsub";
	const bool        minus  = name == "msub" || name == "nmsub";
	const int         size   = single ? 4 : 8;
	const int         count  = scalar ? 1 : (wide ? 32 : 16) / size;
	int               a = 1, b = 2, c = 0; // 231
	if (order == 132) {
		a = 0, b = 2, c = 1;
	} else if (order == 213) {
		a = 1, b = 0, c = 2;
	}
	memset(out, 0, 32);
	if (scalar) {
		memcpy(out, op[0], 16);
	}
	for (int i = 0; i < count; ++i) {
		// VFMADDSUB subtracts in even elements, VFMSUBADD in odd ones.
		const bool subtract = name == "maddsub" ? i % 2 == 0 : name == "msubadd" ? i % 2 == 1 : minus;
		if (single) {
			float x, y, z;
			memcpy(&x, op[a] + i * 4, 4);
			memcpy(&y, op[b] + i * 4, 4);
			memcpy(&z, op[c] + i * 4, 4);
			volatile float product = x * y;
			const float    p       = negate ? -product : product;
			volatile float r       = subtract ? p - z : p + z;
			const float    result  = r;
			memcpy(out + i * 4, &result, 4);
		} else {
			double x, y, z;
			memcpy(&x, op[a] + i * 8, 8);
			memcpy(&y, op[b] + i * 8, 8);
			memcpy(&z, op[c] + i * 8, 8);
			volatile double product = x * y;
			const double    p       = negate ? -product : product;
			volatile double r       = subtract ? p - z : p + z;
			const double    result  = r;
			memcpy(out + i * 8, &result, 8);
		}
	}
	*element_size = size;
	return true;
}

// Bit for bit, except that any NaN stands for an expected NaN.
static bool SameOrBothNaN(const uint8_t* expected, const uint8_t* actual, int size) {
	for (int i = 0; i < 32; i += size) {
		bool nan_e, nan_a;
		if (size == 4) {
			float e, a;
			memcpy(&e, expected + i, 4);
			memcpy(&a, actual + i, 4);
			nan_e = std::isnan(e), nan_a = std::isnan(a);
		} else {
			double e, a;
			memcpy(&e, expected + i, 8);
			memcpy(&a, actual + i, 8);
			nan_e = std::isnan(e), nan_a = std::isnan(a);
		}
		if (nan_e ? !nan_a : memcmp(expected + i, actual + i, size) != 0) {
			return false;
		}
	}
	return true;
}

struct Instance {
	std::vector<uint8_t> bytes;
	std::string          form;
};

// The base of a form's FS or GS segment (0 for a form without one), so its memory operand can be
// aimed at the test's data. False where this thread's base is unknown.
static bool SegmentBase(const std::string& form, uint64_t* base) {
	*base = 0;
	const bool fs = form.find("fs:") != std::string::npos;
	const bool gs = form.find("gs:") != std::string::npos;
	if (!fs && !gs) {
		return true;
	}
#ifdef _WIN32
	// GS is the TEB; 64-bit Windows leaves FS flat (base 0).
	if (gs) {
		*base = reinterpret_cast<uint64_t>(NtCurrentTeb());
	}
	return true;
#else
	return syscall(SYS_arch_prctl, fs ? ARCH_GET_FS : ARCH_GET_GS, base) == 0;
#endif
}

static std::string FormOf(const ZydisDecodedInstruction& in, const ZydisDecodedOperand* ops) {
	std::string s = ZydisMnemonicGetString(in.mnemonic);
	for (int i = 0; i < in.operand_count_visible; ++i) {
		s += i ? "," : " ";
		const auto& o = ops[i];
		if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) {
			const auto c = ZydisRegisterGetClass(o.reg.value);
			s += c == ZYDIS_REGCLASS_YMM ? "y" : c == ZYDIS_REGCLASS_XMM ? "x" : "r" + std::to_string(o.size);
		} else if (o.type == ZYDIS_OPERAND_TYPE_MEMORY) {
			if (o.mem.segment == ZYDIS_REGISTER_FS || o.mem.segment == ZYDIS_REGISTER_GS) {
				s += o.mem.segment == ZYDIS_REGISTER_FS ? "fs:" : "gs:";
			}
			s += "m" + std::to_string(o.size);
		} else {
			s += "i";
		}
	}
	return s;
}

struct Result {
	long runs = 0, failures = 0, inexact = 0, faults = 0, unsupported = 0, bad_isa = 0;
	long in_place = 0; // Instances with a same-length rewrite (also tested).
	std::string first_failure;
};

int main(int argc, char** argv) {
	int per_form = 20;
	const char* only = std::getenv("ONLY"); // Test only forms containing this text, verbosely.
	int runs     = 40;
	std::vector<std::string> files;
	for (int i = 1; i < argc; ++i) {
		if (!strcmp(argv[i], "-n") && i + 1 < argc) {
			per_form = atoi(argv[++i]);
		} else if (!strcmp(argv[i], "-r") && i + 1 < argc) {
			runs = atoi(argv[++i]);
		} else {
			files.push_back(argv[i]);
		}
	}

	ZydisDecoder decoder;
	ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

	// Collect instances of each form (reservoir sampling for variety).
	std::mt19937_64                               rng(12345);
	std::map<std::string, std::vector<Instance>>  forms;
	std::map<std::string, long>                   seen;
	for (const auto& file: files) {
		FILE* f = fopen(file.c_str(), "rb");
		if (!f) {
			perror(file.c_str());
			return 1;
		}
		std::vector<uint8_t> code;
		for (int c; (c = fgetc(f)) != EOF;) {
			code.push_back(static_cast<uint8_t>(c));
		}
		fclose(f);
		ZydisDecodedInstruction in;
		ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT];
		for (size_t off = 0; off < code.size();) {
			if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code.data() + off, code.size() - off,
			                                         &in, ops))) {
				++off;
				continue;
			}
			if (Loader::JaguarCompat::IsMissing(in)) {
				Instance instance {{code.begin() + off, code.begin() + off + in.length},
				                   FormOf(in, ops)};
				auto& list = forms[instance.form];
				long  n    = ++seen[instance.form];
				if (static_cast<int>(list.size()) < per_form) {
					list.push_back(instance);
				} else if (long slot = static_cast<long>(rng() % n); slot < per_form) {
					list[slot] = instance;
				}
			}
			off += in.length;
		}
	}

	g_state       = reinterpret_cast<State*>(Map(4096));
	g_data        = Map(AreaSize);
	g_stack       = Map(AreaSize);
	g_native_code = Map(CodeSize);
	g_repl_code   = Map(CodeSize);
	Xbyak::CodeGenerator native(CodeSize, g_native_code);
	Xbyak::CodeGenerator repl(CodeSize, g_repl_code);

#ifdef _WIN32
	g_recover = Map(4096);
	{
		Xbyak::CodeGenerator recover(4096, g_recover);
		RestoreHost(recover);
	}
	AddVectoredExceptionHandler(1, OnException);
#else
	struct sigaction sa {};
	sa.sa_sigaction = OnSignal;
	sa.sa_flags     = SA_SIGINFO | SA_NODEFER;
	sigaction(SIGSEGV, &sa, nullptr);
	sigaction(SIGILL, &sa, nullptr);
	sigaction(SIGBUS, &sa, nullptr);
	sigaction(SIGFPE, &sa, nullptr);
#endif

	std::vector<uint8_t> data0(AreaSize), stack0(AreaSize), data1(AreaSize), stack1(AreaSize);
	State                state0 {}, state1 {};
	std::map<std::string, Result> results;
	long total_instances = 0;
	long segment_skipped = 0;

	for (auto& [form, instances]: forms) {
		if (only != nullptr && form.find(only) == std::string::npos) {
			continue;
		}
		// FS/GS forms: the operand's offset is aimed at the data minus the segment's base.
		uint64_t segment_base = 0;
		if (!SegmentBase(form, &segment_base)) {
			++segment_skipped;
			continue;
		}
		auto& result = results[form];
		for (const auto& instance: instances) {
			ZydisDecodedInstruction in;
			ZydisDecodedOperand     ops[ZYDIS_MAX_OPERAND_COUNT];
			ZydisDecoderDecodeFull(&decoder, instance.bytes.data(), instance.bytes.size(), &in, ops);
			const bool bzhi = in.mnemonic == ZYDIS_MNEMONIC_BZHI;
			const bool fma  = in.meta.isa_ext == ZYDIS_ISA_EXT_FMA;
			static const bool fast_mode = [] {
				const char* mode = std::getenv("KYTY_JAGUAR_FMA");
				return mode != nullptr && std::string(mode) == "fast";
			}();
			const bool fast_fma = fma && fast_mode;
			// RDRAND/RDSEED: the value is random, so the destination bits it writes are not
			// compared (the rest of the register, the flags and everything else are).
			int      random_reg  = -1;
			uint64_t random_bits = 0;
			if (in.mnemonic == ZYDIS_MNEMONIC_RDRAND || in.mnemonic == ZYDIS_MNEMONIC_RDSEED) {
				const auto reg64 =
				    ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, ops[0].reg.value);
				random_reg  = ZydisRegisterGetId(reg64);
				random_bits = ops[0].size == 64 ? ~uint64_t {0} : (uint64_t {1} << ops[0].size) - 1;
			}

			// Memory operand: where it should point.
			int mem = -1;
			for (int i = 0; i < in.operand_count_visible; ++i) {
				if (ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY) {
					mem = i;
				}
			}
			if (mem >= 0 && ops[mem].mem.base == ZYDIS_REGISTER_NONE) {
				continue; // Absolute address.
			}
			if (segment_base != 0 && mem >= 0 && (ops[mem].mem.base == ZYDIS_REGISTER_RIP ||
			                          ops[mem].mem.base == ZYDIS_REGISTER_RSP ||
			                          ops[mem].mem.base == ops[mem].mem.index)) {
				continue; // Not aimed through a segment's base here.
			}
			++total_instances;
			const uint64_t data_target  = reinterpret_cast<uint64_t>(g_data) + AreaSize / 2;
			const uint64_t stack_middle = reinterpret_cast<uint64_t>(g_stack) + AreaSize / 2;

			// Native: the instruction re-encoded in place (RIP-relative operands aimed at data).
			uint64_t native_address = 0;
			bool     encode_ok      = true;
			BuildRunner(native, [&] {
				native_address = reinterpret_cast<uint64_t>(native.getCurr());
				ZydisEncoderRequest request;
				ZydisEncoderDecodedInstructionToEncoderRequest(&in, ops, in.operand_count_visible,
				                                               &request);
				if (mem >= 0 && ops[mem].mem.base == ZYDIS_REGISTER_RIP) {
					request.operands[mem].mem.displacement = static_cast<ZyanI64>(data_target);
				}
				uint8_t   code[16];
				ZyanUSize size = sizeof(code);
				if (!ZYAN_SUCCESS(ZydisEncoderEncodeInstructionAbsolute(&request, code, &size,
				                                                         native_address))) {
					encode_ok = false;
					return;
				}
				native.db(code, size);
			});
			if (!encode_ok) {
				result.unsupported++;
				continue;
			}
			ZydisDecodedInstruction placed;
			ZydisDecodedOperand     placed_ops[ZYDIS_MAX_OPERAND_COUNT];
			ZydisDecoderDecodeFull(&decoder, reinterpret_cast<void*>(native_address), 16, &placed,
			                       placed_ops);

			// The replacement with RFLAGS live, then dead, then the same-length rewrite (when there
			// is one) as the load-time patcher writes it over the original.
			uint8_t    rewritten[16] = {};
			const bool has_rewrite   = Loader::JaguarCompat::RewriteInPlace(
			    placed, placed_ops, reinterpret_cast<const uint8_t*>(native_address), rewritten);
			if (has_rewrite) {
				result.in_place++;
			}
			for (int variant = 0; variant < (has_rewrite ? 3 : 2); ++variant) {
				const bool flags_live = variant != 1;
				const bool in_place   = variant == 2;
				bool       generated  = true;
				uint64_t   repl_begin = 0, repl_end = 0;
				Xbyak::ClearError();
				BuildRunner(repl, [&] {
					repl_begin = reinterpret_cast<uint64_t>(repl.getCurr());
					if (in_place) {
						// Same bytes; a RIP-relative displacement still aims at the data.
						uint8_t bytes[16];
						memcpy(bytes, rewritten, placed.length);
						if (mem >= 0 && placed_ops[mem].mem.base == ZYDIS_REGISTER_RIP) {
							int32_t disp;
							memcpy(&disp, bytes + placed.raw.disp.offset, 4);
							disp += static_cast<int32_t>(native_address - repl_begin);
							memcpy(bytes + placed.raw.disp.offset, &disp, 4);
						}
						repl.db(bytes, placed.length);
					} else {
						generated = Loader::JaguarCompat::Generate(placed, placed_ops,
						                                           native_address, repl, flags_live);
					}
					repl_end = reinterpret_cast<uint64_t>(repl.getCurr());
				});
				if (!generated || Xbyak::GetError() != 0) {
					result.unsupported++;
					if (result.first_failure.empty()) {
						result.first_failure = "not supported";
					}
					break;
				}
				// The replacement must only use what the Jaguar has.
				for (uint64_t p = repl_begin; p < repl_end;) {
					ZydisDecodedInstruction r;
					if (!ZYAN_SUCCESS(ZydisDecoderDecodeInstruction(&decoder, nullptr,
					                                                reinterpret_cast<void*>(p),
					                                                repl_end - p, &r))) {
						++p;
						continue;
					}
					if (Loader::JaguarCompat::IsMissing(r) ||
					    r.meta.isa_ext == ZYDIS_ISA_EXT_AVX512EVEX ||
					    r.encoding == ZYDIS_INSTRUCTION_ENCODING_EVEX) {
						result.bad_isa++;
						if (result.first_failure.empty()) {
							result.first_failure = std::string("replacement uses ") +
							                       ZydisMnemonicGetString(r.mnemonic);
						}
					}
					// Skip data that the replacement jumps over.
					if (r.mnemonic == ZYDIS_MNEMONIC_JMP && r.raw.imm[0].is_relative) {
						const uint64_t target = p + r.length + r.raw.imm[0].value.s;
						if (target > p && target <= repl_end) {
							p = target;
							continue;
						}
					}
					p += r.length;
				}

				for (int run = 0; run < runs; ++run) {
					// Random state.
					State s {};
					for (auto& v: s.gpr) {
						switch (rng() % 3) {
							case 0: v = rng() % 80; break;
							case 1: v = rng() % 0x100000000ULL; break;
							default: v = rng(); break;
						}
					}
					s.flags = 0x202 | (rng() & Status);
					for (auto& reg: s.ymm) {
						for (int lane = 0; lane < 8; ++lane) {
							uint32_t v = 0;
							float    fv;
							switch (rng() % 6) {
								case 0: v = rng() % 70; break;
								case 1: v = static_cast<uint32_t>(rng()); break;
								case 2:
									fv = std::uniform_real_distribution<float>(-100, 100)(rng);
									memcpy(&v, &fv, 4);
									break;
								case 3: {
									static const uint32_t special[] = {
									    0, 0x80000000, 0x3f800000, 0xbf800000, 0x7f800000,
									    0xff800000, 0x7fc00000, 0x00000001, 0x007fffff, 0x7f7fffff};
									v = special[rng() % 10];
									break;
								}
								case 4: {
									double d = std::uniform_real_distribution<double>(-100, 100)(rng);
									memcpy(reg + lane * 4, &d, 8);
									lane++;
									continue;
								}
								default: v = static_cast<uint32_t>(rng() % 0x100); break;
							}
							memcpy(reg + lane * 4, &v, 4);
						}
					}
					s.gpr[4] = stack_middle;
					if (mem >= 0 && placed_ops[mem].mem.base != ZYDIS_REGISTER_RIP) {
						const auto& m     = placed_ops[mem].mem;
						const int   base  = ZydisRegisterGetId(
                            ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, m.base));
						const auto  index_class = ZydisRegisterGetClass(m.index);
						const bool  vsib = index_class == ZYDIS_REGCLASS_XMM || index_class == ZYDIS_REGCLASS_YMM;
						if (vsib) {
							// Gathers: small signed indices, so every element is near data_target.
							const bool qwords = in.mnemonic == ZYDIS_MNEMONIC_VGATHERQPS ||
							                    in.mnemonic == ZYDIS_MNEMONIC_VGATHERQPD ||
							                    in.mnemonic == ZYDIS_MNEMONIC_VPGATHERQD ||
							                    in.mnemonic == ZYDIS_MNEMONIC_VPGATHERQQ;
							// Half the runs with every mask element set, as most gathers have it.
							if (rng() % 2 == 0) {
								memset(s.ymm[ZydisRegisterGetId(placed_ops[2].reg.value)], 0xff, 32);
							}
							uint8_t* lanes = s.ymm[ZydisRegisterGetId(m.index)];
							for (int lane = 0; lane < (qwords ? 4 : 8); ++lane) {
								const int64_t v = static_cast<int64_t>(rng() % 64) - 32;
								if (qwords) {
									memcpy(lanes + lane * 8, &v, 8);
								} else {
									const int32_t v32 = static_cast<int32_t>(v);
									memcpy(lanes + lane * 4, &v32, 4);
								}
							}
						}
						const int   index = m.index == ZYDIS_REGISTER_NONE || vsib
						                        ? -1
						                        : ZydisRegisterGetId(ZydisRegisterGetLargestEnclosing(
						                              ZYDIS_MACHINE_MODE_LONG_64, m.index));
						const int64_t scale = index < 0 ? 0 : m.scale;
						const int64_t disp  = m.disp.value;
						// The offset that, with the segment's base, addresses data_target.
						const uint64_t target = data_target - segment_base;
						if (base == 4) {
							if (index >= 0) {
								s.gpr[index] = (64 / scale) * (rng() % 4);
							}
						} else if (index == base) {
							s.gpr[base] = (target - disp) / (1 + scale);
						} else {
							if (index >= 0) {
								s.gpr[index] = (64 / scale) * (rng() % 4);
							}
							s.gpr[base] = target - disp - (index >= 0 ? s.gpr[index] * scale : 0);
						}
					}
					for (auto& b: data0) {
						b = static_cast<uint8_t>(rng());
					}
					for (auto& b: stack0) {
						b = static_cast<uint8_t>(rng());
					}

					auto run_one = [&](uint8_t* code, State* out, std::vector<uint8_t>& data_out,
					                   std::vector<uint8_t>& stack_out) -> bool {
						*g_state = s;
						memcpy(g_data, data0.data(), AreaSize);
						memcpy(g_stack, stack0.data(), AreaSize);
						g_signal = 0;
#ifdef _WIN32
						reinterpret_cast<void (*)()>(code)();
						if (g_signal != 0) {
							return false;
						}
#else
						if (sigsetjmp(g_jump, 1) != 0) {
							return false;
						}
						reinterpret_cast<void (*)()>(code)();
#endif
						*out = *g_state;
						memcpy(data_out.data(), g_data, AreaSize);
						memcpy(stack_out.data(), g_stack, AreaSize);
						return true;
					};
					std::vector<uint8_t> data_n(AreaSize), stack_n(AreaSize);
					const bool native_ok = run_one(g_native_code, &state0, data_n, stack_n);
					const bool repl_ok   = run_one(g_repl_code, &state1, data1, stack1);
					if (!native_ok) {
						result.faults++;
						break;
					}
					// RDRAND/RDSEED may have no value ready (CF = 0), RDSEED above all while other
					// processes draw on it. The replacement always has one, so the run proves nothing.
					if (random_reg >= 0 && (state0.flags & 1) == 0) {
						continue;
					}
					result.runs++;
					std::string why;
					// Fast FMA: the destination against multiply-then-add from this run's inputs.
					int     fma_dst = -1;
					int     fma_size = 4;
					uint8_t fma_expected[32];
					if (fast_fma) {
						uint8_t inputs[3][32] = {};
						for (int i = 0; i < 3; ++i) {
							const auto& o = placed_ops[i];
							if (o.type == ZYDIS_OPERAND_TYPE_REGISTER) {
								memcpy(inputs[i], s.ymm[ZydisRegisterGetId(o.reg.value)], 32);
								continue;
							}
							uint64_t address = data_target;
							if (o.mem.base != ZYDIS_REGISTER_RIP) {
								const auto gpr = [&](ZydisRegister r) {
									return s.gpr[ZydisRegisterGetId(ZydisRegisterGetLargestEnclosing(
									    ZYDIS_MACHINE_MODE_LONG_64, r))];
								};
								address = segment_base + gpr(o.mem.base) + o.mem.disp.value +
								          (o.mem.index == ZYDIS_REGISTER_NONE
								               ? 0
								               : gpr(o.mem.index) * o.mem.scale);
							}
							const uint64_t data  = reinterpret_cast<uint64_t>(g_data);
							const uint64_t stack = reinterpret_cast<uint64_t>(g_stack);
							const auto*    from  = address - data < AreaSize
							                           ? data0.data() + (address - data)
							                           : stack0.data() + (address - stack);
							memcpy(inputs[i], from, o.size / 8);
						}
						fma_dst = ZydisRegisterGetId(placed_ops[0].reg.value);
						FastFmaExpected(in, inputs,
						                ZydisRegisterGetClass(placed_ops[0].reg.value) ==
						                    ZYDIS_REGCLASS_YMM,
						                fma_expected, &fma_size);
					}
					if (!repl_ok) {
						why = "replacement faulted (signal " + std::to_string(g_signal) + ")";
					} else {
						for (int i = 0; i < 16 && why.empty(); ++i) {
							if ((state0.gpr[i] ^ state1.gpr[i]) & ~(i == random_reg ? random_bits : 0)) {
								char buf[128];
								snprintf(buf, sizeof buf, "gpr%d %016llx vs %016llx", i,
								         (unsigned long long)state0.gpr[i],
								         (unsigned long long)state1.gpr[i]);
								why = buf;
							}
						}
						const uint64_t mask = bzhi ? 0x8c1 : (Status | 0x400);
						if (why.empty() && flags_live && ((state0.flags ^ state1.flags) & mask)) {
							char buf[96];
							snprintf(buf, sizeof buf, "flags %llx vs %llx",
							         (unsigned long long)state0.flags, (unsigned long long)state1.flags);
							why = buf;
						}
						if (why.empty() && data_n != data1) {
							why = "data memory differs";
						}
						// Below the 128-byte red zone is free for anyone to use (signal handlers,
						// the replacements' own saves): compare from rsp-128 up.
						constexpr size_t Owned = AreaSize / 2 - 128;
						if (why.empty() && memcmp(stack_n.data() + Owned, stack1.data() + Owned,
						                          AreaSize - Owned) != 0) {
							size_t i = Owned;
							while (stack_n[i] == stack1[i]) {
								++i;
							}
							why = "stack memory differs at rsp" +
							      std::to_string(static_cast<long>(i) - static_cast<long>(AreaSize / 2));
						}
						for (int r = 0; r < 16 && why.empty(); ++r) {
							if (r == fma_dst) {
								if (!SameOrBothNaN(fma_expected, state1.ymm[r], fma_size)) {
									why = "ymm" + std::to_string(r) + " is not multiply-then-add";
								}
								continue;
							}
							if (memcmp(state0.ymm[r], state1.ymm[r], 32) != 0) {
								// FMA in single precision may differ in the last bit in rare cases.
								bool close = fma;
								for (int lane = 0; close && lane < 8; ++lane) {
									int32_t a, b;
									memcpy(&a, state0.ymm[r] + lane * 4, 4);
									memcpy(&b, state1.ymm[r] + lane * 4, 4);
									// 64-bit: `long` is 32 bits on Windows.
									if (std::llabs(int64_t {a} - int64_t {b}) > 1) {
										close = false;
									}
								}
								if (close) {
									result.inexact++;
								} else {
									why = "ymm" + std::to_string(r) + " differs";
									if (only != nullptr) {
										printf("%s: ymm%d\n", form.c_str(), r);
										for (int q = 0; q < 16; ++q) {
											printf(" in ymm%-2d", q);
											for (int lane = 0; lane < 4; ++lane) {
												uint32_t v;
												memcpy(&v, s.ymm[q] + lane * 4, 4);
												printf(" %08x", v);
											}
											printf("\n");
										}
										for (const auto* st: {&state0, &state1}) {
											printf(" out     ");
											for (int lane = 0; lane < 4; ++lane) {
												uint32_t v;
												memcpy(&v, st->ymm[r] + lane * 4, 4);
												printf(" %08x", v);
											}
											printf("\n");
										}
									}
								}
							}
						}
					}
					if (!why.empty()) {
						result.failures++;
						if (result.first_failure.empty()) {
							char hex[64] = {};
							for (size_t i = 0; i < instance.bytes.size(); ++i) {
								snprintf(hex + i * 3, 4, "%02x ", instance.bytes[i]);
							}
							result.first_failure = why +
							                       (in_place     ? " [in place] "
							                        : flags_live ? " [flags live] "
							                                     : " ") +
							                       hex;
						}
					}
				}
			}
		}
	}

	long forms_ok = 0, forms_bad = 0, runs_total = 0, inexact_total = 0, in_place_total = 0;
	for (const auto& [form, r]: results) {
		runs_total += r.runs;
		in_place_total += r.in_place;
		inexact_total += r.inexact;
		const bool bad = r.failures || r.unsupported || r.bad_isa;
		bad ? ++forms_bad : ++forms_ok;
		if (bad || r.faults || r.inexact) {
			printf("%-40s runs=%-6ld fail=%-4ld unsupported=%-3ld bad_isa=%-3ld inexact=%-4ld "
			       "native_faults=%-3ld %s\n",
			       form.c_str(), r.runs, r.failures, r.unsupported, r.bad_isa, r.inexact, r.faults,
			       r.first_failure.c_str());
		}
	}
	printf("\nforms: %zu (%ld pass, %ld fail), instances: %ld (%ld rewritten in place), runs: %ld, "
	       "inexact FMA lanes: %ld\n",
	       results.size(), forms_ok, forms_bad, total_instances, in_place_total, runs_total,
	       inexact_total);
	if (segment_skipped != 0) {
		printf("skipped: %ld forms with a segment whose base this host does not expose\n",
		       segment_skipped);
	}
	return forms_bad == 0 ? 0 : 1;
}
