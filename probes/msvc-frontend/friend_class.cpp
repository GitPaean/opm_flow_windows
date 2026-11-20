// Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
// opm-grid CpGridData: unqualified 'friend class Intersection' in a nested
// namespace while an enclosing namespace has a class template of that name.
// The friend declaration must declare Dune::cpgrid::Intersection.
namespace Dune {
template <class GridImp, class IntersectionImp> class Intersection {};
namespace cpgrid {
class CpGridData {
    friend class Intersection;
    int faces_ = 0;
};
class Intersection {
public:
    int faces(const CpGridData& d) const { return d.faces_; }
};
} // namespace cpgrid
} // namespace Dune
int main() { return Dune::cpgrid::Intersection{}.faces(Dune::cpgrid::CpGridData{}); }
