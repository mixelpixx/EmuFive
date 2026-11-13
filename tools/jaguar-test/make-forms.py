#!/usr/bin/env python3
# Writes an assembly file with every AVX2, FMA and BMI2 instruction form (and the other
# instructions the PS4's CPU lacks), each several times with random registers, addressing and
# immediates, as input for jaguarTest when no game code is at hand. Assemble it and take the code
# bytes:
#   python3 make-forms.py > forms.s
#   clang --target=x86_64-linux-gnu -c forms.s -o forms.o
#   llvm-objcopy -O binary -j .text forms.o forms.bin
#   ./jaguarTest forms.bin
import random
import sys

COUNT = int(sys.argv[1]) if len(sys.argv) > 1 else 12
rng = random.Random(1)

GPR64 = ["rax", "rcx", "rdx", "rbx", "rbp", "rsi", "rdi", "r8", "r9", "r10", "r11", "r12",
         "r13", "r14", "r15"]  # No RSP: it's the test's stack.
GPR32 = ["eax", "ecx", "edx", "ebx", "ebp", "esi", "edi", "r8d", "r9d", "r10d", "r11d", "r12d",
         "r13d", "r14d", "r15d"]
GPR16 = ["ax", "cx", "dx", "bx", "bp", "si", "di", "r8w", "r9w", "r10w", "r11w", "r12w", "r13w",
         "r14w", "r15w"]
SIZE = {8: "byte", 16: "word", 32: "dword", 64: "qword", 128: "xmmword", 256: "ymmword"}


def memory(bits, align=False, segment=""):
    disp = rng.choice([0, 8, 0x40, -0x20, 0x100]) if not align else rng.choice([0, 0x40, -0x20])
    if segment:
        # FS/GS (guest thread-local storage): on a base register, which the test aims at its data
        # through the segment's base.
        kind = rng.choice(["base", "index"])
        segment += ":"
    else:
        kind = rng.choice(["base", "base", "index", "rsp", "rip"])
    if kind == "rip":
        return f"{SIZE[bits]} ptr [rip + {disp}]"
    if kind == "rsp":
        return f"{SIZE[bits]} ptr [rsp + {abs(disp)}]"
    base = rng.choice(GPR64)
    if kind == "index":
        index = rng.choice([r for r in GPR64 if r != base])
        return f"{SIZE[bits]} ptr {segment}[{base} + {index}*{rng.choice([1, 2, 4, 8])} + {disp}]"
    return f"{SIZE[bits]} ptr {segment}[{base} + {disp}]"


def operand(kind, segment=""):
    if kind == "y":
        return f"ymm{rng.randrange(16)}"
    if kind == "x":
        return f"xmm{rng.randrange(16)}"
    if kind == "r32":
        return rng.choice(GPR32)
    if kind == "r64":
        return rng.choice(GPR64)
    if kind == "r16":
        return rng.choice(GPR16)
    if kind == "i":
        return str(rng.randrange(256))
    if kind == "i64":
        return str(rng.randrange(64))
    if kind == "ilane":  # Moves whole 128-bit halves (VPERMQ/VPERMPD in-place rewrite).
        return hex(rng.choice([0x44, 0x4E, 0xEE, 0xE4]))
    if kind.startswith("m"):
        return memory(int(kind[1:].rstrip("a")), align=kind.endswith("a"), segment=segment)
    raise ValueError(kind)


def expand(pattern):
    """'y,y,ym' -> ['y,y,y', 'y,y,m256']; 'x,x,xm' -> x and m128; 'r32,rm32' -> r32 and m32."""
    out = [[]]
    for part in pattern.split(","):
        choices = {"ym": ["y", "m256"], "xm": ["x", "m128"], "rm32": ["r32", "m32"],
                   "rm64": ["r64", "m64"]}.get(part)
        if choices is None and part.startswith("xm"):
            choices = ["x", "m" + part[2:]]
        out = [o + [c] for o in out for c in (choices or [part])]
    return out


FORMS = []


def add(mnemonics, *patterns):
    for mnemonic in mnemonics.split():
        for pattern in patterns:
            for parts in expand(pattern):
                FORMS.append((mnemonic, parts))


# AVX2 integer work on both 128-bit halves.
add("vpaddb vpaddw vpaddd vpaddq vpaddsb vpaddsw vpaddusb vpaddusw vpsubb vpsubw vpsubd vpsubq "
    "vpsubsb vpsubsw vpsubusb vpsubusw vpand vpandn vpor vpxor vpcmpeqb vpcmpeqw vpcmpeqd "
    "vpcmpeqq vpcmpgtb vpcmpgtw vpcmpgtd vpcmpgtq vpshufb vpacksswb vpackssdw vpackuswb "
    "vpackusdw vpunpcklbw vpunpcklwd vpunpckldq vpunpcklqdq vpunpckhbw vpunpckhwd vpunpckhdq "
    "vpunpckhqdq vpmullw vpmulld vpmulhw vpmulhuw vpmulhrsw vpmuludq vpmuldq vpmaddwd "
    "vpmaddubsw vpsadbw vpavgb vpavgw vpmaxsb vpmaxsw vpmaxsd vpmaxub vpmaxuw vpmaxud vpminsb "
    "vpminsw vpminsd vpminub vpminuw vpminud vpsignb vpsignw vpsignd vphaddw vphaddd vphaddsw "
    "vphsubw vphsubd vphsubsw vpermd vpermps", "y,y,ym")
add("vpsllvd vpsllvq vpsrlvd vpsrlvq vpsravd", "y,y,ym", "x,x,xm")
add("vpabsb vpabsw vpabsd", "y,ym")
add("vpshufd vpshufhw vpshuflw vpermq vpermpd", "y,ym,i")
add("vpermq vpermpd", "y,ym,ilane")
add("vpalignr vpblendw vpblendd vperm2i128 vmpsadbw", "y,y,ym,i")
add("vpblendd", "x,x,xm,i")
add("vpsllw vpslld vpsllq vpsrlw vpsrld vpsrlq vpsraw vpsrad vpslldq vpsrldq", "y,y,i")
add("vpsllw vpslld vpsllq vpsrlw vpsrld vpsrlq vpsraw vpsrad", "y,y,xm")
add("vpblendvb", "y,y,ym,y")
add("vmovntdqa", "y,m256a")
add("vpmovmskb", "r32,y")
add("vpbroadcastb", "x,xm8", "y,xm8")
add("vpbroadcastw", "x,xm16", "y,xm16")
add("vpbroadcastd", "x,xm32", "y,xm32")
add("vpbroadcastq", "x,xm64", "y,xm64")
add("vbroadcastss", "x,x", "y,x")
add("vbroadcastsd", "y,x")
add("vbroadcasti128", "y,m128")
add("vextracti128", "x,y,i", "m128,y,i")
add("vinserti128", "y,y,xm,i")
add("vpmaskmovd vpmaskmovq", "x,x,m128", "y,y,m256", "m128,x,x", "m256,y,y")
add("vpmovzxbw vpmovsxbw vpmovzxwd vpmovsxwd vpmovzxdq vpmovsxdq", "y,xm")
add("vpmovzxbd vpmovsxbd vpmovzxwq vpmovsxwq", "y,xm64")
add("vpmovzxbq vpmovsxbq", "y,xm32")
# FMA.
for op in ["madd", "msub", "nmadd", "nmsub"]:
    for order in ["132", "213", "231"]:
        add(f"vf{op}{order}ps vf{op}{order}pd", "x,x,xm", "y,y,ym")
        add(f"vf{op}{order}ss", "x,x,xm32")
        add(f"vf{op}{order}sd", "x,x,xm64")
for op in ["maddsub", "msubadd"]:
    for order in ["132", "213", "231"]:
        add(f"vf{op}{order}ps vf{op}{order}pd", "x,x,xm", "y,y,ym")
# BMI2.
add("shlx shrx sarx bzhi", "r32,rm32,r32", "r64,rm64,r64")
add("rorx", "r32,rm32,i64", "r64,rm64,i64")
add("mulx pdep pext", "r32,r32,rm32", "r64,r64,rm64")
# Other extensions Jaguar lacks.
add("rdrand rdseed", "r16", "r32", "r64")
add("adcx adox", "r32,rm32", "r64,rm64")
add("clflushopt", "m8")

GATHERS = {  # mnemonic: [(destination, index, mask)]
    "vgatherdps": [("x", "x", "x"), ("y", "y", "y")],
    "vpgatherdd": [("x", "x", "x"), ("y", "y", "y")],
    "vgatherdpd": [("x", "x", "x"), ("y", "x", "y")],
    "vpgatherdq": [("x", "x", "x"), ("y", "x", "y")],
    "vgatherqps": [("x", "x", "x"), ("x", "y", "x")],
    "vpgatherqd": [("x", "x", "x"), ("x", "y", "x")],
    "vgatherqpd": [("x", "x", "x"), ("y", "y", "y")],
    "vpgatherqq": [("x", "x", "x"), ("y", "y", "y")],
}

def gathers(segment=""):
    for mnemonic, shapes in GATHERS.items():
        for dst, index, mask in shapes:
            regs = rng.sample(range(16), 3)
            data = 32 if mnemonic in ("vgatherdps", "vpgatherdd", "vgatherqps", "vpgatherqd") else 64
            prefix = f"{segment}:" if segment else ""
            print(f"\t{mnemonic} {dst}mm{regs[0]}, {SIZE[data]} ptr {prefix}[{rng.choice(GPR64)} + "
                  f"{index}mm{regs[1]}*{rng.choice([1, 2, 4, 8])} + {rng.choice([0, 8, -16])}], "
                  f"{mask}mm{regs[2]}")


print(".intel_syntax noprefix\n.text")
for _ in range(COUNT):
    for mnemonic, parts in FORMS:
        print(f"\t{mnemonic} " + ", ".join(operand(p) for p in parts))
    gathers()
# Every form with a memory operand again with an FS and a GS override, a few times each.
for _ in range(3):
    for segment in ("fs", "gs"):
        for mnemonic, parts in FORMS:
            if any(p.startswith("m") for p in parts):
                print(f"\t{mnemonic} " + ", ".join(operand(p, segment) for p in parts))
        gathers(segment)
