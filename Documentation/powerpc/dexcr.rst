.. SPDX-License-Identifier: GPL-2.0-or-later

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
    A privileged SPR that can control aspects for userspace and kernel space
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


Kernel Config
=============

The kernel supports a static default DEXCR value determined at config time.
Set the ``PPC_DEXCR_DEFAULT`` config to the value you want all processes to
use.
