/* Fortran C-interface name-mangling for the OpenBLAS/LAPACK ABI on Windows
   (gfortran convention: lowercase + trailing underscore). Normally generated
   by CMake's FortranCInterface, which needs a Fortran compiler. */
#ifndef OPM_COMPAT_FCMACROS_H
#define OPM_COMPAT_FCMACROS_H
#define FC_GLOBAL(name, NAME)  name##_
#define FC_GLOBAL_(name, NAME) name##_
#endif
