#ifndef AMGCL_SOLVER_EIGEN_HPP
#define AMGCL_SOLVER_EIGEN_HPP

/*
The MIT License

Copyright (c) 2012-2014 Denis Demidov <dennis.demidov@gmail.com>

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
\file   amgcl/solver/eigen.hpp
\author Denis Demidov <dennis.demidov@gmail.com>
\brief  Wrapper around eigen direct solvers.
*/

#include <Eigen/Dense>
#include <Eigen/SparseCore>

#include <amgcl/backend/builtin.hpp>
#include <amgcl/util.hpp>

namespace amgcl {
namespace solver {

template < class Solver >
class EigenSolver {
    public:
        typedef typename Solver::MatrixType MatrixType;
        typedef typename Solver::Scalar     value_type;

        struct params {
            params() {}
            params(const boost::property_tree::ptree&) {}
        };

        template <class Matrix>
        EigenSolver(const Matrix &A, const params& = params())
            : n( backend::rows(A) )
        {
            S.compute(
                    MatrixType(
                        Eigen::MappedSparseMatrix<value_type, Eigen::RowMajor, int>(
                            backend::rows(A), backend::cols(A), backend::nonzeros(A),
                            const_cast<int*>(backend::ptr_data(A)),
                            const_cast<int*>(backend::col_data(A)),
                            const_cast<value_type*>(backend::val_data(A))
                            )
                        )
                    );
        }

        template <class Vec1, class Vec2>
        void operator()(const Vec1 &rhs, Vec2 &x) const {
            Eigen::Map< Eigen::Matrix<value_type, Eigen::Dynamic, 1> >
                RHS(const_cast<value_type*>(&rhs[0]), n), X(&x[0], n);

            X = S.solve(RHS);
        }
    private:
        ptrdiff_t n;
        Solver S;
};

} // namespace solver
} // namespace amgcl

#endif
