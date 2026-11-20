// Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics; GPL-3.0-or-later (see LICENSE).
// opm-simulators GenericOutputModule: enum local to a member function of a
// class template, used in a default member initializer of a local struct.
#include <array>
#include <cstddef>
#include <string_view>
#include <variant>
#include <vector>
template <class FluidSystem>
class Module {
    static constexpr int numPhases = 3;
    using ScalarBuffer = std::vector<typename FluidSystem::Scalar>;
    ScalarBuffer saturation_, density_, residual_;
    std::vector<ScalarBuffer*> rockc_;
public:
    void doAllocBuffers(unsigned bufferSize, bool enable);
};
template <class FluidSystem>
void Module<FluidSystem>::doAllocBuffers(unsigned bufferSize, bool enable)
{
    enum class EntryPhaseType { None, GWO, NGWO, GasWatOil, NGasWatOil };
    struct Entry {
        std::variant<ScalarBuffer*,
                     std::array<ScalarBuffer, numPhases>*,
                     std::vector<ScalarBuffer*>*> data;
        std::string_view kw;
        bool supported;
        bool required = true;
        EntryPhaseType phaseType = EntryPhaseType::GasWatOil;
    };
    const auto entries = std::array{
        Entry{&saturation_, "S", true},
        Entry{&density_, "DEN", enable, false, EntryPhaseType::NGWO},
        Entry{&residual_, "RESIDUAL", true, false, EntryPhaseType::None},
        Entry{&rockc_, "ROCKC", true, false},
    };
    for (const auto& e : entries) {
        if (!e.supported) continue;
        switch (e.phaseType) {
        case EntryPhaseType::None: break;
        case EntryPhaseType::NGWO: break;
        default: break;
        }
        if (auto* p = std::get_if<ScalarBuffer*>(&e.data)) (*p)->resize(bufferSize);
    }
}
template <class T> struct FS { using Scalar = T; };
template class Module<FS<double>>;
template class Module<FS<float>>;
