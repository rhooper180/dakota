/*  _______________________________________________________________________

    DAKOTA: Design Analysis Kit for Optimization and Terascale Applications
    Copyright 2014 Sandia Corporation.
    This software is distributed under the GNU Lesser General Public License.
    For more information, see the README file in the top Dakota directory.
    _______________________________________________________________________ */

/*
 * CommonTestUtils.cpp
 * author Elliott Ridgway
 */

/////////////
// Imports //
/////////////

#include <math.h>
#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics.hpp>
#include "CommonTestUtils.hpp"

///////////////////////
// Using / Namespace //
///////////////////////

using namespace Eigen;

///////////////
// Utilities //
///////////////

void error(const std::string msg)
{
  throw(std::runtime_error(msg));
}

bool matrix_equals(const MatrixXd &A, const MatrixXd &B, double tol)
{
  if ( (A.rows()!=B.rows()) || (A.cols()!=B.cols())){
    std::cout << A.rows() << "," << A.cols() << std::endl;
    std::cout << B.rows() << "," << B.cols() << std::endl;
    error("matrix_equals() matrices sizes are inconsistent");
  }
  for (int j=0; j<A.cols(); j++){
    for (int i=0; i<A.rows(); i++){
      if (std::abs(A(i,j)-B(i,j))>tol)
	    return false;
    }
  }
  return true;
}

bool matrix_equals(const RealMatrix &A, const RealMatrix &B, double tol)
{
  if ( (A.numRows()!=B.numRows()) || (A.numCols()!=B.numCols())){
    std::cout << A.numRows() << "," << A.numCols() << std::endl;
    std::cout << B.numRows() << "," << B.numCols() << std::endl;
    error("matrix_equals() matrices sizes are inconsistent");
  }
  for (int j=0; j<A.numCols(); j++){
    for (int i=0; i<A.numRows(); i++){
      if (std::abs(A(i,j)-B(i,j))>tol)
      return false;
    }
  }
  return true;
}

double variance(VectorXd vec)
{
  boost::accumulators::accumulator_set<double, boost::accumulators::features<boost::accumulators::tag::variance>> acc;
  for(int i = 0; i < vec.size(); i++)
  {
    acc(vec(i));
  }
  return boost::accumulators::variance(acc);
}