/** \brief Driver for solving the IPBS problem using P1 piecewise linear Lagrange elements

    Dirichlet boundary conditions are used for domain (outer) boundaries, Neumann b.c. on
    symmetry axis and IPBS Neumann b.c. where we want the IPBS iterative procedure.
*/

/*!
   \param gv the view on the leaf grid
   \param elementIndexToEntity mapper defining the index of inner elements
   \param boundaryIndexToEntity mapper defining the index of boundary elements
*/

#include <dune/pdelab/gridoperator/gridoperator.hh>
#if GRIDDIM == 2
#include<dune/pdelab/finiteelementmap/pk2dfem.hh>	// Pk in 2 dimensions
#elif GRIDDIM == 3
#include <dune/pdelab/finiteelementmap/pk3dfem.hh>
#endif

// pdelab includes
#include<dune/pdelab/finiteelementmap/conformingconstraints.hh>
#include<dune/pdelab/gridfunctionspace/gridfunctionspace.hh>
#include<dune/pdelab/gridfunctionspace/interpolate.hh>
#include<dune/pdelab/backend/istlvectorbackend.hh>
#include<dune/pdelab/backend/istlmatrixbackend.hh>
#include<dune/pdelab/backend/istlsolverbackend.hh>
#include<dune/pdelab/newton/newton.hh>

#if HAVE_MPI
#include <dune/pdelab/backend/novlpistlsolverbackend.hh>
#endif

#include <dune/ipbs/datawriter.hh>
#include <dune/ipbs/ipbsolver.hh>
#include <dune/ipbs/boundaries.hh>
#include <dune/ipbs/PBLocalOperator.hh>

#include <dune/ipbs/ipbsanalysis.hh>

// test some solvers
//#include<dune/pdelab/stationary/linearproblem.hh>
//#include <dune/pdelab/backend/seqistlsolverbackend.hh>

template<class GridType, typename PGMap, int k>
void ipbs_Pk(GridType* grid, const PGMap& elementIndexToEntity,
             const PGMap& boundaryIndexToEntity)
{
  // We want to know the total calulation time
  Dune::Timer timer;
  timer.start();
  
  std::stringstream status;
  //std::ofstream status;
  //status.open ("status.dat", std::ios::out | std::ios::out); 

  // get a grid view on the leaf grid
  typedef typename GridType::LeafGridView GV;
  const GV& gv = grid->leafView();

  // Obtain a reference to the communicator
  typedef typename GV::Traits::CollectiveCommunication CollectiveCommunication;
  const CollectiveCommunication & communicator = gv.comm();
    
  // some typedef
  typedef typename GV::Grid::ctype ctype;
  const int dim = GV::dimension;

  // <<<1>>> Setup the problem from mesh file

  // NOTE only flux b.c. are iterated, so rest is instantiated only once
  // inner region
  typedef Regions<GV,double,std::vector<int> > M;
  M m(gv, elementIndexToEntity);
  // boundary condition type
  typedef BCTypeParam<std::vector<int> > B;
  B b(boundaryIndexToEntity);
  // Class defining Dirichlet B.C.
  typedef BCExtension<GV,double,std::vector<int> > G;
  G g(gv, boundaryIndexToEntity);

  // Create finite element map
#if GRIDDIM == 2
  typedef Dune::PDELab::Pk2DLocalFiniteElementMap<GV, ctype, Real, k> FEM;
#elif GRIDDIM == 3
  typedef Dune::PDELab::Pk3DLocalFiniteElementMap<GV, ctype, Real, k> FEM;
#endif
  FEM fem(gv);

  // <<<2>>> Make grid function space
  
  // Define ISTL Vector backend - template argument is the blocksize!
  typedef Dune::PDELab::ISTLVectorBackend<1> VBE;
  
  // Setup constraints
#if HAVE_MPI  // UG and AluGrid are nonoverlapping grids
  typedef Dune::PDELab::NonoverlappingConformingDirichletConstraints CON;
#else
  typedef Dune::PDELab::ConformingDirichletConstraints CON;
#endif
  CON con;  // initialize constraints

  typedef Dune::PDELab::GridFunctionSpace<GV,FEM,CON,VBE> GFS;
  GFS gfs(gv,fem,con);

#if HAVE_MPI  // Compute Ghost Elements
    con.compute_ghosts(gfs);
#endif

  // Create coefficient vector (with zero values)
  typedef typename Dune::PDELab::BackendVectorSelector<GFS,Real>::Type U;
  U u(gfs,0.0);
  
  // <<<3>>> Make FE function extending Dirichlet boundary conditions
  typedef typename GFS::template ConstraintsContainer<Real>::Type CC;
  CC cc;
  Dune::PDELab::constraints(b,gfs,cc); 
  std::cout << "constrained dofs=" << cc.size() 
            << " of " << gfs.globalSize() << std::endl;

  // interpolate coefficient vector
  // this has to be done only during intitialization as we don't change dirichlet b.c.
  Dune::PDELab::interpolate(g,gfs,u);
  Dune::PDELab::set_nonconstrained_dofs(cc,0.0,u);

  typedef Ipbsolver<GV, GFS> Ipbs;
  Ipbs ipbs(gv, gfs, boundaryIndexToEntity, 1);
  // instanciate boundary fluxes
  typedef BoundaryFlux<GV,double,std::vector<int>, Ipbs > J;
  J j(gv, boundaryIndexToEntity, ipbs);

  // <<<4>>> Make Grid Operator Space
  typedef PBLocalOperator<M,B,J> LOP;
  LOP lop(m,b,j,k+1);   // integration order
  typedef Dune::PDELab::ISTLBCRSMatrixBackend<1,1> MBE;
#if HAVE_MPI    // enable overlapping mode
  typedef Dune::PDELab::GridOperator<GFS,GFS,LOP,MBE,
                                    Real,Real,Real,CC,CC,true> GO;
#else
  typedef Dune::PDELab::GridOperator<GFS,GFS,LOP,MBE,
                                    Real,Real,Real,CC,CC> GO;
#endif
  GO go(gfs,cc,gfs,cc,lop);

const int solverMaxIter = 20000;

  // <<<5a>>> Select a linear solver backend
#if HAVE_MPI
#if LINEARSOLVER == BCGS_SSORk
  typedef Dune::PDELab::ISTLBackend_NOVLP_BCGS_SSORk<GO> LS;
  LS ls( gfs, solverMaxIter, 5, sysParams.get_verbose() );
#endif

#if LINEARSOLVER == BCGS_NOPREC
  typedef Dune::PDELab::ISTLBackend_NOVLP_BCGS_NOPREC<GFS> LS;
  LS ls( gfs, solverMaxIter, sysParams.get_verbose() );
#endif

#if LINEARSOLVER == CG_SSORk
  typedef Dune::PDELab::ISTLBackend_NOVLP_CG_SSORk< GO > LS;
  LS ls( gfs, solverMaxIter, 5, sysParams.get_verbose() );
#endif

#if LINEARSOLVER == CG_NOPREC
  typedef Dune::PDELab::ISTLBackend_NOVLP_CG_NOPREC<GFS> LS;
  LS ls( gfs, solverMaxIter, sysParams.get_verbose() );
#endif

#if LINEARSOLVER == CG_Jacobi
  typedef Dune::PDELab::ISTLBackend_NOVLP_CG_Jacobi< GFS > LS;
  LS ls( gfs, solverMaxIter, sysParams.get_verbose() );
#endif

#if LINEARSOLVER == CG_AMG_SSOR
    typedef Dune::PDELab::ISTLBackend_NOVLP_CG_AMG_SSOR<GO> LS;
    LS ls( gfs, 5, solverMaxIter, sysParams.get_verbose() );
#endif

#if LINEARSOLVER == BCGS_AMG_SSOR
  typedef Dune::PDELab::ISTLBackend_NOVLP_BCGS_AMG_SSOR<GO> LS;
  LS ls( gfs, solverMaxIter, sysParams.get_verbose() );
#endif

#else
  typedef Dune::PDELab::ISTLBackend_SEQ_BCGS_SSOR LS;
  //typedef Dune::PDELab::ISTLBackend_SEQ_SuperLU LS;
  //typedef Dune::PDELab::ISTLBackend_SEQ_CG_ILU0 LS;
  //typedef Dune::PDELab::ISTLBackend_SEQ_BCGS_AMG_SOR<GOS> LS;
  LS ls(5000, true);
#endif

  //typedef Dune::PDELab::StationaryLinearProblemSolver<GOS,LS,U> SLP;
  //SLP slp(gos, u, ls, 1e-10);
  //slp.apply();

  // <<<5b>>> Solve nonlinear problem
  typedef Dune::PDELab::Newton<GO,LS,U> NEWTON;
  NEWTON newton(go,u,ls);
  newton.setLineSearchStrategy(newton.hackbuschReuskenAcceptBest);
  newton.setVerbosityLevel(sysParams.get_verbose());
  newton.setMinLinearReduction(1e-9);
  newton.setMaxIterations(100);
  newton.setLineSearchMaxIterations(50);

  typedef Dune::PDELab::DiscreteGridFunction<GFS,U> DGF;
  
  double inittime = timer.elapsed();
  double solvertime = 0.;
  double itertime = 0.;

  double fluxError, icError;
  int iterations = 0;

  DataWriter<GV,dim> mydatawriter(gv);

  // Analysis class
  typedef IpbsAnalysis<GV,GFS,std::vector<int> , Ipbs> Analyzer;
  const Analyzer analyzer(gv, gfs, boundaryIndexToEntity, ipbs);

  // --- Here the iterative loop starts ---

  unsigned int counter = 0;
  do
  {
    timer.reset();
    try{
        newton.apply();
    }
    catch (Dune::Exception &e){
        status << "# Dune reported error: " << e << std::endl;
        std::cerr << "Dune reported error: " << e << std::endl;
        break;
    }
    catch (...){
        std::cerr << "Unknown exception thrown!" << std::endl;
        break;
    }
    solvertime += timer.elapsed();

    // save snapshots every N step
    if(iterations % sysParams.get_outStep() == 0) {
      std::stringstream out;
      out << sysParams.get_outname() << "_step_" << iterations;
      std::string filename = out.str();
      DGF udgf_snapshot(gfs,u);
      Dune::VTKWriter<GV> vtkwriter(gv,Dune::VTKOptions::conforming);
      vtkwriter.addVertexData(new Dune::PDELab::VTKGridFunctionAdapter<DGF>(udgf_snapshot,"solution"));
      vtkwriter.write(filename,Dune::VTK::appendedraw);
      mydatawriter.writeIpbsCellData(gfs, u, "solution", filename, status);
      std::stringstream fname;
      fname << "forces_step_" << iterations;
      std::string forcefilename = fname.str();
      analyzer.forces(u, forcefilename);
    }

    timer.reset();
    ipbs.updateChargeRegulation(u);
    ipbs.updateBC(u);
    itertime += timer.elapsed();
    counter++;
  }
  while (!ipbs.converged(fluxError,icError,iterations) && counter < sysParams.get_maxiter());

  // --- here the iterative loop ends! ---


  status << "# reached convergence criterion: " << std::boolalpha <<
    ipbs.converged(fluxError, icError, iterations) << std::endl;
  status << "# in iteration " << iterations << std::endl
      << "# maximum relative change in boundary condition calculation is " <<  fluxError << std::endl
      << "# maximum relative change in induced charge density is " << icError << std::endl;

  // <<<6>>> graphical output
  DGF udgf(gfs,u);
  Dune::VTKWriter<GV> vtkwriter(gv,Dune::VTKOptions::conforming);
  vtkwriter.addVertexData(new Dune::PDELab::VTKGridFunctionAdapter<DGF>(udgf,"solution"));
  vtkwriter.write((sysParams.get_outname() + "_solution").c_str(), Dune::VTK::appendedraw);
  // Also do Gnuplot output
  mydatawriter.writeIpbsCellData(gfs, u, "solution", sysParams.get_outname() + "_solution", status);

  analyzer.forces(u, "forces.dat");
  analyzer.surfacepot(u, "surface_potential.dat");
  analyzer.E_ext(u, "e_ext.dat");

  analyzer.energy(u, "idiot");
  std::vector<double> corners;
  corners.push_back(0);
  corners.push_back(10);
  corners.push_back(0);
  corners.push_back(20);
//  analyzer.write_structured_output(u, "structured.dat", corners, 0.2);
  
  if (communicator.rank() == 0) {
    std::cout << "P " << communicator.size() << " N: " << elementIndexToEntity.size() << " M: " << ipbs.get_n() 
      << " init: " << inittime << " solver: " << solvertime/iterations 
      << " boundary update " << itertime/iterations << std::endl;
  }
}
