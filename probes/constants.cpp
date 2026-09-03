/*
  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).

  Do the derived material constants keep their value for an autodiff
  scalar? Opm::Constants and Opm::IAPWS::Common compute kb, hRed, Rs and
  criticalMolarVolume as constants on the Windows branch of opm-common
  (Evaluation arithmetic is not a constant expression under MSVC). This
  evaluates the old in-Scalar expressions at run time for
  Evaluation<float, 3> and prints them beside the constants: the three
  "old" and "new" columns must agree to the last digit, and match the
  plain float column.

  Build (from the harness root, after setup-env.ps1):
    cl /nologo /std:c++20 /permissive- /EHsc /MD /DNDEBUG /Isrc\opm-common
       /Ivcpkg\installed\x64-windows\include probes\constants.cpp /Fe:constants.exe
*/
#include <opm/material/densead/Evaluation.hpp>
#include <opm/material/Constants.hpp>
#include <opm/material/components/iapws/Common.hpp>

#include <cstdio>

int main()
{
    using E = Opm::DenseAd::Evaluation<float, 3>;
    using C = Opm::IAPWS::Common<E>;

    const auto oldRs     = Opm::Constants<E>::R / C::molarMass;
    const auto oldVolume = C::molarMass / C::criticalDensity;
    const auto oldKb     = Opm::Constants<E>::R / Opm::Constants<E>::Na;

    std::printf("Rs:                  old=%.12g new=%.12g scalar=%.12g\n",
                oldRs.value(), C::Rs.value(), Opm::IAPWS::Common<float>::Rs);
    std::printf("criticalMolarVolume: old=%.12g new=%.12g scalar=%.12g\n",
                oldVolume.value(), C::criticalMolarVolume.value(),
                Opm::IAPWS::Common<float>::criticalMolarVolume);
    std::printf("kb:                  old=%.12g new=%.12g scalar=%.12g\n",
                oldKb.value(), Opm::Constants<E>::kb.value(), Opm::Constants<float>::kb);
}
