// Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
// opm-simulators TpsaVector: out-of-line conversion function whose
// conversion-type-id is a member alias of the enclosing class template.
#include <array>
#include <cstddef>
template <class ScalarT>
class TpsaVector {
public:
    static constexpr std::size_t numTpsaEq = 4;
    using Scalar = ScalarT;
    using EqVector = std::array<Scalar, numTpsaEq>;
    class EntryProxy {
    public:
        EntryProxy(TpsaVector& v, std::size_t c) : v_(v), cell_(c) {}
        Scalar operator[](std::size_t eq) const { return v_.data_[cell_ * numTpsaEq + eq]; }
        operator EqVector() const;
    private:
        TpsaVector& v_;
        std::size_t cell_;
    };
    Scalar data_[8]{};
};
template <class ScalarT>
TpsaVector<ScalarT>::EntryProxy::operator EqVector() const
{
    EqVector res;
    for (std::size_t eqIdx = 0; eqIdx < numTpsaEq; ++eqIdx) res[eqIdx] = (*this)[eqIdx];
    return res;
}
template class TpsaVector<double>;
template class TpsaVector<float>;
