/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#ifndef NS3_STRATUM_EMPIRICAL_CDF_LOADER_H
#define NS3_STRATUM_EMPIRICAL_CDF_LOADER_H

#include "ns3/random-variable-stream.h"

#include <string>

namespace ns3::stratum
{

/**
 * @brief Load an empirical CDF from a whitespace-separated text file.
 *
 * File format — one record per line, three whitespace-separated columns:
 *
 *     value  count  cumulative_probability
 *
 * The middle `count` column is ignored, matching ns-2's
 * `RandomVariable/Empirical::loadCDF` (`sscanf("%lf %*f %lf", ...)`). Blank
 * lines and lines whose first non-whitespace character is `#` are skipped.
 *
 * Rows must be ordered by ascending `value` and ascending
 * `cumulative_probability`. The final row is expected to have
 * `cumulative_probability == 1.0`.
 *
 * @param path Path to the CDF file (absolute, or relative to the ns-3 run cwd).
 * @return An `EmpiricalRandomVariable` with interpolation disabled (so only
 *         the tabulated values can be drawn — matching ns-2 default).
 *
 * Fails via `NS_FATAL_ERROR` if the file cannot be opened or a line is
 * malformed.
 */
Ptr<EmpiricalRandomVariable> LoadEmpiricalCdfFromFile(const std::string& path);

} // namespace ns3::stratum

#endif // NS3_STRATUM_EMPIRICAL_CDF_LOADER_H
