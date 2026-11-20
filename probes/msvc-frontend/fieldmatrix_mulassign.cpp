// Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
// opm-upscaling: 'block *= 2' on a 1x1 matrix block with an int literal.
// Shape of dune-common 2.10 FieldMatrix<K,1,1>: CRTP base with
// operator*=(const field_type&), derived operator*=(const K&) that hides it,
// a constrained converting constructor and operator const K&.
#include <type_traits>
#include <vector>
template <class M, class T> struct HasAssigner : std::false_type {};
template <class D>
struct DenseMatrix {
    using field_type = double;
    D& operator*=(const field_type& k) { static_cast<D&>(*this).d *= k; return static_cast<D&>(*this); }
    template <class T> D& operator=(const T&) { return static_cast<D&>(*this); }
};
template <class K>
struct FieldMatrix11 : DenseMatrix<FieldMatrix11<K>> {
    K d{};
    constexpr FieldMatrix11() = default;
    template <class T, typename = std::enable_if_t<HasAssigner<FieldMatrix11, T>::value>>
    FieldMatrix11(const T& rhs) { *this = rhs; }
    using DenseMatrix<FieldMatrix11<K>>::operator=;
    FieldMatrix11& operator+=(const K& k) { d += k; return *this; }
    FieldMatrix11& operator*=(const K& k) { d *= k; return *this; }
    operator const K&() const { return d; }
};
template <class Scalar>
struct Solver {
    std::vector<std::vector<FieldMatrix11<Scalar>>> S_{1, std::vector<FieldMatrix11<Scalar>>(1)};
    void regularize() { S_[0][0] *= 2; }
};
template struct Solver<double>;
