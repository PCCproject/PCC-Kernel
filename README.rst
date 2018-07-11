===
PCC
===

**P**\ erformance-oriented **C**\ ongestion **C**\ ontrol.

A Linux kernel module implementing the PCC as described
`here <http://modong.github.io/pcc-page/>`_.

=======
History
=======

| PCC heavily relies on the pacing mechanism.
| Until recently, this mechanism did not exist in the Linux kernel.
|
| 2014 - Implementation of PCC using `UDT <http://udt.sourceforge.net/>`_ by
  Mo Dong - https://github.com/modong/pcc
| 2016 - Google publishes their BBR congestion algorithm (CA) - an algorithm
  with similar goals to PCC but slightly different, as a Linux kernel module.
| 2016 - Google adds pacing mechanism to the Linux kernel in order to support
  their BBR CA approach.
| 2016 - Using features added by Google in BBR, an implementation of PCC as a 
  Linux CA kernel module by Tomer Gilad (Hebrew University of Jerusalem,
  PCC Team) - https://github.com/tomergi/pcc-kernel
| 2017 - Google's BRR team adds more changes - allowing internal timers to
  implement pacing and multiple bug fixes.
| 2017 - This project continues the previous project by Tomer.
| 2018 - Project released at NetDev 2018

=========
Using PCC
=========

Environment
-----------

| This module was tested and developed on the current latest Ubuntu:
  :code:`17.04 x64`

.. code::

   Linux ubuntu 4.10.0-19-generic #21-Ubuntu SMP Thu Apr 6 17:04:57 UTC 2017 x86_64 x86_64 x86_64 GNU/Linux

| Since latest BBR changes require a newer kernel version we update our kernel
  to the latest stable version: :code:`4.13`

.. code:: bash

   wget http://kernel.ubuntu.com/~kernel-ppa/mainline/v4.13/linux-headers-4.13.0-041300_4.13.0-041300.201709031731_all.deb
   wget http://kernel.ubuntu.com/~kernel-ppa/mainline/v4.13/linux-headers-4.13.0-041300-generic_4.13.0-041300.201709031731_amd64.deb
   wget http://kernel.ubuntu.com/~kernel-ppa/mainline/v4.13/linux-image-4.13.0-041300-generic_4.13.0-041300.201709031731_amd64.deb
   sudo dpkg -i *.deb
   sudo reboot

| Check kernel version with: :code:`uname -r`
| Current kernel sources are located at :code:`/lib/modules/$(uname -r)/build`
| Load :code:`bbr` for sanity: :code:`sudo modprobe tcp_bbr`


Compilation
-----------

| Install :code:`git` and clone this repository.

.. code:: bash

   cd src
   make
   sudo insmod tcp_pcc.ko

Usage
-----

Now you can use :code:`pcc` as a congestion algorithm:

.. code:: python

   import socket

   TCP_CONGESTION = getattr(socket, 'TCP_CONGESTION', 13)
   
   s = socket.socket()
   s.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, 'pcc')
   
   # Use s

=======================
Implementation overview
=======================

Code
----
| All PCC code resides under :code:`src/tcp_pcc.c`
| Code is compatible with the
  `conventions <https://www.kernel.org/doc/html/v4.10/process/coding-style.html>`_
  of the Linux kernel


Congestion Hooks
----------------
| The Linux kernel allows creating "modular" CAs.
| This is achieved by a set of hooks which must or can be implemented by the
  CA module.
| For the complete set of hooks - see :code:`struct tcp_congestion_ops`
  definition in the Linux kernel.

PCC uses the main hook :code:`cong_control` introduced by BBR.

The hook has two purposes in regard to the current PCC implementation:

* A periodic hook which is called on each ack - allowing us to "slice"
  intervals.
* A hook, that on each ack, reports "acks" and "losses" of sent packets -
  allowing us to accumulate statistics for "Monitor Intervals" (MIs) and
  eventually calculate utility.

| The entire PCC logic resides in the following hook, under the
 :code:`pcc_main` function.

Utility calculation
-------------------
At the moment the utility from the paper is being used.

The utility is calculated using a variation of :code:`fixedptc.h`.

| Noticed that the sigmoid is calculated as follows :code:`1/(1+e^x)`.
| If :code:`x` is very big (> 21) we might overflow.
| A check is placed in order to avoid this situation.
|
| Alternatively, the sigmoid can be calculated as :code:`e^-x/(1+e^-x)`
  avoiding this.
| This was the previous implementation by Mo and Tomer - and should be
  returned. 


Congestion windows
------------------
The PCC article does not specify anything regarding the congestion window size.

This is natural since PCC does not limit its throughput using the congestion
window (cwnd) like cubic does, but rather employs pacing.

Still, a cwnd must be given - since the cwnd value limits the number of
packets in flight.

We use a similar implementation as BBR - just set cwnd to twice the pacing
rate in packets: :code:`cwnd = 2 * rate * rtt / mss` - this value is updated
upon every ack and should guarantee that pacing is not limited by the cwnd on
one hand and that the cwnd is not too big on the other hand.

This logic was not tested thoroughly - but rather used as "common sense".

See this discussion for more information:

https://groups.google.com/forum/#!topic/bbr-dev/bC-8XoQB7pQ

Edge cases
----------
The PCC article does not specify cases where the rates are close to zero or
maxint.

When rates are close to zero - rate changes, which are epsilon away from each
other, don't change significantly, resulting in irrelevant measurements. In
order to overcome this - we use a minimal step of 4K bytes.

This number was not tested thoroughly - but rather used as "common sense".

===========================
Additional reading material
===========================

| RTTs and RTOs: http://sgros.blogspot.co.il/2012/02/calculating-tcp-rto.html
| Selective ACKS: http://packetlife.net/blog/2010/jun/17/tcp-selective-acknowledgments-sack/
| LSO/TSO/GSO/LRO: https://en.wikipedia.org/wiki/Large_send_offload
| Pacing: https://reproducingnetworkresearch.wordpress.com/2016/05/30/cs244-16-revisiting-tcp-pacing-on-the-modern-linux-kernel/
| BBR: https://research.google.com/pubs/pub45646.html
| BBR vs PCC blog post: https://groups.google.com/forum/#!topic/bbr-dev/j7FITaY2V3M




