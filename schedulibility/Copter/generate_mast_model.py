#!/usr/bin/env python3
"""
generate_mast_model.py
Generate a MAST model file from ArduCopter task CSV.

Usage: python generate_mast_model.py [--csv mast_model_tasks.csv] [--output model.txt]
"""
from __future__ import annotations
import csv
import argparse
import sys
from typing import List, Dict
from pathlib import Path

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
LOOP_RATE_HZ = 400
FAST_LOOP_PERIOD_US = 1_000_000 // LOOP_RATE_HZ  # 2500 us
SPEED_FACTOR = 1.0
CONTEXT_SWITCH_WORST = 0.0
CONTEXT_SWITCH_AVG = 0.0
CONTEXT_SWITCH_BEST = 0.0
MAX_PRIORITY = 256
MIN_PRIORITY = 1

MODEL_NAME = "ArduCopter_Multicopter"
MODEL_DATE = "2026-05-16"


def us_to_ms(us: float) -> float:
    """Convert microseconds to milliseconds (MAST time unit)."""
    return us / 1000.0


def ap_priority_to_mast(ap_prio: int) -> int:
    """Invert ArduPilot priority (0=highest) to MAST Fixed_Priority (larger=higher).
    AP_prio range [0, 255] <-> MAST range [256, 1]"""
    return MAX_PRIORITY - ap_prio


def load_tasks(csv_path: str) -> list[dict]:
    """Read task CSV and return list of task dicts."""
    tasks = []
    with open(csv_path, newline="", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        for row in reader:
            row["rate_hz"] = float(row["rate_hz"])
            row["period_us"] = float(row["period_us"])
            row["ap_priority"] = int(row["ap_priority"]) if row["ap_priority"].strip() else 0
            row["in_model"] = bool(int(row["in_model"]))
            wcet_str = row["wcet_us"].strip()
            if wcet_str.upper() == "NAN" or wcet_str == "":
                row["wcet_us"] = None
            else:
                row["wcet_us"] = float(wcet_str)
            tasks.append(row)
    return tasks


def indent(n: int) -> str:
    """Return indentation string."""
    return "   " * n


def format_float(v: float, decimals: int = 3) -> str:
    """Format a float with fixed decimal places, strip trailing zeros."""
    return f"{v:.{decimals}f}"


def emit_header(f):
    """Write MAST model header."""
    f.write(f"Model (\n")
    f.write(f"{indent(1)}Model_Name          => {MODEL_NAME},\n")
    f.write(f"{indent(1)}Model_Date          => {MODEL_DATE},\n")
    f.write(f"{indent(1)}System_Pip_Behaviour => STRICT);\n")
    f.write("\n")


def emit_processing_resource(f):
    """Write Processing_Resource section."""
    f.write(f"Processing_Resource (\n")
    f.write(f"{indent(1)}Type                   => Regular_Processor,\n")
    f.write(f"{indent(1)}Name                   => RPi3B_CPU,\n")
    f.write(f"{indent(1)}Max_Interrupt_Priority => {MAX_PRIORITY},\n")
    f.write(f"{indent(1)}Min_Interrupt_Priority => {MIN_PRIORITY},\n")
    f.write(f"{indent(1)}Worst_ISR_Switch       => {format_float(CONTEXT_SWITCH_WORST)},\n")
    f.write(f"{indent(1)}Avg_ISR_Switch         => {format_float(CONTEXT_SWITCH_AVG)},\n")
    f.write(f"{indent(1)}Best_ISR_Switch        => {format_float(CONTEXT_SWITCH_BEST)},\n")
    f.write(f"{indent(1)}Speed_Factor           => {format_float(SPEED_FACTOR, 2)});\n")
    f.write("\n")


def emit_scheduler(f):
    """Write Primary_Scheduler section."""
    f.write(f"Scheduler (\n")
    f.write(f"{indent(1)}Type            => Primary_Scheduler,\n")
    f.write(f"{indent(1)}Name            => fp_scheduler,\n")
    f.write(f"{indent(1)}Host            => RPi3B_CPU,\n")
    f.write(f"{indent(1)}Policy          => \n")
    f.write(f"{indent(2)}( Type                 => Fixed_Priority,\n")
    f.write(f"{indent(3)}Worst_Context_Switch => {format_float(CONTEXT_SWITCH_WORST)},\n")
    f.write(f"{indent(3)}Avg_Context_Switch   => {format_float(CONTEXT_SWITCH_AVG)},\n")
    f.write(f"{indent(3)}Best_Context_Switch  => {format_float(CONTEXT_SWITCH_BEST)},\n")
    f.write(f"{indent(3)}Max_Priority         => {MAX_PRIORITY},\n")
    f.write(f"{indent(3)}Min_Priority         => {MIN_PRIORITY}));\n")
    f.write("\n")


def emit_scheduling_server(f, name: str, mast_prio: int):
    """Write a Scheduling_Server with Fixed_Priority_Policy."""
    f.write(f"Scheduling_Server (\n")
    f.write(f"{indent(1)}Type                       => Regular,\n")
    f.write(f"{indent(1)}Name                       => svr_{name},\n")
    f.write(f"{indent(1)}Server_Sched_Parameters    => \n")
    f.write(f"{indent(2)}( Type         => Fixed_Priority_Policy,\n")
    f.write(f"{indent(3)}The_Priority => {mast_prio},\n")
    f.write(f"{indent(3)}Preassigned  => NO),\n")
    f.write(f"{indent(1)}Scheduler                  => fp_scheduler);\n")
    f.write("\n")


def emit_operation(f, name: str, wcet_us: float):
    """Write a Simple Operation."""
    wcet_ms = us_to_ms(wcet_us)
    f.write(f"Operation (\n")
    f.write(f"{indent(1)}Type                       => Simple,\n")
    f.write(f"{indent(1)}Name                       => op_{name},\n")
    f.write(f"{indent(1)}Worst_Case_Execution_Time  => {format_float(wcet_ms)});\n")
    f.write("\n")


def emit_fast_loop(f, fast_tasks: list[dict]):
    """Write the FAST_TASK chain as a single Transaction with sequential steps."""
    if not fast_tasks:
        print("Warning: No FAST_TASK with WCET data. Skipping fast loop transaction.",
              file=sys.stderr)
        return

    # Scheduling server for fast loop (highest priority = 256)
    mast_prio = ap_priority_to_mast(0)  # FAST_TASK_PRI0 = 0 → 256
    emit_scheduling_server(f, "fast_loop", mast_prio)

    # Operations for each FAST_TASK step
    for t in fast_tasks:
        emit_operation(f, t["task_name"], t["wcet_us"])

    # Transaction
    period_ms = us_to_ms(FAST_LOOP_PERIOD_US)
    f.write(f"Transaction (\n")
    f.write(f"{indent(1)}Type            => Regular,\n")
    f.write(f"{indent(1)}Name            => tr_fast_loop,\n")

    # External event: periodic at 400Hz
    f.write(f"{indent(1)}External_Events => \n")
    f.write(f"{indent(2)}( ( Type       => Periodic,\n")
    f.write(f"{indent(3)}Name       => ev_fast,\n")
    f.write(f"{indent(3)}Period     => {format_float(period_ms)},\n")
    f.write(f"{indent(3)}Max_Jitter => 0.000,\n")
    f.write(f"{indent(3)}Phase      => 0.000)),\n")

    # Internal events: chain links + final deadline event
    f.write(f"{indent(1)}Internal_Events => \n")
    f.write(f"{indent(2)}(\n")
    for i, t in enumerate(fast_tasks):
        ev_name = f"ev_step{i+1}"
        if i == len(fast_tasks) - 1:
            # Last event carries timing requirement
            f.write(f"{indent(3)}( Type => Regular,\n")
            f.write(f"{indent(4)}Name => {ev_name},\n")
            f.write(f"{indent(4)}Timing_Requirements => \n")
            f.write(f"{indent(5)}( Type             => Hard_Global_Deadline,\n")
            f.write(f"{indent(6)}Deadline         => {format_float(period_ms)},\n")
            f.write(f"{indent(6)}Referenced_Event => ev_fast))")
        else:
            f.write(f"{indent(3)}( Type => Regular,\n")
            f.write(f"{indent(4)}Name => {ev_name})")
        if i < len(fast_tasks) - 1:
            f.write(",\n")
        else:
            f.write("\n")
    f.write(f"{indent(2)}),\n")

    # Event handlers: chained activities
    f.write(f"{indent(1)}Event_Handlers  => \n")
    f.write(f"{indent(2)}(\n")
    for i, t in enumerate(fast_tasks):
        input_ev = "ev_fast" if i == 0 else f"ev_step{i}"
        output_ev = f"ev_step{i+1}"
        f.write(f"{indent(3)}(Type               => Activity,\n")
        f.write(f"{indent(4)}Input_Event        => {input_ev},\n")
        f.write(f"{indent(4)}Output_Event       => {output_ev},\n")
        f.write(f"{indent(4)}Activity_Operation => op_{t['task_name']},\n")
        f.write(f"{indent(4)}Activity_Server    => svr_fast_loop)")
        if i < len(fast_tasks) - 1:
            f.write(",\n")
        else:
            f.write("\n")
    f.write(f"{indent(2)}));\n")
    f.write("\n")


def emit_sched_task(f, t: dict):
    """Write Scheduling_Server, Operation, and Transaction for one SCHED_TASK."""
    mast_prio = ap_priority_to_mast(t["ap_priority"])
    name = t["task_name"]
    period_ms = us_to_ms(t["period_us"])
    wcet_ms = us_to_ms(t["wcet_us"])

    emit_scheduling_server(f, name, mast_prio)
    emit_operation(f, name, t["wcet_us"])

    f.write(f"Transaction (\n")
    f.write(f"{indent(1)}Type            => Regular,\n")
    f.write(f"{indent(1)}Name            => tr_{name},\n")

    f.write(f"{indent(1)}External_Events => \n")
    f.write(f"{indent(2)}( ( Type       => Periodic,\n")
    f.write(f"{indent(3)}Name       => ev_{name},\n")
    f.write(f"{indent(3)}Period     => {format_float(period_ms)},\n")
    f.write(f"{indent(3)}Max_Jitter => 0.000,\n")
    f.write(f"{indent(3)}Phase      => 0.000)),\n")

    f.write(f"{indent(1)}Internal_Events => \n")
    f.write(f"{indent(2)}( ( Type => Regular,\n")
    f.write(f"{indent(3)}Name => out_{name},\n")
    f.write(f"{indent(3)}Timing_Requirements => \n")
    f.write(f"{indent(4)}( Type             => Hard_Global_Deadline,\n")
    f.write(f"{indent(5)}Deadline         => {format_float(period_ms)},\n")
    f.write(f"{indent(5)}Referenced_Event => ev_{name}))),\n")

    f.write(f"{indent(1)}Event_Handlers  => \n")
    f.write(f"{indent(2)}( (Type               => Activity,\n")
    f.write(f"{indent(3)}Input_Event        => ev_{name},\n")
    f.write(f"{indent(3)}Output_Event       => out_{name},\n")
    f.write(f"{indent(3)}Activity_Operation => op_{name},\n")
    f.write(f"{indent(3)}Activity_Server    => svr_{name})));\n")
    f.write("\n")


def generate_model(tasks: list[dict], output_path: str):
    """Main generation routine."""
    fast_tasks = [t for t in tasks if t["task_type"] == "FAST_TASK" and t["in_model"]]
    sched_tasks = [t for t in tasks if t["task_type"] == "SCHED_TASK" and t["in_model"]]
    skipped = [t for t in tasks if not t["in_model"]]

    print(f"FAST_TASK steps (in model):  {len(fast_tasks)}")
    for t in fast_tasks:
        print(f"  - {t['task_name']}: WCET={t['wcet_us']}μs")
    print(f"SCHED_TASK (in model):       {len(sched_tasks)}")
    for t in sched_tasks:
        mast_p = ap_priority_to_mast(t["ap_priority"])
        print(f"  - {t['task_name']}: period={t['period_us']}μs, "
              f"AP_prio={t['ap_priority']}→MAST_prio={mast_p}, WCET={t['wcet_us']}μs")
    print(f"Skipped (no WCET):           {len(skipped)}")
    for t in skipped:
        print(f"  - {t['task_name']} [{t['task_type']}]")

    with open(output_path, "w", newline="\n", encoding="utf-8") as f:
        # Preamble comment
        f.write(f"-- MAST model for ArduCopter (Multicopter)\n")
        f.write(f"-- Generated by generate_mast_model.py\n")
        f.write(f"-- Tasks with WCET: {len(fast_tasks)} FAST_TASK steps + "
                f"{len(sched_tasks)} SCHED_TASK\n")
        f.write(f"-- Skipped (no WCET): {len(skipped)} tasks\n")
        f.write(f"-- Processor: Raspberry Pi 3B (Cortex-A53, 1.2GHz, single core)\n")
        f.write(f"-- Loop rate: {LOOP_RATE_HZ} Hz\n")
        f.write(f"-- Priority mapping: MAST_prio = 255 - AP_prio\n\n")

        emit_header(f)
        emit_processing_resource(f)
        emit_scheduler(f)

        # FAST_TASK chain (single transaction)
        emit_fast_loop(f, fast_tasks)

        # SCHED_TASK (individual transactions)
        for t in sched_tasks:
            emit_sched_task(f, t)

    print(f"\nModel written to: {output_path}")


def main():
    parser = argparse.ArgumentParser(
        description="Generate MAST model file from ArduCopter task CSV")
    parser.add_argument("--csv", default="mast_model_tasks.csv",
                        help="Input CSV file (default: mast_model_tasks.csv)")
    parser.add_argument("--output", default="arducopter_mast_model.txt",
                        help="Output MAST model file (default: arducopter_mast_model.txt)")
    args = parser.parse_args()

    if not Path(args.csv).exists():
        print(f"Error: CSV file not found: {args.csv}", file=sys.stderr)
        sys.exit(1)

    tasks = load_tasks(args.csv)
    generate_model(tasks, args.output)


if __name__ == "__main__":
    main()
