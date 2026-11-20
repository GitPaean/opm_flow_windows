// Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
// opm-simulators DILU: OpenMP loop whose bound is a re-evaluated member call
// and whose condition is '!=' (ICE C1001 under /openmp:llvm).
#include <cstddef>
#include <vector>
struct Block { double v[3][3]; };
struct Row { std::vector<Block> b; const Block& operator[](std::size_t i) const { return b[i]; } };
struct Matrix {
    std::vector<Row> rows;
    std::size_t N() const { return rows.size(); }
    const Row& operator[](std::size_t i) const { return rows[i]; }
};
template <class M, class B>
class DILU {
    const M& A_;
    std::vector<B> Dinv_;
    std::vector<std::size_t> natural_to_reorder_;
public:
    explicit DILU(const M& A) : A_(A), Dinv_(A.N()), natural_to_reorder_(A.N()) {}
    void parallelUpdate()
    {
#pragma omp parallel for
        for (std::size_t row = 0; row != A_.N(); ++row) {
            Dinv_[natural_to_reorder_[row]] = A_[row][row];
        }
    }
};
template class DILU<Matrix, Block>;
