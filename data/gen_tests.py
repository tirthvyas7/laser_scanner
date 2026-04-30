#!/usr/bin/env python3
"""Generates the suite of edge-case test CSVs used to verify the pipeline.

Each file has m columns. Block expects columns to be even (pairs).
"""

import math
import os
import random


COLUMNS = 16          # default m for tests
ROWS    = 32          # default rows per test
SEED    = 0xC1AC1A    # deterministic


def write_csv(path, rows):
    with open(path, "w") as f:
        for r in rows:
            assert len(r) == COLUMNS
            f.write(",".join(str(int(v)) for v in r) + "\n")


def all_zeros():
    return [[0] * COLUMNS for _ in range(ROWS)]


def all_max():
    return [[255] * COLUMNS for _ in range(ROWS)]


def impulse():
    out = [[0] * COLUMNS for _ in range(ROWS)]
    out[ROWS // 2][COLUMNS // 2] = 255
    return out


def step_edge():
    half = COLUMNS // 2
    return [[0] * half + [255] * (COLUMNS - half) for _ in range(ROWS)]


def alternating():
    base = [255 if (c % 2) else 0 for c in range(COLUMNS)]
    return [list(base) for _ in range(ROWS)]


def linear_ramp():
    base = [int(round(255 * c / max(1, COLUMNS - 1))) for c in range(COLUMNS)]
    return [list(base) for _ in range(ROWS)]


def threshold_boundary():
    TV = 128
    return [
        *([[TV - 1] * COLUMNS] * (ROWS // 3)),
        *([[TV]     * COLUMNS] * (ROWS // 3)),
        *([[TV + 1] * COLUMNS] * (ROWS - 2 * (ROWS // 3))),
    ]


def single_defect():
    out = [[50] * COLUMNS for _ in range(ROWS)]
    out[ROWS // 2][COLUMNS // 2] = 240
    out[ROWS // 2][COLUMNS // 2 + 1] = 240
    return out


def vertical_stripe():
    out = [[80] * COLUMNS for _ in range(ROWS)]
    col = COLUMNS // 3
    for r in range(ROWS):
        out[r][col] = 220
    return out


def salt_pepper_noise():
    rng = random.Random(SEED)
    out = [[120] * COLUMNS for _ in range(ROWS)]
    for r in range(ROWS):
        for c in range(COLUMNS):
            x = rng.random()
            if x < 0.05:
                out[r][c] = 0
            elif x > 0.95:
                out[r][c] = 255
    return out


def sinusoidal_weave():
    out = []
    for r in range(ROWS):
        row = []
        for c in range(COLUMNS):
            # Two-period sine across the row, modulated row-by-row.
            v = 128 + 100 * math.sin(2 * math.pi * c / 8.0 + r * 0.3)
            row.append(max(0, min(255, int(round(v)))))
        out.append(row)
    return out


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    tests_dir = os.path.join(here, "tests")
    os.makedirs(tests_dir, exist_ok=True)

    suite = {
        "all_zeros.csv":           all_zeros,
        "all_max.csv":              all_max,
        "impulse.csv":              impulse,
        "step_edge.csv":            step_edge,
        "alternating.csv":          alternating,
        "linear_ramp.csv":          linear_ramp,
        "threshold_boundary.csv":   threshold_boundary,
        "single_defect.csv":        single_defect,
        "vertical_stripe.csv":      vertical_stripe,
        "salt_pepper_noise.csv":    salt_pepper_noise,
        "sinusoidal_weave.csv":     sinusoidal_weave,
    }
    for name, fn in suite.items():
        write_csv(os.path.join(tests_dir, name), fn())
        print("wrote", name)

    # Default sample.csv for the top-level pipeline.yaml — moderate noise
    # plus one obvious bright defect. Visible in metrics and output.
    rng = random.Random(SEED)
    sample = []
    for r in range(64):
        sample.append([min(255, max(0, 100 + rng.randint(-20, 20))) for _ in range(COLUMNS)])
    sample[20][8] = sample[20][9] = 245
    sample[40][2] = sample[40][3] = 240
    write_csv(os.path.join(here, "sample.csv"), sample)
    print("wrote sample.csv")


if __name__ == "__main__":
    main()
