/*
 * @BEGIN LICENSE
 *
 * Psi4: an open-source quantum chemistry software package
 *
 * Copyright (c) 2007-2021 The Psi4 Developers.
 *
 * The copyrights for code used from other parties are included in
 * the corresponding files.
 *
 * This file is part of Psi4.
 *
 * Psi4 is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * Psi4 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License along
 * with Psi4; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * @END LICENSE
 */

#include "dct.h"
#include "psi4/psifiles.h"

#include "psi4/libpsi4util/process.h"
#include "psi4/psifiles.h"
#include "psi4/libciomr/libciomr.h"
#include "psi4/libpsio/psio.h"
#include "psi4/libiwl/iwl.h"
#include "psi4/libqt/qt.h"
#include "psi4/libmints/twobody.h"
#include "psi4/libmints/integral.h"
#include "psi4/psi4-dec.h"
#include "psi4/libmints/basisset.h"
#include "psi4/liboptions/liboptions.h"
#include "psi4/libpsio/psio.hpp"
#include "psi4/libtrans/integraltransform.h"
#include "psi4/libdpd/dpd.h"
#include "psi4/lib3index/3index.h"

#include "psi4/libfock/jk.h"
#include "psi4/libfock/apps.h"
#include "psi4/physconst.h"
#include "psi4/libpsi4util/PsiOutStream.h"

#include <iostream>
#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <sstream>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace psi {
namespace dct {

/**
 * Build the density-fitting tensor: b(Q|mn) in AO-basis
 * b(Q|mn) = Sum_P (mn|P) [J^-1/2]_PQ
 * where J is the matrix of (P|Q)
 */
void DCTSolver::initialize_df() {
    dct_timer_on("DCTSolver::df_build_b()");

    outfile->Printf("\n\n\t                  ************************************************\n");
    outfile->Printf("\t                  *         Density Fitting Module in DCT        *\n");
    outfile->Printf("\t                  *                by Xiao Wang                  *\n");
    outfile->Printf("\t                  ************************************************\n");
    outfile->Printf("\n");

    primary_ = get_basisset("ORBITAL");
    auxiliary_ = get_basisset("DF_BASIS_DCT");
    auxiliary_scf_ = get_basisset("DF_BASIS_SCF");

    nn_ = primary_->nbf();
    nQ_ = auxiliary_->nbf();
    nQ_scf_ = auxiliary_scf_->nbf();

    df_memory();
}

void DCTSolver::build_df_b() {
    std::shared_ptr<BasisSet> zero(BasisSet::zero_ao_basis_set());

    // Form J(P,Q)^-1/2
    dct_timer_on("DCTSolver::Form J^-1/2");
    auto Jm12 = formJm12(auxiliary_, "J^-1/2 Correlation");
    dct_timer_off("DCTSolver::Form J^-1/2");

    // Form B(Q, mu, nu)
    dct_timer_on("DCTSolver::Form B(Q,mn)");
    auto bQmn_ao = formb_ao(primary_, auxiliary_, zero, Jm12, "B(Q|mn) Correlation");
    dct_timer_off("DCTSolver::Form B(Q,mn)");

    // Transform B to the SO basis.
    // TODO: Evaluate whether it would be better to have symmetry in the previous steps.
    // FittingMetric makes symmetry of the metric easy.
    dct_timer_on("DCTSolver::Transform B(Q,mn) AO-basis -> SO-basis");
    bQmn_so_ = transform_b_ao2so(bQmn_ao);
    dct_timer_off("DCTSolver::Transform B(Q,mn) AO-basis -> SO-basis");

    // Now do the same for the JKFIT terms.
    dct_timer_on("DCTSolver::Form J^-1/2 (JKFIT)");
    auto Jm12scf = formJm12(auxiliary_scf_, "J^-1/2 Reference");
    dct_timer_off("DCTSolver::Form J^-1/2 (JKFIT)");

    dct_timer_on("DCTSolver::Form B(Q,mn) (JKFIT)");
    auto bQmn_ao_scf = formb_ao(primary_, auxiliary_scf_, zero, Jm12scf, "B(Q|mn) Reference");
    dct_timer_off("DCTSolver::Form B(Q,mn) (JKFIT)");

    dct_timer_on("DCTSolver::Transform B(Q,mn) (JKFIT)");
    bQmn_so_scf_ = transform_b_ao2so(bQmn_ao_scf);
    dct_timer_off("DCTSolver::Transform B(Q,mn) (JKFIT)");

    dct_timer_off("DCTSolver::df_build_b()");
}

/*
 * Form J(P,Q)^-1/2
 */
Matrix DCTSolver::formJm12(std::shared_ptr<BasisSet> auxiliary, const std::string& name) {
    auto metric_obj = FittingMetric(auxiliary, true);
    metric_obj.form_eig_inverse(1.0E-12); // This is hardcoded at present, but should be replaced with a global fitting option...
    auto metric = *metric_obj.get_metric();
    metric.set_name(name);
    // Save the metric for later use.
    metric.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::LowerTriangle); 
    return metric;
}

/**
 * Form b(Q|mn)
 */
Matrix DCTSolver::formb_ao(std::shared_ptr<BasisSet> primary, std::shared_ptr<BasisSet> auxiliary,
                          std::shared_ptr<BasisSet> zero, const Matrix& Jm12, const std::string& name) {
    auto nQ = auxiliary->nbf();
    auto A_ao = Matrix(nQ, nso_ * nso_);
    auto Bp = A_ao.pointer();

    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    // => Integrals <= //
    auto rifactory2 = std::make_shared<IntegralFactory>(auxiliary, zero, primary, primary);
    std::vector<std::shared_ptr<TwoBodyAOInt>> eri;
    std::vector<const double*> buffer;
    for (int t = 0; t < nthreads; t++) {
        eri.push_back(std::shared_ptr<TwoBodyAOInt>(rifactory2->eri()));
        buffer.push_back(eri[t]->buffer());
    }
    const auto& shell_pairs = eri[0]->shell_pairs();
    size_t npairs = shell_pairs.size();

    // => Memory Constraints <= //
    int max_rows;
    max_rows = auxiliary->nshell();

    // => Block Sizing <= //
    std::vector<int> Pstarts;
    int counter = 0;
    Pstarts.push_back(0);
    for (int P = 0; P < auxiliary->nshell(); P++) {
        int nP = auxiliary->shell(P).nfunction();
        if (counter + nP > max_rows) {
            counter = 0;
            Pstarts.push_back(P);
        }
        counter += nP;
    }
    Pstarts.push_back(auxiliary->nshell());

    // => Master Loop <= //

    for (int block = 0; block < Pstarts.size() - 1; block++) {
        // > Sizing < //

        int Pstart = Pstarts[block];
        int Pstop = Pstarts[block + 1];
        int NP = Pstop - Pstart;

        int pstart = auxiliary->shell(Pstart).function_index();
        int pstop = (Pstop == auxiliary->nshell() ? nQ : auxiliary->shell(Pstop).function_index());
        int np = pstop - pstart;

// > Integrals < //
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
        for (long int PMN = 0L; PMN < NP * npairs; PMN++) {
            int thread = 0;
#ifdef _OPENMP
            thread = omp_get_thread_num();
#endif

            int P = PMN / npairs + Pstart;
            int MN = PMN % npairs;
            int M = shell_pairs[MN].first;
            int N = shell_pairs[MN].second;

            eri[thread]->compute_shell(P, 0, M, N);
            buffer[thread] = eri[thread]->buffer();

            int nP = auxiliary->shell(P).nfunction();
            int oP = auxiliary->shell(P).function_index();

            int nM = primary->shell(M).nfunction();
            int oM = primary->shell(M).function_index();

            int nN = primary->shell(N).nfunction();
            int oN = primary->shell(N).function_index();

            int index = 0;
            for (int p = 0; p < nP; p++) {
                for (int m = 0; m < nM; m++) {
                    for (int n = 0; n < nN; n++, index++) {
                        Bp[p + oP][(m + oM) * nso_ + (n + oN)] = buffer[thread][index];
                        Bp[p + oP][(n + oN) * nso_ + (m + oM)] = buffer[thread][index];
                    }
                }
            }
        }
    }

    auto b = linalg::doublet(Jm12, A_ao, false, false);
    b.set_name(name);
    // Cache this for possible use in the gradient program.
    b.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    return b;
}

/**
 * Calculate memory required for density-fitting
 */
void DCTSolver::df_memory() const {
    double memory = Process::environment.get_memory();
    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    outfile->Printf("\t => Sizing <=\n\n");
    outfile->Printf("\t  Memory   = %11ld MB\n", long(memory) / (1024L * 1024L));
    outfile->Printf("\t  Threads  = %11d\n", nthreads);
    outfile->Printf("\t  nn       = %11d\n", nn_);
    outfile->Printf("\t  nQ       = %11d\n\n", nQ_);
    outfile->Printf("\t => Primary Basis <=\n\n");
    primary_->print();
    outfile->Printf("\t => Auxiliary Basis <=\n\n");
    auxiliary_->print();

    // Memory requirements
    outfile->Printf("\t => Memory Requirement <=\n\n");

    double cost_df = 0.0;

    if (options_.get_str("REFERENCE") == "RHF") {
        cost_df += nQ_ * nQ_;                                             // J(P|Q)-1/2
        cost_df += 2 * nQ_ * nso_ * nso_;                                 // b(Q|mn)
        cost_df += nQ_ * nalpha_ * nalpha_;                               // b(Q|oo)
        cost_df += 2 * nQ_ * nalpha_ * navir_;                            // b(Q|ov) and b(Q|vo)
        cost_df += nQ_ * navir_ * navir_;                                 // b(Q|vv)
        cost_df += nQ_ * nso_ * nso_;                                     // b(Q|pq)
        cost_df += 2 * navirpi_.max() * navirpi_.max() * navirpi_.max();  // (V'V|VV)
    } else {
        cost_df += nQ_ * nQ_;                                             // J(P|Q)-1/2
        cost_df += 2 * nQ_ * nso_ * nso_;                                 // b(Q|mn)
        cost_df += 2 * nQ_ * nalpha_ * nalpha_;                           // b(Q|oo)
        cost_df += 4 * nQ_ * nalpha_ * navir_;                            // b(Q|ov) and b(Q|vo)
        cost_df += 2 * nQ_ * navir_ * navir_;                             // b(Q|vv)
        cost_df += 2 * nQ_ * nso_ * nso_;                                 // b(Q|pq)
        cost_df += 2 * navirpi_.max() * navirpi_.max() * navirpi_.max();  // (V'V|VV)
    }

    cost_df *= sizeof(double);
    cost_df /= 1024.0 * 1024.0;

    double memory_mb = (double)memory / (1024.0 * 1024.0);
    outfile->Printf("\tMinimum Memory required                 : %9.2lf MB \n", cost_df);
    outfile->Printf("\tMemory available                        : %9.2lf MB \n\n", memory_mb);
    //    if(cost_df >= memory_mb)
    //            throw PSIEXCEPTION("There is NOT enough memory for ABCD-type contraction!");
}

/**
 * Transform b(Q|mn) -> b(Q|pq)
 */
void DCTSolver::transform_b() {
    dct_timer_on("DCTSolver::Transform B(Q,mn) -> B(Q,pq)");

    auto CaO = *Ca_subset("SO", "OCC");
    auto CaV = *Ca_subset("SO", "VIR");
    auto CaA = *Ca_subset("SO", "ALL");

    bQijA_mo_ = three_idx_primary_transform(bQmn_so_, CaO, CaO);
    bQiaA_mo_ = three_idx_primary_transform(bQmn_so_, CaO, CaV);
    bQabA_mo_ = three_idx_primary_transform(bQmn_so_, CaV, CaV);
    bQpqA_mo_ = three_idx_primary_transform(bQmn_so_, CaA, CaA);

    if (options_.get_str("REFERENCE") != "RHF") {
        auto CbO = *Cb_subset("SO", "OCC");
        auto CbV = *Cb_subset("SO", "VIR");
        auto CbA = *Cb_subset("SO", "ALL");

        bQijB_mo_ = three_idx_primary_transform(bQmn_so_, CbO, CbO);
        bQiaB_mo_ = three_idx_primary_transform(bQmn_so_, CbO, CbV);
        bQabB_mo_ = three_idx_primary_transform(bQmn_so_, CbV, CbV);
        bQpqB_mo_ = three_idx_primary_transform(bQmn_so_, CbA, CbA);
    }

    dct_timer_off("DCTSolver::Transform B(Q,mn) -> B(Q,pq)");
}

/**
 * Transform b(Q|mu,nu) from AO basis to SO basis
 */
Matrix DCTSolver::transform_b_ao2so(const Matrix& bQmn_ao) const {
    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    auto nQ = bQmn_ao.rowspi(0); // Read the number of aux. functions from the b matrix.
    auto bQmn_ao_p = bQmn_ao.pointer();

    // Set up dimensions for SO-basis b(Q|mn)
    Dimension Q(nirrep_), mn(nirrep_);
    for (int hn = 0; hn < nirrep_; ++hn) {
        Q[hn] = nQ;
        for (int hm = 0; hm < nirrep_; ++hm) {
            mn[hm ^ hn] += nsopi_[hm] * nsopi_[hn];
        }
    }
    auto bQmn_so = Matrix("Fully-transformed b", Q, mn);

    std::vector<int> offset(nirrep_, 0);

    // AO-basis b(Q|mn) -> SO-basis b(Q|mn)
    for (int h = 0; h < nirrep_; ++h) {
        auto bQmn_so_p = bQmn_so.pointer(h);
        for (int hm = 0; hm < nirrep_; ++hm) {
            int hn = h ^ hm;
            if (nsopi_[hm] > 0 && nsopi_[hn] > 0) {
                auto tmp = Matrix("Half-transformed b", nQ, nso_ * nsopi_[hn]);
                auto tmpp = tmp.pointer();
                auto ao2so_n_p = reference_wavefunction()->aotoso()->pointer(hn);
                auto ao2so_m_p = reference_wavefunction()->aotoso()->pointer(hm);
                // First-half transformation
                C_DGEMM('N', 'N', nQ * nso_, nsopi_[hn], nso_, 1.0, bQmn_ao_p[0], nso_, ao2so_n_p[0], nsopi_[hn], 0.0,
                        tmpp[0], nsopi_[hn]);
// Second-half transformation
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                for (int Q = 0; Q < nQ; ++Q) {
                    C_DGEMM('T', 'N', nsopi_[hm], nsopi_[hn], nso_, 1.0, ao2so_m_p[0], nsopi_[hm], tmpp[Q], nsopi_[hn],
                            0.0, bQmn_so_p[Q] + offset[h], nsopi_[hn]);
                }
            }
            offset[h] += nsopi_[hm] * nsopi_[hn];
        }
    }

    return bQmn_so;
}

// Convenience function for the simple case of a (Q|pq) pr qs -> (Q|qs).
Matrix DCTSolver::three_idx_primary_transform(const Matrix& three_idx, const Matrix& left, const Matrix& right) const {
    auto nQ = three_idx.rowdim(0);
    // Set up dimensions for b(Aux|PQ)
    Dimension LR(nirrep_), Aux(nirrep_);
    for (int hL = 0; hL < nirrep_; ++hL) {
        Aux[hL] = nQ;
        for (int hR = 0; hR < nirrep_; ++hR) {
            LR[hL ^ hR] += left.colspi(hL) * right.colspi(hR);
        }
    }

    auto result = Matrix("Three-Index Tensor", Aux, LR);

    three_idx_primary_transform_gemm(three_idx, left, right, result, 1.0, 0.0);

    return result;
}

// TODO: This should probably be migrated to/replaced with lib3index's DFHelper.
// However, we need symmetry, and lib3index currently doesn't support it. JPM 01/2021
void DCTSolver::three_idx_primary_transform_gemm(const Matrix& three_idx, const Matrix& left, const Matrix& right, Matrix& result, double alpha, double beta) const {
    dct_timer_on("DCTSolver::Three-Index SO -> MO");

    if (three_idx.symmetry() || left.symmetry() || right.symmetry() || result.symmetry())
        throw PSIEXCEPTION("three_idx_primary_transform_gemm: Can only handle totally symmetric matrices.");

    if (three_idx.nirrep() != left.nirrep() || three_idx.nirrep() != right.nirrep() || three_idx.nirrep() != result.nirrep()) {
        throw PSIEXCEPTION("three_idx_primary_transform_gemm: Number of irreps don't equal.");
    }

    if (three_idx.rowspi() != result.rowspi()) {
        throw PSIEXCEPTION("three_idx_primary_transform_gemm: Tensor to transform and result must agree about number of number of aux. functions");
    }

    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    auto nQ = three_idx.rowdim(0);
    std::vector<int> offset_mo(three_idx.nirrep(), 0), offset_so(three_idx.nirrep(), 0);

    for (int h = 0; h < nirrep_; ++h) {
        auto three_idx_p = three_idx.pointer(h);
        auto result_p = result.pointer(h);
        for (int hL = 0; hL < nirrep_; ++hL) {
            const auto hR = h ^ hL;
            if (left.colspi(hL) > 0 && right.colspi(hR) > 0 && left.rowspi(hL) > 0 && right.rowspi(hR) > 0) {
                const auto leftP = left.pointer(hL);
                const auto rightP = right.pointer(hR);
                auto tmp = Matrix("Half-Transformed", nQ, left.rowspi(hL) * right.colspi(hR));
                auto tmpp = tmp.pointer();
//#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                for (int Q = 0; Q < nQ; ++Q) {
                    // First-half transformation
                    C_DGEMM('N', 'N', left.rowspi(hL), right.colspi(hR), right.rowspi(hR), 1.0, three_idx_p[Q] + offset_so[h],
                            right.rowspi(hR), rightP[0], right.colspi(hR), 0.0, tmpp[Q], right.colspi(hR));
                    // Second-half transformation
                    C_DGEMM('T', 'N', left.colspi(hL), right.colspi(hR), left.rowspi(hL), alpha, leftP[0], left.colspi(hL), tmpp[Q],
                            right.colspi(hR), beta, result_p[Q] + offset_mo[h], right.colspi(hR));
                }
            }
            offset_so[h] += left.rowspi(hL) * right.rowspi(hR);
            offset_mo[h] += left.colspi(hL) * right.colspi(hR);
        }
        if (offset_so[h] != three_idx.colspi(h))
            throw PSIEXCEPTION("three_idx_primary_transform: Dimension mismatch");
    }

    dct_timer_off("DCTSolver::Three-Index SO -> MO");
}

/**
 * Transform b(Q|mu,nu) from SO to AO basis
 */
Matrix DCTSolver::transform_b_so2ao(const Matrix& bQmn_so) const {
    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    auto nQ = bQmn_so.rowspi(0);
    auto bQmn_ao = Matrix("AO basis quantity", nQ, bQmn_so.ncol());
    auto bQmn_ao_p = bQmn_ao.pointer();

    std::vector<int> offset(nirrep_, 0);

    for (int h = 0; h < nirrep_; ++h) {
        auto bQmn_so_p = bQmn_so.pointer(h);
        for (int hm = 0; h < nirrep_; ++h) {
            int hn = h ^ hm;
            auto morbs = aotoso()->colspi(hm);
            auto norbs = aotoso()->colspi(hn);
            if (morbs > 0 && norbs > 0) {
                auto m_p = aotoso()->pointer(hm);
                auto n_p = aotoso()->pointer(hn);
                auto tmp = Matrix("Half-transformed Matrix", nQ, morbs * nso_);
                auto tmpp = tmp.pointer();
                // First transformation
                C_DGEMM('N', 'T', nQ * morbs, nso_, norbs, 1.0, bQmn_so_p[0] + offset[h], norbs, n_p[0], nsopi_[hn], 0.0, tmpp[0], nso_);
                // Second transformation
                for (int Q = 0; Q < nQ; ++Q) {
                    C_DGEMM('N', 'N', nso_, nso_, morbs, 1.0, m_p[0], nsopi_[hm], tmpp[Q], nso_, 1.0, bQmn_ao_p[Q], nso_);
                }
            }
            offset[h] += morbs * norbs;
        }
    }

    return bQmn_ao;
}

/**
 * Form density-fitted MO-basis TEI g(OV|OV)
 */
void DCTSolver::form_df_g_ovov() {
    dct_timer_on("DCTSolver::DF Transform_OVOV");

    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    // Alpha-Alpha
    dpdbuf4 I;
    global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[O,V]"), ID("[O,V]"), ID("[O,V]"), ID("[O,V]"), 0,
                           "MO Ints (OV|OV)");
    for (int h = 0; h < nirrep_; ++h) {
        if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
            global_dpd_->buf4_mat_irrep_init(&I, h);
            double** bQiaA_mo_p = bQiaA_mo_.pointer(h);
            C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQiaA_mo_p[0], bQiaA_mo_.coldim(h),
                    bQiaA_mo_p[0], bQiaA_mo_.coldim(h), 0.0, I.matrix[h][0], I.params->coltot[h]);
            global_dpd_->buf4_mat_irrep_wrt(&I, h);
            global_dpd_->buf4_mat_irrep_close(&I, h);
        }
    }
    global_dpd_->buf4_close(&I);

    if (options_.get_str("REFERENCE") != "RHF") {
        // Alpha-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[O,V]"), ID("[o,v]"), ID("[O,V]"), ID("[o,v]"), 0,
                               "MO Ints (OV|ov)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                global_dpd_->buf4_mat_irrep_init(&I, h);
                double** bQiaA_mo_p = bQiaA_mo_.pointer(h);
                double** bQiaB_mo_p = bQiaB_mo_.pointer(h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQiaA_mo_p[0],
                        bQiaA_mo_.coldim(h), bQiaB_mo_p[0], bQiaB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);

        // Beta-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[o,v]"), ID("[o,v]"), ID("[o,v]"), ID("[o,v]"), 0,
                               "MO Ints (ov|ov)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                global_dpd_->buf4_mat_irrep_init(&I, h);
                double** bQiaB_mo_p = bQiaB_mo_.pointer(h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQiaB_mo_p[0],
                        bQiaB_mo_.coldim(h), bQiaB_mo_p[0], bQiaB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);
    }

    dct_timer_off("DCTSolver::DF Transform_OVOV");
}

/**
 * Form density-fitted MO-basis TEI g(OO|OO)
 */
void DCTSolver::form_df_g_oooo() {
    dct_timer_on("DCTSolver::DF Transform_OOOO");

    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    // Alpha-Alpha
    dpdbuf4 I;
    global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[O,O]"), ID("[O,O]"), ID("[O>=O]+"), ID("[O>=O]+"), 0,
                           "MO Ints (OO|OO)");
    for (int h = 0; h < nirrep_; ++h) {
        if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
            double** bQijA_mo_p = bQijA_mo_.pointer(h);
            global_dpd_->buf4_mat_irrep_init(&I, h);
            C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQijA_mo_p[0], bQijA_mo_.coldim(h),
                    bQijA_mo_p[0], bQijA_mo_.coldim(h), 0.0, I.matrix[h][0], I.params->coltot[h]);
            global_dpd_->buf4_mat_irrep_wrt(&I, h);
            global_dpd_->buf4_mat_irrep_close(&I, h);
        }
    }
    global_dpd_->buf4_close(&I);

    if (options_.get_str("REFERENCE") != "RHF") {
        // Alpha-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[O,O]"), ID("[o,o]"), ID("[O>=O]+"), ID("[o>=o]+"), 0,
                               "MO Ints (OO|oo)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQijA_mo_p = bQijA_mo_.pointer(h);
                double** bQijB_mo_p = bQijB_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQijA_mo_p[0],
                        bQijA_mo_.coldim(h), bQijB_mo_p[0], bQijB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);

        // Beta-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[o,o]"), ID("[o,o]"), ID("[o>=o]+"), ID("[o>=o]+"), 0,
                               "MO Ints (oo|oo)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQijB_mo_p = bQijB_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQijB_mo_p[0],
                        bQijB_mo_.coldim(h), bQijB_mo_p[0], bQijB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);
    }

    dct_timer_off("DCTSolver::DF Transform_OOOO");
}

/**
 * Form density-fitted MO-basis TEI g(VV|OO)
 */
void DCTSolver::form_df_g_vvoo() {
    dct_timer_on("DCTSolver::DF Transform_OOVV");

    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    dpdbuf4 I;

    if (options_.get_str("REFERENCE") == "RHF") {
        // g(AB|IJ) = Sum_Q b(AB|Q) b(Q|IJ)
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[V,V]"), ID("[O,O]"), ID("[V>=V]+"), ID("[O>=O]+"), 0,
                               "MO Ints (VV|OO)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQabA_mo_p = bQabA_mo_.pointer(h);
                double** bQijA_mo_p = bQijA_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQabA_mo_p[0],
                        bQabA_mo_.coldim(h), bQijA_mo_p[0], bQijA_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);

    } else {
        // g(ab|ij) = Sum_Q b(ab|Q) b(Q|ij)

        // Alpha-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[V,V]"), ID("[o,o]"), ID("[V>=V]+"), ID("[o>=o]+"), 0,
                               "MO Ints (VV|oo)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQabA_mo_p = bQabA_mo_.pointer(h);
                double** bQijB_mo_p = bQijB_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQabA_mo_p[0],
                        bQabA_mo_.coldim(h), bQijB_mo_p[0], bQijB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);

        // g(ij|ab) = Sum_Q b(ij|Q) b(Q|ab)

        // Alpha-Alpha
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[O,O]"), ID("[V,V]"), ID("[O>=O]+"), ID("[V>=V]+"), 0,
                               "MO Ints (OO|VV)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQijA_mo_p = bQijA_mo_.pointer(h);
                double** bQabA_mo_p = bQabA_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQijA_mo_p[0],
                        bQijA_mo_.coldim(h), bQabA_mo_p[0], bQabA_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);

        // Alpha-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[O,O]"), ID("[v,v]"), ID("[O>=O]+"), ID("[v>=v]+"), 0,
                               "MO Ints (OO|vv)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQijA_mo_p = bQijA_mo_.pointer(h);
                double** bQabB_mo_p = bQabB_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQijA_mo_p[0],
                        bQijA_mo_.coldim(h), bQabB_mo_p[0], bQabB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);

        // Beta-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[o,o]"), ID("[v,v]"), ID("[o>=o]+"), ID("[v>=v]+"), 0,
                               "MO Ints (oo|vv)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQijB_mo_p = bQijB_mo_.pointer(h);
                double** bQabB_mo_p = bQabB_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQijB_mo_p[0],
                        bQijB_mo_.coldim(h), bQabB_mo_p[0], bQabB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);
    }

    dct_timer_off("DCTSolver::DF Transform_OOVV");
}

/**
 * Form density-fitted MO-basis TEI g(VO|OO)
 */
void DCTSolver::form_df_g_vooo() {
    dct_timer_on("DCTSolver::DF Transform_VOOO");

    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    dpdbuf4 I;

    /*** Form b(Q|AI) ***/

    // Put detailed information of b(Q|ai) block into 'block_Qai'
    // Put detailed information of b(Q|ia) block into 'block_Qia'
    std::vector<std::vector<std::pair<long int, long int>>> block_Qai, block_Qia;
    Dimension VO(nirrep_), Q(nirrep_);
    for (int h = 0; h < nirrep_; ++h) {
        long int entrance_Qai = 0;
        long int entrance_Qia = 0;
        std::vector<std::pair<long int, long int>> subblock_Qai;
        std::vector<std::pair<long int, long int>> subblock_Qia;
        // b(Q|ai) subblocks
        for (int ha = 0; ha < nirrep_; ++ha) {
            int hi = h ^ ha;
            std::pair<long int, long int> subsubblock(entrance_Qai, navirpi_[ha] * naoccpi_[hi]);
            subblock_Qai.push_back(subsubblock);
            entrance_Qai += subsubblock.second;
        }
        block_Qai.push_back(subblock_Qai);
        // b(Q|ia) subblocks
        for (int hi = 0; hi < nirrep_; ++hi) {
            int ha = h ^ hi;
            std::pair<long int, long int> subsubblock(entrance_Qia, naoccpi_[hi] * navirpi_[ha]);
            subblock_Qia.push_back(subsubblock);
            entrance_Qia += subsubblock.second;
        }
        block_Qia.push_back(subblock_Qia);
        // Dimension of b(Q|ai)
        Q[h] = nQ_;
        VO[h] = entrance_Qai;
    }

    // Sort b(Q|IA) -> b(Q|AI)
    auto bQaiA_mo = Matrix("b(Q|AI)", Q, VO);
    for (int h = 0; h < nirrep_; ++h) {
        for (int hA = 0; hA < nirrep_; ++hA) {
            int hI = h ^ hA;
            if (navirpi_[hA] > 0 && naoccpi_[hI] > 0) {
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                for (int A = 0; A < navirpi_[hA]; ++A) {
                    for (int I = 0; I < naoccpi_[hI]; ++I) {
                        long int IA = block_Qia[h][hI].first + I * navirpi_[hA] + A;
                        long int AI = block_Qai[h][hA].first + A * naoccpi_[hI] + I;
                        bQaiA_mo.set_column(h, AI, bQiaA_mo_.get_column(h, IA));
                    }
                }
            }
        }
    }

    // g(ai|jk) = Sum_Q b(ai|Q) (Q|jk)

    // Alpha-Alpha
    global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[V,O]"), ID("[O,O]"), ID("[V,O]"), ID("[O>=O]+"), 0,
                           "MO Ints (VO|OO)");
    for (int h = 0; h < nirrep_; ++h) {
        if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
            double** bQaiA_mo_p = bQaiA_mo.pointer(h);
            double** bQijA_mo_p = bQijA_mo_.pointer(h);
            global_dpd_->buf4_mat_irrep_init(&I, h);
            C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQaiA_mo_p[0], bQaiA_mo.coldim(h),
                    bQijA_mo_p[0], bQijA_mo_.coldim(h), 0.0, I.matrix[h][0], I.params->coltot[h]);
            global_dpd_->buf4_mat_irrep_wrt(&I, h);
            global_dpd_->buf4_mat_irrep_close(&I, h);
        }
    }
    global_dpd_->buf4_close(&I);

    if (options_.get_str("REFERENCE") != "RHF") {
        /*** Form b(Q|ai) ***/

        // Put detailed information of b(Q|ai) block into 'block_Qai'
        // Put detailed information of b(Q|ia) block into 'block_Qia'
        std::vector<std::vector<std::pair<long int, long int>>> block_Qai, block_Qia;
        Dimension vo(nirrep_), Q(nirrep_);
        for (int h = 0; h < nirrep_; ++h) {
            long int entrance_Qai = 0;
            long int entrance_Qia = 0;
            std::vector<std::pair<long int, long int>> subblock_Qai;
            std::vector<std::pair<long int, long int>> subblock_Qia;
            // b(Q|ai) subblocks
            for (int ha = 0; ha < nirrep_; ++ha) {
                int hi = h ^ ha;
                std::pair<long int, long int> subsubblock(entrance_Qai, nbvirpi_[ha] * nboccpi_[hi]);
                subblock_Qai.push_back(subsubblock);
                entrance_Qai += subsubblock.second;
            }
            block_Qai.push_back(subblock_Qai);
            // b(Q|ia) subblocks
            for (int hi = 0; hi < nirrep_; ++hi) {
                int ha = h ^ hi;
                std::pair<long int, long int> subsubblock(entrance_Qia, nboccpi_[hi] * nbvirpi_[ha]);
                subblock_Qia.push_back(subsubblock);
                entrance_Qia += subsubblock.second;
            }
            block_Qia.push_back(subblock_Qia);
            // Dimension of b(Q|ai)
            Q[h] = nQ_;
            vo[h] = entrance_Qai;
        }

        // Sort b(Q|ia) -> b(Q|ai)
        auto bQaiB_mo = Matrix("b(Q|ai)", Q, vo);
        for (int h = 0; h < nirrep_; ++h) {
            for (int ha = 0; ha < nirrep_; ++ha) {
                int hi = h ^ ha;
                if (nbvirpi_[ha] > 0 && nboccpi_[hi] > 0) {
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                    for (int a = 0; a < nbvirpi_[ha]; ++a) {
                        for (int i = 0; i < nboccpi_[hi]; ++i) {
                            long int ia = block_Qia[h][hi].first + i * nbvirpi_[ha] + a;
                            long int ai = block_Qai[h][ha].first + a * nboccpi_[hi] + i;
                            bQaiB_mo.set_column(h, ai, bQiaB_mo_.get_column(h, ia));
                        }
                    }
                }
            }
        }

        // g(ai|jk) = Sum_Q b(ai|Q) (Q|jk)

        // Alpha-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[V,O]"), ID("[o,o]"), ID("[V,O]"), ID("[o>=o]+"), 0,
                               "MO Ints (VO|oo)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQaiA_mo_p = bQaiA_mo.pointer(h);
                double** bQijB_mo_p = bQijB_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQaiA_mo_p[0],
                        bQaiA_mo.coldim(h), bQijB_mo_p[0], bQijB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);

        // Beta-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[v,o]"), ID("[o,o]"), ID("[v,o]"), ID("[o>=o]+"), 0,
                               "MO Ints (vo|oo)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQaiB_mo_p = bQaiB_mo.pointer(h);
                double** bQijB_mo_p = bQijB_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQaiB_mo_p[0],
                        bQaiB_mo.coldim(h), bQijB_mo_p[0], bQijB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);

        // g(jk|ai) = Sum_Q b(jk|Q) (Q|ai)

        // Alpha-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[O,O]"), ID("[v,o]"), ID("[O>=O]+"), ID("[v,o]"), 0,
                               "MO Ints (OO|vo)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQijA_mo_p = bQijA_mo_.pointer(h);
                double** bQaiB_mo_p = bQaiB_mo.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQijA_mo_p[0],
                        bQijA_mo_.coldim(h), bQaiB_mo_p[0], bQaiB_mo.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
    }

    dct_timer_off("DCTSolver::DF Transform_VOOO");
}

/**
 * Form density-fitted MO-basis TEI g(OV|VV)
 */
void DCTSolver::form_df_g_ovvv() {
    dct_timer_on("DCTSolver::DF Transform_OVVV");

    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    dpdbuf4 I;

    // g(ia|bc) = Sum_Q b(ia|Q) (Q|bc)

    // Alpha-Alpha
    global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[O,V]"), ID("[V,V]"), ID("[O,V]"), ID("[V>=V]+"), 0,
                           "MO Ints (OV|VV)");
    for (int h = 0; h < nirrep_; ++h) {
        if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
            double** bQiaA_mo_p = bQiaA_mo_.pointer(h);
            double** bQabA_mo_p = bQabA_mo_.pointer(h);
            global_dpd_->buf4_mat_irrep_init(&I, h);
            C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQiaA_mo_p[0], bQiaA_mo_.coldim(h),
                    bQabA_mo_p[0], bQabA_mo_.coldim(h), 0.0, I.matrix[h][0], I.params->coltot[h]);
            global_dpd_->buf4_mat_irrep_wrt(&I, h);
            global_dpd_->buf4_mat_irrep_close(&I, h);
        }
    }
    global_dpd_->buf4_close(&I);

    if (options_.get_str("REFERENCE") != "RHF") {
        // g(ia|bc) = Sum_Q b(ia|Q) (Q|bc)

        // Alpha-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[O,V]"), ID("[v,v]"), ID("[O,V]"), ID("[v>=v]+"), 0,
                               "MO Ints (OV|vv)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQiaA_mo_p = bQiaA_mo_.pointer(h);
                double** bQabB_mo_p = bQabB_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQiaA_mo_p[0],
                        bQiaA_mo_.coldim(h), bQabB_mo_p[0], bQabB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);

        // Beta-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[o,v]"), ID("[v,v]"), ID("[o,v]"), ID("[v>=v]+"), 0,
                               "MO Ints (ov|vv)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQiaB_mo_p = bQiaB_mo_.pointer(h);
                double** bQabB_mo_p = bQabB_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQiaB_mo_p[0],
                        bQiaB_mo_.coldim(h), bQabB_mo_p[0], bQabB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);

        // g(bc|ia) = Sum_Q b(bc|Q) (Q|ia)

        // Alpha-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[V,V]"), ID("[o,v]"), ID("[V>=V]+"), ID("[o,v]"), 0,
                               "MO Ints (VV|ov)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQabA_mo_p = bQabA_mo_.pointer(h);
                double** bQiaB_mo_p = bQiaB_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQabA_mo_p[0],
                        bQabA_mo_.coldim(h), bQiaB_mo_p[0], bQiaB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);
    }

    dct_timer_off("DCTSolver::DF Transform_OVVV");
}

/**
 * Form density-fitted MO-basis TEI g(VV|VV)
 */
void DCTSolver::form_df_g_vvvv() {
    dct_timer_on("DCTSolver::DF Transform_VVVV");

    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    dpdbuf4 I;

    // g(ab|cd) = Sum_Q b(ab|Q) b(Q|cd)
    // Alpha-Alpha
    global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[V,V]"), ID("[V,V]"), ID("[V>=V]+"), ID("[V>=V]+"), 0,
                           "MO Ints (VV|VV)");
    for (int h = 0; h < nirrep_; ++h) {
        if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
            double** bQabA_mo_p = bQabA_mo_.pointer(h);
            global_dpd_->buf4_mat_irrep_init(&I, h);
            C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQabA_mo_p[0], bQabA_mo_.coldim(h),
                    bQabA_mo_p[0], bQabA_mo_.coldim(h), 0.0, I.matrix[h][0], I.params->coltot[h]);
            global_dpd_->buf4_mat_irrep_wrt(&I, h);
            global_dpd_->buf4_mat_irrep_close(&I, h);
        }
    }
    global_dpd_->buf4_close(&I);

    if (options_.get_str("REFERENCE") != "RHF") {
        // Alpha-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[V,V]"), ID("[v,v]"), ID("[V>=V]+"), ID("[v>=v]+"), 0,
                               "MO Ints (VV|vv)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQabA_mo_p = bQabA_mo_.pointer(h);
                double** bQabB_mo_p = bQabB_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQabA_mo_p[0],
                        bQabA_mo_.coldim(h), bQabB_mo_p[0], bQabB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);

        // Beta-Beta
        global_dpd_->buf4_init(&I, PSIF_LIBTRANS_DPD, 0, ID("[v,v]"), ID("[v,v]"), ID("[v>=v]+"), ID("[v>=v]+"), 0,
                               "MO Ints (vv|vv)");
        for (int h = 0; h < nirrep_; ++h) {
            if (I.params->rowtot[h] > 0 && I.params->coltot[h] > 0) {
                double** bQabB_mo_p = bQabB_mo_.pointer(h);
                global_dpd_->buf4_mat_irrep_init(&I, h);
                C_DGEMM('T', 'N', I.params->rowtot[h], I.params->coltot[h], nQ_, 1.0, bQabB_mo_p[0],
                        bQabB_mo_.coldim(h), bQabB_mo_p[0], bQabB_mo_.coldim(h), 0.0, I.matrix[h][0],
                        I.params->coltot[h]);
                global_dpd_->buf4_mat_irrep_wrt(&I, h);
                global_dpd_->buf4_mat_irrep_close(&I, h);
            }
        }
        global_dpd_->buf4_close(&I);
    }

    dct_timer_off("DCTSolver::DF Transform_VVVV");
}

/**
 * Compute the density-fitted ERI <vv||vv> tensors in G intermediates
 * and contract with lambda_ijcd.
 * Compute the density-fitted ERI <qs|pr> tensors
 * and contract with gamma<r|s>
 */
void DCTSolver::build_DF_tensors_RHF() {
    dct_timer_on("DCTSolver::build_df_tensors_RHF()");
    // Form gbar<AB|CD> lambda<CD|IJ>
    build_gbarlambda_RHF_v3mem();

    // Build Tau matrix in MO basis (All)
    mo_tauA_ = Matrix("MO basis Tau", nirrep_, nmopi_, nmopi_);
#pragma omp parallel for
    for (int h = 0; h < nirrep_; ++h) {
        for (int i = 0; i < naoccpi_[h]; ++i) {
            for (int j = 0; j < naoccpi_[h]; ++j) {
                mo_tauA_.set(h, i, j, aocc_tau_->get(h, i, j));
            }
        }
    }

#pragma omp parallel for
    for (int h = 0; h < nirrep_; ++h) {
        for (int a = naoccpi_[h]; a < nmopi_[h]; ++a) {
            for (int b = naoccpi_[h]; b < nmopi_[h]; ++b) {
                mo_tauA_.set(h, a, b, avir_tau_->get(h, a - naoccpi_[h], b - naoccpi_[h]));
            }
        }
    }

    /* Build [Gbar*Gamma]<Q|P> */
    build_gbarGamma_RHF();

    dct_timer_off("DCTSolver::build_df_tensors_RHF()");
}

/**
 * Compute the contraction, gbar<ab|cd> lambda<ij|cd>, using density fitting.
 * Memory required: O(V^3)
 */
void DCTSolver::build_gbarlambda_RHF_v3mem() {
    dct_timer_on("DCTSolver::DF lambda<ij|cd> gbar<ab|cd> (v3 in memory)");

    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    // Put detailed information of b(Q|ab) block into 'block'
    std::vector<std::vector<std::pair<long int, long int>>> block;
    for (int hab = 0; hab < nirrep_; ++hab) {
        long int entrance = 0;
        std::vector<std::pair<long int, long int>> subblock;
        for (int ha = 0; ha < nirrep_; ++ha) {
            int hb = hab ^ ha;
            std::pair<long int, long int> subsubblock(entrance, navirpi_[ha] * navirpi_[hb]);
            subblock.push_back(subsubblock);
            entrance += subsubblock.second;
        }
        block.push_back(subblock);
    }

    /*
     * Intermediate G_SF_<IJ|AB> = lambda_SF_<IJ|CD> g<AB|CD>
     */

    dpdbuf4 Laa, Gaa;

    global_dpd_->buf4_init(&Laa, PSIF_DCT_DPD, 0, ID("[O,O]"), ID("[V,V]"), ID("[O,O]"), ID("[V,V]"), 0,
                           "Amplitude SF <OO|VV>");
    global_dpd_->buf4_init(&Gaa, PSIF_DCT_DPD, 0, ID("[O,O]"), ID("[V,V]"), ID("[O,O]"), ID("[V,V]"), 0,
                           "tau(temp) SF <OO|VV>");
    global_dpd_->buf4_scm(&Gaa, 0.0);

    for (int hac = 0; hac < nirrep_; ++hac) {
        for (int ha = 0; ha < nirrep_; ++ha) {
            int hc = hac ^ ha;
            int hbd = hac;
            for (int hb = 0; hb < nirrep_; ++hb) {
                int hd = hbd ^ hb;
                int hab = ha ^ hb;
                int hcd = hc ^ hd;
                int hij = hcd;

                if (Laa.params->rowtot[hij] > 0 && Laa.params->coltot[hcd] > 0 && Gaa.params->rowtot[hij] > 0 &&
                    Gaa.params->coltot[hab] > 0 && navirpi_[ha] > 0 && navirpi_[hc] > 0 && navirpi_[hb] > 0 &&
                    navirpi_[hd] > 0) {
                    double** bQvvAp = bQabA_mo_.pointer(hac);

                    global_dpd_->buf4_mat_irrep_init(&Laa, hij);
                    global_dpd_->buf4_mat_irrep_rd(&Laa, hij);
                    global_dpd_->buf4_mat_irrep_init(&Gaa, hij);
                    global_dpd_->buf4_mat_irrep_rd(&Gaa, hij);

                    if (hb == hd) {
                        std::vector<SharedMatrix> CBD;
                        for (int i = 0; i < nthreads; ++i) {
                            CBD.push_back(
                                std::make_shared<Matrix>("g(A'C|BD)", navirpi_[hc], navirpi_[hb] * navirpi_[hd]));
                        }
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                        for (int A = 0; A < navirpi_[ha]; ++A) {
                            int thread = 0;
#ifdef _OPENMP
                            thread = omp_get_thread_num();
#endif
                            double** CBDp = CBD[thread]->pointer();

                            // g(A'C|BD) = b(A'C|Q) b(Q|BD)
                            C_DGEMM('T', 'N', navirpi_[hc], navirpi_[hb] * navirpi_[hd], nQ_, 1.0,
                                    bQvvAp[0] + block[hac][ha].first + A * navirpi_[hc], bQabA_mo_.coldim(hac),
                                    bQvvAp[0] + block[hbd][hb].first, bQabA_mo_.coldim(hbd), 0.0, CBDp[0],
                                    navirpi_[hb] * navirpi_[hd]);
                            // G<IJ|A'B> = lambda<IJ|CD> g(A'C|DB)
                            C_DGEMM('N', 'N', Gaa.params->rowtot[hij], navirpi_[hb], navirpi_[hc] * navirpi_[hd], 1.0,
                                    Laa.matrix[hij][0] + block[hcd][hc].first, Laa.params->coltot[hij], CBDp[0],
                                    navirpi_[hb], 1.0, Gaa.matrix[hij][0] + block[hab][ha].first + A * navirpi_[hb],
                                    Gaa.params->coltot[hij]);
                        }
                    } else {
                        std::vector<SharedMatrix> CBD, CDB;
                        for (int i = 0; i < nthreads; ++i) {
                            CBD.push_back(
                                std::make_shared<Matrix>("g(A'C|BD)", navirpi_[hc], navirpi_[hb] * navirpi_[hd]));
                            CDB.push_back(
                                std::make_shared<Matrix>("g(A'C|DB)", navirpi_[hc], navirpi_[hd] * navirpi_[hb]));
                        }
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                        for (int A = 0; A < navirpi_[ha]; ++A) {
                            int thread = 0;
#ifdef _OPENMP
                            thread = omp_get_thread_num();
#endif
                            double** CBDp = CBD[thread]->pointer();

                            // g(A'C|BD) = b(A'C|Q) b(Q|BD)
                            C_DGEMM('T', 'N', navirpi_[hc], navirpi_[hb] * navirpi_[hd], nQ_, 1.0,
                                    bQvvAp[0] + block[hac][ha].first + A * navirpi_[hc], bQabA_mo_.coldim(hac),
                                    bQvvAp[0] + block[hbd][hb].first, bQabA_mo_.coldim(hbd), 0.0, CBDp[0],
                                    navirpi_[hb] * navirpi_[hd]);

                            // g(A'C|BD) -> g(A'C|DB)
                            for (int B = 0; B < navirpi_[hb]; ++B) {
                                for (int D = 0; D < navirpi_[hd]; ++D)
                                    CDB[thread]->set_column(0, D * navirpi_[hb] + B,
                                                            CBD[thread]->get_column(0, B * navirpi_[hd] + D));
                            }

                            double** CDBp = CDB[thread]->pointer();

                            // G<IJ|A'B> = lambda<IJ|CD> g(A'C|DB)
                            C_DGEMM('N', 'N', Gaa.params->rowtot[hij], navirpi_[hb], navirpi_[hc] * navirpi_[hd], 1.0,
                                    Laa.matrix[hij][0] + block[hcd][hc].first, Laa.params->coltot[hij], CDBp[0],
                                    navirpi_[hb], 1.0, Gaa.matrix[hij][0] + block[hab][ha].first + A * navirpi_[hb],
                                    Gaa.params->coltot[hij]);
                        }
                    }
                    global_dpd_->buf4_mat_irrep_wrt(&Gaa, hij);
                    global_dpd_->buf4_mat_irrep_close(&Gaa, hij);

                    global_dpd_->buf4_mat_irrep_close(&Laa, hij);
                }
            }
        }
    }

    global_dpd_->buf4_close(&Laa);
    global_dpd_->buf4_close(&Gaa);

    dct_timer_off("DCTSolver::DF lambda<ij|cd> gbar<ab|cd> (v3 in memory)");
}

/**
 * Form MO-based contraction [Gbar*Gamma]<q|p>
 * [Gbar*Gamma]<q|p> = Sum_rs Gbar<qs|pr> Gamma<r|s>
 */
void DCTSolver::build_gbarGamma_RHF() {
    dct_timer_on("DCTSolver::Gbar<QS|PR> Gamma<R|S> (FastBuilder)");

    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    // Form gamma<R|S> = kappa<R|S> + tau<R|S>
    mo_gammaA_ = Matrix("MO-basis Gamma", nirrep_, nmopi_, nmopi_);
    //    mo_gammaA_->copy(kappa_mo_a_);
    //    mo_gammaA_->add(mo_tauA_);
    mo_gbarGamma_A_ = Matrix("MO-basis Gbar*Gamma", nirrep_, nmopi_, nmopi_);
    mo_gammaA_.copy(mo_tauA_);
    mo_gammaA_.add(kappa_mo_a_);

    // Put detailed information of b(Q|pq) block into 'block'
    std::vector<std::vector<std::pair<long int, long int>>> block;
    for (int hpq = 0; hpq < nirrep_; ++hpq) {
        long int entrance = 0;
        std::vector<std::pair<long int, long int>> subblock;
        for (int hp = 0; hp < nirrep_; ++hp) {
            int hq = hpq ^ hp;
            std::pair<long int, long int> subsubblock(entrance, nsopi_[hp] * nsopi_[hq]);
            subblock.push_back(subsubblock);
            entrance += subsubblock.second;
        }
        block.push_back(subblock);
    }

    const auto bQpqA_mo_scf = three_idx_primary_transform(bQmn_so_scf_, *Ca_subset("SO", "ALL"), *Ca_subset("SO", "ALL"));

/*
 *  f_tilde <Q|P> = gbar<QS|PR> gamma<R|S> + gbar<Qs|Pr> gamma<r|s>
 *                  = 2 g(QP|SR) gamma<R|S> - g(QR|SP) gamma<R|S>
 *                  = 2 b(QP|Aux) b(Aux|SR) gamma<R|S> - b(QR|Aux) b(Aux|SP) gamma<R|S>
 */

    // (Q) = b(Q|SR) gamma<R|S>
    auto Q = Matrix("b(Q|SR)gamma<R|S>", 1, nQ_scf_);
    auto Qp = Q.pointer();
    const auto bQpqAp0 = bQpqA_mo_scf.pointer(0);
    for (int hr = 0; hr < nirrep_; ++hr) {
        int hs = hr;
        if (nsopi_[hr] > 0) {
            double** gamma_rs_p = mo_gammaA_.pointer(hr);
            C_DGEMV('N', nQ_scf_, nsopi_[hr] * nsopi_[hs], 1.0, bQpqAp0[0] + block[0][hr].first,
                    bQpqA_mo_scf.coldim(0), gamma_rs_p[0], 1, 1.0, Qp[0], 1);
        }
    }
    // This Q intermediate can be reused when computing gradients! Save it.
    Q.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);

// f_tilde <Q|P> = 2 b(QP|Aux) b(Aux|SR) gamma<R|S>
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
    for (int hq = 0; hq < nirrep_; ++hq) {
        int hp = hq;
        if (nsopi_[hq] > 0) {
            auto tFAp = mo_gbarGamma_A_.pointer(hq);
            // tilde_f <Q|P> = 2 b(QP|Aux)*(Aux) where (Aux) = (Q)
            C_DGEMV('T', nQ_scf_, nsopi_[hp] * nsopi_[hq], 2.0, bQpqAp0[0] + block[0][hp].first, bQpqA_mo_scf.coldim(0), Qp[0],
                    1, 0.0, tFAp[0], 1);
        }
    }

    // f_tilde <Q|P> -= b(QR|Aux) b(Aux|SP) gamma<R|S>
    for (int hq = 0; hq < nirrep_; ++hq) {
        int hp = hq;
        if (nsopi_[hq] > 0) {
            for (int hr = 0; hr < nirrep_; ++hr) {
                int hs = hr;
                if (nsopi_[hr] > 0) {
                    double** bQpqAp = bQpqA_mo_scf.pointer(hq ^ hr);
                    double** gamma_rs_p = mo_gammaA_.pointer(hr);

                    std::vector<SharedMatrix> rs;
                    for (int i = 0; i < nthreads; ++i) {
                        rs.push_back(std::make_shared<Matrix>("<Q'P'|RS>", nsopi_[hr], nsopi_[hs]));
                    }

#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                    for (int q = 0; q < nsopi_[hq]; ++q) {
                        for (int p = q; p < nsopi_[hp]; ++p) {
                            int thread = 0;
#ifdef _OPENMP
                            thread = omp_get_thread_num();
#endif
                            double** rsp = rs[thread]->pointer();

                            // <Q'P'|RS> = b(Q'R|Aux) b(Aux|P'S)
                            C_DGEMM('T', 'N', nsopi_[hr], nsopi_[hs], nQ_scf_, 1.0,
                                    bQpqAp[0] + block[hq ^ hr][hq].first + q * nsopi_[hr], bQpqA_mo_scf.coldim(hq ^ hr),
                                    bQpqAp[0] + block[hp ^ hs][hp].first + p * nsopi_[hs], bQpqA_mo_scf.coldim(hp ^ hs),
                                    0.0, rsp[0], nsopi_[hs]);
                            // - <Q'P'|RS> * gamma<R|S>
                            double value = -C_DDOT(nsopi_[hr] * nsopi_[hs], rsp[0], 1, gamma_rs_p[0], 1);
                            mo_gbarGamma_A_.add(hp, q, p, value);
                            if (q != p) {
                                mo_gbarGamma_A_.add(hp, p, q, value);
                            }
                        }
                    }
                }
            }
        }
    }

    dct_timer_off("DCTSolver::Gbar<QS|PR> Gamma<R|S> (FastBuilder)");
}

/**
 * Compute the density-fitted ERI <vv||vv> tensors in G intermediates
 * and contract with lambda_ijcd.
 * Compute the density-fitted ERI <qs|pr> tensors
 * and contract with gamma<r|s>
 */
void DCTSolver::build_DF_tensors_UHF() {
    dct_timer_on("DCTSolver::build_df_tensors_UHF");

    // Form gbar<AB|CD> lambda<CD|IJ>
    build_gbarlambda_UHF_v3mem();

    // Build Tau matrix in MO basis (All)
    // Alpha-Alpha
    mo_tauA_ = Matrix("MO basis Tau Alpha", nirrep_, nmopi_, nmopi_);
#pragma omp parallel for
    for (int h = 0; h < nirrep_; ++h) {
        for (int i = 0; i < naoccpi_[h]; ++i) {
            for (int j = 0; j < naoccpi_[h]; ++j) {
                mo_tauA_.set(h, i, j, aocc_tau_->get(h, i, j));
            }
        }
    }
#pragma omp parallel for
    for (int h = 0; h < nirrep_; ++h) {
        for (int a = naoccpi_[h]; a < nmopi_[h]; ++a) {
            for (int b = naoccpi_[h]; b < nmopi_[h]; ++b) {
                mo_tauA_.set(h, a, b, avir_tau_->get(h, a - naoccpi_[h], b - naoccpi_[h]));
            }
        }
    }

    // Beta-Beta
    mo_tauB_ = Matrix("MO basis Tau Beta", nirrep_, nmopi_, nmopi_);
#pragma omp parallel for
    for (int h = 0; h < nirrep_; ++h) {
        for (int i = 0; i < nboccpi_[h]; ++i) {
            for (int j = 0; j < nboccpi_[h]; ++j) {
                mo_tauB_.set(h, i, j, bocc_tau_->get(h, i, j));
            }
        }
    }
#pragma omp parallel for
    for (int h = 0; h < nirrep_; ++h) {
        for (int a = nboccpi_[h]; a < nmopi_[h]; ++a) {
            for (int b = nboccpi_[h]; b < nmopi_[h]; ++b) {
                mo_tauB_.set(h, a, b, bvir_tau_->get(h, a - nboccpi_[h], b - nboccpi_[h]));
            }
        }
    }

    /* Build [gbar*gamma]<q|p> */
    build_gbarGamma_UHF();

    dct_timer_off("DCTSolver::build_df_tensors_UHF");
}

/**
 * Compute the contraction, gbar<ab|cd> lambda<ij|cd>, using density fitting.
 * Memory required: O(V^3)
 */
void DCTSolver::build_gbarlambda_UHF_v3mem() {
    dct_timer_on("DCTSolver::DF lambda<ij|cd> gbar<ab|cd> (v3 in memory)");

    // Thread considerations
    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    /********** Alpha-Alpha **********/

    // block_ab[h1][h2] is (#AB pairs of irrep h1 and A irrep *before* h2, #AB pairs of irrep h1 and A irrep *of* h2)
    std::vector<std::vector<std::pair<long int, long int>>> block_AB;
    for (int hAB = 0; hAB < nirrep_; ++hAB) {
        long int entrance = 0;
        std::vector<std::pair<long int, long int>> subblock;
        for (int hA = 0; hA < nirrep_; ++hA) {
            int hB = hAB ^ hA;
            std::pair<long int, long int> subsubblock(entrance, navirpi_[hA] * navirpi_[hB]);
            subblock.push_back(subsubblock);
            entrance += subsubblock.second;
        }
        block_AB.push_back(subblock);
    }

    /*
     * Intermediate G <IJ|AB> = 1/2 lambda<IJ|CD> gbar<AB|CD>
     *                        = 1/2 lambda<IJ|CD> g(AC|BD) - 1/2 lambda<IJ|CD> g(AD|BC)
     *                        = lambda<IJ|CD> g(AC|BD)
     */

    dpdbuf4 Laa, Gaa;

    global_dpd_->buf4_init(&Laa, PSIF_DCT_DPD, 0, ID("[O,O]"), ID("[V,V]"), ID("[O>O]-"), ID("[V>V]-"), 0,
                           "Amplitude <OO|VV>");
    global_dpd_->buf4_init(&Gaa, PSIF_DCT_DPD, 0, ID("[O,O]"), ID("[V,V]"), ID("[O,O]"), ID("[V,V]"), 0,
                           "tau(temp) <OO|VV>");
    global_dpd_->buf4_scm(&Gaa, 0.0);

    for (int hAC = 0; hAC < nirrep_; ++hAC) {
        for (int hA = 0; hA < nirrep_; ++hA) {
            int hC = hAC ^ hA;
            int hBD = hAC;
            for (int hB = 0; hB < nirrep_; ++hB) {
                int hD = hBD ^ hB;
                int hAB = hA ^ hB;
                int hCD = hC ^ hD;
                int hIJ = hCD;

                if (Laa.params->rowtot[hIJ] > 0 && Laa.params->coltot[hCD] > 0 && Gaa.params->rowtot[hIJ] > 0 &&
                    Gaa.params->coltot[hAB] > 0 && navirpi_[hA] > 0 && navirpi_[hC] > 0 && navirpi_[hB] > 0 &&
                    navirpi_[hD] > 0) {
                    double** bQvvAp = bQabA_mo_.pointer(hAC);

                    global_dpd_->buf4_mat_irrep_init(&Laa, hIJ);
                    global_dpd_->buf4_mat_irrep_rd(&Laa, hIJ);
                    global_dpd_->buf4_mat_irrep_init(&Gaa, hIJ);
                    global_dpd_->buf4_mat_irrep_rd(&Gaa, hIJ);

                    if (hB == hD) {
                        std::vector<SharedMatrix> CBD;
                        for (int i = 0; i < nthreads; ++i) {
                            CBD.push_back(
                                std::make_shared<Matrix>("g(A'C|BD)", navirpi_[hC], navirpi_[hB] * navirpi_[hD]));
                        }
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                        for (int A = 0; A < navirpi_[hA]; ++A) {
                            int thread = 0;
#ifdef _OPENMP
                            thread = omp_get_thread_num();
#endif
                            double** CBDp = CBD[thread]->pointer();
                            // g(A'C|BD) = b(A'C|Q) b(Q|BD)
                            C_DGEMM('T', 'N', navirpi_[hC], navirpi_[hB] * navirpi_[hD], nQ_, 1.0,
                                    bQvvAp[0] + block_AB[hAC][hA].first + A * navirpi_[hC], bQabA_mo_.coldim(hAC),
                                    bQvvAp[0] + block_AB[hBD][hB].first, bQabA_mo_.coldim(hBD), 0.0, CBDp[0],
                                    navirpi_[hB] * navirpi_[hD]);
                            // G<IJ|A'B> = lambda<IJ|CD> g(A'C|DB)
                            C_DGEMM('N', 'N', Gaa.params->rowtot[hIJ], navirpi_[hB], navirpi_[hC] * navirpi_[hD], 1.0,
                                    Laa.matrix[hIJ][0] + block_AB[hCD][hC].first, Laa.params->coltot[hIJ], CBDp[0],
                                    navirpi_[hB], 1.0, Gaa.matrix[hIJ][0] + block_AB[hAB][hA].first + A * navirpi_[hB],
                                    Gaa.params->coltot[hIJ]);
                        }
                    } else {
                        std::vector<SharedMatrix> CBD, CDB;
                        for (int i = 0; i < nthreads; ++i) {
                            CBD.push_back(
                                std::make_shared<Matrix>("g(A'C|BD)", navirpi_[hC], navirpi_[hB] * navirpi_[hD]));
                            CDB.push_back(
                                std::make_shared<Matrix>("g(A'C|DB)", navirpi_[hC], navirpi_[hD] * navirpi_[hB]));
                        }
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                        for (int A = 0; A < navirpi_[hA]; ++A) {
                            int thread = 0;
#ifdef _OPENMP
                            thread = omp_get_thread_num();
#endif
                            double** CBDp = CBD[thread]->pointer();

                            // g(A'C|BD) = b(A'C|Q) b(Q|BD)
                            C_DGEMM('T', 'N', navirpi_[hC], navirpi_[hB] * navirpi_[hD], nQ_, 1.0,
                                    bQvvAp[0] + block_AB[hAC][hA].first + A * navirpi_[hC], bQabA_mo_.coldim(hAC),
                                    bQvvAp[0] + block_AB[hBD][hB].first, bQabA_mo_.coldim(hBD), 0.0, CBDp[0],
                                    navirpi_[hB] * navirpi_[hD]);

                            // g(A'C|BD) -> g(A'C|DB)
                            for (int B = 0; B < navirpi_[hB]; ++B) {
                                for (int D = 0; D < navirpi_[hD]; ++D)
                                    CDB[thread]->set_column(0, D * navirpi_[hB] + B,
                                                            CBD[thread]->get_column(0, B * navirpi_[hD] + D));
                            }

                            double** CDBp = CDB[thread]->pointer();

                            // G<IJ|A'B> = lambda<IJ|CD> g(A'C|DB)
                            C_DGEMM('N', 'N', Gaa.params->rowtot[hIJ], navirpi_[hB], navirpi_[hC] * navirpi_[hD], 1.0,
                                    Laa.matrix[hIJ][0] + block_AB[hCD][hC].first, Laa.params->coltot[hIJ], CDBp[0],
                                    navirpi_[hB], 1.0, Gaa.matrix[hIJ][0] + block_AB[hAB][hA].first + A * navirpi_[hB],
                                    Gaa.params->coltot[hIJ]);
                        }
                    }
                    global_dpd_->buf4_mat_irrep_wrt(&Gaa, hIJ);
                    global_dpd_->buf4_mat_irrep_close(&Gaa, hIJ);

                    global_dpd_->buf4_mat_irrep_close(&Laa, hIJ);
                }
            }
        }
    }

    global_dpd_->buf4_close(&Laa);
    global_dpd_->buf4_close(&Gaa);

    /********** Beta-Beta **********/

    // Put detailed information of b(Q|ab) block into 'block_ab'
    std::vector<std::vector<std::pair<long int, long int>>> block_ab;
    for (int hab = 0; hab < nirrep_; ++hab) {
        long int entrance = 0;
        std::vector<std::pair<long int, long int>> subblock;
        for (int ha = 0; ha < nirrep_; ++ha) {
            int hb = hab ^ ha;
            std::pair<long int, long int> subsubblock(entrance, nbvirpi_[ha] * nbvirpi_[hb]);
            subblock.push_back(subsubblock);
            entrance += subsubblock.second;
        }
        block_ab.push_back(subblock);
    }

    /*
     * Intermediate G <ij|ab> = 1/2 lambda<ij|cd> gbar<ab|cd>
     *                        = lambda<ij|cd> g(ac|bd)
     */

    dpdbuf4 Lbb, Gbb;

    global_dpd_->buf4_init(&Lbb, PSIF_DCT_DPD, 0, ID("[o,o]"), ID("[v,v]"), ID("[o>o]-"), ID("[v>v]-"), 0,
                           "Amplitude <oo|vv>");
    global_dpd_->buf4_init(&Gbb, PSIF_DCT_DPD, 0, ID("[o,o]"), ID("[v,v]"), ID("[o,o]"), ID("[v,v]"), 0,
                           "tau(temp) <oo|vv>");
    global_dpd_->buf4_scm(&Gbb, 0.0);

    for (int hac = 0; hac < nirrep_; ++hac) {
        for (int ha = 0; ha < nirrep_; ++ha) {
            int hc = hac ^ ha;
            int hbd = hac;
            for (int hb = 0; hb < nirrep_; ++hb) {
                int hd = hbd ^ hb;
                int hab = ha ^ hb;
                int hcd = hc ^ hd;
                int hij = hcd;

                if (Lbb.params->rowtot[hij] > 0 && Lbb.params->coltot[hcd] > 0 && Gbb.params->rowtot[hij] > 0 &&
                    Gbb.params->coltot[hab] > 0 && nbvirpi_[ha] > 0 && nbvirpi_[hc] > 0 && nbvirpi_[hb] > 0 &&
                    nbvirpi_[hd] > 0) {
                    double** bQvvBp = bQabB_mo_.pointer(hac);

                    global_dpd_->buf4_mat_irrep_init(&Lbb, hij);
                    global_dpd_->buf4_mat_irrep_rd(&Lbb, hij);
                    global_dpd_->buf4_mat_irrep_init(&Gbb, hij);
                    global_dpd_->buf4_mat_irrep_rd(&Gbb, hij);

                    if (hb == hd) {
                        std::vector<SharedMatrix> cbd;
                        for (int i = 0; i < nthreads; ++i) {
                            cbd.push_back(
                                std::make_shared<Matrix>("g(a'c|bd)", nbvirpi_[hc], nbvirpi_[hb] * nbvirpi_[hd]));
                        }
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                        for (int a = 0; a < nbvirpi_[ha]; ++a) {
                            int thread = 0;
#ifdef _OPENMP
                            thread = omp_get_thread_num();
#endif
                            double** cbdp = cbd[thread]->pointer();
                            // g(a'c|bd) = b(a'c|Q) b(Q|bd)
                            C_DGEMM('T', 'N', nbvirpi_[hc], nbvirpi_[hb] * nbvirpi_[hd], nQ_, 1.0,
                                    bQvvBp[0] + block_ab[hac][ha].first + a * nbvirpi_[hc], bQabB_mo_.coldim(hac),
                                    bQvvBp[0] + block_ab[hbd][hb].first, bQabB_mo_.coldim(hbd), 0.0, cbdp[0],
                                    nbvirpi_[hb] * nbvirpi_[hd]);
                            // G<ij|a'b> = lambda<ij|cd> g(a'c|db)
                            C_DGEMM('N', 'N', Gbb.params->rowtot[hij], nbvirpi_[hb], nbvirpi_[hc] * nbvirpi_[hd], 1.0,
                                    Lbb.matrix[hij][0] + block_ab[hcd][hc].first, Lbb.params->coltot[hij], cbdp[0],
                                    nbvirpi_[hb], 1.0, Gbb.matrix[hij][0] + block_ab[hab][ha].first + a * nbvirpi_[hb],
                                    Gbb.params->coltot[hij]);
                        }
                    } else {
                        std::vector<SharedMatrix> cbd, cdb;
                        for (int i = 0; i < nthreads; ++i) {
                            cbd.push_back(
                                std::make_shared<Matrix>("g(a'c|bd)", nbvirpi_[hc], nbvirpi_[hb] * nbvirpi_[hd]));
                            cdb.push_back(
                                std::make_shared<Matrix>("g(a'c|db)", nbvirpi_[hc], nbvirpi_[hd] * nbvirpi_[hb]));
                        }
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                        for (int a = 0; a < nbvirpi_[ha]; ++a) {
                            int thread = 0;
#ifdef _OPENMP
                            thread = omp_get_thread_num();
#endif
                            double** cbdp = cbd[thread]->pointer();

                            // g(a'c|bd) = b(a'c|Q) b(Q|bd)
                            C_DGEMM('T', 'N', nbvirpi_[hc], nbvirpi_[hb] * nbvirpi_[hd], nQ_, 1.0,
                                    bQvvBp[0] + block_ab[hac][ha].first + a * nbvirpi_[hc], bQabB_mo_.coldim(hac),
                                    bQvvBp[0] + block_ab[hbd][hb].first, bQabB_mo_.coldim(hbd), 0.0, cbdp[0],
                                    nbvirpi_[hb] * nbvirpi_[hd]);

                            // g(a'c|bd) -> g(a'c|db)
                            for (int b = 0; b < nbvirpi_[hb]; ++b) {
                                for (int d = 0; d < nbvirpi_[hd]; ++d)
                                    cdb[thread]->set_column(0, d * nbvirpi_[hb] + b,
                                                            cbd[thread]->get_column(0, b * nbvirpi_[hd] + d));
                            }

                            double** cdbp = cdb[thread]->pointer();

                            // G<ij|a'b> = lambda<ij|cd> g(a'c|db)
                            C_DGEMM('N', 'N', Gbb.params->rowtot[hij], nbvirpi_[hb], nbvirpi_[hc] * nbvirpi_[hd], 1.0,
                                    Lbb.matrix[hij][0] + block_ab[hcd][hc].first, Lbb.params->coltot[hij], cdbp[0],
                                    nbvirpi_[hb], 1.0, Gbb.matrix[hij][0] + block_ab[hab][ha].first + a * nbvirpi_[hb],
                                    Gbb.params->coltot[hij]);
                        }
                    }
                    global_dpd_->buf4_mat_irrep_wrt(&Gbb, hij);
                    global_dpd_->buf4_mat_irrep_close(&Gbb, hij);

                    global_dpd_->buf4_mat_irrep_close(&Lbb, hij);
                }
            }
        }
    }

    global_dpd_->buf4_close(&Lbb);
    global_dpd_->buf4_close(&Gbb);

    /********** Alpha-Beta **********/

    // Put detailed information of Ab block (as in lambda<Ij|Ab>) into 'block_Ab'
    std::vector<std::vector<std::pair<long int, long int>>> block_Ab;
    for (int hAb = 0; hAb < nirrep_; ++hAb) {
        long int entrance = 0;
        std::vector<std::pair<long int, long int>> subblock;
        for (int hA = 0; hA < nirrep_; ++hA) {
            int hb = hAb ^ hA;
            std::pair<long int, long int> subsubblock(entrance, navirpi_[hA] * nbvirpi_[hb]);
            subblock.push_back(subsubblock);
            entrance += subsubblock.second;
        }
        block_Ab.push_back(subblock);
    }

    /*
     * Intermediate G<Ij|Ab> = lambda<Ij|Cd> gbar<Ab|Cd>
     *                       = lambda<Ij|Cd> g(AC|bd)
     */

    dpdbuf4 Lab, Gab;

    global_dpd_->buf4_init(&Lab, PSIF_DCT_DPD, 0, ID("[O,o]"), ID("[V,v]"), ID("[O,o]"), ID("[V,v]"), 0,
                           "Amplitude <Oo|Vv>");
    global_dpd_->buf4_init(&Gab, PSIF_DCT_DPD, 0, ID("[O,o]"), ID("[V,v]"), ID("[O,o]"), ID("[V,v]"), 0,
                           "tau(temp) <Oo|Vv>");
    global_dpd_->buf4_scm(&Gab, 0.0);

    for (int hAC = 0; hAC < nirrep_; ++hAC) {
        for (int hA = 0; hA < nirrep_; ++hA) {
            int hC = hAC ^ hA;
            int hbd = hAC;
            for (int hb = 0; hb < nirrep_; ++hb) {
                int hd = hbd ^ hb;
                int hAb = hA ^ hb;
                int hCd = hC ^ hd;
                int hIj = hCd;

                if (Lab.params->rowtot[hIj] > 0 && Lab.params->coltot[hCd] > 0 && Gab.params->rowtot[hIj] > 0 &&
                    Gab.params->coltot[hAb] > 0 && navirpi_[hA] > 0 && navirpi_[hC] > 0 && nbvirpi_[hb] > 0 &&
                    nbvirpi_[hd] > 0) {
                    double** bQvvAp = bQabA_mo_.pointer(hAC);
                    double** bQvvBp = bQabB_mo_.pointer(hbd);

                    global_dpd_->buf4_mat_irrep_init(&Lab, hIj);
                    global_dpd_->buf4_mat_irrep_rd(&Lab, hIj);
                    global_dpd_->buf4_mat_irrep_init(&Gab, hIj);
                    global_dpd_->buf4_mat_irrep_rd(&Gab, hIj);

                    if (hb == hd) {
                        std::vector<SharedMatrix> Cbd;
                        for (int i = 0; i < nthreads; ++i) {
                            Cbd.push_back(
                                std::make_shared<Matrix>("g(A'C|bd)", navirpi_[hC], nbvirpi_[hb] * nbvirpi_[hd]));
                        }
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                        for (int A = 0; A < navirpi_[hA]; ++A) {
                            int thread = 0;
#ifdef _OPENMP
                            thread = omp_get_thread_num();
#endif
                            double** Cbdp = Cbd[thread]->pointer();
                            // g(A'C|bd) = b(A'C|Q) b(Q|bd)
                            C_DGEMM('T', 'N', navirpi_[hC], nbvirpi_[hb] * nbvirpi_[hd], nQ_, 1.0,
                                    bQvvAp[0] + block_AB[hAC][hA].first + A * navirpi_[hC], bQabA_mo_.coldim(hAC),
                                    bQvvBp[0] + block_ab[hbd][hb].first, bQabB_mo_.coldim(hbd), 0.0, Cbdp[0],
                                    nbvirpi_[hb] * nbvirpi_[hd]);
                            // G<Ij|A'b> = lambda<Ij|Cd> g(A'C|db)
                            C_DGEMM('N', 'N', Gab.params->rowtot[hIj], nbvirpi_[hb], navirpi_[hC] * nbvirpi_[hd], 1.0,
                                    Lab.matrix[hIj][0] + block_Ab[hCd][hC].first, Lab.params->coltot[hIj], Cbdp[0],
                                    nbvirpi_[hb], 1.0, Gab.matrix[hIj][0] + block_Ab[hAb][hA].first + A * nbvirpi_[hb],
                                    Gab.params->coltot[hIj]);
                        }
                    } else {
                        std::vector<SharedMatrix> Cbd, Cdb;
                        for (int i = 0; i < nthreads; ++i) {
                            Cbd.push_back(
                                std::make_shared<Matrix>("g(A'C|bd)", navirpi_[hC], nbvirpi_[hb] * nbvirpi_[hd]));
                            Cdb.push_back(
                                std::make_shared<Matrix>("g(A'C|db)", navirpi_[hC], nbvirpi_[hd] * nbvirpi_[hb]));
                        }
#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                        for (int A = 0; A < navirpi_[hA]; ++A) {
                            int thread = 0;
#ifdef _OPENMP
                            thread = omp_get_thread_num();
#endif
                            double** Cbdp = Cbd[thread]->pointer();

                            // g(A'C|bd) = b(A'C|Q) b(Q|bd)
                            C_DGEMM('T', 'N', navirpi_[hC], nbvirpi_[hb] * nbvirpi_[hd], nQ_, 1.0,
                                    bQvvAp[0] + block_AB[hAC][hA].first + A * navirpi_[hC], bQabA_mo_.coldim(hAC),
                                    bQvvBp[0] + block_ab[hbd][hb].first, bQabB_mo_.coldim(hbd), 0.0, Cbdp[0],
                                    nbvirpi_[hb] * nbvirpi_[hd]);

                            // g(A'C|bd) -> g(A'C|db)
                            for (int b = 0; b < nbvirpi_[hb]; ++b) {
                                for (int d = 0; d < nbvirpi_[hd]; ++d)
                                    Cdb[thread]->set_column(0, d * nbvirpi_[hb] + b,
                                                            Cbd[thread]->get_column(0, b * nbvirpi_[hd] + d));
                            }

                            double** Cdbp = Cdb[thread]->pointer();

                            // G<Ij|A'b> = lambda<Ij|Cd> g(A'C|db)
                            C_DGEMM('N', 'N', Gab.params->rowtot[hIj], nbvirpi_[hb], navirpi_[hC] * nbvirpi_[hd], 1.0,
                                    Lab.matrix[hIj][0] + block_Ab[hCd][hC].first, Lab.params->coltot[hIj], Cdbp[0],
                                    nbvirpi_[hb], 1.0, Gab.matrix[hIj][0] + block_Ab[hAb][hA].first + A * nbvirpi_[hb],
                                    Gab.params->coltot[hIj]);
                        }
                    }
                    global_dpd_->buf4_mat_irrep_wrt(&Gab, hIj);
                    global_dpd_->buf4_mat_irrep_close(&Gab, hIj);

                    global_dpd_->buf4_mat_irrep_close(&Lab, hIj);
                }
            }
        }
    }

    global_dpd_->buf4_close(&Lab);
    global_dpd_->buf4_close(&Gab);

    dct_timer_off("DCTSolver::DF lambda<ij|cd> gbar<ab|cd> (v3 in memory)");
}

/**
 * Form MO-based contraction [Gbar*Gamma]<q|p>
 * [Gbar*Gamma]<q|p> = Sum_rs Gbar<qs|pr> Gamma<r|s>
 */
void DCTSolver::build_gbarGamma_UHF() {
    dct_timer_on("DCTSolver::Gbar<QS|PR> Gamma<R|S> (FastBuilder)");

    int nthreads = 1;
#ifdef _OPENMP
    nthreads = Process::environment.get_n_threads();
#endif

    // Form gamma<R|S> = kappa<R|S> + tau<R|S>
    mo_gammaA_ = Matrix("MO-basis Gamma Alpha", nirrep_, nmopi_, nmopi_);
    mo_gbarGamma_A_ = Matrix("MO-basis Gbar_Gamma_A", nirrep_, nmopi_, nmopi_);
    mo_gammaB_ = Matrix("MO-basis Gamma Beta", nirrep_, nmopi_, nmopi_);
    mo_gbarGamma_B_ = Matrix("MO-basis Gbar_Gamma_B", nirrep_, nmopi_, nmopi_);

    mo_gammaA_.copy(mo_tauA_);
    mo_gammaA_.add(kappa_mo_a_);
    mo_gammaB_.copy(mo_tauB_);
    mo_gammaB_.add(kappa_mo_b_);

    // Put detailed information of b(Q|pq) block into 'block'
    std::vector<std::vector<std::pair<long int, long int>>> block;
    for (int hpq = 0; hpq < nirrep_; ++hpq) {
        long int entrance = 0;
        std::vector<std::pair<long int, long int>> subblock;
        for (int hp = 0; hp < nirrep_; ++hp) {
            int hq = hpq ^ hp;
            std::pair<long int, long int> subsubblock(entrance, nsopi_[hp] * nsopi_[hq]);
            subblock.push_back(subsubblock);
            entrance += subsubblock.second;
        }
        block.push_back(subblock);
    }

// TODO: Efficiency Optimization: Replace the full gamma matrix with its occupied and virtual blocks.
// This means we need overall smaller DGEMV, smaller primary transforms below, and can reuse the bQpq
// blocks when constructing the 

    const auto bQpqA_mo_scf = three_idx_primary_transform(bQmn_so_scf_, *Ca_subset("SO", "ALL"), *Ca_subset("SO", "ALL"));
    const auto bQpqB_mo_scf = three_idx_primary_transform(bQmn_so_scf_, *Cb_subset("SO", "ALL"), *Cb_subset("SO", "ALL"));

/*
 *  f_tilde <Q|P> = gbar<QS|PR> gamma<R|S> + gbar<Qs|Pr> gamma<r|s>
 *             = g(QP|SR) gamma<R|S> - g(QR|SP) gamma<R|S> + g(QP|sr) gamma<r|s>
 *
 *  f_tilde <q|p> = gbar<qs|pr> gamma<r|s> + gbar<qS|pR> gamma<R|S>
 *             = g(qp|sr) gamma<r|s> - g(qr|sp) gamma<r|s> + g(qp|SR) gamma<R|S>
 */

    // (Q) = b(Q|SR)*gamma<R|S> + b(Q|sr)*gamma<r|s>
    auto Q = Matrix("b(Q|SR)gamma<R|S>", 1, nQ_scf_);
    auto Qp = Q.pointer();
    const auto bQpqAp0 = bQpqA_mo_scf.pointer(0);
    const auto bQpqBp0 = bQpqB_mo_scf.pointer(0);
    for (int hR = 0; hR < nirrep_; ++hR) {
        int hS = hR;
        if (nsopi_[hR] > 0) {
            auto gamma_rsAp = mo_gammaA_.pointer(hR);
            auto gamma_rsBp = mo_gammaB_.pointer(hR);
            // (Q) = b(Q|SR) gamma<R|S>
            C_DGEMV('N', nQ_scf_, nsopi_[hR] * nsopi_[hS], 1.0, bQpqAp0[0] + block[0][hR].first,
                    bQpqA_mo_scf.coldim(0), gamma_rsAp[0], 1, 1.0, Qp[0], 1);
            // (Q) += b(Q|sr) gamma<r|s>
            C_DGEMV('N', nQ_scf_, nsopi_[hR] * nsopi_[hS], 1.0, bQpqBp0[0] + block[0][hR].first,
                    bQpqB_mo_scf.coldim(0), gamma_rsBp[0], 1, 1.0, Qp[0], 1);
        }
    }
    // This Q intermediate can be reused when computing gradients! Save it.
    Q.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);

#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
    for (int hQ = 0; hQ < nirrep_; ++hQ) {
        int hP = hQ;
        if (nsopi_[hQ] > 0) {
            double** tFAp = mo_gbarGamma_A_.pointer(hQ);
            double** tFBp = mo_gbarGamma_B_.pointer(hQ);

            double** bQpqAp = bQpqA_mo_scf.pointer(0);
            double** bQpqBp = bQpqB_mo_scf.pointer(0);


            // f_tilde <Q|P> = b(QP|Aux)*(Aux) where (Aux) = (Q)
            C_DGEMV('T', nQ_scf_, nsopi_[hP] * nsopi_[hQ], 1.0, bQpqAp[0] + block[0][hP].first, bQpqA_mo_scf.coldim(0), Qp[0],
                    1, 0.0, tFAp[0], 1);

            // f_tilde <q|p> = b(qp|Aux)*(Aux) where (Aux) = (Q)
            C_DGEMV('T', nQ_scf_, nsopi_[hP] * nsopi_[hQ], 1.0, bQpqBp[0] + block[0][hP].first, bQpqB_mo_scf.coldim(0), Qp[0],
                    1, 0.0, tFBp[0], 1);
        }
    }

    // f_tilde <Q|P> -= b(QR|Aux) b(Aux|SP) gamma<R|S>
    for (int hQ = 0; hQ < nirrep_; ++hQ) {
        int hP = hQ;
        if (nsopi_[hQ] > 0) {
            for (int hR = 0; hR < nirrep_; ++hR) {
                int hS = hR;
                if (nsopi_[hR] > 0) {
                    double** bQpqAp = bQpqA_mo_scf.pointer(hQ ^ hR);
                    double** gamma_rsA_p = mo_gammaA_.pointer(hR);

                    std::vector<SharedMatrix> RS;
                    for (int i = 0; i < nthreads; ++i) {
                        RS.push_back(std::make_shared<Matrix>("<Q'P'|RS>", nsopi_[hR], nsopi_[hS]));
                    }

#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                    for (int Q = 0; Q < nsopi_[hQ]; ++Q) {
                        for (int P = Q; P < nsopi_[hP]; ++P) {
                            int thread = 0;
#ifdef _OPENMP
                            thread = omp_get_thread_num();
#endif
                            double** RSp = RS[thread]->pointer();

                            // <Q'P'|RS> = b(Q'R|Aux) b(Aux|P'S)
                            C_DGEMM('T', 'N', nsopi_[hR], nsopi_[hS], nQ_scf_, 1.0,
                                    bQpqAp[0] + block[hQ ^ hR][hQ].first + Q * nsopi_[hR], bQpqA_mo_scf.coldim(hQ ^ hR),
                                    bQpqAp[0] + block[hP ^ hS][hP].first + P * nsopi_[hS], bQpqA_mo_scf.coldim(hP ^ hS),
                                    0.0, RSp[0], nsopi_[hS]);
                            // - <Q'P'|RS> * gamma<R|S>
                            double value = -C_DDOT(nsopi_[hR] * nsopi_[hS], RSp[0], 1, gamma_rsA_p[0], 1);
                            mo_gbarGamma_A_.add(hP, Q, P, value);
                            if (Q != P) {
                                mo_gbarGamma_A_.add(hP, P, Q, value);
                            }
                        }
                    }
                }
            }
        }
    }

    // f_tilde <q|p> -= b(qr|Aux) b(Aux|sp) gamma<r|s>
    for (int hq = 0; hq < nirrep_; ++hq) {
        int hp = hq;
        if (nsopi_[hq] > 0) {
            for (int hr = 0; hr < nirrep_; ++hr) {
                int hs = hr;
                if (nsopi_[hr] > 0) {
                    double** bQpqBp = bQpqB_mo_scf.pointer(hq ^ hr);
                    double** gamma_rsB_p = mo_gammaB_.pointer(hr);

                    std::vector<SharedMatrix> rs;
                    for (int i = 0; i < nthreads; ++i) {
                        rs.push_back(std::make_shared<Matrix>("<q'p'|rs>", nsopi_[hr], nsopi_[hs]));
                    }

#pragma omp parallel for schedule(dynamic) num_threads(nthreads)
                    for (int q = 0; q < nsopi_[hq]; ++q) {
                        for (int p = q; p < nsopi_[hp]; ++p) {
                            int thread = 0;
#ifdef _OPENMP
                            thread = omp_get_thread_num();
#endif
                            double** rsp = rs[thread]->pointer();

                            // <q'p'|rs> = b(q'r|Aux) b(Aux|p's)
                            C_DGEMM('T', 'N', nsopi_[hr], nsopi_[hs], nQ_scf_, 1.0,
                                    bQpqBp[0] + block[hq ^ hr][hq].first + q * nsopi_[hr], bQpqB_mo_scf.coldim(hq ^ hr),
                                    bQpqBp[0] + block[hp ^ hs][hp].first + p * nsopi_[hs], bQpqB_mo_scf.coldim(hp ^ hs),
                                    0.0, rsp[0], nsopi_[hs]);
                            // - <q'p'|rs> * gamma<r|s>
                            double value = -C_DDOT(nsopi_[hr] * nsopi_[hs], rsp[0], 1, gamma_rsB_p[0], 1);
                            mo_gbarGamma_B_.add(hp, q, p, value);
                            if (q != p) {
                                mo_gbarGamma_B_.add(hp, p, q, value);
                            }
                        }
                    }
                }
            }
        }
    }

    dct_timer_off("DCTSolver::Gbar<QS|PR> Gamma<R|S> (FastBuilder)");
}

// Compute gpq = (Q|rs) L^pr_qs where (Q|rs) is a B tensor, and L is a cumulant element.
// In DCT, the 2RDM is always written as 1RDM^p_r 1RDM^q_s - 1RDM^p_s 1RDM^q_r + L^pq_rs.
// When density-fit, the first two terms contract against JKFIT integrals. The last contracts against RIFIT
// integrals. We are concerned about the RIFIT three index density in this function.
void DCTSolver::three_idx_cumulant_density() {
    dpdbuf4 G;

    psio_->open(PSIF_DCT_DENSITY, PSIO_OPEN_OLD);

    /// OOOO Spin-Blocks
    // 1. From IJKL
    // TODO: If we can fit all needed intermediates in-core, we generate Gamma (OO|OO) in-core, write it to disk, then
    // read it from disk to get it back in core. That's just wasteful.
    // TODO: There's probably a bad scaling factor, from how the I intermediate is defined. Check it.
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[O,O]"), ID("[O,O]"), ID("[O>O]-"), ID("[O>O]-"), 0, "I <OO|OO>");
    global_dpd_->buf4_sort(&G, PSIF_DCT_DPD, prqs, ID("[O,O]"), ID("[O,O]"), "Lambda (OO|OO)");
    global_dpd_->buf4_close(&G);

    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[O,O]"), ID("[O,O]"), ID("[O,O]"), ID("[O,O]"), 0, "Lambda (OO|OO)");
    // The memory will be automatically freed when we re-assign result.
    auto result = Matrix("3-Center PDM B: IJ", bQijA_mo_.rowspi(), bQijA_mo_.colspi());
    // gIJ = b(Q|KL) L^IK_JL
    contract343(bQijA_mo_, G, result, false, 1.0, 0.0);
    global_dpd_->buf4_close(&G);

    // 2. From IjKl
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[O,o]"), ID("[O,o]"), ID("[O,o]"), ID("[O,o]"), 0, "I <Oo|Oo>");
    global_dpd_->buf4_sort(&G, PSIF_DCT_DPD, qspr, ID("[o,o]"), ID("[O,O]"), "Lambda (oo|OO)");
    global_dpd_->buf4_close(&G);

    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[o,o]"), ID("[O,O]"), ID("[o,o]"), ID("[O,O]"), 0, "Lambda (oo|OO)");
    // gIJ += b(Q|ij) L^iI_jJ
    contract343(bQijB_mo_, G, result, false, 1.0, 1.0);
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    result = Matrix("3-Center PDM B: ij", bQijB_mo_.rowspi(), bQijB_mo_.colspi());
    // gij = b(Q|IJ) L^Ii_Jj
    contract343(bQijA_mo_, G, result, true, 1.0, 0.0);
    global_dpd_->buf4_close(&G);

    // 3. From ijkl
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[o,o]"), ID("[o,o]"), ID("[o>o]-"), ID("[o>o]-"), 0, "I <oo|oo>");
    global_dpd_->buf4_sort(&G, PSIF_DCT_DPD, prqs, ID("[o,o]"), ID("[o,o]"), "Lambda (oo|oo)");
    global_dpd_->buf4_close(&G);

    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[o,o]"), ID("[o,o]"), ID("[o,o]"), ID("[o,o]"), 0, "Lambda (oo|oo)");
    // gij += b(Q|kl) L^ki_lj
    contract343(bQijB_mo_, G, result, false, 1.0, 1.0);
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    global_dpd_->buf4_close(&G);

    //// OVOV Spin-Blocks
    // 4. From IAJB
    // -L^IA_JB = K (IJ|AB)
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[O,O]"), ID("[V,V]"), ID("[O,O]"), ID("[V,V]"), 0, "K (OO|VV)");
    result = Matrix("3-Center PDM B: AB", bQabA_mo_.rowspi(), bQabA_mo_.colspi());
    // gAB = b(Q|IJ) L^IA_JB
    contract343(bQijA_mo_, G, result, false, -1.0, 0.0);
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    result = Matrix("3-Center PDM B: IJ", bQijA_mo_.rowspi(), bQijA_mo_.colspi());
    result.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks); 
    // gIJ += b(Q|AB) L^AI_BJ
    contract343(bQabA_mo_, G, result, true, -1.0, 1.0);
    global_dpd_->buf4_close(&G);
    // K(IA|JB) = -L^IB_JA = L^IB_AJ
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[O,V]"), ID("[O,V]"), ID("[O,V]"), ID("[O,V]"), 0, "K (OV|OV)");
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    result = Matrix("3-Center PDM B: IA", bQiaA_mo_.rowspi(), bQiaA_mo_.colspi());
    // gIA = b(Q|BJ) L^BI_JA = b(Q|BJ) K(IA|JB) = b(Q|JB) K(IA|JB)
    contract343(bQiaA_mo_, G, result, true, 1.0, 0.0);
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    global_dpd_->buf4_close(&G);

    // 5. From iajb
    // -L^ia_jb = K (ij|ab)
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[o,o]"), ID("[v,v]"), ID("[o,o]"), ID("[v,v]"), 0, "K (oo|vv)");
    result = Matrix("3-Center PDM B: ab", bQabB_mo_.rowspi(), bQabB_mo_.colspi());
    // gab = b(Q|ij) L^ia_jb
    contract343(bQijB_mo_, G, result, false, -1.0, 0.0);
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    result = Matrix("3-Center PDM B: ij", bQijB_mo_.rowspi(), bQijB_mo_.colspi());
    result.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks); 
    // gij += b(Q|ab) L^ia_jb
    contract343(bQabB_mo_, G, result, true, -1.0, 1.0);
    global_dpd_->buf4_close(&G);
    // K(ia|jb) = -L^ib_ja = L^ib_aj
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[o,v]"), ID("[o,v]"), ID("[o,v]"), ID("[o,v]"), 0, "K (ov|ov)");
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    result = Matrix("3-Center PDM B: ia", bQiaB_mo_.rowspi(), bQiaB_mo_.colspi());
    // gia = b(Q|bj) L^ib_aj = b(Q|bj) K(ia|jb) = b(Q|jb) K(ia|jb)
    contract343(bQiaB_mo_, G, result, true, 1.0, 0.0);
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    global_dpd_->buf4_close(&G);

    // 6. From IabJ
    // -LIa_Jb = K <Ja|Ib> = K (JI|ab)
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[O,O]"), ID("[v,v]"), ID("[O,O]"), ID("[v,v]"), 0, "K (OO|vv)");
    result = Matrix("3-Center PDM B: ab", bQabB_mo_.rowspi(), bQabB_mo_.colspi());
    result.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    // gab = b(Q|IJ) L^Ia_Jb = - b(Q|JI) K(JI|ab)
    contract343(bQijA_mo_, G, result, false, -1.0, 1.0);
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    result = Matrix("3-Center PDM B: IJ", bQijA_mo_.rowspi(), bQijA_mo_.colspi());
    result.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    // gIJ = b(Q|ab) L^aI_bJ
    contract343(bQabB_mo_, G, result, true, -1.0, 1.0);
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    global_dpd_->buf4_close(&G);

    // 7. From iABj
    // -L^iA_jB = K <jA|iB> = K (ji|AB)
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[o,o]"), ID("[V,V]"), ID("[o,o]"), ID("[V,V]"), 0, "K (oo|VV)");
    result = Matrix("3-Center PDM B: AB", bQabA_mo_.rowspi(), bQabA_mo_.colspi());
    result.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    // gAB = b(Q|ij) L^iA_jB = - b(Q|ji) K(ji|AB)
    contract343(bQijB_mo_, G, result, false, -1.0, 1.0);
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    result = Matrix("3-Center PDM B: ij", bQijB_mo_.rowspi(), bQijB_mo_.colspi());
    result.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    // gij = b(Q|AB) L^Ai_Bj
    contract343(bQabA_mo_, G, result, true, -1.0, 1.0);
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    global_dpd_->buf4_close(&G);

    // 8. From IaBj (Hermiticity-equivalent to iAbJ case)
    // L^Ia_Ai = -L^Ia_iA = K (IA|ia)
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[O,V]"), ID("[o,v]"), ID("[O,V]"), ID("[o,v]"), 0, "K (OV|ov");
    result = Matrix("3-Center PDM B: ia", bQiaB_mo_.rowspi(), bQiaB_mo_.colspi());
    result.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    // gia += b(Q|AI) L^Ia_Ai = b(Q|AI) K(IA|ia) = b(Q|IA) K(IA|ia)
    contract343(bQiaA_mo_, G, result, false, 1.0, 1.0);
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    result = Matrix("3-Center PDM B: IA", bQiaA_mo_.rowspi(), bQiaA_mo_.colspi());
    result.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    // gIA += b(Q|ai) L^aI_iA = b(Q|ai) K(IA|ia) = b(Q|ia) K(IA|ia)
    contract343(bQiaB_mo_, G, result, true, 1.0, 1.0);
    global_dpd_->buf4_close(&G);

    // OOVV Spin-Blocks
    // 9. From IJAB
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[O,V]"), ID("[O,V]"), ID("[O,V]"), ID("[O,V]"), 0, "Lambda (OV|OV)");
    // gIA += b(Q|jb) L^IJ_AB
    contract343(bQiaA_mo_, G, result, false, 1.0, 1.0);
    global_dpd_->buf4_close(&G);

    // 10. From IjAb
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[O,V]"), ID("[o,v]"), ID("[O,V]"), ID("[o,v]"), 0, "Lambda (OV|ov)");
    // gIA += b(Q|jb) L^Ij_Ab
    contract343(bQiaB_mo_, G, result, true, 1, 1);
    result.save(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    result = Matrix("3-Center PDM B: ia", bQiaB_mo_.rowspi(), bQiaB_mo_.colspi());
    result.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    // gia += b(Q|IA) L^Ij_Ab
    contract343(bQiaA_mo_, G, result, false, 1.0, 1.0);
    global_dpd_->buf4_close(&G);
    
    // 11. From ijab
    global_dpd_->buf4_init(&G, PSIF_DCT_DPD, 0, ID("[o,v]"), ID("[o,v]"), ID("[o,v]"), ID("[o,v]"), 0, "Lambda (ov|ov)");
    // gia += b(Q|jb) L^ij_ab
    contract343(bQiaB_mo_, G, result, false, 1.0, 1.0);
    global_dpd_->buf4_close(&G);

    // VVVV Spin-Blocks
    // AS A FIRST CODE, assume nvir^4 fits in memory. This is a dangerous assumption, so we'll add an nvir^3
    // but slower algorithm later.
    // We can't assume that nvir^4 fits in memory. Our algorithm instead is to compute L^AB_CD for fixed A,
    // which is only nvir^3 memory, then construct its contribution to gAB and gab..
    // This is a potentially large intermediate.
    // 12. From ABCD
    // TODO: Add this.

    auto J = Matrix("J^-1/2 Correlation", nQ_, nQ_);
    J.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::LowerTriangle);

    auto CaO = *Ca_subset("SO", "OCC");
    auto CbO = *Cb_subset("SO", "OCC");
    auto CaV = *Ca_subset("SO", "VIR");
    auto CbV = *Cb_subset("SO", "VIR");

    auto temp = Matrix("3-Center PDM B: IJ", bQijA_mo_.rowspi(), bQijA_mo_.colspi());
    auto SO_matrix = three_idx_cumulant_helper(temp, J, CaO, CaO);

    temp = Matrix("3-Center PDM B: ij", bQijB_mo_.rowspi(), bQijB_mo_.colspi());
    SO_matrix.add(three_idx_cumulant_helper(temp, J, CbO, CbO));

    temp = Matrix("3-Center PDM B: AB", bQabA_mo_.rowspi(), bQabA_mo_.colspi());
    SO_matrix.add(three_idx_cumulant_helper(temp, J, CaV, CaV));

    temp = Matrix("3-Center PDM B: ab", bQabB_mo_.rowspi(), bQabB_mo_.colspi());
    SO_matrix.add(three_idx_cumulant_helper(temp, J, CbV, CbV));
    
    temp = Matrix("3-Center PDM B: IA", bQiaA_mo_.rowspi(), bQiaA_mo_.colspi());
    SO_matrix.add(three_idx_cumulant_helper(temp, J, CaO, CaV));

    temp = Matrix("3-Center PDM B: ia", bQiaB_mo_.rowspi(), bQiaB_mo_.colspi());
    SO_matrix.add(three_idx_cumulant_helper(temp, J, CbO, CbV));

    // Now transform from SO back to AO
    auto AO_matrix = transform_b_so2ao(SO_matrix);
    AO_matrix.set_name("3-Center Correlation Density");
    AO_matrix.save(psio_, PSIF_AO_TPDM, Matrix::SaveType::Full);

    psio_->close(PSIF_DCT_DENSITY, 1);
}

Matrix DCTSolver::three_idx_cumulant_helper(Matrix& temp, const Matrix& J, const Matrix& bt1, const Matrix& bt2) {
    temp.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    // 10.1063/1.4896235:55 - MO basis
    auto int55 = contract233(J, temp);
    return three_idx_primary_transform(int55, *bt1.transpose(), *bt2.transpose());
}

void DCTSolver::contract343(const Matrix& b, dpdbuf4 &G, Matrix& result, bool transpose, double alpha, double beta) const {
    if (b.rowspi() != result.rowspi()) {
        throw PSIEXCEPTION("contract343: Left operand and result disagree about number of rows.");
    }
    char trans; 
    int* N, *K;
    if (transpose) {
        trans = 'T';
        N = G.params->rowtot;
        K = G.params->coltot;
    } else {
        trans = 'N';
        N = G.params->coltot;
        K = G.params->rowtot;
    }
    for (int h = 0; h < nirrep_; ++h) {
        if (b.colspi(h) > 0 && result.colspi(h) > 0) {
            global_dpd_->buf4_mat_irrep_init(&G, h);
            global_dpd_->buf4_mat_irrep_rd(&G, h);
            auto bp = b.pointer(h);
            auto rp = result.pointer(h);
            C_DGEMM('N', trans, b.rowspi(h), N[h], K[h], alpha, bp[0], b.colspi(h), G.matrix[h][0], G.params->coltot[h], beta, rp[0], result.colspi(h));
        }
    }
} 

// See documentation for three_idx_cumulant_density. We now care about those last two terms.
void DCTSolver::three_idx_separable_density() {

    // Load useful intermediates.
    auto Q = Matrix("b(Q|SR)gamma<R|S>", 1, nQ_scf_);
    Q.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);

    auto J = Matrix("J^-1/2 Reference", nQ_scf_, nQ_scf_);
    J.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::LowerTriangle);

    auto SO_matrix = three_idx_separable_helper(Q, J, mo_gammaA_, *Ca_);
    SO_matrix.add(three_idx_separable_helper(Q, J, mo_gammaB_, *Cb_));

    // Now transform from SO back to AO
    auto AO_matrix = transform_b_so2ao(SO_matrix);
    AO_matrix.set_name("3-Center Reference Density");
    AO_matrix.save(psio_, PSIF_AO_TPDM, Matrix::SaveType::Full);

    // We have Hermitian symmetry in the last two indices. To reduce IO costs,
    // only store the lower triangle of each pair.
/*
    auto p = AO_matrix.pointer(0);
    int count = 0
    int ntri = nso_ * (nso_ + 1) / 2
    std::vector<double> temp(nQ_scf_ * ntri, 0);
    for (auto P = 0; P < nQ_scf_; P++) {
        for (auto i = 0; i < nso_; i++) {
            for (auto j = 0; j <= o; j++; count++) {
                temp[count] = p[P][i * nso_ + j];
            }
        }
    }

    psio->open(PSIF_AO_TPDM, PSIO_OPEN_OLD);
    psio->write_entry(PSIF_AO_TPDM, "3-Center Reference Density", (char *) temp.get, sizeof(double) * temp.size());
    psio->close(PSIF_AO_TPDM, 1); // Close and keep
    */
}

Matrix DCTSolver::three_idx_separable_helper(const Matrix& Q, const Matrix& J, const Matrix& RDM, const Matrix& C_subset) {
    // Coulomb-like term of 10.1063/1.4896235:54 b(Q|pq) gamma^p_q gamma^r_s
    auto temp = contract123(Q, RDM);
    // Exchange-like term of 10.1063/1.4896235:54 b(Q|pq) gamma^p_s gamma^r_q
    // This doublet compensates for not having MO basis B integrals in the three_idx transform below
    auto gamma = linalg::doublet(C_subset, RDM, false, false);
    three_idx_primary_transform_gemm(bQmn_so_scf_, gamma, gamma, temp, -1.0, 1.0);
    // 10.1063/1.4896235:55 - MO basis
    auto int55 = contract233(J, temp);
    auto backtransformer = *C_subset.transpose();
    // Backtransform eq. 55 to SO basiss
    return three_idx_primary_transform(int55, backtransformer, backtransformer);
}

void DCTSolver::construct_metric_density(const std::string& basis_type) {
    auto nQ = (basis_type == "Correlation") ? nQ_ : nQ_scf_;
    auto b = Matrix("B(Q|mn) " + basis_type, nQ, nso_ * nso_);
    b.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::SubBlocks);
    auto J = Matrix("J^-1/2 " + basis_type, nQ, nQ);
    J.load(psio_, PSIF_DCT_DENSITY, Matrix::SaveType::LowerTriangle);
    auto c = linalg::doublet(J, b, true, false);
    // TODO: How do I clear J and b? Those are large objects.
    auto g = Matrix("3-Center " + basis_type + " Density", nQ, nso_ * nso_);
    g.load(psio_, PSIF_AO_TPDM, Matrix::SaveType::Full);
    auto G = linalg::doublet(c, g, false, true);
    G.set_name("Metric " + basis_type + " Density");
    G.save(psio_, PSIF_AO_TPDM, Matrix::SaveType::LowerTriangle);
}

// r(Q|pq) = \sum_Q J(PQ) B(P|pq)
Matrix DCTSolver::contract233(const Matrix& J, const Matrix& B) const {
    if (J.nirrep() != 1) {
        throw PSIEXCEPTION("contract233: Expected first argument to have no symmetry.");
    }
    auto result = Matrix(B.rowspi(), B.colspi());
    auto Jp = J.pointer()[0];
    auto Jcols = J.colspi(0);
    // Sadly, we can't just make this a doublet due to symmetry...
    for (int h = 0; h < nirrep_; ++h) {
        if (B.colspi(h) > 0) {
            C_DGEMM('T', 'N', result.rowspi(h), result.colspi(h), B.rowspi(h), 1.0, Jp, Jcols, B.pointer(h)[0], B.colspi(h), 0.0, result.pointer(h)[0], result.colspi(h)); 
        }
    }

    return result;
}

// (Q) (p|q) -> (Q|pq)
Matrix DCTSolver::contract123(const Matrix& Q, const Matrix& G) const {
    if (Q.nirrep() != 1) {throw PSIEXCEPTION("contract123: Left argument must have exactly one irrep.");}
    if (G.symmetry()) {throw PSIEXCEPTION("contract123: Right argument must have trivial pont group symmetry");}

    // Sizing for the result
    auto nirrep = G.nirrep();
    auto Aux = Q.colspi();
    Dimension LR(nirrep);
    for (int h = 0; h < nirrep; ++h) {
        for (int hp = 0; hp < nirrep; ++ hp) {
            LR[h] += G.rowspi(hp) * G.colspi(h ^ hp);
        }
    }
    auto result = Matrix("Result", Aux, LR);

    int offset = 0;
    for (int h = 0; h < nirrep; ++h) {
        if (G.colspi(h) > 0) {
            C_DGER(Q.ncol(), G.rowspi(h) * G.colspi(h), 1.0, Q.pointer()[0], 1, G.pointer(h)[0], 1, result.pointer(0)[0] + offset, result.colspi(0)); 
        }
        offset += G.rowspi(h) * G.colspi(h);
    }

    return result;
}

}  // namespace dct
}  // namespace psi
