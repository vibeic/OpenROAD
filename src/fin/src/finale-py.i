// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2022-2025, The OpenROAD Authors

%{
#include "ord/OpenRoad.hh"
#include "fin/Finale.h"

%}

%include "../../Exception-py.i"

// The metal density check takes a DensityLimits and returns a rich
// DensityCheckResult (both in fin/density_check.h, not %included here); it is
// driven from the Tcl side, so we do not wrap it for Python.
%ignore fin::Finale::checkDensity;

%include "fin/Finale.h"
