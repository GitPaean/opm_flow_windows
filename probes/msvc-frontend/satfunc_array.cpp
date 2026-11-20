// Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
// opm-simulators SatfuncConsistencyChecks: a std::array data member whose
// element is a nested class, in a header that does not include <array>.
// Define WITH_ARRAY to add the include and leave the class nested.
#ifdef WITH_ARRAY
#include <array>
#endif
#include <cstddef>
#include <functional>
#include <random>
#include <string>
#include <vector>
template <typename Scalar>
class Checks {
public:
    enum class ViolationLevel : std::size_t { Standard, Critical, NumLevels };
    using PointIDFormatCallback = std::function<std::string(std::size_t)>;
    Checks(std::string name, std::size_t n) : pointName_(std::move(name)), numSamplePoints_(n) {}
    bool anyFailed(ViolationLevel l) const { return !violations_[static_cast<std::size_t>(l)].count.empty(); }
    void clear();
private:
    using RandomBitGenerator = std::minstd_rand;
    struct ViolationSample {
        std::vector<std::size_t> count{};
        std::vector<std::size_t> pointID{};
        std::vector<Scalar> checkValues{};
        void clear();
    };
    using ViolationCollection = std::array
        <ViolationSample, static_cast<std::size_t>(ViolationLevel::NumLevels)>;
    std::string pointName_{};
    std::size_t numSamplePoints_;
    PointIDFormatCallback formatPointID_{};
    std::vector<typename std::vector<Scalar>::size_type> startCheckValues_{};
    ViolationCollection violations_{};
    void buildStructure(ViolationSample& violation);
    template <typename F> void processViolation(ViolationSample& v, F&& f) { f(v.checkValues); }
};
template <typename Scalar> void Checks<Scalar>::ViolationSample::clear()
{ count.clear(); pointID.clear(); checkValues.clear(); }
template <typename Scalar> void Checks<Scalar>::clear()
{ for (auto& v : violations_) v.clear(); }
template <typename Scalar> void Checks<Scalar>::buildStructure(ViolationSample& v)
{ v.count.assign(4, 0); }
template class Checks<float>;
template class Checks<double>;
