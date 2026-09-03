/*
  Copyright (C) 2026 SINTEF Digital, Mathematics & Cybernetics

  Part of the opm_flow_windows harness; GPL v3+ (see repository LICENSE).

  Does this MPI spawn a child under its process manager? A parent/child
  probe for MPI_Comm_spawn, written to check the claim that MS-MPI lacks
  dynamic process management (it does not: MS-MPI 10.1 passes this).

  Build (MS-MPI SDK):
    cl /nologo /MD "/IC:\Program Files (x86)\Microsoft SDKs\MPI\Include" mpi-spawn.c
       /Fe:mpi-spawn.exe /link "/LIBPATH:C:\Program Files (x86)\Microsoft SDKs\MPI\Lib\x64" msmpi.lib
  Run:
    mpiexec -n 1 mpi-spawn.exe            expected: BEFORE SPAWN,
                                          SPAWNED CHILD REACHED MPI_INIT,
                                          AFTER SPAWN result=0
    mpi-spawn.exe                         started without a process manager:
                                          the spawn fails (that is the launch
                                          context, not the API)
    mpi-spawn.exe --return                same, with MPI_ERRORS_RETURN so the
                                          failure comes back as a return code
    mpiexec -n 1 mpi-spawn.exe --missing  a command the process manager
                                          cannot find
*/
#include <mpi.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char** argv)
{
    MPI_Init(&argc, &argv);
    MPI_Comm parent;
    MPI_Comm_get_parent(&parent);
    if (parent != MPI_COMM_NULL) {
        puts("SPAWNED CHILD REACHED MPI_INIT"); fflush(stdout);
        MPI_Comm_disconnect(&parent);
        MPI_Finalize();
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--return") == 0)
        MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN);
    MPI_Comm child = MPI_COMM_NULL;
    int errors[1] = { MPI_SUCCESS };
    puts("BEFORE SPAWN"); fflush(stdout);
    const char* command = (argc > 1 && strcmp(argv[1], "--missing") == 0)
                        ? "nonexistent-review-child.exe" : argv[0];
    int result = MPI_Comm_spawn(command, MPI_ARGV_NULL,
                                1, MPI_INFO_NULL, 0, MPI_COMM_WORLD, &child, errors);
    printf("AFTER SPAWN result=%d\n", result); fflush(stdout);
    if (child != MPI_COMM_NULL) MPI_Comm_disconnect(&child);
    MPI_Finalize();
    return 0;
}
