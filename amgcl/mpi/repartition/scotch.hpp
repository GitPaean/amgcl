#ifndef AMGCL_MPI_REPARTITION_SCOTCH_HPP
#define AMGCL_MPI_REPARTITION_SCOTCH_HPP

/*
The MIT License

Copyright (c) 2012-2018 Denis Demidov <dennis.demidov@gmail.com>

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
 * \file   amgcl/mpi/repartition/scotch.hpp
 * \author Denis Demidov <dennis.demidov@gmail.com>
 * \brief  SCOTCH repartitioner.
 */

#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/foreach.hpp>

#include <amgcl/backend/interface.hpp>
#include <amgcl/value_type/interface.hpp>
#include <amgcl/mpi/util.hpp>
#include <amgcl/mpi/distributed_matrix.hpp>
#include <amgcl/mpi/repartition/util.hpp>

#include <ptscotch.h>

namespace amgcl {
namespace mpi {
namespace repartition {

template <class Backend>
struct scotch {
    typedef typename Backend::value_type value_type;
    typedef distributed_matrix<Backend>  matrix;

    struct params {
        bool      enable;
        ptrdiff_t min_per_proc;
        int       shrink_ratio;

        params() :
            enable(false), min_per_proc(10000), shrink_ratio(8)
        {}

        params(const boost::property_tree::ptree &p)
            : AMGCL_PARAMS_IMPORT_VALUE(p, enable),
              AMGCL_PARAMS_IMPORT_VALUE(p, min_per_proc),
              AMGCL_PARAMS_IMPORT_VALUE(p, shrink_ratio)
        {
            AMGCL_PARAMS_CHECK(p, (enable)(min_per_proc)(shrink_ratio));
        }

        void get(
                boost::property_tree::ptree &p,
                const std::string &path = ""
                ) const
        {
            AMGCL_PARAMS_EXPORT_VALUE(p, path, enable);
            AMGCL_PARAMS_EXPORT_VALUE(p, path, min_per_proc);
            AMGCL_PARAMS_EXPORT_VALUE(p, path, shrink_ratio);
        }
    } prm;

    scotch(const params &prm = params()) : prm(prm) {}

    bool is_needed(const matrix &A) const {
        if (!prm.enable) return false;

        communicator comm = A.comm();
        ptrdiff_t n = A.loc_rows();
        std::vector<ptrdiff_t> row_dom = mpi::exclusive_sum(comm, n);

        int non_empty = 0;
        ptrdiff_t min_n = std::numeric_limits<ptrdiff_t>::max();
        for(int i = 0; i < comm.size; ++i) {
            ptrdiff_t m = row_dom[i+1] - row_dom[i];
            if (m) {
                min_n = std::min(min_n, m);
                ++non_empty;
            }
        }

        return (non_empty > 1) && (min_n <= prm.min_per_proc);
    }

    boost::shared_ptr<matrix> operator()(const matrix &A) const {
        communicator comm = A.comm();
        ptrdiff_t n = A.loc_rows();
        ptrdiff_t row_beg = A.loc_col_shift();

        // Partition the graph.
        int active = (n > 0);
        int active_ranks;
        MPI_Allreduce(&active, &active_ranks, 1, MPI_INT, MPI_SUM, comm);

        SCOTCH_Num npart = std::max(1, active_ranks / prm.shrink_ratio);

        if (comm.rank == 0)
            std::cout << "Repartitioning[SCOTCH] " << active_ranks << " -> " << npart << std::endl;

        std::vector<ptrdiff_t> perm(n);
        ptrdiff_t col_beg, col_end;

        if (npart == 1) {
            col_beg = (comm.rank == 0) ? 0 : A.glob_rows();
            col_end = A.glob_rows();

            for(ptrdiff_t i = 0; i < n; ++i) {
                perm[i] = row_beg + i;
            }
        } else {
            std::vector<SCOTCH_Num> ptr;
            std::vector<SCOTCH_Num> col;
            std::vector<SCOTCH_Num> part(std::max<ptrdiff_t>(1, n));

            symm_graph(A, ptr, col);

            SCOTCH_Dgraph G;
            check(comm, SCOTCH_dgraphInit(&G, comm));
            check(comm, SCOTCH_dgraphBuild(&G,
                        0,          // baseval
                        n,          // vertlocnbr
                        n,          // vertlocmax
                        &ptr[0],    // vertloctab
                        NULL,       // vendloctab
                        NULL,       // veloloctab
                        NULL,       // vlblloctab
                        ptr.back(), // edgelocnbr
                        ptr.back(), // edgelocsiz
                        &col[0],    // edgeloctab
                        NULL,       // edgegsttab
                        NULL        // edloloctab
                        ));
            check(comm, SCOTCH_dgraphCheck(&G));

            SCOTCH_Strat S;
            check(comm, SCOTCH_stratInit(&S));

            check(comm, SCOTCH_dgraphPart(&G, npart, &S, &part[0]));

            SCOTCH_stratExit(&S);
            SCOTCH_dgraphExit(&G);

            boost::tie(col_beg, col_end) = graph_perm_index(comm, npart, part, perm);
        }

        return graph_perm_matrix<Backend>(comm, col_beg, col_end, perm);
    }

    static void check(communicator comm, int ierr) {
        amgcl::mpi::precondition(comm, ierr == 0, "SCOTCH error");
    }
};


} // namespace repartition
} // namespace mpi
} // namespace amgcl

#endif