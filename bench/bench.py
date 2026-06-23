#!/usr/bin/env python3
# bench.py — оркестратор бенчмарка ГПСЧ rnd128_.
#
# Что делает:
#   1) собирает baseline / opt_seq / opt_par одинаковыми флагами;
#   2) sanity-check: baseline ≈ opt_seq (ULP-расхождение допустимо) и
#                    opt_seq == opt_par(T=4) (побайтно);
#   3) гоняет матрицу N x T, повторов repeats, берёт min времени;
#   4) считает speedup_opt1, speedup_par(T), speedup_total;
#   5) пишет results.md и raw_runs.log в /work.
#
# Запускается как ENTRYPOINT Docker-образа. Под капотом полностью stdlib.

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

WORK = Path("/work")
SRC = WORK              # cpp-файлы лежат прямо в /work (монтируем parallel/ сюда)
BIN = WORK / "_bin"     # сюда складываем собранные exe (точнее, ELF)
LOG_PATH = WORK / "raw_runs.log"
RESULTS_PATH = WORK / "results.md"

# По умолчанию — четыре размера и пять числа потоков. Можно переопределить
# через переменные окружения BENCH_SIZES, BENCH_THREADS, BENCH_REPEATS.
DEFAULT_SIZES = [1_000_000, 10_000_000, 100_000_000, 500_000_000]
DEFAULT_THREADS = [1, 2, 4, 8]
DEFAULT_REPEATS = 3


def log(msg, also_stdout=True):
    LOG_PATH.parent.mkdir(parents=True, exist_ok=True)
    with LOG_PATH.open("a", encoding="utf-8") as f:
        f.write(msg.rstrip() + "\n")
    if also_stdout:
        print(msg.rstrip(), flush=True)


def run(cmd, check=True, capture=True):
    """Запуск процесса. Логирует команду и stdout/stderr целиком."""
    log(f"$ {' '.join(cmd) if isinstance(cmd, list) else cmd}")
    try:
        res = subprocess.run(
            cmd,
            shell=isinstance(cmd, str),
            check=False,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.STDOUT if capture else None,
            text=True,
        )
    except FileNotFoundError as e:
        log(f"!! не нашёл бинарник: {e}")
        if check:
            sys.exit(2)
        return None

    out = res.stdout or ""
    if out:
        log(out, also_stdout=False)
    if res.returncode != 0:
        log(f"!! exit code = {res.returncode}")
        if check:
            sys.exit(res.returncode)
    return out


def detect_env():
    """Собираем кусок среды для шапки results.md."""
    info = {}
    info["date_utc"] = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime())

    # Модель CPU — из /proc/cpuinfo.
    try:
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("model name"):
                    info["cpu_model"] = line.split(":", 1)[1].strip()
                    break
    except Exception:
        info["cpu_model"] = "unknown"

    # Логические потоки — nproc.
    try:
        info["nproc"] = int(subprocess.check_output(["nproc"], text=True).strip())
    except Exception:
        info["nproc"] = os.cpu_count() or 0

    # Физические ядра — из /proc/cpuinfo (по уникальным core id внутри одного physical id).
    try:
        cores = set()
        phys_id, core_id = None, None
        with open("/proc/cpuinfo") as f:
            for line in f:
                if line.startswith("physical id"):
                    phys_id = line.split(":", 1)[1].strip()
                elif line.startswith("core id"):
                    core_id = line.split(":", 1)[1].strip()
                elif line.strip() == "" and phys_id is not None and core_id is not None:
                    cores.add((phys_id, core_id))
                    phys_id, core_id = None, None
        info["physical_cores"] = len(cores) if cores else info["nproc"]
    except Exception:
        info["physical_cores"] = info["nproc"]

    # Версия компилятора.
    cxx = os.environ.get("CXX", "g++")
    try:
        ver = subprocess.check_output([cxx, "--version"], text=True).splitlines()[0]
    except Exception:
        ver = f"{cxx} (version unknown)"
    info["compiler"] = ver
    info["cxxflags"] = os.environ.get("CXXFLAGS", "")
    info["ompflags"] = os.environ.get("OMPFLAGS", "")
    return info


def build_all():
    """Собираем три бинарника одинаковыми CXXFLAGS."""
    BIN.mkdir(parents=True, exist_ok=True)
    cxx = os.environ.get("CXX", "g++")
    cxxflags = os.environ.get("CXXFLAGS", "-O3 -march=native -std=c++17").split()
    ompflags = os.environ.get("OMPFLAGS", "-fopenmp").split()

    targets = [
        ("rng_baseline", ["rng_baseline.cpp"], False),
        ("rng_seq",      ["rng_seq.cpp"],      False),
        ("rng_par",      ["rng_par.cpp"],      True),
    ]
    for name, srcs, omp in targets:
        out = BIN / name
        cmd = [cxx, *cxxflags]
        if omp:
            cmd += ompflags
        cmd += [*[str(SRC / s) for s in srcs], "-o", str(out)]
        run(cmd, check=True, capture=True)
        if not out.exists():
            log(f"!! сборка {name} не дала бинарника")
            sys.exit(2)
    log("=== build OK ===")


# Регулярка для парсинга "time_ms=…" и "checksum=…" из stdout бинарников.
RE_TIME = re.compile(r"time_ms=([0-9]+\.?[0-9]*)")
RE_CHK  = re.compile(r"checksum=([0-9]+\.?[0-9]*)")


def measure(bin_name, n, threads=None):
    """Один прогон. Возвращает (time_ms, checksum, dump_path)."""
    dump = WORK / f"_dump_{bin_name}.txt"
    cmd = [str(BIN / bin_name), str(n), str(dump)]
    if threads is not None:
        cmd.append(str(threads))
    out = run(cmd, check=True, capture=True)
    m_t = RE_TIME.search(out or "")
    m_c = RE_CHK.search(out or "")
    if not m_t or not m_c:
        log(f"!! не распарсил вывод {bin_name}: {out!r}")
        sys.exit(3)
    return float(m_t.group(1)), float(m_c.group(1)), dump


def measure_min(bin_name, n, threads=None, repeats=DEFAULT_REPEATS):
    """Несколько повторов, минимум времени, проверка стабильности checksum."""
    times = []
    chks = set()
    for r in range(repeats):
        t, c, _ = measure(bin_name, n, threads)
        times.append(t)
        chks.add(round(c, 3))
        log(f"  [{bin_name} N={n} T={threads} run={r+1}] {t:.3f} ms  chk={c}")
    if len(chks) > 1:
        log(f"  !! нестабильная checksum в {bin_name} N={n} T={threads}: {chks}")
    return min(times), max(times), statistics.mean(times)


def sanity_check():
    """
    1) baseline и opt_seq должны давать одну последовательность с точностью
       до ULP double (расхождение в 15-м знаке формата).
    2) opt_seq и opt_par(T=4) должны совпасть побайтно после удаления
       строк-комментариев.
    """
    log("=== sanity check ===")
    measure("rng_baseline", 200_000)
    base_dump = WORK / "_dump_rng_baseline.txt"
    measure("rng_seq", 200_000)
    seq_dump = WORK / "_dump_rng_seq.txt"
    measure("rng_par", 200_000, 4)
    par_dump = WORK / "_dump_rng_par.txt"

    def read_data(p):
        return [ln for ln in p.read_text(encoding="utf-8").splitlines()
                if ln and not ln.startswith("#")]

    base = read_data(base_dump)
    seq = read_data(seq_dump)
    par = read_data(par_dump)

    if len(base) != len(seq) or len(seq) != len(par):
        log(f"!! разное число строк: base={len(base)} seq={len(seq)} par={len(par)}")
        sys.exit(4)

    # opt_seq vs opt_par — должно быть полное побайтное совпадение.
    if seq != par:
        log("!! opt_seq и opt_par(T=4) расходятся побайтно — bug в распараллеливании")
        for i, (a, b) in enumerate(zip(seq, par)):
            if a != b:
                log(f"   первая разница на строке {i}:\n     seq: {a}\n     par: {b}")
                break
        sys.exit(4)
    log("  opt_seq == opt_par(T=4): побайтно совпали ✓")

    # baseline vs opt_seq — допустим ULP double.
    max_diff = 0.0
    for i, (a, b) in enumerate(zip(base, seq)):
        try:
            va = float(a.split()[1]); vb = float(b.split()[1])
            d = abs(va - vb)
            if d > max_diff: max_diff = d
        except Exception:
            log(f"  не разобрал строку {i}: {a!r} / {b!r}")
            sys.exit(4)
    log(f"  baseline vs opt_seq max diff = {max_diff:.3e} (ожидалось ≤ 1e-15)")
    if max_diff > 1e-13:
        log("!! слишком большая разница, остановка")
        sys.exit(4)
    log("=== sanity check OK ===")


def fmt_ms(x):
    return f"{x:>10.3f}" if x is not None else "      —   "


def fmt_speedup(x):
    return f"{x:>6.2f}x" if x is not None else "    —  "


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeats", type=int,
                    default=int(os.environ.get("BENCH_REPEATS", DEFAULT_REPEATS)))
    ap.add_argument("--sizes", type=str,
                    default=os.environ.get(
                        "BENCH_SIZES",
                        ",".join(str(x) for x in DEFAULT_SIZES)))
    ap.add_argument("--threads", type=str,
                    default=os.environ.get(
                        "BENCH_THREADS",
                        ",".join(str(x) for x in DEFAULT_THREADS)))
    ap.add_argument("--skip-build", action="store_true")
    ap.add_argument("--skip-sanity", action="store_true")
    args = ap.parse_args()

    sizes = [int(x) for x in args.sizes.split(",") if x.strip()]
    threads = [int(x) for x in args.threads.split(",") if x.strip()]

    # Стартовая зачистка лога — каждый прогон бенчмарка пишется с нуля.
    if LOG_PATH.exists():
        LOG_PATH.unlink()

    info = detect_env()
    log(f"=== env ===")
    for k, v in info.items():
        log(f"  {k}: {v}")

    # Если physical_cores не входит в список потоков — добавим (если оно > 0).
    pc = info.get("physical_cores", 0)
    if pc > 0 and pc not in threads:
        threads.append(pc)
    threads = sorted(set(threads))

    if not args.skip_build:
        build_all()

    if not args.skip_sanity:
        sanity_check()

    log(f"=== benchmark: sizes={sizes} threads={threads} repeats={args.repeats} ===")

    # raw[(N, mode, T_or_None)] = (min_ms, max_ms, mean_ms)
    raw = {}
    for n in sizes:
        log(f"--- N = {n} ---")
        raw[(n, "baseline", None)] = measure_min("rng_baseline", n, repeats=args.repeats)
        raw[(n, "opt_seq", None)]  = measure_min("rng_seq", n, repeats=args.repeats)
        for t in threads:
            raw[(n, "opt_par", t)] = measure_min("rng_par", n, threads=t,
                                                 repeats=args.repeats)

    # === Формирование results.md ===
    md = []
    md.append("# Результаты бенчмарка ГПСЧ rnd128_\n")
    md.append("## Окружение\n")
    md.append(f"- Дата (UTC): {info['date_utc']}")
    md.append(f"- CPU: {info.get('cpu_model','?')}")
    md.append(f"- Физических ядер: {info.get('physical_cores','?')}, "
              f"логических потоков: {info.get('nproc','?')}")
    md.append(f"- Компилятор: `{info['compiler']}`")
    md.append(f"- Флаги: `CXXFLAGS={info['cxxflags']}` `OMPFLAGS={info['ompflags']}`")
    md.append(f"- Повторов на каждый замер: {args.repeats} (берётся минимум времени)")
    md.append("")

    # --- Таблица raw, ms ---
    md.append("## 1. Сырые времена (минимум из повторов), мс\n")
    header = "| N            | baseline   | opt_seq    "
    for t in threads:
        header += f"| par(T={t})  "
    header += "|"
    sep = "|" + "|".join(["-" * 14] + ["-" * 12] * (2 + len(threads))) + "|"
    md.append(header)
    md.append(sep)
    for n in sizes:
        b = raw[(n, "baseline", None)][0]
        s = raw[(n, "opt_seq", None)][0]
        row = f"| {n:<12} |{fmt_ms(b)} |{fmt_ms(s)} "
        for t in threads:
            p = raw[(n, "opt_par", t)][0]
            row += f"|{fmt_ms(p)} "
        row += "|"
        md.append(row)
    md.append("")

    # --- Таблица speedup_opt1 ---
    md.append("## 2. speedup_opt1 = time(baseline) / time(opt_seq)\n")
    md.append("Выигрыш от переписывания арифметики (один поток против одного потока).\n")
    md.append("| N            | baseline, ms | opt_seq, ms  | speedup_opt1 |")
    md.append("|--------------|--------------|--------------|--------------|")
    sp1 = {}
    for n in sizes:
        b = raw[(n, "baseline", None)][0]
        s = raw[(n, "opt_seq", None)][0]
        v = b / s if s > 0 else None
        sp1[n] = v
        md.append(f"| {n:<12} | {b:>12.3f} | {s:>12.3f} | {fmt_speedup(v)}     |")
    md.append("")

    # --- Таблица speedup_par(T) ---
    md.append("## 3. speedup_par(T) = time(opt_seq) / time(opt_par, T)\n")
    md.append("Выигрыш от распараллеливания внутри уже оптимизированной версии.\n")
    header = "| N            | opt_seq, ms  "
    for t in threads:
        header += f"| par(T={t}) ms | sp(T={t}) "
    header += "| T*  | min ms |"
    md.append(header)
    md.append("|" + "|".join(["-" * 14, "-" * 14] +
                              ["-" * 13, "-" * 10] * len(threads) +
                              ["-" * 5, "-" * 8]) + "|")
    best_t = {}
    best_par_ms = {}
    for n in sizes:
        s = raw[(n, "opt_seq", None)][0]
        row = f"| {n:<12} | {s:>12.3f} "
        # Найдём лучшее T.
        best = None
        for t in threads:
            p = raw[(n, "opt_par", t)][0]
            if best is None or p < best[1]:
                best = (t, p)
            sp = s / p if p > 0 else None
            row += f"| {p:>10.3f} | {fmt_speedup(sp)}"
        best_t[n] = best[0]
        best_par_ms[n] = best[1]
        row += f" | T={best[0]:<2}| {best[1]:>6.1f} |"
        md.append(row)
    md.append("")

    # --- Таблица speedup_total ---
    md.append("## 4. speedup_total = time(baseline) / time(opt_par, T*)\n")
    md.append("Конец-в-конец: от исходной версии до оптимизированной + лучший T.\n")
    md.append("| N            | baseline, ms | par(T*), ms  | T*  | speedup_total |")
    md.append("|--------------|--------------|--------------|-----|---------------|")
    for n in sizes:
        b = raw[(n, "baseline", None)][0]
        p = best_par_ms[n]
        t = best_t[n]
        v = b / p if p > 0 else None
        md.append(f"| {n:<12} | {b:>12.3f} | {p:>12.3f} | T={t:<2}| "
                  f"{fmt_speedup(v)}        |")
    md.append("")

    # --- Краткий вывод ---
    sp_opt_avg = statistics.mean([v for v in sp1.values() if v is not None])
    biggest_n = max(sizes)
    sp_total_big = (raw[(biggest_n, "baseline", None)][0]
                    / best_par_ms[biggest_n])
    md.append("## 5. Короткий вывод\n")
    md.append(
        f"- Оптимизация арифметики (отказ от 10-цифровой распаковки в пользу "
        f"двух `uint64_t`) даёт выигрыш ~{sp_opt_avg:.2f}x в одном потоке. "
        f"Видно из таблицы 2.\n"
    )
    md.append(
        f"- При параллельном запуске на N={biggest_n} лучшее время даёт "
        f"T = {best_t[biggest_n]} (видно в таблице 3). На меньших N "
        f"оверхед OpenMP может перекрывать выигрыш — это ожидаемо.\n"
    )
    md.append(
        f"- Суммарное ускорение конец-в-конец на N={biggest_n}: "
        f"~{sp_total_big:.2f}x (таблица 4).\n"
    )

    RESULTS_PATH.write_text("\n".join(md), encoding="utf-8")
    log(f"=== results.md записан в {RESULTS_PATH} ===")

    # Сводка в stdout под конец — чтобы было видно сразу.
    print()
    print("============================================================")
    print(f"  done. Результаты: {RESULTS_PATH}")
    print(f"  Сырые логи:       {LOG_PATH}")
    print("============================================================")


if __name__ == "__main__":
    main()
