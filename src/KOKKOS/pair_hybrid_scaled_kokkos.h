/* -*- c++ -*- ----------------------------------------------------------
   LAMMPS - Large-scale Atomic/Molecular Massively Parallel Simulator
   https://www.lammps.org/, Sandia National Laboratories
   LAMMPS development team: developers@lammps.org

   Copyright (2003) Sandia Corporation.  Under the terms of Contract
   DE-AC04-94AL85000 with Sandia Corporation, the U.S. Government retains
   certain rights in this software.  This software is distributed under
   the GNU General Public License.

   See the README file in the top-level LAMMPS directory.
------------------------------------------------------------------------- */

#ifdef PAIR_CLASS
// clang-format off
PairStyle(hybrid/scaled/kk,PairHybridScaledKokkos);
// clang-format on
#else

// clang-format off
#ifndef LMP_PAIR_HYBRID_SCALED_KOKKOS_H
#define LMP_PAIR_HYBRID_SCALED_KOKKOS_H

#include "pair_hybrid_kokkos.h"

namespace LAMMPS_NS {

class PairHybridScaledKokkos : public PairHybridKokkos {
 public:
  PairHybridScaledKokkos(class LAMMPS *);
  void coeff(int, char **) override;

  void init_svector() override;
  void copy_svector(int, int) override;
};

}

#endif
#endif

