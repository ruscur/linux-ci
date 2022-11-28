==========================================
DEXCR (Dynamic Execution Control Register)
==========================================

Overview
========

The DEXCR is a privileged special purpose register (SPR) introduced in
PowerPC ISA 3.1B (Power10) that allows per-cpu control over several dynamic
execution behaviours. These behaviours include speculation (e.g., indirect
branch target prediction) and enabling return-oriented programming (ROP)
protection instructions.

The execution control is exposed in hardware as up to 32 bits ('aspects') in
the DEXCR. Each aspect controls a certain behaviour, and can be set or cleared
to enable/disable the aspect. There are several variants of the DEXCR for
different purposes:

DEXCR
    A priviliged SPR that can control aspects for userspace and kernel space
HDEXCR
    A hypervisor-privileged SPR that can control aspects for the hypervisor and
    enforce aspects for the kernel and userspace.
UDEXCR
    An optional ultravisor-privileged SPR that can control aspects for the ultravisor.

Userspace can examine the current DEXCR state using a dedicated SPR that
provides a non-privileged read-only view of the userspace DEXCR aspects.
There is also an SPR that provides a read-only view of the hypervisor enforced
aspects, which ORed with the userspace DEXCR view gives the effective DEXCR
state for a process.


User API
========

prctl()
-------

A process can control its own userspace DEXCR value using the
``PR_PPC_GET_DEXCR`` and ``PR_PPC_SET_DEXCR`` pair of
:manpage:`prctl(2)` commands. These calls have the form::

    prctl(PR_PPC_GET_DEXCR, unsigned long aspect, 0, 0, 0);
    prctl(PR_PPC_SET_DEXCR, unsigned long aspect, unsigned long flags, 0, 0);

Where ``aspect`` (``arg1``) is a constant and ``flags`` (``arg2``) is a bifield.
The possible aspect and flag values are as follows. Note there is no relation
between aspect value and ``prctl()`` constant value.

.. flat-table::
   :header-rows: 1
   :widths: 2 7 1

   * - ``prctl()`` constant
     - Aspect name
     - Aspect bit

   * - ``PR_PPC_DEXCR_SBHE``
     - Speculative Branch Hint Enable (SBHE)
     - 0

   * - ``PR_PPC_DEXCR_IBRTPD``
     - Indirect Branch Recurrent Target Prediction Disable (IBRTPD)
     - 3

   * - ``PR_PPC_DEXCR_SRAPD``
     - Subroutine Return Address Prediction Disable (SRAPD)
     - 4

   * - ``PR_PPC_DEXCR_NPHIE``
     - Non-Privileged Hash Instruction Enable (NPHIE)
     - 5

.. flat-table::
   :header-rows: 1
   :widths: 2 8

   * - ``prctl()`` flag
     - Meaning

   * - ``PR_PPC_DEXCR_PRCTL``
     - This aspect can be configured with ``prctl(PR_PPC_SET_DEXCR, ...)``

   * - ``PR_PPC_DEXCR_SET_ASPECT``
     - This aspect is set

   * - ``PR_PPC_DEXCR_FORCE_SET_ASPECT``
     - This aspect is set and cannot be undone. A subsequent
       ``prctl(..., PR_PPC_DEXCR_CLEAR_ASPECT)`` will fail.

   * - ``PR_PPC_DEXCR_CLEAR_ASPECT``
     - This aspect is clear

Note that

* The ``*_SET_ASPECT`` / ``*_CLEAR_ASPECT`` refers to setting/clearing the bit in the DEXCR.
  For example::

      prctl(PR_PPC_SET_DEXCR, PR_PPC_DEXCR_IBRTPD, PR_PPC_DEXCR_SET_ASPECT, 0, 0);

  will set the IBRTPD aspect bit in the DEXCR, causing indirect branch prediction
  to be disabled.

* The status returned by ``PR_PPC_GET_DEXCR`` does not include any alternative
  config overrides. To see the true DEXCR state software should read the appropriate
  SPRs directly.

* A forced aspect will still report ``PR_PPC_DEXCR_PRCTL`` if it would
  otherwise be editable.

* The aspect state when starting a process is copied from the parent's
  state on :manpage:`fork(2)` and :manpage:`execve(2)`. Aspects may also be set
  or cleared by the kernel on process creation.

Use ``PR_PPC_SET_DEXCR`` with one of ``PR_PPC_DEXCR_SET_ASPECT``,
``PR_PPC_DEXCR_FORCE_SET_ASPECT``, or ``PR_PPC_DEXCR_CLEAR_ASPECT`` to edit a
 given aspect.

Common error codes for both getting and setting the DEXCR are as follows:

.. flat-table::
   :header-rows: 1
   :widths: 2 8

   * - Error
     - Meaning

   * - ``EINVAL``
     - The DEXCR is not supported by the kernel.

   * - ``ENODEV``
     - The aspect is not recognised by the kernel or not supported by the hardware.

``PR_PPC_SET_DEXCR`` may also report the following error codes:

.. flat-table::
   :header-rows: 1
   :widths: 2 8

   * - Error
     - Meaning

   * - ``ERANGE``
     - ``arg2`` is incorrect. E.g., it does not select an action (set/clear),
       or the flags are not recognised by the kernel.

   * - ``ENXIO``
     - The aspect is not editable via ``prctl()``.

   * - ``EPERM``
     - The process does not have sufficient privilege to modify this aspect,
       or the aspect has been force set and cannot be modified.


sysctl
------

Some aspects can be modified globally via :manpage:`sysctl(8)` entries. Such global
modifications are applied after any process modifications. Any ``prctl()`` call to
an overridden aspect this aspect may still report it as editable. The prctl setting
will take effect again if the global override is restored to its default state.

A global SBHE config is exposed in ``/proc/sys/kernel/speculative_branch_hint_enable``.
Any process can read the current config value from it. Privileged processes can
write to it to change the config. The new config is applied to all current and future
processes (though note the kernel cannot override any hypervisor enforced aspects).

.. flat-table::
   :header-rows: 1
   :widths: 2 8

   * - Value
     - Meaning

   * - ``-1``
     - Do not change from default or ``prctl()`` config.

   * - ``0``
     - Force clear aspect.

   * - ``1``
     - Force set aspect.
