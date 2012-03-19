/** \file
    \brief A class containing all information for the systems setup.
    \todo Doc me!
*/

#ifndef _SYSPARAMS_H
#define _SYSPARAMS_H

#include <string>

class SysParams {
 public:
	SysParams();	// We now only need a default constructor

	static constexpr double pi = 3.14159265358979323846;
  
  // Return parameters needed during runtime
	double get_bjerrum();
  double get_radius();
	double get_epsilon();
	double get_lambda2i();
	double get_charge_density();
	double get_error();
	double get_alpha_ic();
	double get_alpha_ipbs();
	int get_maxiter();
	double get_tolerance();
	double get_newton_tolerance();
 	int get_symmetry();
 	double get_boxLength();
 	double get_refinementFraction();
 	int get_refinementSteps();
  int get_refinement();
  int get_verbose();
  int get_salt();
  int get_npart();
  std::string get_meshfile();

  // Functions setting the private members
  void add_error(double);
	void reset_error();
  void reset_error(double error);
	void set_lambda (double value);
	void set_bjerrum (double value);
	void set_alpha_ic(double);
	void set_alpha_ipbs(double);
  void set_maxiter(int);
  void set_meshfile(std::string filename);
  void set_refinement(int level);
  void set_tolerance(double value);
  void set_newton_tolerance(double value);
  void set_verbose(int value);
	void set_charge_density (double value);
	void set_symmetry (int value);
	void set_boxLength (double value);
	void set_refinementFraction (double value);
	void set_refinementSteps (int value);
	void set_salt (int value);
  void set_npart (int value);
	
  private:
	double lambda;
	double lambda2i;
	double bjerrum;
	double charge_density;
	double charge;
	double epsilon;
  double tolerance;
  double newton_tolerance;
  double totalError;
	int symmetry;
  int npart;
  int salt;
	int boxLength;
	double alpha_ic;	// SOR parameter
	double alpha_ipbs;	// SOR parameter
  int maxiter;
  std::string meshfile;
  int refinement;
  int verbose;
  double refinementFraction;
  int refinementSteps;
};

#endif  // _SYSPARAMS_H