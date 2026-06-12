/*
 * Copyright (C) 2026 Sergio Andreozzi
 *
 * SPDX-License-Identifier: GPL-2.0-only
 */

#include "stratum-empirical-cdf-loader.h"

#include "ns3/fatal-error.h"
#include "ns3/log.h"

#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>

namespace ns3::stratum
{

NS_LOG_COMPONENT_DEFINE("ns3::stratum::EmpiricalCdfLoader");

Ptr<EmpiricalRandomVariable>
LoadEmpiricalCdfFromFile(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
    {
        NS_FATAL_ERROR("empirical-cdf-loader: cannot open '"
                       << path
                       << "'. If this path is relative, the example must run with "
                          "cwd=ns3/ns-3-dev/ (via `./ns3 run ...`) or the caller "
                          "must pass --cdfDir=<absolute-path>.");
    }

    Ptr<EmpiricalRandomVariable> rv = CreateObject<EmpiricalRandomVariable>();
    rv->SetInterpolate(false);

    std::string line;
    size_t lineNo = 0;
    size_t rowsAdded = 0;
    double lastCumProb = -1.0;
    while (std::getline(in, line))
    {
        lineNo++;
        std::string trimmed;
        for (char c : line)
        {
            if (!std::isspace(static_cast<unsigned char>(c)))
            {
                trimmed = line;
                break;
            }
        }
        if (trimmed.empty() || trimmed[trimmed.find_first_not_of(" \t\r\n")] == '#')
        {
            continue;
        }

        // Defensive: cap line length. Well-formed CDF rows are three small numbers
        // (<~64 bytes); anything larger is almost certainly malformed input.
        if (line.size() > 64 * 1024)
        {
            NS_FATAL_ERROR("empirical-cdf-loader: line " << lineNo << " in " << path
                                                         << " exceeds 64 KiB; malformed input?");
        }

        std::istringstream iss(line);
        double value;
        double count;
        double cumProb;
        if (!(iss >> value >> count >> cumProb))
        {
            NS_FATAL_ERROR("empirical-cdf-loader: malformed line " << lineNo << " in " << path);
        }
        (void)count;

        if (!std::isfinite(value) || !std::isfinite(cumProb))
        {
            NS_FATAL_ERROR("empirical-cdf-loader: non-finite value or cum_prob at "
                           "line "
                           << lineNo << " in " << path);
        }
        if (cumProb < 0.0 || cumProb > 1.0)
        {
            NS_FATAL_ERROR("empirical-cdf-loader: cum_prob out of [0,1] at line "
                           << lineNo << " in " << path << " (got " << cumProb << ")");
        }
        if (cumProb < lastCumProb)
        {
            NS_FATAL_ERROR("empirical-cdf-loader: non-monotone cum_prob at line "
                           << lineNo << " in " << path);
        }
        rv->CDF(value, cumProb);
        lastCumProb = cumProb;
        rowsAdded++;
    }

    if (rowsAdded == 0)
    {
        NS_FATAL_ERROR("empirical-cdf-loader: no rows read from " << path);
    }

    NS_LOG_INFO("loaded " << rowsAdded << " CDF rows from " << path);
    return rv;
}

} // namespace ns3::stratum
