"""
WCET Extreme Value Analysis
===========================
Reads timing_log.txt (format: "task_name execution_time_us") and performs
Extreme Value Analysis (EVA) per task using the pyextremes library.

Usage:
    python3 wcet_eva.py --input timing_log.txt

Features:
  - GEV fitting via Block Maxima method
  - Initial artifact filtering (IQR/percentile/MAD) before data splitting
  - Per-task diagnostic and return-level plots
  - Markdown summary report

Dependencies (pip3):
    pyextremes matplotlib pandas numpy
"""

import argparse
import csv
import os
import sys
import warnings
from collections import defaultdict

import numpy as np
import pandas as pd
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

warnings.filterwarnings("ignore", category=UserWarning)

RETURN_PERIODS = [100, 1000, 10_000, 100_000, 1_000_000]
DEFAULT_TUNE_RETURN_PERIOD = 1_000_000


# ---------------------------------------------------------------------------
# 1. Parse timing_log.txt
# ---------------------------------------------------------------------------
def parse_timing_log(path):
    """Return dict: task_name -> list of duration_us (int)."""
    data = defaultdict(list)
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            parts = line.rsplit(None, 1)
            if len(parts) != 2:
                print(f"  [warn] line {lineno} malformed, skipped: {line}")
                continue
            name, val_str = parts
            try:
                val = int(val_str)
            except ValueError:
                print(f"  [warn] line {lineno} non-integer value, skipped: {line}")
                continue
            data[name].append(val)
    return dict(data)


# ---------------------------------------------------------------------------
# 2. Initial artifact filtering
# ---------------------------------------------------------------------------
def filter_outliers(vals, tail_percentile=90.0, iqr_multiplier=1.5,
                    mad_z=8.0):
    """
    Remove measurement artifacts before train/test splitting.

    These artifacts are caused by file writes or frequent print calls and are
    not considered part of the task execution-time distribution.

    Strategy: use a conservative upper-tail threshold:
        threshold = max(Q3 + iqr_multiplier * IQR, P<tail_percentile>)

    If MAD is non-zero, also compute a robust modified-z threshold and use the
    larger of the two thresholds. This keeps the real WCET right tail while
    removing isolated instrumentation spikes.

    Returns (filtered_vals, n_removed).
    """
    n_before = len(vals)
    if n_before < 100:
        # Too few samples to compute meaningful tail statistics; keep all
        return vals, 0

    q1, q3 = np.percentile(vals, [25, 75])
    iqr = q3 - q1
    tail_value = np.percentile(vals, tail_percentile)

    iqr_threshold = q3 + iqr_multiplier * iqr
    threshold = max(iqr_threshold, tail_value)

    median = np.median(vals)
    mad = np.median(np.abs(np.asarray(vals) - median))
    if mad > 0:
        mad_threshold = median + (mad_z / 0.6745) * mad
        threshold = max(threshold, mad_threshold)
    else:
        mad_threshold = None

    filtered = [v for v in vals if v <= threshold]
    n_removed = n_before - len(filtered)

    if n_removed > 0:
        print(f"  [filter] Q1={q1:.2f}, Q3={q3:.2f}, IQR={iqr:.2f}, "
              f"P{tail_percentile:g}={tail_value:.2f}, "
              f"MAD={mad:.2f}, "
              f"threshold={threshold:.2f} "
              f"(max of Q3+{iqr_multiplier:g}×IQR, "
              f"P{tail_percentile:g}"
              f"{', MAD-z' if mad_threshold is not None else ''}), "
              f"removed {n_removed}/{n_before} outliers "
              f"(max kept={max(filtered):,})")
    return filtered, n_removed


def clean_all_tasks(data, no_filter=False, tail_percentile=95.0,
                    iqr_multiplier=1.5, mad_z=8.0):
    """Apply initial artifact filtering per task before any data split."""
    if no_filter:
        return data, {}

    cleaned = {}
    report = {}
    for task, vals in data.items():
        filtered_vals, n_removed = filter_outliers(
            vals,
            tail_percentile=tail_percentile,
            iqr_multiplier=iqr_multiplier,
            mad_z=mad_z,
        )
        cleaned[task] = filtered_vals
        report[task] = {
            "before": len(vals),
            "after": len(filtered_vals),
            "removed": n_removed,
        }
    return cleaned, report


def build_series(vals):
    """Build pyextremes-compatible synthetic 1 Hz time series."""
    index = pd.date_range(pd.Timestamp("2025-01-01"), periods=len(vals), freq="1s")
    return pd.Series(vals, index=index, name="duration_us")


def fit_gev_model(vals, block_seconds):
    """Fit a BM/GEV model and return (model, extremes, reason)."""
    try:
        import pyextremes as pe
    except ImportError:
        return None, None, "pyextremes not installed"

    if len(vals) < max(10, block_seconds * 3):
        return None, None, "too few samples for block size"

    series = build_series(vals)
    model = pe.EVA(series)
    model.get_extremes(method="BM", block_size=f"{block_seconds}s")

    extremes = np.asarray(model.extremes.values, dtype=float)
    n_unique_extremes = len(np.unique(extremes))
    if len(extremes) < 10:
        return None, extremes, "too few block maxima"
    if n_unique_extremes < 3 or np.isclose(np.std(extremes), 0.0):
        return None, extremes, "degenerate block maxima"

    try:
        model.fit_model()
    except Exception as exc:
        return None, extremes, f"fit failed: {exc}"

    return model, extremes, None


def empirical_block_maxima(vals, block_seconds):
    """Return block maxima computed directly from the sample array."""
    maxima = []
    for start in range(0, len(vals), block_seconds):
        block = vals[start:start + block_seconds]
        if block:
            maxima.append(max(block))
    return np.asarray(maxima, dtype=float)


def fit_scipy_gev(extremes):
    """Fit scipy's GEV distribution to block maxima."""
    from scipy.stats import genextreme

    extremes = np.asarray(extremes, dtype=float)
    if len(extremes) < 20:
        return None
    if len(np.unique(extremes)) < 3 or np.isclose(np.std(extremes), 0.0):
        return None

    try:
        c, loc, scale = genextreme.fit(extremes)
    except Exception:
        return None
    if not all(np.isfinite([c, loc, scale])) or scale <= 0:
        return None
    return c, loc, scale


def chi_square_gev_pvalue(extremes, params):
    """
    Pearson chi-square goodness-of-fit test for fitted GEV.

    Uses equal-probability bins under the fitted distribution and keeps the
    expected count per bin at roughly >= 5.
    """
    from scipy.stats import chisquare, genextreme

    extremes = np.asarray(extremes, dtype=float)
    n = len(extremes)
    if params is None or n < 20:
        return None

    max_bins = max(3, n // 5)
    bins = min(max(5, int(np.sqrt(n))), max_bins, 20)
    if bins < 3:
        return None

    c, loc, scale = params
    probs = np.linspace(0, 1, bins + 1)
    internal_edges = genextreme.ppf(probs[1:-1], c, loc=loc, scale=scale)
    if not np.all(np.isfinite(internal_edges)):
        return None

    edges = np.concatenate(([-np.inf], internal_edges, [np.inf]))
    observed, _ = np.histogram(extremes, bins=edges)
    expected = np.full(bins, n / bins)

    try:
        stat, pvalue = chisquare(observed, expected)
    except Exception:
        return None
    if not np.isfinite(pvalue):
        return None
    return float(pvalue)


def plot_ratio_curve(x_values, ratios, counts, total, xlabel, ylabel, title, path,
                     x_tick_labels=None):
    """Plot a paper-style percentage curve with count-aware annotations."""
    fig, ax = plt.subplots(figsize=(7.2, 4.2))
    x_positions = np.arange(len(x_values)) if x_tick_labels is not None else x_values
    ax.plot(
        x_positions,
        ratios,
        marker="o",
        markersize=5,
        linewidth=2,
        color="#2a7fb8",
        label="WCET",
    )

    for x, ratio, count in zip(x_positions, ratios, counts):
        ax.annotate(
            f"{ratio:.1f}%\n({count}/{total})",
            (x, ratio),
            textcoords="offset points",
            xytext=(0, -18 if ratio > 95 else 8),
            ha="center",
            fontsize=8,
            color="#2a7fb8",
            fontweight="bold",
        )

    ax.set_xlabel(xlabel, fontweight="bold")
    ax.set_ylabel(ylabel, fontweight="bold")
    ax.set_title(title, fontweight="bold")
    lower = max(0, min(ratios) - 10) if ratios else 0
    ax.set_ylim(lower, 105)
    ax.grid(True, alpha=0.25)
    ax.legend(loc="lower right")
    if x_tick_labels is not None:
        ax.set_xticks(x_positions)
        ax.set_xticklabels(x_tick_labels)
    fig.tight_layout()
    fig.savefig(path, dpi=300)
    plt.close(fig)


def run_param_selection_experiment(fit_data, validation_data, out_dir,
                                   no_filter=False, tail_percentile=99.5,
                                   k_min=40, k_max=300,
                                   k_step=10, alphas=None):
    """
    Reproduce the paper-style k/alpha selection experiment and plots.

    fit_data and validation_data are dicts of task_name -> [duration_us].
    GEV is fitted on fit_data; coverage is validated on validation_data.
    """
    if alphas is None:
        alphas = [0.99, 0.995, 0.999, 0.9995, 0.9999]

    os.makedirs(out_dir, exist_ok=True)

    common_tasks = sorted(set(fit_data.keys()) & set(validation_data.keys()))
    if len(common_tasks) == 0:
        print("[ERROR] No common tasks between fit and validation data.")
        return

    tasks = list(common_tasks)

    total_tasks = len(tasks)

    k_values = list(range(k_min, k_max + 1, k_step))
    k_rows = []
    selected_k = None
    best_k = None
    best_k_count = -1

    fitted_by_k = {}
    for k in k_values:
        success_count = 0
        task_results = {}
        for task in tasks:
            extremes = empirical_block_maxima(fit_data[task], k)
            params = fit_scipy_gev(extremes)
            pvalue = chi_square_gev_pvalue(extremes, params)
            success = pvalue is not None and pvalue > 0.05
            if success:
                success_count += 1
            task_results[task] = {
                "params": params,
                "pvalue": pvalue,
                "success": success,
                "extremes": len(extremes),
            }
            k_rows.append({
                "task": task,
                "k": k,
                "n_extremes": len(extremes),
                "pvalue": "" if pvalue is None else pvalue,
                "success": success,
            })

        fitted_by_k[k] = task_results
        if success_count > best_k_count:
            best_k_count = success_count
            best_k = k
        if success_count == total_tasks and selected_k is None:
            selected_k = k

    if selected_k is None:
        selected_k = best_k
        print(f"  [experiment] no k fits all tasks; using best k={selected_k} "
              f"({best_k_count}/{total_tasks})")
    else:
        print(f"  [experiment] selected k={selected_k} "
              f"({total_tasks}/{total_tasks})")

    k_counts = [
        sum(1 for result in fitted_by_k[k].values() if result["success"])
        for k in k_values
    ]
    k_ratios = [count / total_tasks * 100.0 for count in k_counts]

    k_csv = os.path.join(out_dir, "k_fit_results.csv")
    with open(k_csv, "w", newline="") as f:
        writer = csv.DictWriter(
            f, fieldnames=["task", "k", "n_extremes", "pvalue", "success"]
        )
        writer.writeheader()
        writer.writerows(k_rows)

    k_plot = os.path.join(out_dir, "gev_k_fit_ratio.png")
    plot_ratio_curve(
        k_values,
        k_ratios,
        k_counts,
        total_tasks,
        xlabel="GEV Window Size (k)",
        ylabel="Fitted Tasks (%)",
        title="Tasks Successfully Fitting the GEV Model",
        path=k_plot,
    )

    alpha_rows = []
    alpha_counts = []
    selected_alpha = None
    best_alpha = None
    best_alpha_count = -1

    for alpha in alphas:
        covered_count = 0
        for task in tasks:
            extremes = empirical_block_maxima(fit_data[task], selected_k)
            params = fit_scipy_gev(extremes)
            wcet_alpha = ""
            covered = False
            validation_max = max(validation_data[task])
            if params is not None:
                from scipy.stats import genextreme

                c, loc, scale = params
                value = genextreme.ppf(alpha, c, loc=loc, scale=scale)
                if np.isfinite(value):
                    wcet_alpha = float(value)
                    covered = validation_max <= wcet_alpha
            if covered:
                covered_count += 1
            alpha_rows.append({
                "task": task,
                "k": selected_k,
                "alpha": alpha,
                "wcet_alpha": wcet_alpha,
                "validation_max": validation_max,
                "covered": covered,
            })

        alpha_counts.append(covered_count)
        if covered_count > best_alpha_count:
            best_alpha_count = covered_count
            best_alpha = alpha
        if covered_count == total_tasks and selected_alpha is None:
            selected_alpha = alpha

    if selected_alpha is None:
        selected_alpha = best_alpha
        print(f"  [experiment] no alpha covers all tasks; using best alpha="
              f"{selected_alpha:g} ({best_alpha_count}/{total_tasks})")
    else:
        print(f"  [experiment] selected alpha={selected_alpha:g} "
              f"({total_tasks}/{total_tasks})")

    alpha_ratios = [count / total_tasks * 100.0 for count in alpha_counts]
    alpha_csv = os.path.join(out_dir, "alpha_coverage_results.csv")
    with open(alpha_csv, "w", newline="") as f:
        writer = csv.DictWriter(
            f,
            fieldnames=[
                "task", "k", "alpha", "wcet_alpha", "validation_max", "covered"
            ],
        )
        writer.writeheader()
        writer.writerows(alpha_rows)

    alpha_plot = os.path.join(out_dir, "gev_alpha_coverage_ratio.png")
    alpha_labels = [f"{alpha:g}" for alpha in alphas]
    plot_ratio_curve(
        alphas,
        alpha_ratios,
        alpha_counts,
        total_tasks,
        xlabel="Confidence Level",
        ylabel="Tasks within Estimated WCET (%)",
        title="Validation Tasks Covered by WCET_alpha",
        path=alpha_plot,
        x_tick_labels=alpha_labels,
    )

    summary_path = os.path.join(out_dir, "param_selection_summary.md")
    with open(summary_path, "w") as f:
        f.write("# GEV Parameter Selection Summary\n\n")
        f.write(f"- Fit tasks: {total_tasks}\n")
        f.write(f"- Fit/validation from separate files\n")
        f.write(f"- Selected k: {selected_k}\n")
        f.write(f"- Selected alpha: {selected_alpha:g}\n")
        f.write(f"- k plot: `{os.path.basename(k_plot)}`\n")
        f.write(f"- alpha plot: `{os.path.basename(alpha_plot)}`\n")

    print(f"  k results CSV → {k_csv}")
    print(f"  alpha results CSV → {alpha_csv}")
    print(f"  k plot → {k_plot}")
    print(f"  alpha plot → {alpha_plot}")
    print(f"  summary → {summary_path}")


def run_per_task_param_table(fit_data, validation_data, out_dir,
                             no_filter=False, tail_percentile=99.5,
                             k_min=40, k_max=300,
                             k_step=10, alphas=None):
    """
    Select k and alpha independently for each task and write a table.

    For each task:
      1. GEV is fitted on fit_data; validation is on validation_data.
      2. Select the smallest k whose chi-square p-value is > 0.05.
      3. If none pass, select the fitted k with the largest p-value and mark it.
      4. Select the smallest alpha whose WCET_alpha covers validation samples.
      5. If none cover, keep the largest alpha and mark it.
    """
    if alphas is None:
        alphas = [0.99, 0.995, 0.999, 0.9995, 0.9999]

    from scipy.stats import genextreme

    os.makedirs(out_dir, exist_ok=True)
    rows = []
    detail_rows = []

    common_tasks = sorted(set(fit_data.keys()) & set(validation_data.keys()))
    if len(common_tasks) == 0:
        print("[ERROR] No common tasks between fit and validation data.")
        return

    for task in common_tasks:
        fit_vals = fit_data[task]
        validation_vals = validation_data[task]
        original_samples = len(fit_vals)

        selected = None
        best_effort = None
        best_effort_score = None

        for k in range(k_min, k_max + 1, k_step):
            extremes = empirical_block_maxima(fit_vals, k)
            params = fit_scipy_gev(extremes)
            pvalue = chi_square_gev_pvalue(extremes, params)
            success = pvalue is not None and pvalue > 0.05

            detail_rows.append({
                "task": task,
                "k": k,
                "n_extremes": len(extremes),
                "pvalue": "" if pvalue is None else pvalue,
                "success": success,
            })

            if params is not None and pvalue is not None:
                score = (pvalue, -k)
                if best_effort_score is None or score > best_effort_score:
                    best_effort_score = score
                    best_effort = {
                        "k": k,
                        "params": params,
                        "pvalue": pvalue,
                        "n_extremes": len(extremes),
                    }

            if success and selected is None:
                selected = {
                    "k": k,
                    "params": params,
                    "pvalue": pvalue,
                    "n_extremes": len(extremes),
                    "k_status": "passed",
                }

        if selected is None:
            if best_effort is None:
                rows.append({
                    "task": task,
                    "status": "skipped",
                    "reason": "no valid GEV fit for any k",
                    "original_samples": original_samples,
                    "samples": len(fit_vals),
                    "fit_samples": len(fit_vals),
                    "validation_samples": len(validation_vals),
                })
                continue
            selected = {
                **best_effort,
                "k_status": "best_effort_pvalue",
            }

        validation_max = max(validation_vals)
        chosen_alpha = None
        chosen_wcet = None
        alpha_status = "covered"

        c, loc, scale = selected["params"]
        for alpha in alphas:
            wcet_alpha = genextreme.ppf(alpha, c, loc=loc, scale=scale)
            if not np.isfinite(wcet_alpha):
                continue
            if validation_max <= wcet_alpha:
                chosen_alpha = alpha
                chosen_wcet = float(wcet_alpha)
                break

        if chosen_alpha is None:
            alpha_status = "not_covered"
            for alpha in reversed(alphas):
                wcet_alpha = genextreme.ppf(alpha, c, loc=loc, scale=scale)
                if np.isfinite(wcet_alpha):
                    chosen_alpha = alpha
                    chosen_wcet = float(wcet_alpha)
                    break

        if chosen_alpha is None:
            rows.append({
                "task": task,
                "status": "skipped",
                "reason": "invalid WCET_alpha for all alpha values",
                "original_samples": original_samples,
                "samples": len(fit_vals),
                "fit_samples": len(fit_vals),
                "validation_samples": len(validation_vals),
                "k": selected["k"],
                "k_pvalue": selected["pvalue"],
                "k_status": selected["k_status"],
            })
            continue

        validation_exceed = sum(v > chosen_wcet for v in validation_vals)
        rows.append({
            "task": task,
            "status": "ok" if selected["k_status"] == "passed" and alpha_status == "covered" else "warning",
            "reason": "" if selected["k_status"] == "passed" and alpha_status == "covered"
                      else f"{selected['k_status']}; {alpha_status}",
            "original_samples": original_samples,
            "samples": len(fit_vals),
            "fit_samples": len(fit_vals),
            "validation_samples": len(validation_vals),
            "k": selected["k"],
            "n_extremes": selected["n_extremes"],
            "k_pvalue": selected["pvalue"],
            "k_status": selected["k_status"],
            "alpha": chosen_alpha,
            "alpha_status": alpha_status,
            "wcet_alpha": chosen_wcet,
            "fit_max": max(fit_vals),
            "validation_max": validation_max,
            "validation_exceed": validation_exceed,
            "validation_exceed_rate": validation_exceed / max(1, len(validation_vals)),
        })
    csv_path = os.path.join(out_dir, "per_task_k_alpha_table.csv")
    columns = [
        "task", "status", "reason", "original_samples", "samples",
        "filtered", "fit_samples", "validation_samples", "k", "n_extremes",
        "k_pvalue", "k_status", "alpha", "alpha_status", "wcet_alpha",
        "fit_max", "validation_max", "validation_exceed",
        "validation_exceed_rate",
    ]
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        for row in rows:
            writer.writerow({col: row.get(col, "") for col in columns})

    detail_csv_path = os.path.join(out_dir, "per_task_k_fit_details.csv")
    with open(detail_csv_path, "w", newline="") as f:
        writer = csv.DictWriter(
            f, fieldnames=["task", "k", "n_extremes", "pvalue", "success"]
        )
        writer.writeheader()
        writer.writerows(detail_rows)

    md_path = os.path.join(out_dir, "per_task_k_alpha_table.md")
    lines = [
        "# Per-Task k and Alpha Selection\n",
        "| Task | Status | k | p-value | alpha | WCET_alpha (us) | Fit Max (us) | Validation Max (us) | Validation Exceed |",
        "|------|--------|---|---------|-------|-----------------|--------------|---------------------|-------------------|",
    ]
    for row in rows:
        if row.get("status") == "skipped":
            lines.append(
                f"| {row['task']} | skipped: {row.get('reason', '')} | | | | | | | |"
            )
            continue
        lines.append(
            f"| {row['task']} | {row['status']} | {row['k']} "
            f"| {row['k_pvalue']:.4g} | {row['alpha']:g} "
            f"| {row['wcet_alpha']:,.0f} | {row['fit_max']:,.0f} "
            f"| {row['validation_max']:,.0f} "
            f"| {row['validation_exceed']}/{row['validation_samples']} |"
        )
    with open(md_path, "w") as f:
        f.write("\n".join(lines) + "\n")

    print(f"  per-task table CSV → {csv_path}")
    print(f"  per-task k details CSV → {detail_csv_path}")
    print(f"  per-task table Markdown → {md_path}")


def safe_return_value(model, return_period, alpha=None, n_samples=50):
    """Return scalar (value, lower, upper), or (None, None, None) on failure."""
    try:
        kwargs = {"n_samples": n_samples} if alpha is not None else {}
        values, lower, upper = model.get_return_value(
            [return_period], alpha=alpha, **kwargs
        )
    except Exception:
        return None, None, None

    value = float(np.asarray(values).reshape(-1)[0])
    lower_value = None if lower is None else float(np.asarray(lower).reshape(-1)[0])
    upper_value = None if upper is None else float(np.asarray(upper).reshape(-1)[0])
    if not np.isfinite(value):
        return None, None, None
    if upper_value is not None and not np.isfinite(upper_value):
        return None, None, None
    return value, lower_value, upper_value


def tune_task(task_name, fit_vals, test_vals,
              block_min=25, block_max=300, block_step=1,
              alpha_min=97.0, alpha_max=99.0, alpha_step=0.2,
              return_period=DEFAULT_TUNE_RETURN_PERIOD, ci_samples=50):
    """
    Tune block size and confidence level for one task.

    fit_vals is the training set (for GEV fitting), test_vals is the
    external validation set (for selection and final evaluation).
    """
    n_fit = len(fit_vals)
    if len(test_vals) < 1:
        return {
            "task": task_name,
            "status": "skipped",
            "reason": "no test samples",
            "samples": n_fit,
        }

    best_block = None
    best_block_model = None
    best_block_value = None
    best_block_score = None
    best_fallback = None
    best_fallback_score = None
    valid_candidates = 0

    for block_seconds in range(block_min, block_max + 1, block_step):
        model, extremes, reason = fit_gev_model(fit_vals, block_seconds)
        if model is None:
            if extremes is None:
                extremes = empirical_block_maxima(fit_vals, block_seconds)
            if extremes is not None and len(extremes) > 0:
                value = float(np.max(extremes))
                n_unique = len(np.unique(extremes))
                test_exceed_cnt = sum(v > value for v in test_vals)
                test_rate = test_exceed_cnt / max(1, len(test_vals))
                score = (
                    1 if test_exceed_cnt > 0 else 0,
                    test_rate,
                    -n_unique,
                    value,
                    block_seconds,
                )
                if best_fallback_score is None or score < best_fallback_score:
                    best_fallback_score = score
                    best_fallback = {
                        "block_seconds": block_seconds,
                        "value": value,
                        "reason": reason,
                        "extremes": len(extremes),
                        "unique_extremes": n_unique,
                    }
            continue

        value, _, _ = safe_return_value(model, return_period)
        if value is None:
            if extremes is not None and len(extremes) > 0:
                fallback_value = float(np.max(extremes))
                n_unique = len(np.unique(extremes))
                test_exceed_cnt = sum(v > fallback_value for v in test_vals)
                test_rate = test_exceed_cnt / max(1, len(test_vals))
                score = (
                    1 if test_exceed_cnt > 0 else 0,
                    test_rate,
                    -n_unique,
                    fallback_value,
                    block_seconds,
                )
                if best_fallback_score is None or score < best_fallback_score:
                    best_fallback_score = score
                    best_fallback = {
                        "block_seconds": block_seconds,
                        "value": fallback_value,
                        "reason": "return value failed",
                        "extremes": len(extremes),
                        "unique_extremes": n_unique,
                    }
            continue

        valid_candidates += 1
        test_exceed_cnt = sum(v > value for v in test_vals)
        test_rate = test_exceed_cnt / max(1, len(test_vals))
        conservatism = value / max(1.0, np.percentile(test_vals, 99.5))

        score = (
            1 if test_exceed_cnt > 0 else 0,
            test_rate,
            conservatism,
            value,
            -float(model.model.loglikelihood),
        )
        if best_block_score is None or score < best_block_score:
            best_block_score = score
            best_block = block_seconds
            best_block_model = model
            best_block_value = value

    if best_block_model is None:
        if best_fallback is None:
            return {
                "task": task_name,
                "status": "skipped",
                "reason": "no valid block-size candidate",
                "samples": n_fit,
            }

        fallback_value = max(best_fallback["value"], max(fit_vals))
        test_n = len(test_vals)
        test_exceed = sum(v > fallback_value for v in test_vals)
        return {
            "task": task_name,
            "status": "fallback",
            "reason": best_fallback["reason"],
            "samples": n_fit,
            "test_samples": test_n,
            "block_seconds": best_fallback["block_seconds"],
            "valid_block_candidates": valid_candidates,
            "alpha_pct": float(alpha_min),
            "return_period": return_period,
            "model": "EmpiricalFallback",
            "loglikelihood": float("nan"),
            "extremes": best_fallback["extremes"],
            "unique_extremes": best_fallback["unique_extremes"],
            "wcet_point": float(fallback_value),
            "ci_lower": "",
            "ci_upper": float(fallback_value),
            "test_max": max(test_vals),
            "test_point_exceed": test_exceed,
            "test_upper_exceed": test_exceed,
            "test_point_exceed_rate": test_exceed / max(1, test_n),
            "test_upper_exceed_rate": test_exceed / max(1, test_n),
        }

    alpha_values = np.arange(alpha_min, alpha_max + alpha_step / 2.0, alpha_step)
    best_alpha = None
    best_upper = None
    best_alpha_score = None
    best_lower = None

    for alpha_pct in alpha_values:
        alpha = float(alpha_pct / 100.0)
        value, lower, upper = safe_return_value(
            best_block_model, return_period, alpha=alpha, n_samples=ci_samples
        )
        if value is None or upper is None:
            continue

        test_exceed_cnt = sum(v > upper for v in test_vals)
        test_rate = test_exceed_cnt / max(1, len(test_vals))
        conservatism = upper / max(1.0, np.percentile(test_vals, 99.5))

        score = (
            1 if test_exceed_cnt > 0 else 0,
            test_rate,
            conservatism,
            upper,
            alpha_pct,
        )
        if best_alpha_score is None or score < best_alpha_score:
            best_alpha_score = score
            best_alpha = float(alpha_pct)
            best_upper = upper
            best_lower = lower

    if best_alpha is None:
        best_alpha = float(alpha_min)
        best_upper = best_block_value
        best_lower = None

    # Final model on full training set
    final_model, final_extremes, final_reason = fit_gev_model(fit_vals, best_block)
    if final_model is None:
        final_value = best_block_value
        final_lower = best_lower
        final_upper = best_upper
        final_loglikelihood = float("nan")
        final_model_name = "Fallback"
        final_extreme_count = 0 if final_extremes is None else len(final_extremes)
        final_unique_extremes = 0 if final_extremes is None else len(np.unique(final_extremes))
    else:
        final_value, final_lower, final_upper = safe_return_value(
            final_model, return_period, alpha=best_alpha / 100.0,
            n_samples=ci_samples
        )
        if final_value is None:
            final_value = best_block_value
        if final_upper is None:
            final_upper = best_upper if best_upper is not None else final_value
        final_loglikelihood = float(final_model.model.loglikelihood)
        final_model_name = final_model.model.name
        final_extreme_count = len(final_extremes)
        final_unique_extremes = len(np.unique(final_extremes))

    test_point_exceed = sum(v > final_value for v in test_vals)
    test_upper_exceed = sum(v > final_upper for v in test_vals)
    test_n = len(test_vals)

    return {
        "task": task_name,
        "status": "ok",
        "reason": "",
        "samples": n_fit,
        "test_samples": test_n,
        "block_seconds": best_block,
        "valid_block_candidates": valid_candidates,
        "alpha_pct": best_alpha,
        "return_period": return_period,
        "model": final_model_name,
        "loglikelihood": final_loglikelihood,
        "extremes": final_extreme_count,
        "unique_extremes": final_unique_extremes,
        "wcet_point": float(final_value),
        "ci_lower": None if final_lower is None else float(final_lower),
        "ci_upper": float(final_upper),
        "test_max": max(test_vals),
        "test_point_exceed": test_point_exceed,
        "test_upper_exceed": test_upper_exceed,
        "test_point_exceed_rate": test_point_exceed / max(1, test_n),
        "test_upper_exceed_rate": test_upper_exceed / max(1, test_n),
    }


def write_tuning_report(results, out_dir):
    """Write CSV and Markdown tuning reports."""
    csv_path = os.path.join(out_dir, "wcet_tuning_results.csv")
    md_path = os.path.join(out_dir, "wcet_tuning_summary.md")

    columns = [
        "task", "status", "reason", "samples", "filtered",
        "fit_samples", "calibration_samples", "test_samples",
        "block_seconds", "valid_block_candidates", "alpha_pct",
        "return_period", "model", "loglikelihood", "extremes",
        "unique_extremes", "wcet_point", "ci_lower", "ci_upper",
        "train_max", "calibration_max", "test_max",
        "test_point_exceed", "test_upper_exceed",
        "test_point_exceed_rate", "test_upper_exceed_rate",
    ]
    with open(csv_path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        for result in results:
            writer.writerow({col: result.get(col, "") for col in columns})

    lines = [
        "# WCET Parameter Tuning Summary\n",
        "| Task | Status | Samples | Block Size | Alpha (%) | WCET Point (μs) | CI Upper (μs) | Test Max (μs) | Test > Point | Test > Upper |",
        "|------|--------|---------|------------|-----------|-----------------|---------------|---------------|--------------|--------------|",
    ]
    for r in results:
        if r.get("status") not in ("ok", "fallback"):
            lines.append(
                f"| {r.get('task')} | {r.get('status')} ({r.get('reason')}) "
                f"| {r.get('samples', '')} | | | | | | | |"
            )
            continue
        status = r["status"] if r["status"] == "ok" else f"fallback ({r['reason']})"
        lines.append(
            f"| {r['task']} | {status} | {r['samples']:,} | {r['block_seconds']} "
            f"| {r['alpha_pct']:.1f} | {r['wcet_point']:,.0f} "
            f"| {r['ci_upper']:,.0f} | {r['test_max']:,.0f} "
            f"| {r['test_point_exceed']}/{r['test_samples']} "
            f"| {r['test_upper_exceed']}/{r['test_samples']} |"
        )

    with open(md_path, "w") as f:
        f.write("\n".join(lines) + "\n")

    print(f"\nTuning CSV → {csv_path}")
    print(f"Tuning summary → {md_path}")


# ---------------------------------------------------------------------------
# 3. Per-task EVA
# ---------------------------------------------------------------------------
def analyse_task(task_name, durations_us, out_dir, no_filter=False,
                 tail_percentile=95.0, target_blocks=40,
                 block_seconds=None, iqr_multiplier=1.5, mad_z=8.0,
                 figsize=(14, 6)):
    """
    Fit GEV to block-maxima and report return-level WCET estimates.
    """
    print(f"\n{'=' * 62}")
    print(f"  Task: {task_name}")
    print(f"  Samples: {len(durations_us):,}")
    print(f"  Observed max: {max(durations_us):,} us")
    print(f"{'=' * 62}")

    # --- optional outlier filtering ---
    filtered = False
    if not no_filter:
        durations_us, n_removed = filter_outliers(
            durations_us,
            tail_percentile=tail_percentile,
            iqr_multiplier=iqr_multiplier,
            mad_z=mad_z,
        )
        if n_removed > 0:
            filtered = True

    n = len(durations_us)
    # Build a time series with synthetic 1-second spacing (pyextremes needs datetime index)
    base = pd.Timestamp("2025-01-01")
    index = pd.date_range(base, periods=n, freq="1s")
    series = pd.Series(durations_us, index=index, name="duration_us")

    # Block size: pyextremes treats the synthetic 1-second index as samples.
    # Smaller blocks produce more block maxima, but can weaken the asymptotic
    # EVT assumption; larger blocks do the opposite.
    if block_seconds is None:
        block_seconds = max(1, n // target_blocks)
    else:
        block_seconds = min(max(1, block_seconds), n)
    block_size = f"{block_seconds}s"
    n_blocks = n // block_seconds
    print(f"  Block size: {block_size}  ({n_blocks} blocks)")

    try:
        import pyextremes as pe
    except ImportError:
        print("  [ERROR] pyextremes not installed. Run: pip3 install pyextremes")
        return None

    # --- EVA pipeline ---
    model = pe.EVA(series)
    model.get_extremes(method="BM", block_size=block_size)

    extremes = np.asarray(model.extremes.values, dtype=float)
    n_unique_extremes = len(np.unique(extremes))
    if n_unique_extremes < 3 or np.isclose(np.std(extremes), 0.0):
        print(f"  [warn] Block maxima are degenerate "
              f"({len(extremes)} extremes, {n_unique_extremes} unique); "
              "skipping GEV fit.")
        print("  [warn] Return levels fall back to the filtered observed max.")
        return fallback_result(task_name, durations_us, filtered)

    try:
        model.fit_model()
    except Exception as exc:
        print(f"  [warn] GEV fit failed: {exc}")
        print("  [warn] Return levels fall back to the filtered observed max.")
        return fallback_result(task_name, durations_us, filtered)

    print(f"  Fitted model: {model.model.name}")
    print(f"  Parameters:    {model.model.fit_parameters}")
    print(f"  Log-likelihood: {model.model.loglikelihood:.2f}")

    # Return levels (number of runs)
    return_periods = [100, 1000, 10_000, 100_000, 1_000_000]
    rl_values, rl_lower, rl_upper = model.get_return_value(return_periods)

    print(f"\n  ┌──────────────────────┬──────────────┐")
    print(f"  │ Return period (runs) │ WCET (μs)    │")
    print(f"  ├──────────────────────┼──────────────┤")
    for rp, val in zip(return_periods, rl_values):
        print(f"  │ {rp:>20,} │ {val:>12,.0f} │")
    print(f"  └──────────────────────┴──────────────┘")

    suffix = "_filtered" if filtered else ""

    # --- Diagnostic plots ---
    diag_fig, diag_axes = model.plot_diagnostic(figsize=(6, 5))
    diag_fig.suptitle(f"{task_name} – Diagnostic{' (filtered)' if filtered else ''}", fontsize=13)
    diag_fig.tight_layout()
    diag_path = os.path.join(out_dir, f"{sanitise(task_name)}_diagnostic{suffix}.png")
    diag_fig.savefig(diag_path, dpi=150)
    plt.close(diag_fig)
    print(f"  Diagnostic plot → {diag_path}")

    # --- Return-value plot ---
    rl_fig, rl_ax = model.plot_return_values(
        return_period=[1, 10, 100, 1000, 10_000, 100_000, 1_000_000],
        figsize=(7, 5),
    )
    rl_fig.suptitle(f"{task_name} – Return Value (GEV){' (filtered)' if filtered else ''}", fontsize=13)
    rl_fig.tight_layout()
    rl_path = os.path.join(out_dir, f"{sanitise(task_name)}_return_level{suffix}.png")
    rl_fig.savefig(rl_path, dpi=150)
    plt.close(rl_fig)
    print(f"  Return-level plot → {rl_path}")

    return {
        "task": task_name,
        "samples": n,
        "filtered": filtered,
        "observed_max": max(durations_us),
        "model": model.model.name,
        "params": model.model.fit_parameters,
        "loglikelihood": model.model.loglikelihood,
        "return_levels": {str(rp): float(rl_values[i])
                          for i, rp in enumerate(return_periods)},
    }


def fallback_result(task_name, durations_us, filtered):
    """Return a conservative summary when GEV fitting is not meaningful."""
    observed_max = max(durations_us)
    return_periods = [100, 1000, 10_000, 100_000, 1_000_000]

    print(f"\n  ┌──────────────────────┬──────────────┐")
    print(f"  │ Return period (runs) │ WCET (μs)    │")
    print(f"  ├──────────────────────┼──────────────┤")
    for rp in return_periods:
        print(f"  │ {rp:>20,} │ {observed_max:>12,.0f} │")
    print(f"  └──────────────────────┴──────────────┘")

    return {
        "task": task_name,
        "samples": len(durations_us),
        "filtered": filtered,
        "observed_max": observed_max,
        "model": "Degenerate",
        "params": {},
        "loglikelihood": float("nan"),
        "return_levels": {str(rp): float(observed_max)
                          for rp in return_periods},
    }


def sanitise(name):
    """Filesystem-safe string."""
    return "".join(c if c.isalnum() or c in ("-", "_") else "_" for c in name)


# ---------------------------------------------------------------------------
# 4. Summary report (Markdown)
# ---------------------------------------------------------------------------
def write_summary(results, out_dir):
    path = os.path.join(out_dir, "wcet_summary.md")
    lines = [
        "# WCET Extreme Value Analysis Summary\n",
        "| Task | Samples | Filtered | Observed Max (μs) | Model | 100-run WCET (μs) | 1M-run WCET (μs) |",
        "|------|---------|----------|-------------------|-------|-------------------|------------------|",
    ]
    for r in results:
        if r is None:
            continue
        rl100 = r["return_levels"].get("100", float("nan"))
        rl1M = r["return_levels"].get("1000000", float("nan"))
        filtered_mark = "✓" if r.get("filtered") else ""
        lines.append(
            f"| {r['task']} | {r['samples']:,} | {filtered_mark} | {r['observed_max']:,} "
            f"| {r['model']} | {rl100:,.0f} | {rl1M:,.0f} |"
        )
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    print(f"\nSummary → {path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    parser = argparse.ArgumentParser(description="WCET Extreme Value Analysis")
    parser.add_argument("--input", default="timing_log.txt",
                        help="Path to training/fit timing_log.txt")
    parser.add_argument("--test-input", default=None,
                        help="Path to test/validation timing_log.txt (required for "
                             "--experiment-param-select, --per-task-param-table, --tune)")
    parser.add_argument("--output", default="results",
                        help="Output directory")
    parser.add_argument("--task", default=None,
                        help="Analyse only a specific task")
    parser.add_argument("--no-filter", action="store_true",
                        help="Disable outlier filtering (not recommended)")
    parser.add_argument("--tail-percentile", type=float, default=95.0,
                        help="Tail percentile used in initial artifact filter "
                             "(default: 95). Threshold includes "
                             "max(Q3 + m*IQR, P<tail-percentile>, MAD-z)")
    parser.add_argument("--artifact-iqr-multiplier", type=float, default=1.5,
                        help="IQR multiplier for initial artifact filtering "
                             "(default: 1.5)")
    parser.add_argument("--artifact-mad-z", type=float, default=8.0,
                        help="Modified z-score cutoff for initial artifact "
                             "filtering (default: 8.0)")
    parser.add_argument("--target-blocks", type=int, default=40,
                        help="Target number of blocks for block maxima when "
                             "--block-seconds is not set (default: 40)")
    parser.add_argument("--block-seconds", type=int, default=None,
                        help="Explicit block size in synthetic seconds/samples; "
                             "overrides --target-blocks")
    parser.add_argument("--tune", action="store_true",
                        help="Tune block size and confidence level using "
                             "train/calibration/test split")
    parser.add_argument("--block-min", type=int, default=25,
                        help="Minimum explicit block size for --tune "
                             "(default: 25)")
    parser.add_argument("--block-max", type=int, default=300,
                        help="Maximum explicit block size for --tune "
                             "(default: 300)")
    parser.add_argument("--block-step", type=int, default=1,
                        help="Step size for block-size grid in --tune "
                             "(default: 1)")
    parser.add_argument("--alpha-min", type=float, default=97.0,
                        help="Minimum confidence level percentage for --tune "
                             "(default: 97)")
    parser.add_argument("--alpha-max", type=float, default=99.0,
                        help="Maximum confidence level percentage for --tune "
                             "(default: 99)")
    parser.add_argument("--alpha-step", type=float, default=0.2,
                        help="Confidence level grid step for --tune "
                             "(default: 0.2)")
    parser.add_argument("--tune-return-period", type=int,
                        default=DEFAULT_TUNE_RETURN_PERIOD,
                        help="Return period optimized by --tune "
                             "(default: 1000000)")
    parser.add_argument("--ci-samples", type=int, default=50,
                        help="Bootstrap samples for confidence intervals in "
                             "--tune (default: 50)")
    parser.add_argument("--experiment-param-select", action="store_true",
                        help="Run the paper-style k/alpha parameter selection "
                             "experiment and generate figures")
    parser.add_argument("--per-task-param-table", action="store_true",
                        help="Select k and alpha independently for each task "
                             "and write a final table")
    parser.add_argument("--experiment-k-min", type=int, default=40,
                        help="Minimum k for --experiment-param-select "
                             "(default: 40)")
    parser.add_argument("--experiment-k-max", type=int, default=300,
                        help="Maximum k for --experiment-param-select "
                             "(default: 300)")
    parser.add_argument("--experiment-k-step", type=int, default=10,
                        help="k step for --experiment-param-select "
                             "(default: 10)")
    parser.add_argument("--experiment-alphas",
                        default="0.99,0.995,0.999,0.9995,0.9999",
                        help="Comma-separated alpha values for "
                             "--experiment-param-select")
    args = parser.parse_args()

    if not (0 < args.tail_percentile < 100):
        print("[ERROR] --tail-percentile must be between 0 and 100.")
        sys.exit(1)
    if args.artifact_iqr_multiplier < 0:
        print("[ERROR] --artifact-iqr-multiplier must be non-negative.")
        sys.exit(1)
    if args.artifact_mad_z <= 0:
        print("[ERROR] --artifact-mad-z must be positive.")
        sys.exit(1)
    if args.target_blocks < 2:
        print("[ERROR] --target-blocks must be at least 2.")
        sys.exit(1)
    if args.block_seconds is not None and args.block_seconds < 1:
        print("[ERROR] --block-seconds must be at least 1.")
        sys.exit(1)
    if args.block_min < 1 or args.block_max < args.block_min:
        print("[ERROR] --block-min/--block-max must define a valid positive range.")
        sys.exit(1)
    if args.block_step < 1:
        print("[ERROR] --block-step must be at least 1.")
        sys.exit(1)
    if not (0 < args.alpha_min <= args.alpha_max < 100):
        print("[ERROR] --alpha-min/--alpha-max must be in (0, 100).")
        sys.exit(1)
    if args.alpha_step <= 0:
        print("[ERROR] --alpha-step must be positive.")
        sys.exit(1)
    if args.tune_return_period < 1:
        print("[ERROR] --tune-return-period must be positive.")
        sys.exit(1)
    if args.ci_samples < 1:
        print("[ERROR] --ci-samples must be positive.")
        sys.exit(1)
    if args.experiment_k_min < 1 or args.experiment_k_max < args.experiment_k_min:
        print("[ERROR] --experiment-k-min/--experiment-k-max must define a valid positive range.")
        sys.exit(1)
    if args.experiment_k_step < 1:
        print("[ERROR] --experiment-k-step must be at least 1.")
        sys.exit(1)
    try:
        experiment_alphas = [
            float(item.strip())
            for item in args.experiment_alphas.split(",")
            if item.strip()
        ]
    except ValueError:
        print("[ERROR] --experiment-alphas must be comma-separated floats.")
        sys.exit(1)
    if not experiment_alphas or any(not (0 < alpha < 1) for alpha in experiment_alphas):
        print("[ERROR] --experiment-alphas values must be in (0, 1).")
        sys.exit(1)

    script_dir = os.path.dirname(os.path.abspath(__file__))
    in_path = os.path.join(script_dir, args.input)
    out_dir = os.path.join(script_dir, args.output)
    os.makedirs(out_dir, exist_ok=True)

    print(f"Input:  {in_path}")
    print(f"Output: {out_dir}")
    print(f"Initial artifact filtering: {'OFF' if args.no_filter else f'ON (threshold=max(Q3+{args.artifact_iqr_multiplier:g}×IQR, P{args.tail_percentile:g}, MAD-z{args.artifact_mad_z:g}))'}")
    if args.block_seconds is None:
        print(f"Block maxima: target {args.target_blocks} blocks")
    else:
        print(f"Block maxima: explicit block size {args.block_seconds}s")

    if not os.path.isfile(in_path):
        print(f"\n[ERROR] timing_log.txt not found at {in_path}")
        print("Run an ArduPilot SITL simulation first or specify --input")
        sys.exit(1)

    data = parse_timing_log(in_path)
    if not data:
        print("[ERROR] No valid timing data found.")
        sys.exit(1)

    print(f"\nFound {len(data)} task(s):")
    for name, vals in sorted(data.items(), key=lambda x: -len(x[1])):
        print(f"  {name:50s}  {len(vals):>8,} samples")

    # Print per-task mean after outlier filtering
    print("\nPer-task mean after filtering:")
    filtered_mean_data, _ = clean_all_tasks(
        data,
        no_filter=args.no_filter,
        tail_percentile=args.tail_percentile,
        iqr_multiplier=args.artifact_iqr_multiplier,
        mad_z=args.artifact_mad_z,
    )
    for name, vals in sorted(filtered_mean_data.items(), key=lambda x: -len(x[1])):
        mean_us = np.mean(vals)
        print(f"  {name:50s}  mean={mean_us:>8.2f} us  ({len(vals):>8,} samples after filter)")

    if args.task:
        if args.task not in data:
            print(f"[ERROR] Task '{args.task}' not found in data.")
            sys.exit(1)
        data = {args.task: data[args.task]}

    if args.experiment_param_select:
        if args.test_input is None:
            print("[ERROR] --test-input is required for --experiment-param-select.")
            sys.exit(1)
        fit_data = data
        test_data = parse_timing_log(
            os.path.join(script_dir, args.test_input)
        )
        if not test_data:
            print("[ERROR] No valid test data found in --test-input.")
            sys.exit(1)
        print(f"\nTest input: {args.test_input}")
        print(f"  Test tasks: {len(test_data)}")

        fit_data, fit_clean = clean_all_tasks(
            fit_data,
            no_filter=args.no_filter,
            tail_percentile=args.tail_percentile,
            iqr_multiplier=args.artifact_iqr_multiplier,
            mad_z=args.artifact_mad_z,
        )
        test_data, test_clean = clean_all_tasks(
            test_data,
            no_filter=args.no_filter,
            tail_percentile=args.tail_percentile,
            iqr_multiplier=args.artifact_iqr_multiplier,
            mad_z=args.artifact_mad_z,
        )
        if fit_clean:
            removed = sum(item["removed"] for item in fit_clean.values())
            before = sum(item["before"] for item in fit_clean.values())
            print(f"\nFit data artifact filtering removed {removed:,}/{before:,} samples.")
        if test_clean:
            removed = sum(item["removed"] for item in test_clean.values())
            before = sum(item["before"] for item in test_clean.values())
            print(f"Test data artifact filtering removed {removed:,}/{before:,} samples.")

        common_tasks = sorted(set(fit_data.keys()) & set(test_data.keys()))
        if len(common_tasks) == 0:
            print("[ERROR] No common tasks between fit and test data.")
            sys.exit(1)
        only_fit = set(fit_data.keys()) - set(test_data.keys())
        only_test = set(test_data.keys()) - set(fit_data.keys())
        if only_fit:
            print(f"  [warn] Tasks only in fit data (skipped): {', '.join(sorted(only_fit))}")
        if only_test:
            print(f"  [warn] Tasks only in test data (skipped): {', '.join(sorted(only_test))}")

        fit_data = {t: fit_data[t] for t in common_tasks}
        test_data = {t: test_data[t] for t in common_tasks}

        print("\nParameter-selection experiment:")
        print(f"  Fit tasks: {len(fit_data)}")
        print(f"  Test tasks: {len(test_data)}")
        print(f"  k range: {args.experiment_k_min}..{args.experiment_k_max} "
              f"(step {args.experiment_k_step})")
        print(f"  alpha values: {', '.join(f'{a:g}' for a in experiment_alphas)}")
        run_param_selection_experiment(
            fit_data,
            test_data,
            out_dir,
            no_filter=args.no_filter,
            tail_percentile=args.tail_percentile,
            k_min=args.experiment_k_min,
            k_max=args.experiment_k_max,
            k_step=args.experiment_k_step,
            alphas=experiment_alphas,
        )
        print("\nDone.")
        return

    if args.per_task_param_table:
        if args.test_input is None:
            print("[ERROR] --test-input is required for --per-task-param-table.")
            sys.exit(1)
        fit_data = data
        test_data = parse_timing_log(
            os.path.join(script_dir, args.test_input)
        )
        if not test_data:
            print("[ERROR] No valid test data found in --test-input.")
            sys.exit(1)
        print(f"\nTest input: {args.test_input}")
        print(f"  Test tasks: {len(test_data)}")

        fit_data, fit_clean = clean_all_tasks(
            fit_data,
            no_filter=args.no_filter,
            tail_percentile=args.tail_percentile,
            iqr_multiplier=args.artifact_iqr_multiplier,
            mad_z=args.artifact_mad_z,
        )
        test_data, test_clean = clean_all_tasks(
            test_data,
            no_filter=args.no_filter,
            tail_percentile=args.tail_percentile,
            iqr_multiplier=args.artifact_iqr_multiplier,
            mad_z=args.artifact_mad_z,
        )
        if fit_clean:
            removed = sum(item["removed"] for item in fit_clean.values())
            before = sum(item["before"] for item in fit_clean.values())
            print(f"\nFit data artifact filtering removed {removed:,}/{before:,} samples.")
        if test_clean:
            removed = sum(item["removed"] for item in test_clean.values())
            before = sum(item["before"] for item in test_clean.values())
            print(f"Test data artifact filtering removed {removed:,}/{before:,} samples.")

        common_tasks = sorted(set(fit_data.keys()) & set(test_data.keys()))
        if len(common_tasks) == 0:
            print("[ERROR] No common tasks between fit and test data.")
            sys.exit(1)
        only_fit = set(fit_data.keys()) - set(test_data.keys())
        only_test = set(test_data.keys()) - set(fit_data.keys())
        if only_fit:
            print(f"  [warn] Tasks only in fit data (skipped): {', '.join(sorted(only_fit))}")
        if only_test:
            print(f"  [warn] Tasks only in test data (skipped): {', '.join(sorted(only_test))}")

        fit_data_subset = {t: fit_data[t] for t in common_tasks}
        test_data_subset = {t: test_data[t] for t in common_tasks}

        print("\nPer-task parameter table:")
        print(f"  Fit tasks: {len(fit_data_subset)}")
        print(f"  Test tasks: {len(test_data_subset)}")
        print(f"  k range: {args.experiment_k_min}..{args.experiment_k_max} "
              f"(step {args.experiment_k_step})")
        print(f"  alpha values: {', '.join(f'{a:g}' for a in experiment_alphas)}")
        run_per_task_param_table(
            fit_data_subset,
            test_data_subset,
            out_dir,
            no_filter=args.no_filter,
            tail_percentile=args.tail_percentile,
            k_min=args.experiment_k_min,
            k_max=args.experiment_k_max,
            k_step=args.experiment_k_step,
            alphas=experiment_alphas,
        )
        print("\nDone.")
        return

    if args.tune:
        if args.test_input is None:
            print("[ERROR] --test-input is required for --tune.")
            sys.exit(1)
        fit_data = data
        test_data = parse_timing_log(
            os.path.join(script_dir, args.test_input)
        )
        if not test_data:
            print("[ERROR] No valid test data found in --test-input.")
            sys.exit(1)
        print(f"\nTest input: {args.test_input}")
        print(f"  Test tasks: {len(test_data)}")

        fit_data, fit_clean = clean_all_tasks(
            fit_data,
            no_filter=args.no_filter,
            tail_percentile=args.tail_percentile,
            iqr_multiplier=args.artifact_iqr_multiplier,
            mad_z=args.artifact_mad_z,
        )
        test_data, test_clean = clean_all_tasks(
            test_data,
            no_filter=args.no_filter,
            tail_percentile=args.tail_percentile,
            iqr_multiplier=args.artifact_iqr_multiplier,
            mad_z=args.artifact_mad_z,
        )
        if fit_clean:
            removed = sum(item["removed"] for item in fit_clean.values())
            before = sum(item["before"] for item in fit_clean.values())
            print(f"\nFit data artifact filtering removed {removed:,}/{before:,} samples.")
        if test_clean:
            removed = sum(item["removed"] for item in test_clean.values())
            before = sum(item["before"] for item in test_clean.values())
            print(f"Test data artifact filtering removed {removed:,}/{before:,} samples.")

        print("\nTuning mode:")
        print(f"  Block size range: {args.block_min}..{args.block_max} "
              f"(step {args.block_step})")
        print(f"  Alpha range: {args.alpha_min:g}..{args.alpha_max:g} "
              f"(step {args.alpha_step:g})")
        print(f"  Tune return period: {args.tune_return_period:,} runs")

        common_tasks = sorted(set(fit_data.keys()) & set(test_data.keys()))
        if len(common_tasks) == 0:
            print("[ERROR] No common tasks between fit and test data.")
            sys.exit(1)
        only_fit = set(fit_data.keys()) - set(test_data.keys())
        only_test = set(test_data.keys()) - set(fit_data.keys())
        if only_fit:
            print(f"  [warn] Tasks only in fit data (skipped): {', '.join(sorted(only_fit))}")
        if only_test:
            print(f"  [warn] Tasks only in test data (skipped): {', '.join(sorted(only_test))}")

        tuning_results = []
        for task_name in common_tasks:
            fit_vals = fit_data[task_name]
            test_vals = test_data[task_name]
            print(f"\n[tune] {task_name} (fit={len(fit_vals):,}, test={len(test_vals):,} samples)")
            result = tune_task(
                task_name,
                fit_vals,
                test_vals,
                block_min=args.block_min,
                block_max=args.block_max,
                block_step=args.block_step,
                alpha_min=args.alpha_min,
                alpha_max=args.alpha_max,
                alpha_step=args.alpha_step,
                return_period=args.tune_return_period,
                ci_samples=args.ci_samples,
            )
            tuning_results.append(result)
            if result.get("status") in ("ok", "fallback"):
                print(f"  best block={result['block_seconds']}, "
                      f"alpha={result['alpha_pct']:.1f}%, "
                      f"CI upper={result['ci_upper']:,.0f} us, "
                      f"test>{result['test_upper_exceed']}/"
                      f"{result['test_samples']} "
                      f"({result['status']})")
            else:
                print(f"  skipped: {result.get('reason')}")

        write_tuning_report(tuning_results, out_dir)
        print("\nDone.")
        return

    results = []
    for task_name, durations in data.items():
        res = analyse_task(task_name, durations, out_dir,
                           no_filter=args.no_filter,
                           tail_percentile=args.tail_percentile,
                           target_blocks=args.target_blocks,
                           block_seconds=args.block_seconds,
                           iqr_multiplier=args.artifact_iqr_multiplier,
                           mad_z=args.artifact_mad_z)
        results.append(res)

    write_summary(results, out_dir)
    print("\nDone.")


if __name__ == "__main__":
    main()
