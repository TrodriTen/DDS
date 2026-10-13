#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
analyze_results.py — Agregación y comparación de resultados del benchmark DDS.

Entrada: uno o varios directorios; se buscan recursivamente ejecuciones
(directorios con *_summary.csv generados por los binarios del arnés).

Salidas (en --out, por defecto el primer directorio indicado):
  runs.csv         una fila por (ejecución, flujo): pub + sub unidos
                   (pérdida extremo a extremo, RTT, E2E, jitter, deadline, CPU, RSS)
  phases.csv       una fila por (ejecución, flujo, fase): baseline/noise/recovery
                   con percentiles por fase y degradación respecto a baseline
  comparison.csv   mediana (y rango) sobre repeticiones por
                   (escenario, transporte, ruido, flujo, fase, middleware)
  report.md        tablas comparativas Fast DDS vs Cyclone DDS
  aggregate.prom   exposición Prometheus de la comparativa (--pushgateway URL
                   la envía al Pushgateway de Proxmox)
  plots/*.png      con --plots (requiere matplotlib)

Sólo usa la biblioteca estándar de Python (matplotlib es opcional).
"""
from __future__ import annotations

import argparse
import csv
import glob
import json
import math
import os
import re
import statistics
import sys
import urllib.request
from collections import defaultdict

LAT_KEYS = ("mean", "std", "p50", "p95", "p99", "p999", "max")


# ----------------------------------------------------------------- lectura --
def fnum(v):
    """Convierte a float; '' / NaN / ausente -> None."""
    if v is None or v == "":
        return None
    try:
        x = float(v)
    except ValueError:
        return None
    return None if math.isnan(x) else x


def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="") as f:
        return list(csv.DictReader(f))


def find_run_dirs(paths):
    dirs = set()
    for p in paths:
        for f in glob.glob(os.path.join(p, "**", "*_summary.csv"), recursive=True):
            dirs.add(os.path.dirname(f))
    return sorted(dirs)


def load_run(d):
    run = {"dir": d}
    info_path = os.path.join(d, "run_info.json")
    run["info"] = json.load(open(info_path)) if os.path.exists(info_path) else {}
    for role in ("pub", "sub"):
        summ = glob.glob(os.path.join(d, f"{role}_*_summary.csv"))
        phases = glob.glob(os.path.join(d, f"{role}_*_phases.csv"))
        ts = glob.glob(os.path.join(d, f"{role}_*_timeseries.csv"))
        meta = glob.glob(os.path.join(d, f"{role}_*_meta.json"))
        run[f"{role}_summary"] = read_csv(summ[0]) if summ else []
        run[f"{role}_phases"] = read_csv(phases[0]) if phases else []
        run[f"{role}_ts"] = ts[0] if ts else None
        run[f"{role}_meta"] = json.load(open(meta[0])) if meta else {}
    rows = run["pub_summary"] or run["sub_summary"]
    if not rows:
        return None
    r0 = rows[0]
    info = run["info"]
    info.setdefault("run_id", r0.get("run_id"))
    info.setdefault("middleware", r0.get("middleware"))
    info.setdefault("transport", r0.get("transport"))
    info.setdefault("scenario", r0.get("scenario"))
    info.setdefault("noise", "none")
    info.setdefault("noise_target", "")
    info.setdefault("rep", 1)
    return run


# --------------------------------------------------------------- agregación --
def build_runs(runs):
    out = []
    for run in runs:
        info = run["info"]
        pubs = {r["stream"]: r for r in run["pub_summary"]}
        subs = {r["stream"]: r for r in run["sub_summary"]}
        for stream in sorted(set(pubs) | set(subs)):
            p, s = pubs.get(stream, {}), subs.get(stream, {})
            sent, recv = fnum(p.get("sent")), fnum(s.get("received"))
            row = {
                "run_id": info["run_id"], "scenario": info["scenario"], "middleware": info["middleware"],
                "transport": info["transport"], "noise": info["noise"], "noise_target": info.get("noise_target", ""),
                "rep": info.get("rep", 1), "stream": stream,
                "middleware_version": (p or s).get("middleware_version", ""),
                "reliability": (p or s).get("reliability", ""), "history": (p or s).get("history", ""),
                "depth": (p or s).get("depth", ""), "deadline_ms": (p or s).get("deadline_ms", ""),
                "rate_hz": (p or s).get("rate_hz", ""),
                "sent": sent, "received": recv,
                # Pérdida extremo a extremo: incluye la de cola (muestras tras la última vista)
                "loss_pct_e2e": (100.0 * (sent - recv) / sent) if sent and recv is not None else None,
                "lost_seq": fnum(s.get("lost")), "ooo": fnum(s.get("ooo")),
                "msg_per_s": fnum(s.get("msg_per_s")), "mb_per_s": fnum(s.get("mb_per_s")),
            }
            for k in LAT_KEYS:
                row[f"rtt_{k}_us"] = fnum(p.get(f"rtt_{k}_us"))
            row["rtt_net_p50_us"] = fnum(p.get("rtt_net_p50_us"))
            row["rtt_net_p99_us"] = fnum(p.get("rtt_net_p99_us"))
            row["one_way_est_p50_us"] = fnum(p.get("one_way_est_p50_us"))
            for k in LAT_KEYS:
                row[f"e2e_{k}_us"] = fnum(s.get(f"e2e_{k}_us"))
                row[f"e2e_corr_{k}_us"] = fnum(s.get(f"e2e_corr_{k}_us"))
            # Jitter = desviación estándar del retardo (E2E corregida si existe)
            row["jitter_us"] = row["e2e_corr_std_us"] if row["e2e_corr_std_us"] is not None else row["e2e_std_us"]
            row.update({
                "offset_us": fnum(p.get("offset_us")),
                "deadline_missed_sub": fnum(s.get("deadline_missed")),
                "deadline_miss_rate_sub": fnum(s.get("deadline_miss_rate")),
                "deadline_missed_pub": fnum(p.get("deadline_missed")),
                "sample_lost": fnum(s.get("sample_lost")), "sample_rejected": fnum(s.get("sample_rejected")),
                "liveliness_not_alive": fnum(s.get("liveliness_not_alive")),
                "write_errors": fnum(p.get("write_errors")), "overruns": fnum(p.get("overruns")),
                "loans_active": fnum(p.get("loans_active")),
                "cpu_pub_pct": fnum(p.get("cpu_pct_avg")), "cpu_sub_pct": fnum(s.get("cpu_pct_avg")),
                "rss_pub_kb": fnum(p.get("rss_kb")), "rss_sub_kb": fnum(s.get("rss_kb")),
                "invol_ctxsw_sub": fnum(s.get("invol_ctxsw")), "majflt_sub": fnum(s.get("majflt")),
            })
            out.append(row)
    return out


def build_phases(runs):
    out = []
    for run in runs:
        info = run["info"]
        pub = {(r["stream"], r["phase"]): r for r in run["pub_phases"] if r.get("phase") != "warmup"}
        sub = {(r["stream"], r["phase"]): r for r in run["sub_phases"] if r.get("phase") != "warmup"}
        keys = sorted(set(pub) | set(sub), key=lambda k: (k[0], ["baseline", "run", "noise", "recovery"].index(k[1])
                                                           if k[1] in ("baseline", "run", "noise", "recovery") else 9))
        base = {}
        for stream, phase in keys:
            p, s = pub.get((stream, phase), {}), sub.get((stream, phase), {})
            row = {
                "run_id": info["run_id"], "scenario": info["scenario"], "middleware": info["middleware"],
                "transport": info["transport"], "noise": info["noise"], "rep": info.get("rep", 1),
                "stream": stream, "phase": phase,
                "seconds": fnum(s.get("seconds")) or fnum(p.get("seconds")),
                "msgs": fnum(s.get("msgs")), "lost": fnum(s.get("lost")), "loss_pct": fnum(s.get("loss_pct")),
                "msg_per_s": fnum(s.get("msg_per_s")),
                "deadline_missed": fnum(s.get("deadline_missed")),
                "deadline_miss_rate": fnum(s.get("deadline_miss_rate")),
                "cpu_sub_pct": fnum(s.get("cpu_pct_avg")), "cpu_pub_pct": fnum(p.get("cpu_pct_avg")),
            }
            for k in LAT_KEYS:
                row[f"e2e_{k}_us"] = fnum(s.get(f"lat_{k}_us"))
                row[f"e2e_corr_{k}_us"] = fnum(s.get(f"lat2_{k}_us"))
                row[f"rtt_{k}_us"] = fnum(p.get(f"lat_{k}_us"))
            if phase in ("baseline", "run"):
                base[stream] = row
            b = base.get(stream)
            # Degradación respecto a la fase baseline de la misma ejecución
            for m in ("rtt_p99_us", "rtt_p999_us", "e2e_corr_p99_us", "e2e_p99_us"):
                row[f"{m.replace('_us', '')}_ratio"] = (row[m] / b[m]) if b and row[m] and b.get(m) else None
            row["loss_pct_delta"] = (row["loss_pct"] - b["loss_pct"]) if b and row["loss_pct"] is not None \
                and b.get("loss_pct") is not None else None
            out.append(row)
    return out


def median(vals):
    v = [x for x in vals if x is not None]
    return statistics.median(v) if v else None


def build_comparison(rows, by, metrics):
    groups = defaultdict(list)
    for r in rows:
        groups[tuple(r[k] for k in by)].append(r)
    out = []
    for key, items in sorted(groups.items(), key=lambda kv: tuple(str(x) for x in kv[0])):
        row = dict(zip(by, key))
        row["n_runs"] = len(items)
        for m in metrics:
            vals = [x[m] for x in items if x.get(m) is not None]
            row[m] = median(vals)
            row[f"{m}_min"] = min(vals) if vals else None
            row[f"{m}_max"] = max(vals) if vals else None
        out.append(row)
    return out


# ------------------------------------------------------------------ salidas --
def write_csv(path, rows):
    if not rows:
        return
    keys = list(rows[0].keys())
    for r in rows[1:]:
        for k in r:
            if k not in keys:
                keys.append(k)
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=keys)
        w.writeheader()
        for r in rows:
            w.writerow({k: ("" if r.get(k) is None else (f"{r[k]:.4f}" if isinstance(r.get(k), float) else r[k]))
                        for k in keys})


def fmt(v, d=1):
    return "–" if v is None else f"{v:.{d}f}"


def write_report(path, runs_cmp, phase_cmp):
    L = ["# Comparativa Fast DDS vs Cyclone DDS", "",
         "Valores = mediana sobre repeticiones (rango min–max entre corchetes para p99).",
         "Latencias en µs. RTT medido en el reloj MONOTONIC del publicador; E2E corregida = E2E − θ "
         "(offset de reloj estimado por intercambio tipo NTP sobre el eco).", ""]
    groups = defaultdict(list)
    for r in runs_cmp:
        groups[(r["scenario"], r["transport"], r["noise"])].append(r)
    for (scen, tr, noise), rows in sorted(groups.items()):
        L += [f"## Escenario `{scen}` · transporte `{tr}` · ruido `{noise}`", "",
              "| flujo | middleware | n | RTT p50 | RTT p99 [rango] | RTT p99.9 | E2E corr p50 | E2E corr p99 | "
              "jitter σ | pérdida % | deadline miss rate | CPU pub % | CPU sub % | RSS sub MiB |",
              "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in sorted(rows, key=lambda x: (x["stream"], x["middleware"])):
            rng = f"[{fmt(r['rtt_p99_us_min'])}–{fmt(r['rtt_p99_us_max'])}]"
            rss = r["rss_sub_kb"] / 1024 if r["rss_sub_kb"] else None
            L.append(f"| {r['stream']} | {r['middleware']} | {r['n_runs']} | {fmt(r['rtt_p50_us'])} | "
                     f"{fmt(r['rtt_p99_us'])} {rng} | {fmt(r['rtt_p999_us'])} | {fmt(r['e2e_corr_p50_us'])} | "
                     f"{fmt(r['e2e_corr_p99_us'])} | {fmt(r['jitter_us'])} | {fmt(r['loss_pct_e2e'], 3)} | "
                     f"{fmt(r['deadline_miss_rate_sub'], 4)} | {fmt(r['cpu_pub_pct'])} | {fmt(r['cpu_sub_pct'])} | "
                     f"{fmt(rss)} |")
        L.append("")
    if phase_cmp:
        L += ["## Degradación por fase (ruido iperf3)", "",
              "| escenario | transporte | ruido | flujo | middleware | fase | E2E corr p99 | ×baseline | RTT p99 | "
              "×baseline | pérdida % | deadline miss rate |",
              "|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in phase_cmp:
            L.append(f"| {r['scenario']} | {r['transport']} | {r['noise']} | {r['stream']} | {r['middleware']} | "
                     f"{r['phase']} | {fmt(r['e2e_corr_p99_us'])} | {fmt(r['e2e_corr_p99_ratio'], 2)} | "
                     f"{fmt(r['rtt_p99_us'])} | {fmt(r['rtt_p99_ratio'], 2)} | {fmt(r['loss_pct'], 3)} | "
                     f"{fmt(r['deadline_miss_rate'], 4)} |")
        L.append("")
    with open(path, "w") as f:
        f.write("\n".join(L))


def to_prometheus(runs_cmp, phase_cmp):
    lines = []
    metrics = ["rtt_p50_us", "rtt_p99_us", "rtt_p999_us", "e2e_corr_p50_us", "e2e_corr_p99_us", "jitter_us",
               "loss_pct_e2e", "deadline_miss_rate_sub", "cpu_pub_pct", "cpu_sub_pct", "rss_sub_kb", "msg_per_s"]
    for m in metrics:
        name = f"dds_bench_cmp_{m}"
        lines.append(f"# TYPE {name} gauge")
        for r in runs_cmp:
            if r.get(m) is None:
                continue
            lab = ",".join(f'{k}="{r[k]}"' for k in ("scenario", "transport", "noise", "stream", "middleware"))
            lines.append(f"{name}{{{lab}}} {r[m]}")
    for m in ("e2e_corr_p99_us", "rtt_p99_us", "loss_pct", "deadline_miss_rate", "e2e_corr_p99_ratio"):
        name = f"dds_bench_phase_{m}"
        lines.append(f"# TYPE {name} gauge")
        for r in phase_cmp:
            if r.get(m) is None:
                continue
            lab = ",".join(f'{k}="{r[k]}"' for k in ("scenario", "transport", "noise", "stream", "middleware", "phase"))
            lines.append(f"{name}{{{lab}}} {r[m]}")
    return "\n".join(lines) + "\n"


def push(url, text, job="dds_bench_analysis"):
    req = urllib.request.Request(f"{url.rstrip('/')}/metrics/job/{job}", data=text.encode(), method="PUT",
                                 headers={"Content-Type": "text/plain; version=0.0.4"})
    with urllib.request.urlopen(req, timeout=5) as resp:
        return resp.status


def plots(runs, out_dir):
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("matplotlib no disponible: se omiten las gráficas", file=sys.stderr)
        return
    os.makedirs(out_dir, exist_ok=True)
    colors = {"baseline": "#d8e8d0", "noise": "#f4c7c3", "recovery": "#d0dcf0"}
    for run in runs:
        ts = read_csv(run["sub_ts"]) if run.get("sub_ts") else []
        if not ts:
            continue
        fig, ax = plt.subplots(figsize=(11, 4))
        t0 = fnum(ts[0]["t_wall_s"])
        for stream in sorted({r["stream"] for r in ts}):
            pts = [(fnum(r["t_wall_s"]) - t0, fnum(r["e2e_corr_p99_us"])) for r in ts
                   if r["stream"] == stream and fnum(r["e2e_corr_p99_us"]) is not None]
            if pts:
                ax.plot(*zip(*pts), label=f"{stream} E2E corr p99")
        # sombreado de fases
        prev, start = None, None
        for r in ts:
            t = fnum(r["t_wall_s"]) - t0
            if r["phase"] != prev:
                if prev in colors and start is not None:
                    ax.axvspan(start, t, color=colors[prev], alpha=0.5, lw=0)
                prev, start = r["phase"], t
        if prev in colors and start is not None:
            ax.axvspan(start, t, color=colors[prev], alpha=0.5, lw=0)
        ax.set_yscale("log")
        ax.set_xlabel("tiempo (s)")
        ax.set_ylabel("µs")
        ax.set_title(run["info"]["run_id"])
        ax.legend(loc="upper left", fontsize=8)
        fig.tight_layout()
        fig.savefig(os.path.join(out_dir, f"{run['info']['run_id']}_e2e_p99.png"), dpi=110)
        plt.close(fig)


# --------------------------------------------------------------------- main --
def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+", help="directorios de resultados (se buscan ejecuciones recursivamente)")
    ap.add_argument("--out", help="directorio de salida (por defecto el primero de paths)")
    ap.add_argument("--pushgateway", help="URL del Pushgateway (p.ej. http://proxmox:9091)")
    ap.add_argument("--plots", action="store_true", help="generar gráficas PNG (matplotlib)")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()

    out = a.out or a.paths[0]
    os.makedirs(out, exist_ok=True)
    runs = [r for r in (load_run(d) for d in find_run_dirs(a.paths)) if r]
    if not runs:
        print("no se encontraron ejecuciones (*_summary.csv)", file=sys.stderr)
        return 1

    runs_rows = build_runs(runs)
    phase_rows = build_phases(runs)
    cmp_metrics = ["sent", "received", "loss_pct_e2e", "msg_per_s", "mb_per_s", "jitter_us", "offset_us",
                   "deadline_miss_rate_sub", "deadline_missed_sub", "sample_lost", "write_errors",
                   "cpu_pub_pct", "cpu_sub_pct", "rss_pub_kb", "rss_sub_kb", "one_way_est_p50_us"] + \
                  [f"rtt_{k}_us" for k in LAT_KEYS] + [f"e2e_corr_{k}_us" for k in LAT_KEYS] + \
                  [f"e2e_{k}_us" for k in ("p50", "p99")]
    runs_cmp = build_comparison(runs_rows, ("scenario", "transport", "noise", "stream", "middleware"), cmp_metrics)
    phase_cmp = build_comparison(phase_rows, ("scenario", "transport", "noise", "stream", "middleware", "phase"),
                                 ["e2e_corr_p50_us", "e2e_corr_p99_us", "e2e_corr_p999_us", "rtt_p50_us",
                                  "rtt_p99_us", "loss_pct", "deadline_miss_rate", "e2e_corr_p99_ratio",
                                  "rtt_p99_ratio", "cpu_sub_pct", "msg_per_s"])

    write_csv(os.path.join(out, "runs.csv"), runs_rows)
    write_csv(os.path.join(out, "phases.csv"), phase_rows)
    write_csv(os.path.join(out, "comparison.csv"), runs_cmp)
    write_csv(os.path.join(out, "comparison_phases.csv"), phase_cmp)
    write_report(os.path.join(out, "report.md"), runs_cmp, phase_cmp)
    prom = to_prometheus(runs_cmp, phase_cmp)
    with open(os.path.join(out, "aggregate.prom"), "w") as f:
        f.write(prom)
    if a.pushgateway:
        try:
            push(a.pushgateway, prom)
            # Resúmenes por ejecución generados por los binarios
            for run in runs:
                for pf in glob.glob(os.path.join(run["dir"], "*_summary.prom")):
                    inst = re.sub(r"[^A-Za-z0-9_.-]", "_", f"{run['info']['run_id']}_{os.path.basename(pf)[:-12]}")
                    with open(pf) as f:
                        push(a.pushgateway, f.read(), job=f"dds_bench_run/instance/{inst}")
        except Exception as e:  # noqa: BLE001 — el push es opcional
            print(f"pushgateway: {e}", file=sys.stderr)
    if a.plots:
        plots(runs, os.path.join(out, "plots"))
    if not a.quiet:
        print(f"{len(runs)} ejecuciones, {len(runs_rows)} filas -> {out}/{{runs,phases,comparison}}.csv, report.md")
        with open(os.path.join(out, "report.md")) as f:
            print(f.read())
    return 0


if __name__ == "__main__":
    sys.exit(main())
