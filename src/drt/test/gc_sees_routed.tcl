# What an IN-LOOP GC worker can and cannot see, measured on a real route.
#
# The premise this file pins down is NOT obvious and was got wrong twice
# upstream of it. FlexGC_init.cpp gates initNetsFromDesign() on
# `getDRWorker() == nullptr`, so a whole-design worker loads every routed shape
# in its extBox from the design and an in-loop worker loads none of them --
# which reads like the in-loop worker is blind to already-routed metal.
#
# It is not, and the reason is one line away: FlexDRWorker::initNetObjs() builds
# the worker's OWN drNet set from the SAME `queryDRObj(getExtBox(), ...)`. Every
# net holding routed metal in the worker's extBox therefore becomes a drNet of
# that worker, and initDRWorker() loads its shapes. The two paths reach the same
# geometry from different sides.
#
# `-gc_sees_routed` loads the difference -- routed objects in the extBox on nets
# the DR worker does NOT own -- and DRT-0708 reports how much that was. On this
# design, and on every design measured for it, the answer is ZERO: the set is
# empty, so the flag has nothing to add and the router is not converging blind
# for this reason.
#
# THIS FILE EXISTS TO KEEP THAT TRUE. If a future change narrows initNetObjs()
# -- querying routeBox instead of extBox, dropping a net class, skipping nets
# with initial routing -- the count stops being zero and this test goes red,
# with the exact number, instead of the router quietly starting to check its
# tiles against a partial picture.
source "helpers.tcl"
read_lef Nangate45/Nangate45_tech.lef
read_lef Nangate45/Nangate45_stdcell.lef
read_def gcd_nangate45_preroute.def
read_guides gcd_nangate45.route_guide
set_thread_count 1
detailed_route -gc_sees_routed -verbose 0
