#!/usr/bin/env python3
"""Speed and code size of opuspp vs libopus under named build profiles.

For every compiler and profile, ONE set of flags is used for everything:
libopus (static library, the reference decoder and the benchmark's encoder),
the benchmark (tests/bench.cpp) and the decoder program measured for code
size (tools/opus_wav/decode.cpp, built with opuspp, with libopus, and as a
stub without a decoder). libopus is also built as a shared library with the
same flags, to show the code an application loads when it links libopus
dynamically. The result is Markdown tables (settings, speed, code size), and
the raw numbers are saved as JSON in the work directory.

Builds are cached in the work directory. Each compiler x profile gets a
directory keyed by a hash of its full configuration (compiler version, flags,
libopus options, opuspp defines). libopus is built once per configuration,
and the programs are rebuilt only when their sources change. All
configurations are built in parallel; the benchmarks then run one at a time,
pinned to one core. With --reuse-results, a measurement is reused when the
benchmark binary is byte-identical to the one that produced it.

Usage:
  tools/profile_report.py [--work DIR] [--compilers gcc,clang]
                          [--profiles size,balanced,fast] [--seconds 60] [--passes 7]
                          [--reuse-results] [--from-json results.json]
                          [--no-bench] [--no-size] [--no-music] [--music-passes 15]
                          [--shared] [--cases 128,320,510,loss]
                          [--fast-flags "-O3 -ffast-math ..."]
                          [--pp-defines "-DOPUSPP_NO_VECTOR_EXT"]
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# The libopus reference sources live outside the project, by default next to it.
LIBOPUS = Path(os.environ.get("OPUSPP_LIBOPUS_DIR", ROOT.parent / "opus-1.6.1")).resolve()

# name -> (description, compiler flags, extra libopus CMake options, extra opuspp defines)
PROFILES = {
    "size": ("Code size optimised", ["-Os"], [], []),
    "balanced": ("Balanced", ["-O2"], [], []),
    "fast": ("Fast", ["-O3", "-march=native", "-ffast-math", "-falign-loops=32"],
             ["-DOPUS_FLOAT_APPROX=ON", "-DOPUS_FAST_MATH=ON"], ["-DOPUSPP_FLOAT_APPROX"]),
}
COMMON = ["-DNDEBUG", "-ffunction-sections", "-fdata-sections"]
LDFLAGS = ["-Wl,--gc-sections"]
LIBOPUS_OPTS = ["-DOPUS_BUILD_PROGRAMS=OFF", "-DOPUS_BUILD_TESTING=OFF", "-DOPUS_HARDENING=OFF",
                "-DOPUS_STACK_PROTECTOR=OFF", "-DOPUS_FORTIFY_SOURCE=OFF"]
CXX = {"gcc": "g++", "clang": "clang++"}
# The tables report streams from an encoder at this complexity; the benchmark
# also measures 0 and 2, and results.json keeps those and their combination.
REPORT_COMPLEXITY = 4
MUSIC_SECONDS = 15  # length of tests/data/figaro_overture.s16le


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode != 0:
        sys.exit(f"command failed: {shlex.join(map(str, cmd))}\n{r.stdout[-3000:]}{r.stderr[-3000:]}")
    return r.stdout


# Program sources: any change here rebuilds the programs (not libopus).
SOURCES = [*(ROOT / "tests" / f for f in ("bench.cpp", "bench_signal.hpp", "bench_stream.hpp", "stack_usage.cpp")),
           *sorted((ROOT / "include").rglob("*.hpp")),
           *sorted((ROOT / "tools/opus_wav").glob("*.[ch]pp"))]


def digest(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(p if isinstance(p, bytes) else str(p).encode())
        h.update(b"\0")
    return h.hexdigest()


def build(cc, prof, work):
    """Builds (or reuses) libopus and the programs for one compiler x profile."""
    _, flags, opus_opts, pp_defs = PROFILES[prof]
    cflags = flags + COMMON
    config = dict(cc=cc, cxx=CXX[cc], version=run([cc, "--version"]) + run([CXX[cc], "--version"]),
                  cflags=cflags, ldflags=LDFLAGS, libopus=LIBOPUS_OPTS + opus_opts, defines=pp_defs)
    key = digest(json.dumps(config, sort_keys=True))
    d = work / f"{cc}-{prof}-{key[:12]}"
    d.mkdir(parents=True, exist_ok=True)
    (d / "config.json").write_text(json.dumps(config, indent=1))
    lib = d / "opus" / "libopus.a"
    if not lib.exists():
        run(["cmake", "-S", LIBOPUS, "-B", d / "opus", "-DCMAKE_BUILD_TYPE=",
             f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_C_FLAGS={' '.join(cflags)}", *LIBOPUS_OPTS, *opus_opts])
        run(["cmake", "--build", d / "opus", "-j", "--target", "opus"])
    # The same libopus as a shared library (position-independent, same flags),
    # for the code size of an application that links libopus dynamically.
    so = d / "opus-shared" / "libopus.so"
    if not so.exists():
        run(["cmake", "-S", LIBOPUS, "-B", d / "opus-shared", "-DCMAKE_BUILD_TYPE=", "-DBUILD_SHARED_LIBS=ON",
             f"-DCMAKE_C_COMPILER={cc}", f"-DCMAKE_C_FLAGS={' '.join(cflags)}",
             f"-DCMAKE_SHARED_LINKER_FLAGS={' '.join(LDFLAGS)}", *LIBOPUS_OPTS, *opus_opts])
        run(["cmake", "--build", d / "opus-shared", "-j", "--target", "opus"])
    stamp = d / "programs.stamp"
    src = digest(*(f.read_bytes() for f in SOURCES), lib.read_bytes(), so.resolve().read_bytes())
    outputs = ["bench", "bench_shared", "decode_stub", "decode_opuspp", "decode_libopus", "decode_libopus_shared",
               "stack_usage"]
    if stamp.exists() and stamp.read_text() == src and all((d / o).exists() for o in outputs):
        return d, False
    cxx = [CXX[cc], "-std=c++23", *cflags, *pp_defs, f"-I{ROOT / 'include'}",
           f"-I{LIBOPUS / 'include'}", f"-I{ROOT / 'tools/opus_wav'}", f"-I{ROOT / 'tests'}"]
    audio = f'-DOPUSPP_BENCH_AUDIO="{ROOT / "tests/data/figaro_overture.s16le"}"'
    dec = ROOT / "tools/opus_wav/decode.cpp"
    jobs = [[*cxx, audio, ROOT / "tests/bench.cpp", lib, "-lm", *LDFLAGS, "-o", d / "bench"],
            # The same benchmark with libopus linked as a shared library.
            [*cxx, audio, ROOT / "tests/bench.cpp", so, f"-Wl,-rpath,{so.parent}", "-lm", *LDFLAGS,
             "-o", d / "bench_shared"],
            [*cxx, "-DDECODE_STUB", dec, *LDFLAGS, "-o", d / "decode_stub"],
            [*cxx, dec, *LDFLAGS, "-o", d / "decode_opuspp"],
            [*cxx, "-DDECODE_LIBOPUS", dec, lib, "-lm", *LDFLAGS, "-o", d / "decode_libopus"],
            [*cxx, "-DDECODE_LIBOPUS", dec, so, f"-Wl,-rpath,{so.parent}", "-lm", *LDFLAGS,
             "-o", d / "decode_libopus_shared"],
            # Peak stack per decoding path, both decoders built with this profile.
            [*cxx, ROOT / "tests/stack_usage.cpp", lib, "-lm", "-pthread", *LDFLAGS, "-o", d / "stack_usage"]]
    with concurrent.futures.ThreadPoolExecutor() as ex:
        list(ex.map(run, jobs))
    stamp.write_text(src)
    return d, True


def measured(d, name, args, reuse, fn, binary="bench"):
    """Runs fn(), or with `reuse` returns the saved result for the same binary and arguments."""
    cache = d / "measurements.json"
    saved = json.loads(cache.read_text()) if cache.exists() else {}
    key = digest((d / binary).read_bytes(), json.dumps(args))
    if reuse and saved.get(name, {}).get("key") == key:
        return saved[name]["value"], True
    value = fn()
    saved[name] = {"key": key, "value": value}
    cache.write_text(json.dumps(saved, indent=1))
    return value, False


def size(path):
    # Berkeley text + data: code, constants and initialised data.
    text, data = run(["size", path]).splitlines()[1].split()[:2]
    return int(text) + int(data)


def bench(d, seconds, passes, segments=None, cases=None, binary="bench"):
    segs = segments or "tone,noise,transients,music"
    out = run(["taskset", "-c", "2", d / binary, str(seconds), str(passes), segs, *([cases] if cases else [])])
    result = {}
    for line in out.splitlines():
        m = re.match(r"^cx(\d+)\s+(.+?)\s+([\d.]+)\s+([\d.]+)\s+[\d.]+x", line)
        if m and "subtotal" not in m.group(2):
            ref, pp = result.setdefault(m.group(2), [0.0, 0.0])
            result[m.group(2)] = [ref + float(m.group(3)), pp + float(m.group(4))]
            # Also per encoder complexity, as "cx<n>|<stream>" (left out of the combined tables).
            result[f"cx{m.group(1)}|{m.group(2)}"] = [float(m.group(3)), float(m.group(4))]
    return result


def stack(d):
    # Peak stack in bytes per decoding path: {path: [opuspp, libopus]}.
    result = {}
    for line in run([d / "stack_usage"]).splitlines():
        m = re.match(r"^(.+?)\s+(\d+)\s+(\d+)$", line)
        if m:
            result[m.group(1)] = [int(m.group(2)), int(m.group(3))]
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", default=str(ROOT / "build-profiles"))
    ap.add_argument("--compilers", default="gcc,clang")
    ap.add_argument("--profiles", default="size,balanced,fast")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--passes", type=int, default=7)
    ap.add_argument("--cases", help="benchmark cases, e.g. 510 (see tests/bench.cpp); default: its standard set")
    ap.add_argument("--no-bench", action="store_true")
    ap.add_argument("--no-size", action="store_true")
    ap.add_argument("--no-stack", action="store_true")
    ap.add_argument("--no-music", action="store_true", help="skip the music-only benchmark")
    ap.add_argument("--shared", action="store_true",
                    help="also benchmark against libopus linked as a shared library")
    ap.add_argument("--music-passes", type=int, default=15)
    ap.add_argument("--fast-flags", help="override the fast profile's compiler flags")
    ap.add_argument("--reuse-results", action="store_true",
                    help="reuse saved measurements whose benchmark binary is unchanged")
    ap.add_argument("--from-json", help="print the tables from a saved results.json instead of measuring")
    ap.add_argument("--pp-defines", default="",
                    help="extra opuspp defines for every profile, e.g. -DOPUSPP_NO_VECTOR_EXT (ablation)")
    a = ap.parse_args()
    if not (LIBOPUS / "CMakeLists.txt").exists():
        sys.exit("run tools/fetch_deps.sh first")
    if a.fast_flags:
        desc, _, o, p = PROFILES["fast"]
        PROFILES["fast"] = (desc, a.fast_flags.split(), o, p)
    if a.pp_defines:
        for k, (desc, f, o, p) in list(PROFILES.items()):
            PROFILES[k] = (desc, f, o, p + a.pp_defines.split())
    work = Path(a.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    compilers = a.compilers.split(",")
    profiles = a.profiles.split(",")

    res = json.loads(Path(a.from_json).read_text()) if a.from_json else {}
    combos = [] if a.from_json else [(cc, prof) for cc in compilers for prof in profiles]
    if combos:
        print(f"building {len(combos)} configurations in parallel", file=sys.stderr, flush=True)
    with concurrent.futures.ThreadPoolExecutor() as ex:
        built = dict(zip(combos, ex.map(lambda c: build(*c, work), combos)))
    for (cc, prof), (d, rebuilt) in built.items():
        print(f"[{cc} {prof}] {'built' if rebuilt else 'cached'}: {d.name}", file=sys.stderr, flush=True)
    for cc, prof in combos:
        d = built[(cc, prof)][0]
        r = res.setdefault(cc, {}).setdefault(prof, {})
        if not a.no_size:
            base = size(d / "decode_stub")
            r["size"] = {"opuspp": size(d / "decode_opuspp") - base,
                         "libopus": size(d / "decode_libopus") - base,
                         "libopus_so": size((d / "opus-shared" / "libopus.so").resolve()),
                         "libopus_so_caller": size(d / "decode_libopus_shared") - base}
        if not a.no_stack:
            r["stack"] = stack(d)
        if not a.no_bench:
            r["bench"], reused = measured(d, "bench", [a.seconds, a.passes, a.cases], a.reuse_results,
                                          lambda: bench(d, a.seconds, a.passes, cases=a.cases))
            print(f"[{cc} {prof}] benchmark {'reused' if reused else 'measured'}", file=sys.stderr, flush=True)
        if a.shared:
            r["bench_shared"], reused = measured(d, "bench_shared", [a.seconds, a.passes, a.cases], a.reuse_results,
                                                 lambda: bench(d, a.seconds, a.passes, cases=a.cases,
                                                               binary="bench_shared"),
                                                 binary="bench_shared")
            print(f"[{cc} {prof}] shared-library benchmark {'reused' if reused else 'measured'}", file=sys.stderr,
                  flush=True)
        if not a.no_music:
            # The whole 15 s clip, exactly once (segments "music" only).
            r["music"], reused = measured(d, "music", [MUSIC_SECONDS, a.music_passes], a.reuse_results,
                                          lambda: bench(d, MUSIC_SECONDS, a.music_passes, "music"))
            print(f"[{cc} {prof}] music benchmark {'reused' if reused else 'measured'}", file=sys.stderr,
                  flush=True)
    if not a.from_json:
        (work / "results.json").write_text(json.dumps(res, indent=1))
    has = lambda key: all(key in res[cc][p] for cc in compilers for p in profiles)

    vers = {cc: run([cc, "--version"]).splitlines()[0] for cc in compilers}
    print("Compilers: " + "; ".join(vers.values()) + "\n")
    print("| Profile | Compiler flags (libopus, opuspp, benchmark and decoder alike) | libopus CMake options | opuspp defines |")
    print("|---|---|---|---|")
    for prof in profiles:
        desc, flags, o, p = PROFILES[prof]
        print(f"| **{desc}** | `{' '.join(flags + COMMON)}`, linked with `{' '.join(LDFLAGS)}` "
              f"| {' '.join(f'`{x}`' for x in o) or '-'} | {' '.join(f'`{x}`' for x in p) or '-'} |")
    cols = [(cc, prof) for cc in compilers for prof in profiles]
    cname = {"gcc": "GCC", "clang": "Clang"}
    hdr = " | ".join(f"{cname.get(cc, cc)} {PROFILES[prof][0]}" for cc, prof in cols)
    def speed_table(key, title, complexity=REPORT_COMPLEXITY, relative=False):
        print(f"\n{title}\n\n| Stream | {hdr} |")
        print("|---|" + "---|" * len(cols))
        prefix = f"cx{complexity}|"
        names = [n for n in res[compilers[0]][profiles[0]][key] if n.startswith(prefix)]
        tot = {c: [0.0, 0.0] for c in cols}
        for n in names:
            row = []
            for c in cols:
                ref, pp = res[c[0]][c[1]][key][n]
                tot[c][0] += ref
                tot[c][1] += pp
                row.append(f"{ref / pp:.2f}x")
            print(f"| {n.removeprefix(prefix)} | " + " | ".join(row) + " |")
        print("| **all** | " + " | ".join(f"**{tot[c][0] / tot[c][1]:.2f}x**" for c in cols) + " |")
        print("| opuspp, all streams | " + " | ".join(f"{tot[c][1]:.0f} ms" for c in cols) + " |")
        print("| libopus, all streams | " + " | ".join(f"{tot[c][0]:.0f} ms" for c in cols) + " |")
        if relative and "size" in profiles:
            # Each decoder's speed relative to its own code-size-optimised build.
            print(f"\nEach decoder's speed relative to its own Code size optimised build ({key}):\n")
            print(f"| Decoder | {hdr} |")
            print("|---|" + "---|" * len(cols))
            for i, k in ((1, "opuspp"), (0, "libopus")):
                print(f"| {k} | " + " | ".join(f"{tot[(c[0], 'size')][i] / tot[c][i]:.2f}x" for c in cols) + " |")

    def matrix(key, case):
        # Every opuspp build against every libopus build for one stream; the
        # second column is each libopus build's speed relative to the fastest.
        if not all(case in res[c[0]][c[1]][key] for c in cols):
            return
        lib = {c: res[c[0]][c[1]][key][case][0] for c in cols}
        pp = {c: res[c[0]][c[1]][key][case][1] for c in cols}
        fastest = min(lib.values())
        label = lambda c: f"{cname.get(c[0], c[0])} {PROFILES[c[1]][0]}"
        print(f"\n{case.removeprefix(f'cx{REPORT_COMPLEXITY}|')}: speed-up of each opuspp build (columns) over each libopus build (rows), "
              f"and each libopus build's speed relative to the fastest:\n")
        print("| libopus build ↓ · opuspp build → | libopus speed | " + " | ".join(label(c) for c in cols) + " |")
        print("|---|---|" + "---|" * len(cols))
        for r in cols:
            cells = [(f"**{lib[r] / pp[c]:.2f}x**" if r == c else f"{lib[r] / pp[c]:.2f}x") for c in cols]
            name = f"**{label(r)}** (fastest)" if lib[r] == fastest else label(r)
            print(f"| {name} | {fastest / lib[r]:.2f} | " + " | ".join(cells) + " |")

    def case_tables(case, shared):
        # For one stream: each decoder against its own code-size-optimised
        # build, and libopus statically linked against the shared library.
        t = lambda c, key, i: res[c[0]][c[1]][key][case][i]
        name = case.split("|")[-1]
        print(f"\n{name}: each decoder's speed relative to its own Code size optimised build:\n")
        print(f"| Decoder | {hdr} |\n|---|" + "---|" * len(cols))
        for i, k in ((1, "opuspp"), (0, "libopus")):
            print(f"| {k} | " + " | ".join(f"{t((c[0], 'size'), 'bench', i) / t(c, 'bench', i):.2f}x" for c in cols)
                  + " |")
        print("| opuspp over libopus, like for like | "
              + " | ".join(f"{t(c, 'bench', 0) / t(c, 'bench', 1):.2f}x" for c in cols) + " |")
        if not shared:
            return
        print(f"\n{name}: libopus statically linked against libopus as a shared library (same flags):\n")
        print(f"| | {hdr} |\n|---|" + "---|" * len(cols))
        print("| libopus static | " + " | ".join(f"{t(c, 'bench', 0):.1f} ms" for c in cols) + " |")
        print("| libopus shared | " + " | ".join(f"{t(c, 'bench_shared', 0):.1f} ms" for c in cols) + " |")
        print("| libopus shared, speed relative to static | " + " | ".join(
            f"{t(c, 'bench', 0) / t(c, 'bench_shared', 0):.2f}x" for c in cols) + " |")
        print("| opuspp over static libopus | " + " | ".join(
            f"{t(c, 'bench', 0) / t(c, 'bench', 1):.2f}x" for c in cols) + " |")
        print("| opuspp over shared libopus | " + " | ".join(
            f"{t(c, 'bench_shared', 0) / t(c, 'bench_shared', 1):.2f}x" for c in cols) + " |")
        print("| opuspp, static run against shared run | " + " | ".join(
            f"{100 * (t(c, 'bench_shared', 1) / t(c, 'bench', 1) - 1):+.1f} %" for c in cols) + " |")

    if not a.no_bench and has("bench"):
        speed_table("bench", f"Speed-up of opuspp over libopus (encoder complexity {REPORT_COMPLEXITY}):")
        matrix("bench", f"cx{REPORT_COMPLEXITY}|320 kb/s")
        case_tables(f"cx{REPORT_COMPLEXITY}|320 kb/s", a.shared and has("bench_shared"))
        for cx in (0, 2):
            if all(f"cx{cx}|320 kb/s" in res[c][p]["bench"] for c in compilers for p in profiles):
                speed_table("bench", f"Additional results: speed-up of opuspp over libopus (encoder complexity {cx}):",
                            cx, relative=False)
    if a.shared and has("bench_shared"):
        speed_table("bench_shared", "Speed-up of opuspp over libopus linked as a shared library "
                                    f"(encoder complexity {REPORT_COMPLEXITY}):")
    if not a.no_music and has("music"):
        speed_table("music", f"Music only: the whole {MUSIC_SECONDS} s clip, speed-up of opuspp over libopus "
                             f"(encoder complexity {REPORT_COMPLEXITY}):")
    if not a.no_stack and has("stack"):
        for i, k in ((0, "opuspp"), (1, "libopus")):
            print(f"\nPeak stack of {k} in bytes, per decoding path (both decoders built with the profile):\n")
            print(f"| Path | {hdr} |\n|---|" + "---|" * len(cols))
            for n in res[compilers[0]][profiles[0]]["stack"]:
                print(f"| {n} | " + " | ".join(f"{res[c[0]][c[1]]['stack'][n][i]:,}" for c in cols) + " |")
    if not a.no_size and has("size"):
        print(f"\nDecoder code size (program minus stub, text + data):\n\n| | {hdr} |")
        print("|---|" + "---|" * len(cols))
        for k in ("opuspp", "libopus"):
            print(f"| {k} | " + " | ".join(f"{res[c[0]][c[1]]['size'][k] / 1024:.1f} KB" for c in cols) + " |")
        print("| opuspp smaller by | " + " | ".join(
            f"{res[c[0]][c[1]]['size']['libopus'] / res[c[0]][c[1]]['size']['opuspp']:.2f}x" for c in cols) + " |")
        if all("libopus_so" in res[c[0]][c[1]]["size"] for c in cols):
            sz = lambda c, k: res[c[0]][c[1]]["size"][k]
            print(f"\nWith libopus as a shared library (text + data):\n\n| | {hdr} |")
            print("|---|" + "---|" * len(cols))
            print("| libopus.so (whole library) | " + " | ".join(f"{sz(c, 'libopus_so') / 1024:.1f} KB" for c in cols) + " |")
            print("| program code for calling it | " + " | ".join(
                f"{sz(c, 'libopus_so_caller') / 1024:.1f} KB" for c in cols) + " |")
            print("| opuspp (compiled in) | " + " | ".join(f"{sz(c, 'opuspp') / 1024:.1f} KB" for c in cols) + " |")
            print("| opuspp smaller than libopus.so by | " + " | ".join(
                f"{sz(c, 'libopus_so') / sz(c, 'opuspp'):.2f}x" for c in cols) + " |")


if __name__ == "__main__":
    main()
