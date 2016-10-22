/*
 * Copyright (c) The Shogun Machine Learning Toolbox
 * Written (w) 2016 Heiko Strathmann
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are those
 * of the authors and should not be interpreted as representing official policies,
 * either expressed or implied, of the Shogun Development Team.
 */

#include <shogun/lib/config.h>
#include <shogun/lib/SGMatrix.h>
#include <shogun/lib/SGVector.h>
#include <shogun/mathematics/eigen3.h>
#include <shogun/mathematics/Math.h>
#include <shogun/io/SGIO.h>

#include "Full.h"
#include "kernel/Base.h"

using namespace shogun;
using namespace shogun::kernel_exp_family_impl;
using namespace Eigen;

Full::Full(SGMatrix<float64_t> data,
		kernel::Base* kernel, float64_t lambda) :
		Base(data, kernel, lambda)
{
}

SGVector<float64_t> Full::compute_h() const
{
	auto D = get_num_dimensions();
	auto N = get_num_lhs();
	auto ND = N*D;
	SGVector<float64_t> h(ND);
	Map<VectorXd> eigen_h(h.vector, ND);
	eigen_h = VectorXd::Zero(ND);

#pragma omp parallel for
	for (auto idx_b=0; idx_b<N; idx_b++)
		for (auto idx_a=0; idx_a<N; idx_a++)
		{
			// TODO optimise, no need to store matrix
			// TODO in Nystrom, this needs to be sub-sampled as well
			SGMatrix<float64_t> temp = m_kernel->dx_dx_dy(idx_a, idx_b);
			eigen_h.segment(idx_b*D, D) += Map<MatrixXd>(temp.matrix, D,D).colwise().sum();
		}

	eigen_h /= N;

	return h;
}

float64_t Full::compute_xi_norm_2() const
{
	auto N = get_num_lhs();
	float64_t xi_norm_2=0;

#pragma omp parallel for reduction (+:xi_norm_2)
	for (auto idx_a=0; idx_a<N; idx_a++)
		for (auto idx_b=0; idx_b<N; idx_b++)
			xi_norm_2 += m_kernel->dx_dx_dy_dy_sum(idx_a, idx_b);

	xi_norm_2 /= (N*N);

	return xi_norm_2;
}

std::pair<SGMatrix<float64_t>, SGVector<float64_t>> Full::build_system() const
{
	auto D = get_num_dimensions();
	auto N = get_num_lhs();
	auto ND = N*D;

	// TODO A matrix should be stored exploiting symmetry
	SG_SINFO("Allocating memory for system.\n");
	SGMatrix<float64_t> A(ND+1,ND+1);
	Map<MatrixXd> eigen_A(A.matrix, ND+1,ND+1);
	SGVector<float64_t> b(ND+1);
	Map<VectorXd> eigen_b(b.vector, ND+1);

	// TODO all this can be done using a single pass over all data

	SG_SINFO("Computing h.\n");
	auto h = compute_h();
	auto eigen_h=Map<VectorXd>(h.vector, ND);

	SG_SINFO("Computing all kernel Hessians.\n");
	auto all_hessians = m_kernel->dx_dy_all();
	auto eigen_all_hessians = Map<MatrixXd>(all_hessians.matrix, ND, ND);

	SG_SINFO("Computing xi norm.\n");
	auto xi_norm_2 = compute_xi_norm_2();

	SG_SINFO("Populating A matrix.\n");
	// A[0, 0] = np.dot(h, h) / n + lmbda * xi_norm_2
	A(0,0) = eigen_h.squaredNorm() / N + m_lambda * xi_norm_2;

	// A[1:, 1:] = np.dot(all_hessians, all_hessians) / N + lmbda * all_hessians
	// A[0, 1:] = np.dot(h, all_hessians) / n + lmbda * h; A[1:, 0] = A[0, 1:]
	// can use noalias to speed up as matrices are definitely different
	eigen_A.block(1,1,ND,ND).noalias()=eigen_all_hessians*eigen_all_hessians / N + m_lambda*eigen_all_hessians;
	eigen_A.row(0).segment(1, ND).noalias() = eigen_all_hessians*eigen_h / N + m_lambda*eigen_h;
	eigen_A.col(0).segment(1, ND).noalias() = eigen_A.row(0).segment(1, ND);

	// b[0] = -xi_norm_2; b[1:] = -h.reshape(-1)
	b[0] = -xi_norm_2;
	eigen_b.segment(1, ND) = -eigen_h;

	return std::pair<SGMatrix<float64_t>, SGVector<float64_t>>(A, b);
}

float64_t Full::log_pdf(index_t idx_test) const
{
	auto D = get_num_dimensions();
	auto N = get_num_lhs();

	float64_t xi = 0;
	float64_t beta_sum = 0;

	Map<VectorXd> eigen_alpha_beta(m_alpha_beta.vector, N*D+1);
	for (auto idx_a=0; idx_a<N; idx_a++)
	{
		SGVector<float64_t> k=m_kernel->dx_dx(idx_a, idx_test);
		Map<VectorXd> eigen_k(k.vector, D);
		xi += eigen_k.sum() / N;

		auto grad_x_xa = m_kernel->dx(idx_a, idx_test);
		Map<VectorXd> eigen_grad_x_xa(grad_x_xa.vector, D);

		// betasum += np.dot(gradient_x_xa, beta[a, :])
		// note: sign flip as different argument order compared to Python code
		beta_sum -= eigen_grad_x_xa.transpose()*eigen_alpha_beta.segment(1+idx_a*D, D);

	}
	return m_alpha_beta[0]*xi + beta_sum;
}

SGVector<float64_t> Full::grad(index_t idx_test) const
{
	// TODO this produces junk for 1D case
	auto D = get_num_dimensions();
	auto N = get_num_lhs();

	SGVector<float64_t> xi_grad(D);
	SGVector<float64_t> beta_sum_grad(D);
	Map<VectorXd> eigen_xi_grad(xi_grad.vector, D);
	Map<VectorXd> eigen_beta_sum_grad(beta_sum_grad.vector, D);
	eigen_xi_grad = VectorXd::Zero(D);
	eigen_beta_sum_grad.array() = VectorXd::Zero(D);

	Map<VectorXd> eigen_alpha_beta(m_alpha_beta.vector, N*D+1);
	for (auto a=0; a<N; a++)
	{
		SGMatrix<float64_t> g=m_kernel->dx_i_dx_i_dx_j(a, idx_test);
		Map<MatrixXd> eigen_g(g.matrix, D, D);
		eigen_xi_grad -= eigen_g.colwise().sum();

		// left_arg_hessian = gaussian_kernel_dx_i_dx_j(x, x_a, sigma)
		// betasum_grad += beta[a, :].dot(left_arg_hessian)
		// TODO storage is not necessary here
		// note: sign flip as different argument order compared to Python code
		auto left_arg_hessian = m_kernel->dx_i_dx_j(a, idx_test);
		Map<MatrixXd> eigen_left_arg_hessian(left_arg_hessian.matrix, D, D);
		eigen_beta_sum_grad += eigen_left_arg_hessian*eigen_alpha_beta.segment(1+a*D, D).matrix();
	}

	// return alpha * xi_grad + betasum_grad
	eigen_xi_grad *= m_alpha_beta[0] / N;
	return xi_grad + beta_sum_grad;
}

SGMatrix<float64_t> Full::hessian(index_t idx_test) const
{
	auto N = get_num_lhs();
	auto D = get_num_dimensions();

	SGMatrix<float64_t> xi_hessian(D, D);
	SGMatrix<float64_t> beta_sum_hessian(D, D);

	Map<MatrixXd> eigen_xi_hessian(xi_hessian.matrix, D, D);
	Map<MatrixXd> eigen_beta_sum_hessian(beta_sum_hessian.matrix, D, D);

	eigen_xi_hessian = MatrixXd::Zero(D, D);
	eigen_beta_sum_hessian = MatrixXd::Zero(D, D);

	// Entire alpha-beta vector
	Map<VectorXd> eigen_alpha_beta(m_alpha_beta.vector, N*D+1);

	for (auto a=0; a<N; a++)
	{
		// Arguments are opposite order of Python code but sign flip is not
		// needed since the function is symmetric
		auto xi_hess_sum = m_kernel->dx_i_dx_j_dx_k_dx_k_row_sum(a, idx_test);

		Map<MatrixXd> eigen_xi_hess_sum(xi_hess_sum.matrix, D, D);
		eigen_xi_hessian += eigen_xi_hess_sum;

		// Beta segment vector
		SGVector<float64_t> beta_a(eigen_alpha_beta.segment(1+a*D, D).data(), D, false);

		// Note sign flip because arguments are opposite order of Python code
		auto beta_hess_sum = m_kernel->dx_i_dx_j_dx_k_dot_vec(a, idx_test, beta_a);
		Map<MatrixXd> eigen_beta_hess_sum(beta_hess_sum.matrix, D, D);
		eigen_beta_sum_hessian -= eigen_beta_hess_sum;
	}

	eigen_xi_hessian.array() *= m_alpha_beta[0] / N;

	// re-use memory rather than re-allocating a new result matrix
	eigen_xi_hessian += eigen_beta_sum_hessian;

	return xi_hessian;
}

SGVector<float64_t> Full::hessian_diag(index_t idx_test) const
{
	// Note: code modifed from full hessian case
	auto N = get_num_lhs();
	auto D = get_num_dimensions();

	SGVector<float64_t> xi_hessian_diag(D);
	SGVector<float64_t> beta_sum_hessian_diag(D);

	Map<VectorXd> eigen_xi_hessian_diag(xi_hessian_diag.vector, D);
	Map<VectorXd> eigen_beta_sum_hessian_diag(beta_sum_hessian_diag.vector, D);

	eigen_xi_hessian_diag = VectorXd::Zero(D);
	eigen_beta_sum_hessian_diag = VectorXd::Zero(D);

	Map<VectorXd> eigen_alpha_beta(m_alpha_beta.vector, N*D+1);

	for (auto a=0; a<N; a++)
	{
		SGVector<float64_t> beta_a(eigen_alpha_beta.segment(1+a*D, D).data(), D, false);
		for (auto i=0; i<D; i++)
		{
			eigen_xi_hessian_diag[i] += m_kernel->dx_i_dx_j_dx_k_dx_k_row_sum_component(
					a, idx_test, i, i);
			eigen_beta_sum_hessian_diag[i] -= m_kernel->dx_i_dx_j_dx_k_dot_vec_component(
					a, idx_test, beta_a, i, i);
		}
	}

	eigen_xi_hessian_diag.array() *= m_alpha_beta[0] / N;
	eigen_xi_hessian_diag += eigen_beta_sum_hessian_diag;

	return xi_hessian_diag;
}

SGVector<float64_t> Full::leverage() const
{
	auto ND = get_num_lhs()*get_num_dimensions();

	auto leverage = SGVector<float64_t>(ND);

	SG_SINFO("Computing exact leverage scores using SVD.\n");

	auto A = Map<MatrixXd>(build_system().first.matrix, ND+1, ND+1);

//	SelfAdjointEigenSolver<MatrixXd> solver(A);
//	auto s = solver.eigenvalues();
//	auto U = solver.eigenvectors();
//
//	switch (solver.info())
//	{
//	case NumericalIssue:
//		SG_SWARNING("Numerical problems computing Eigendecomposition.\n");
//		break;
//	case NoConvergence:
//		SG_SWARNING("No convergence computing Eigendecomposition.\n");
//		break;
//	default:
//		break;
//	}

	// using SVD here since eigen3's self-adjoint eigenvalue produces negatives
	JacobiSVD<MatrixXd> solver(A.block(1,1,ND,ND), Eigen::ComputeThinU);
	auto s = solver.singularValues().array().pow(2);
	auto U = solver.matrixU();

	SG_SINFO("Eigenspectrum range is [%f, %f], or [exp(%f), exp(%f)].\n",
			s.array().minCoeff(), s.array().maxCoeff(),
			CMath::log(s.array().minCoeff()), CMath::log(s.array().maxCoeff()));

	for (auto i=0; i<ND; i++)
	{
		leverage[i]=0;
		for (auto j=0; j<ND; j++)
			leverage[i] += s[j] / (s[j]+ND*m_lambda)*pow(U(i,j), 2);
	}

	return leverage;
}