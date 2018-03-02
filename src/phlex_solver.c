/* Copyright (C) 2015-2017 Matthew Dawson
 * Licensed under the GNU General Public License version 2 or (at your
 * option) any later version. See the file COPYING for details.
 *
 * This is the c ODE solver for the chemistry module 
 * It is currently set up to use the SUNDIALS BDF method, Newton
 * iteration with the KLU sparse linear solver.
 *
 * It uses a scalar relative tolerance and a vector absolute tolerance.
 *
*/
/** \file
 * \brief Interface to c solvers for chemistry
*/
#include "phlex_solver.h"

#define PHLEX_SOLVER_SUCCESS 0
#define PHLEX_SOLVER_FAIL 1

// State variable types (Must match parameters defined in pmc_chem_spec_data module)
#define CHEM_SPEC_UNKNOWN_TYPE 0
#define CHEM_SPEC_VARIABLE 1
#define CHEM_SPEC_CONSTANT 2
#define CHEM_SPEC_PSSA 3

/** \brief Solver initialization
 *
 * Allocate and initialize solver objects
 * 
 * \param n_state_var Number of variables on the state array
 * \param var_type Pointer to array of state variable types (solver, constant, PSSA)
 * \param abstol Pointer to array of absolute tolerances
 * \param reltol Relative integration tolerance
 * \param max_steps Maximum number of internal integration steps
 * \param max_conv_fails Maximum number of convergence failures
 * \param n_rxn Number of reactions to include
 * \param n_int_param Total number of integer reaction parameters
 * \param n_float_param Total number of floating-point reaction parameters
 * \return Pointer to an initialized SolverData object
 */
void * solver_initialize(int n_state_var, int *var_type, double *abstol,
		double reltol, int max_steps, int max_conv_fails, 
		int n_rxn, int n_int_param, int n_float_param)
{
#ifdef PMC_USE_SUNDIALS
  SolverData *sd;		// new SolverData object
  int flag;			// return code from SUNDIALS functions
  int n_dep_var;		// number of dependent variables
  int i_dep_var; 		// index of dependent variables in loops

  // Create the SolverData object
  sd = (SolverData*) malloc(sizeof(SolverData));

  // Save the number of state variables
  sd->model_data.n_state_var = n_state_var;

  // Get the number of solver variables
  n_dep_var = 0;
  for (int i; i<n_state_var; i++) 
    if (var_type[i]==CHEM_SPEC_VARIABLE) (n_dep_var)++;

  // Add the variable types to the solver data
  sd->model_data.var_type = var_type;

  // Set up the solver variable array
  sd->y = N_VNew_Serial(n_dep_var);

  // Allocate space for the reaction data and set the number
  // of reactions
  sd->model_data.rxn_data = (void*) malloc(n_int_param * sizeof(int) + 
		  n_float_param * sizeof(realtype));
  int *ptr = sd->model_data.rxn_data;
  ptr[0] = n_rxn;
  sd->model_data.nxt_rxn = (void*) &(ptr[1]);

  // Create a new solver object
  sd->cvode_mem = CVodeCreate(CV_BDF, CV_NEWTON);
  check_flag_fail((void *)sd->cvode_mem, "CVodeCreate", 0);

  // Set the model data
  flag = CVodeSetUserData(sd->cvode_mem, &(sd->model_data));
  check_flag_fail(&flag, "CVodeSetUserData", 1);

  /* Call CVodeInit to initialize the integrator memory and specify the
   * right-hand side function in y'=f(t,y), the initial time t0, and
   * the initial dependent variable vector y. */
  flag = CVodeInit(sd->cvode_mem, f, (realtype) 0.0, sd->y);
  check_flag_fail(&flag, "CVodeInit", 1);

  // Set the relative and absolute tolerances
  N_Vector abstol_nv = N_VNew_Serial(n_dep_var);
  i_dep_var = 0;
  for (int i=0; i<n_state_var; i++)
    if (var_type[i]==CHEM_SPEC_VARIABLE) NV_Ith_S(abstol_nv, i_dep_var++) = (realtype) abstol[i];
  flag = CVodeSVtolerances(sd->cvode_mem, (realtype) reltol, abstol_nv);
  check_flag_fail(&flag, "CVodeSVtolerances", 1);

  // Set the maximum number of iterations
  flag = CVodeSetMaxNumSteps(sd->cvode_mem, max_steps);
  check_flag_fail(&flag, "CVodeSetMaxNumSteps", 1);

  // Set the maximum number of convergence failures
  flag = CVodeSetMaxConvFails(sd->cvode_mem, max_conv_fails);
  check_flag_fail(&flag, "CVodeSetMaxConvFails", 1);

  // Get the structure of the Jacobian matrix
  SUNMatrix J = get_jac_init(n_dep_var, sd->model_data.rxn_data);
  sd->model_data.J_init = SUNMatClone(J);
  SUNMatCopy(J, sd->model_data.J_init);

  // Create a KLU SUNLinearSolver
  SUNLinearSolver LS = SUNKLU(sd->y, J);
  check_flag_fail((void*) LS, "SUNKLU", 0);

  // Attach the linear solver and Jacobian to the CVodeMem object
  flag = CVDlsSetLinearSolver(sd->cvode_mem, LS, J);
  check_flag_fail(&flag, "CVDlsSetLinearSolver", 1);

  // Set the Jacobian function to Jac
  flag = CVDlsSetJacFn(sd->cvode_mem, Jac);
  check_flag_fail(&flag, "CVDlsSetJacFn", 1);

  // Return a pointer to the initialized solver data
  return (void *) sd;

#else
  return NULL;
#endif
}

/** \brief Solve for a given timestep
 *
 * \param solver_data A pointer to the initialized solver data
 * \param state A pointer to the state array
 * \param env A pointer to the array of environmental conditions
 * \param t_initial Initial time (s)
 * \param t_final (s)
 * \return Flag indicating PHLEX_SOLVER_SUCCESS or PHLEX_SOLVER_FAIL
 */
int solver_run(void *solver_data, double *state, double *env, double t_initial,
		double t_final)
{
#ifdef PMC_USE_SUNDIALS
  SolverData *sd = (SolverData*) solver_data;

  // Update the dependent variables
  for (int i_spec=0, i_dep_var=0; i_spec<sd->model_data.n_state_var; i_spec++)
    if (sd->model_data.var_type[i_spec]==CHEM_SPEC_VARIABLE) NV_Ith_S(sd->y,i_dep_var++) = state[i_spec];

  // Update model data pointers
  sd->model_data.state = state;
  sd->model_data.env = env;

  // Update reaction data for new environmental state
  // (This is set up to assume the environmental variables do not change during
  //  solving. This can be changed in the future if necessary.)
  rxn_update_env_state(env, sd->model_data.rxn_data);

  // Reinitialize the solver
  int flag = CVodeReInit(sd->cvode_mem, t_initial, sd->y);
  check_flag_fail(&flag, "CVodeReInit", 1);

 // Run the solver
  realtype t_rt = (realtype) t_initial;
  flag = CVode(sd->cvode_mem, (realtype) t_final, sd->y, &t_rt, CV_NORMAL);
  if (check_flag(&flag, "CVode", 1)==PHLEX_SOLVER_FAIL) return PHLEX_SOLVER_FAIL;

  // Update the species concentrations on the state array
  for (int i_spec=0, i_dep_var=0; i_spec<sd->model_data.n_state_var; i_spec++)
    if (sd->model_data.var_type[i_spec]==CHEM_SPEC_VARIABLE) NV_Ith_S(sd->y,i_dep_var++) = state[i_spec];

  return PHLEX_SOLVER_SUCCESS;
#else
  return PHLEX_SOLVER_FAIL;
#endif
}

#ifdef PMC_USE_SUNDIALS
/** \brief Compute the time derivative f(t,y)
 *
 * \param t Current model time (s)
 * \param y Dependent variable array
 * \param deriv Time derivative vector f(t,y) to calculate
 * \param model_data Pointer to the model data
 * \return Status code
 */
int f(realtype t, N_Vector y, N_Vector deriv, void *model_data)
{
  ModelData *md = (ModelData*) model_data;

  // Update the state array with the current dependent variable values
  for (int i_spec=0, i_dep_var=0; i_spec<md->n_state_var; i_spec++)
    if (md->var_type[i_spec]==CHEM_SPEC_VARIABLE) md->state[i_spec] = NV_DATA_S(y)[i_dep_var++];

  // Initialize the derivative
  for (int i_spec=0; i_spec<NV_LENGTH_S(deriv); i_spec++) NV_DATA_S(deriv)[i_spec] = ZERO;

  // Calculate the time derivative f(t,y)
  rxn_calc_deriv(md, deriv);

  return (0);

}

/** \brief Compute the Jacobian
 *
 * \param t Current model time (s)
 * \param y Dependent variable array
 * \param deriv Time derivative vector f(t,y)
 * \param J Jacobian to calculate
 * \param model_data Pointer to the model data
 * \param tmp1 Unused vector
 * \param tmp2 Unused vector
 * \param tmp3 Unused vector
 * \return Status code
 */
int Jac(realtype t, N_Vector y, N_Vector deriv, SUNMatrix J, void *model_data,
		N_Vector tmp1, N_Vector tmp2, N_Vector tmp3)
{
  ModelData *md = (ModelData*) model_data;

  // Update the state array with the current dependent variable values
  for (int i_spec=0, i_dep_var=0; i_spec<md->n_state_var; i_spec++)
    if (md->var_type[i_spec]==CHEM_SPEC_VARIABLE) md->state[i_spec] = NV_DATA_S(y)[i_dep_var++];

  // TODO Figure out how to keep the Jacobian from being redimensioned
  // Reset the Jacobian dimensions
  if (SM_NNZ_S(J)<SM_NNZ_S(md->J_init)) {
    SM_INDEXVALS_S(J) = realloc(SM_INDEXVALS_S(J), SM_NNZ_S(md->J_init)*sizeof(sunindextype));
    SM_DATA_S(J) = realloc(SM_DATA_S(J), SM_NNZ_S(md->J_init)*sizeof(realtype));
  }
  SM_NNZ_S(J) = SM_NNZ_S(md->J_init);
  for (int i=0; i<SM_NNZ_S(J); i++) {
    (SM_DATA_S(J))[i] = (realtype)0.0;
    (SM_INDEXVALS_S(J))[i] = (SM_INDEXVALS_S(md->J_init))[i];
  }
  for (int i=0; i<=SM_NP_S(J); i++) {
    (SM_INDEXPTRS_S(J))[i] = (SM_INDEXPTRS_S(md->J_init))[i];
  } 

  // Calculate the Jacobian
  rxn_calc_jac(md, J);

  return (0);

}

/** \brief Create a sparse Jacobian matrix based on model data
 *
 * \param n_dep_var Number of dependent variables
 * \param rxn_data A pointer to the reaction data
 * \return Sparse Jacobian matrix with all possible non-zero elements intialized to 1.0
 */
SUNMatrix get_jac_init(int n_dep_var, void *rxn_data)
{
  int n_rxn;			/* number of reactions in the mechanism 
  				 * (stored in first position in *rxn_data) */
  bool **jac_struct;		/* structure of Jacobian with flags to indicate
				 * elements that could be used. */
  sunindextype n_jac_elem; 	/* number of potentially non-zero Jacobian elements */

  // Set up the 2D array of flags
  jac_struct = (bool**) malloc(sizeof(bool*) * n_dep_var);
  for (int i_spec=0; i_spec<n_dep_var; i_spec++) {
    jac_struct[i_spec] = (bool*) malloc(sizeof(bool) * n_dep_var);
    for (int j_spec=0; j_spec<n_dep_var; j_spec++) jac_struct[i_spec][j_spec] = false;
  }

  // Fill in the 2D array of flags with Jacobian elements used by the
  // mechanism reactions
  rxn_get_used_jac_elem(rxn_data, jac_struct);

  // Determine the number of non-zero Jacobian elements
  for (int i=0, n_jac_elem = 0; i<n_dep_var; i++)
    for (int j=0; j<n_dep_var; j++)
      if (jac_struct[i][j]==true) n_jac_elem++;

  // Initialize the sparse matrix
  SUNMatrix M = SUNSparseMatrix(n_dep_var, n_dep_var, n_jac_elem, CSC_MAT);

  // Set the column and row indices
  int i_col=0, i_elem=0;
  for (; i_col<n_dep_var; i_col++) {
    (SM_INDEXPTRS_S(M))[i_col] = i_elem;
    for (int i_row=0; i_row<n_dep_var; i_row++) {
      if (jac_struct[i_col][i_row]==true) {
	(SM_DATA_S(M))[i_elem] = (realtype) 1.0;
	(SM_INDEXVALS_S(M))[i_elem++] = i_row;
      }
    }
  }
  (SM_INDEXPTRS_S(M))[i_col] = i_elem;

  // Free the memory used for the 2D array of flags
  for (int i_spec=0; i_spec<n_dep_var; i_spec++) free(jac_struct[i_spec]);
  free(jac_struct);

  return M;
    
}

/** \brief Check the return value of a SUNDIALS function
 *
 * \param flag_value A pointer to check (either for NULL, or as an int pointer giving the flag value
 * \param func_name A string giving the function name returning this result code
 * \param opt A flag indicating the type of check to perform (0 for NULL pointer check; 1 for integer flag check)
 * \return Flag indicating PHLEX_SOLVER_SUCCESS or PHLEX_SOLVER_FAIL
 */
int check_flag(void *flag_value, char *func_name, int opt)
{
  int *err_flag;

  /* Check for a NULL pointer */
  if (opt==0 && flag_value == NULL) {
    fprintf(stderr, "\nSUNDIALS_ERROR: %s() failed - returned NULL pointer\n\n",
		    func_name);
    return PHLEX_SOLVER_FAIL;
  }

  /* Check if flag < 0 */
  else if (opt == 1) {
    err_flag = (int *) flag_value;
    if (err_flag < 0) {
      fprintf(stderr, "\nSUNDIALS_ERROR: %s() failed with flag = %d\n\n",
		      func_name, *err_flag);
      return PHLEX_SOLVER_FAIL;
    }
  }
  return PHLEX_SOLVER_SUCCESS;
}

/** \brief Check the return value of a SUNDIALS function and exit on failure
 *
 * \param flag_value A pointer to check (either for NULL, or as an int pointer giving the flag value
 * \param func_name A string giving the function name returning this result code
 * \param opt A flag indicating the type of check to perform (0 for NULL pointer check; 1 for integer flag check)
 */
void check_flag_fail(void *flag_value, char *func_name, int opt)
{
  if (check_flag(flag_value, func_name, opt)==PHLEX_SOLVER_FAIL) {
    exit(EXIT_FAILURE);
  }
}
#endif
