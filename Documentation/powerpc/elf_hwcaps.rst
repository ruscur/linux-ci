.. _elf_hwcaps_index:

==================
POWERPC ELF hwcaps
==================

This document describes the usage and semantics of the powerpc ELF hwcaps.


1. Introduction
---------------

Some hardware or software features are only available on some CPU
implementations, and/or with certain kernel configurations, but have no
architected discovery mechanism available to userspace code. The kernel
exposes the presence of these features to userspace through a set
of flags called hwcaps, exposed in the auxilliary vector.

Userspace software can test for features by acquiring the AT_HWCAP or
AT_HWCAP2 entry of the auxiliary vector, and testing whether the relevant
flags are set, e.g.::

	bool floating_point_is_present(void)
	{
		unsigned long hwcaps = getauxval(AT_HWCAP);
		if (hwcaps & PPC_FEATURE_HAS_FPU)
			return true;

		return false;
	}

Where software relies on a feature described by a hwcap, it should check
the relevant hwcap flag to verify that the feature is present before
attempting to make use of the feature.

Features cannot be probed reliably through other means. When a feature
is not available, attempting to use it may result in unpredictable
behaviour, and is not guaranteed to result in any reliable indication
that the feature is unavailable, such as a SIGILL.

2. hwcap allocation
-------------------

HWCAPs are allocated as described in Power Architecture 64-Bit ELF V2 ABI
Specification (which will be reflected in the kernel's uapi headers).

3. The hwcaps exposed in AT_HWCAP
---------------------------------

PPC_FEATURE_32
    32-bit CPU

PPC_FEATURE_64
    64-bit CPU (userspace may be running in 32-bit mode).

PPC_FEATURE_601_INSTR
    The processor is PowerPC 601

PPC_FEATURE_HAS_ALTIVEC
    Vector (aka Altivec, VSX) facility is available.

PPC_FEATURE_HAS_FPU
    Floating point facility is available.

PPC_FEATURE_HAS_MMU
    Memory management unit is present.

PPC_FEATURE_HAS_4xxMAC
    ?

PPC_FEATURE_UNIFIED_CACHE
    ?

PPC_FEATURE_HAS_SPE
    ?

PPC_FEATURE_HAS_EFP_SINGLE
    ?

PPC_FEATURE_HAS_EFP_DOUBLE
    ?

PPC_FEATURE_NO_TB
    The timebase facility (mftb instruction) is not available.

PPC_FEATURE_POWER4
    The processor is POWER4.

PPC_FEATURE_POWER5
    The processor is POWER5.

PPC_FEATURE_POWER5_PLUS
    The processor is POWER5+.

PPC_FEATURE_CELL
    The processor is Cell.

PPC_FEATURE_BOOKE
    The processor implements the BookE architecture.

PPC_FEATURE_SMT
    The processor implements SMT.

PPC_FEATURE_ICACHE_SNOOP
    The processor icache is coherent with the dcache, and instruction storage
    can be made consistent with data storage for the purpose of executing
    instructions with the instruction sequence:
        sync
        icbi (to any address)
        isync

PPC_FEATURE_ARCH_2_05
    The processor supports the v2.05 userlevel architecture. Processors
    supporting later architectures also set this feature.

PPC_FEATURE_PA6T
    The processor is PA6T.

PPC_FEATURE_HAS_DFP
    DFP facility is available.

PPC_FEATURE_POWER6_EXT
    The processor is POWER6.

PPC_FEATURE_ARCH_2_06
    The processor supports the v2.06 userlevel architecture. Processors
    supporting later architectures also set this feature.

PPC_FEATURE_HAS_VSX
    VSX facility is available.

PPC_FEATURE_PSERIES_PERFMON_COMPAT

PPC_FEATURE_TRUE_LE
    Reserved, do not use

PPC_FEATURE_PPC_LE
    Reserved, do not use

3. The hwcaps exposed in AT_HWCAP2
----------------------------------

PPC_FEATURE2_ARCH_2_07
    The processor supports the v2.07 userlevel architecture. Processors
    supporting later architectures also set this feature.

PPC_FEATURE2_HTM
    Transactional Memory feature is available.

PPC_FEATURE2_DSCR
    DSCR facility is available.

PPC_FEATURE2_EBB
    EBB facility is available.

PPC_FEATURE2_ISEL
    isel instruction is available. This is superseded by ARCH_2_07 and
    later.

PPC_FEATURE2_TAR
    VSX facility is available.

PPC_FEATURE2_VEC_CRYPTO
    v2.07 crypto instructions are available.

PPC_FEATURE2_HTM_NOSC
    System calls fail if called in a transactional state, see
    Documentation/powerpc/syscall64-abi.rst

PPC_FEATURE2_ARCH_3_00
    The processor supports the v3.0B / v3.0C userlevel architecture. Processors
    supporting later architectures also set this feature.

PPC_FEATURE2_HAS_IEEE128
    IEEE 128 is available? What instructions/data?

PPC_FEATURE2_DARN
    darn instruction is available.

PPC_FEATURE2_SCV
    scv instruction is available.

PPC_FEATURE2_HTM_NO_SUSPEND
    A limited Transactional Memory facility that does not support suspend is
    available, see Documentation/powerpc/transactional_memory.rst.

PPC_FEATURE2_ARCH_3_1
    The processor supports the v3.1 userlevel architecture. Processors
    supporting later architectures also set this feature.

PPC_FEATURE2_MMA
    MMA facility is available.
