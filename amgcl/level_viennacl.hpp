#ifndef AMGCL_LEVEL_VIENNACL_HPP
#define AMGCL_LEVEL_VIENNACL_HPP

/*
The MIT License

Copyright (c) 2012 Denis Demidov <ddemidov@ksu.ru>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

/**
 * \file   level_vexcl.hpp
 * \author Denis Demidov <ddemidov@ksu.ru>
 * \brief  Level of an AMG hierarchy for use with VexCL vectors.
 */

#include <array>

#include <amgcl/level_params.hpp>
#include <amgcl/spmat.hpp>
#include <amgcl/operations_viennacl.hpp>

#include <viennacl/vector.hpp>
#include <viennacl/compressed_matrix.hpp>
#include <viennacl/ell_matrix.hpp>
#include <viennacl/hyb_matrix.hpp>
#include <viennacl/linalg/inner_prod.hpp>
#include <viennacl/linalg/prod.hpp>

namespace amgcl {
namespace level {

/// Possible matrix storage formats.
enum cl_matrix_format {
    CL_MATRIX_CRS, ///< Compressed matrix. Fastest construction, slowest operation (on a GPU). Best suited for CPU compute devices.
    CL_MATRIX_ELL, ///< ELL format. Ideal for matrices with constant number of nonzeros per row on GPU compute devices.
    CL_MATRIX_HYB  ///< Hybrid ELL format. Best choice for general matrices on GPU compute devices.
};

template <cl_matrix_format Format, typename value_type>
struct matrix_format;

template <typename value_type>
struct matrix_format<CL_MATRIX_CRS, value_type> {
    typedef viennacl::compressed_matrix<value_type> type;
};

template <typename value_type>
struct matrix_format<CL_MATRIX_ELL, value_type> {
    typedef viennacl::ell_matrix<value_type> type;
};

template <typename value_type>
struct matrix_format<CL_MATRIX_HYB, value_type> {
    typedef viennacl::hyb_matrix<value_type> type;
};

/// VexCL-based AMG hierarchy.
/**
 * Level of an AMG hierarchy for use with ViennaCL vectors.
 *
 * \param Format Matrix storage format.
 */
template <cl_matrix_format Format = CL_MATRIX_HYB>
struct ViennaCL {

/// Parameters for CPU-based level storage scheme.
struct params
    : public amgcl::level::params
{ };

template <typename value_t, typename index_t = long long>
class instance {
    public:
        typedef sparse::matrix<value_t, index_t>              cpu_matrix;
        typedef typename matrix_format<Format, value_t>::type matrix;
        typedef viennacl::vector<value_t>                     vector;

        // Construct complete multigrid level from system matrix (a),
        // prolongation (p) and restriction (r) operators.
        // The matrices are moved into the local members.
        instance(cpu_matrix &&a, cpu_matrix &&p, cpu_matrix &&r, const params &prm, unsigned nlevel)
            : d(a.rows), t(a.rows)
        {
            viennacl::copy(sparse::viennacl_map(a), A);
            viennacl::copy(sparse::viennacl_map(p), P);
            viennacl::copy(sparse::viennacl_map(r), R);

            viennacl::fast_copy(diagonal(a), d);

            if (nlevel) {
                u.resize(a.rows);
                f.resize(a.rows);

                if (prm.kcycle && nlevel % prm.kcycle == 0)
                    for(auto v = cg.begin(); v != cg.end(); v++)
                        v->resize(a.rows);
            }

            a.clear();
            p.clear();
            r.clear();
        }

        // Construct the coarsest hierarchy level from system matrix (a) and
        // its inverse (ai).
        instance(cpu_matrix &&a, cpu_matrix &&ai, const params &prm, unsigned nlevel)
            : d(a.rows), u(a.rows), f(a.rows), t(a.rows)
        {
            viennacl::copy(sparse::viennacl_map(a),  A);
            viennacl::copy(sparse::viennacl_map(ai), Ainv);

            viennacl::fast_copy(diagonal(a), d);

            a.clear();
            ai.clear();
        }

        // Perform one relaxation (smoothing) step.
        void relax(const vector &rhs, vector &x) const {
            const index_t n = x.size();

            t = viennacl::linalg::prod(A, x);
            t = rhs - t;
            t = viennacl::linalg::element_div(t, d);
            x += 0.72 * t;
        }

        // Compute residual value.
        value_t resid(const vector &rhs, vector &x) const {
            t = viennacl::linalg::prod(A, x);
            t = rhs - t;

            return sqrt(viennacl::linalg::inner_prod(t, t));
        }

        // Perform one V-cycle. Coarser levels are cycled recursively. The
        // coarsest level is solved directly.
        template <class Iterator>
        static void cycle(Iterator lvl, Iterator end, const params &prm,
                const vector &rhs, vector &x)
        {
            Iterator nxt = lvl; ++nxt;

            if (nxt != end) {
                for(unsigned j = 0; j < prm.ncycle; ++j) {
                    for(unsigned i = 0; i < prm.npre; ++i) lvl->relax(rhs, x);

                    lvl->t = viennacl::linalg::prod(lvl->A, x);
                    lvl->t = rhs - lvl->t;
                    nxt->f = viennacl::linalg::prod(lvl->R, lvl->t);
                    nxt->u.clear();

                    if (nxt->cg[0].size())
                        kcycle(nxt, end, prm, nxt->f, nxt->u);
                    else
                        cycle(nxt, end, prm, nxt->f, nxt->u);

                    lvl->t = viennacl::linalg::prod(lvl->P, nxt->u);
                    x += lvl->t;

                    for(unsigned i = 0; i < prm.npost; ++i) lvl->relax(rhs, x);
                }
            } else {
                x = viennacl::linalg::prod(lvl->Ainv, rhs);
            }
        }

        template <class Iterator>
        static void kcycle(Iterator lvl, Iterator end, const params &prm,
                const vector &rhs, vector &x)
        {
            Iterator nxt = lvl; ++nxt;

            if (nxt != end) {
                auto &r = lvl->cg[0];
                auto &s = lvl->cg[1];
                auto &p = lvl->cg[2];
                auto &q = lvl->cg[3];

                r = rhs;

                value_t rho1 = 0, rho2 = 0;

                for(int iter = 0; iter < 2; ++iter) {
                    s.clear();
                    cycle(lvl, end, prm, r, s);

                    rho2 = rho1;
                    rho1 = viennacl::linalg::inner_prod(r, s);

                    if (iter)
                        p = s + (rho1 / rho2) * p;
                    else
                        p = s;

                    q = viennacl::linalg::prod(lvl->A, p);

                    value_t alpha = rho1 / viennacl::linalg::inner_prod(q, p);

                    x += alpha * p;
                    r -= alpha * q;
                }
            } else {
                x = viennacl::linalg::prod(lvl->Ainv, rhs);
            }
        }
    private:
        matrix A;
        matrix P;
        matrix R;
        matrix Ainv;

        vector d;

        mutable vector u;
        mutable vector f;
        mutable vector t;

        mutable std::array<vector, 4> cg;
};

};

} // namespace level
} // namespace amgcl

#endif
