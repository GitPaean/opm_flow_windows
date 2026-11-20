// Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
// opm-grid ElementChunks: range-based for directly after 'omp parallel for'
// (an OpenMP 5.0 form).
#include <vector>
double sum(const std::vector<double>& chunks)
{
    double s = 0;
#pragma omp parallel for reduction(+:s)
    for (const auto& c : chunks) s += c;
    return s;
}
