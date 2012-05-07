#ifndef _IPBSOLVER_HH
#define _IPBSOLVER_HH

// Single Geometry Single Codim Mapper
#include <dune/grid/common/scsgmapper.hh>
#include<dune/common/fvector.hh>
#include <gsl/gsl_sf_ellint.h>
#include "sysparams.hh"
#include "boundary.hh"
#include "maxwelltensor.hh"

extern SysParams sysParams;
extern std::vector<Boundary*> boundary;



template <class GV, class GFS>
class Ipbsolver

/** \brief Encapsulation of the algorithm determining the iterated boundary values

    Here goes some explanation on what is done :-)
    \todo { Doc Me ! }   
*/

{

  // Some typedef
  typedef typename GV::Grid::ctype ctype;
  static const int dim = GV::dimension;
  typedef typename GFS::template VectorContainer<Real>::Type U;

  public:
    Ipbsolver(const GV& gv_, const GFS& gfs_, Dune::MPIHelper& helper_, 
        const std::vector<int>& boundaryIndexToEntity_, const bool use_guess=true) :
      gv(gv_), gfs(gfs_), helper(helper_), boundaryIndexToEntity(boundaryIndexToEntity_),
      boundaryElemMapper(gv), communicator(helper.getCommunicator()), my_offset(0), my_len(0), iterationCounter(0), fluxError(0)
     
    /*!
       \param gv the view on the leaf grid
       \param boundaryIndexToEntity physical property of boundary elements
    */
    {
      init(); // Detect iterative elements
      communicateIpbsData(); 
      bContainer.resize(ipbsPositions.size(),0);
      inducedChargeDensity.resize(ipbsPositions.size(),0);
      bEfield.resize(ipbsPositions.size(),0);
      if (use_guess) initial_guess();
    }

    int get_n()
    {
      return ipbsPositions.size();
    }

    bool next_step()
    {
      std::cout << "in iteration " << iterationCounter << " the relative fluxError is " << fluxError
        << " relative error in induced charge density is " << icError << std::endl;
      if (fluxError > sysParams.get_tolerance() || icError > 1e-3) {
        iterationCounter++;
        return true;
      }
      else
        return false;
    }
    
    /*!
     * \param _fluxError return the current maximum relative change in boundary condition calulation
     * \param _icError return the current maximum relative change in induced charge computation
     */

    bool next_step(double& _fluxError, double& _icError, int& _iterations)
    {
      _fluxError = fluxError;
      _icError = icError;
      _iterations = iterationCounter;
      std::cout << "in iteration " << iterationCounter << " the relative fluxError is " << fluxError
        << " relative error in induced charge density is " << icError << std::endl;
      if (fluxError > sysParams.get_tolerance() || icError > 1e-3) {
        iterationCounter++;
        return true;
      }
      else
        return false;
    }
    
    template <typename I>
    double get_flux(const I& i) const
    {
      int mappedIndex = indexLookupMap.find(boundaryElemMapper.map(*i.inside()))->second + my_offset;
      double y = bContainer[mappedIndex];
      return y;
    }

    // ------------------------------------------------------------------------
    /// Induced charge computation
    // ------------------------------------------------------------------------
    void updateIC()
    {
      icError = 0;  // reset the fluxError for next iteration step
      //std::cout << "in updateIC() my_offset = " << my_offset << " my_len = " << my_len << std::endl;
      bContainerType ic(inducedChargeDensity.size(), 0.);
      double eps_out = sysParams.get_epsilon();  
      unsigned int target = my_offset + my_len;
      for (unsigned int i = my_offset; i < target; i++) {
        double eps_in = boundary[ipbsType[i]]->get_epsilon();
        double my_charge = boundary[ipbsType[i]]->get_charge_density();
        // calculate the induced charge in this surface element
        ic[i] = -(eps_in - eps_out) / (eps_in+eps_out)
                                * ( my_charge + eps_out/(2.*sysParams.pi*sysParams.get_bjerrum())
                                    * bEfield[i] );
      }
      communicator.barrier();
      communicator.sum(&ic[0], ic.size());

      // Do the SOR 
      for (unsigned int i = 0; i < inducedChargeDensity.size(); i++) {
        inducedChargeDensity[i] = sysParams.get_alpha_ic() * ic[i]
                          + ( 1 - sysParams.get_alpha_ic()) * inducedChargeDensity[i];
        double local_icError = fabs(2.0*(ic[i]-inducedChargeDensity[i])
                      /(ic[i]+inducedChargeDensity[i]));
        icError = std::max(icError, local_icError);
      }
      communicator.barrier();
      communicator.max(&icError, 1);
    }

    // ------------------------------------------------------------------------
    /// This method will do the update on the boundary conditions
    // ------------------------------------------------------------------------

    void updateBC(const U& u)
    {
      fluxError = 0;  // reset the fluxError for next iteration step
      
      /// Construct a discrete grid function space for access to solution
      typedef Dune::PDELab::DiscreteGridFunction<GFS,U> DGF;
      DGF udgf(gfs,u);
      typedef typename GV::template Codim<0>::template Partition
              <Dune::Interior_Partition>::Iterator LeafIterator;

      /// Store the new calculated values
      bContainerType fluxes(ipbsPositions.size(),0.);
      bEfield.assign(ipbsPositions.size(),0.);

      // Loop over all elements and calculate the volume integral contribution
      for (LeafIterator it = gv.template begin<0,Dune::Interior_Partition>();
               	it!=gv.template end<0,Dune::Interior_Partition>(); ++it)
      {
        typedef typename DGF::Traits::RangeType RT;	// store potential during integration 
                       						        // (for calculating sinh-term)
        // Evaluate the potential at the elements center
   	    RT value;
        udgf.evaluate(*it,it->geometry().local(it->geometry().center()),value);

         // Get the position vector of the this element's center
        Dune::FieldVector<ctype,dim> r_prime = it->geometry().center();

        // For each element on this processor calculate the contribution to volume integral part of the flux
        for (size_t i = 0; i < ipbsPositions.size(); i++)
        {
          Dune::FieldVector<ctype,dim> r (ipbsPositions[i]);
          Dune::FieldVector<ctype,dim> dist = r - r_prime;
          Dune::FieldVector<ctype, dim> unitNormal(ipbsNormals[i]);
          unitNormal *= -1.;
          double volumeElem_flux = 0.;

          // integration depends on symmetry
          switch( sysParams.get_symmetry() )
          {
            case 0: // cartesian coordinates 
            {
              Dune::FieldVector<ctype,dim> E_field(0.);
              E_field = dist;
#if     GRIDDIM == 2
                E_field /= dist.two_norm() * dist.two_norm();
#elif   GRIDDIM == 3
                E_field /= dist.two_norm() * dist.two_norm() * dist.two_norm();
#else
                DUNE_THROW(Dune::NotImplemented, "IPBS only knows about dim=2 and dim=3");
#endif                  
                volumeElem_flux = E_field * unitNormal;
                volumeElem_flux *= sysParams.get_lambda2i() / (4.0 * sysParams.pi) * it->geometry().volume();
              }
              break;

              case 1:   // mirror particle charge on y-axis (sph. red. sym)
              {
                double a = dist[0]*dist[0] + r[1]*r[1] + r_prime[1]*r_prime[1] + 2.0 * r[1] * r_prime[1];
                double b = 4.0 * r[1] * r_prime[1];
                double k = sqrt(b/a);
                double E = gsl_sf_ellint_Ecomp (k, GSL_PREC_DOUBLE);
                double K = gsl_sf_ellint_Kcomp (k, GSL_PREC_DOUBLE);
                Dune::FieldVector<ctype,dim> E_field(0.);
                E_field[0] = -2.0 * dist[0] / ((a-b)*sqrt(a)) * E;
                E_field[1] = 2.0 * ( 2.0 * r_prime[1] * a - b * r_prime[1] - b * r[1])
                               / ((a-b)*sqrt(a)*b) * E
                             + 2.0 * (-2.0 * r_prime[1] * a + 2.0 * b * r_prime[1]) /  ((a-b)*sqrt(a)*b) * K;
                // now mirror
                Dune::FieldVector<ctype,dim> tmpr_prime(r_prime);
                tmpr_prime[0] *= -1.;
                Dune::FieldVector<ctype,dim> tmpdist = r - tmpr_prime;
                a = tmpdist[0]*tmpdist[0] + r[1]*r[1] + tmpr_prime[1]*tmpr_prime[1] + 2.0 * r[1] * tmpr_prime[1];
                b = 4.0 * r[1] * tmpr_prime[1];
                k = sqrt(b/a);
                E = gsl_sf_ellint_Ecomp (k, GSL_PREC_DOUBLE);
                K = gsl_sf_ellint_Kcomp (k, GSL_PREC_DOUBLE);
                E_field[0] += -2.0 * tmpdist[0] / ((a-b)*sqrt(a)) * E;
                E_field[1] += 2.0 * ( 2.0 * tmpr_prime[1] * a - b * tmpr_prime[1] - b * r[1])
                               / ((a-b)*sqrt(a)*b) * E
                             + 2.0 * (-2.0 * tmpr_prime[1] * a + 2.0 * b * tmpr_prime[1]) /  ((a-b)*sqrt(a)*b) * K;
                volumeElem_flux = E_field * unitNormal;
                volumeElem_flux *= -2.0 / (4.0*sysParams.pi) * sysParams.get_lambda2i() * it->geometry().volume() * r_prime[1];
              }
              break;
          
              case 2:   // spherical reduced symmetry
              {
                double a = dist[0]*dist[0] + r[1]*r[1] + r_prime[1]*r_prime[1] + 2.0 * r[1] * r_prime[1];
                double b = 4.0 * r[1] * r_prime[1];
                double k = sqrt(b/a);
                double E = gsl_sf_ellint_Ecomp (k, GSL_PREC_DOUBLE);
                double K = gsl_sf_ellint_Kcomp (k, GSL_PREC_DOUBLE);
                Dune::FieldVector<ctype,dim> E_field(0.);
                E_field[0] = -2.0 * dist[0] / ((a-b)*sqrt(a)) * E;
                E_field[1] = 2.0 * ( 2.0 * r_prime[1] * a - b * r_prime[1] - b * r[1])
                               / ((a-b)*sqrt(a)*b) * E
                             + 2.0 * (-2.0 * r_prime[1] * a + 2.0 * b * r_prime[1]) /  ((a-b)*sqrt(a)*b) * K;
                volumeElem_flux = E_field * unitNormal;
                volumeElem_flux *= -2.0 / (4.0*sysParams.pi) * sysParams.get_lambda2i() * it->geometry().volume() * r_prime[1];
              }
              break;

           }

            // Now get the counterion distribution and calculate flux
            switch (sysParams.get_salt())
            {
              case 0:
                volumeElem_flux *= std::sinh(value);
                break;
              case 1:
                volumeElem_flux *= std::exp(value); // Counterions have opposite sign!
                break;
            }

            /// Integrate
            fluxes[i] += volumeElem_flux;
            bEfield[i] += volumeElem_flux;
          }
        }

        unsigned int target = my_offset + my_len;
        // For each element on this processor calculate the contribution to surface integral part of the flux
        for (unsigned int i = my_offset; i < target; i++)
        {
          Dune::FieldVector<ctype,dim> r (ipbsPositions[i]);
          Dune::FieldVector<ctype, dim> unitNormal(ipbsNormals[i]);
          unitNormal *= -1.;
          double surfaceElem_flux = 0.;
          for (size_t j = 0; j < ipbsPositions.size(); j++)
          {
            if (i!=j)
            { 
              double lcd = boundary[ipbsType[j]]->get_charge_density() 
                            + inducedChargeDensity[j]; /**< The local charge density 
                                                         on this particular surface element */
              Dune::FieldVector<ctype,dim> r_prime (ipbsPositions[j]);
              Dune::FieldVector<ctype,dim> dist = r - r_prime;

              // Integration depends on symmetry!
              switch ( sysParams.get_symmetry() )
              {
                case 0:     // cartesian coordinates 
                {
                  Dune::FieldVector<ctype,dim> E_field(0.);
                  E_field = dist;
#if     GRIDDIM == 2
                  E_field /= dist.two_norm() * dist.two_norm();
#elif   GRIDDIM == 3
                  E_field /= dist.two_norm() * dist.two_norm() * dist.two_norm();
#else
                DUNE_THROW(Dune::NotImplemented, "IPBS only knows about dim=2 and dim=3");
#endif                  
                  surfaceElem_flux = E_field * unitNormal;
                  surfaceElem_flux *= -2.0 * sysParams.get_bjerrum() * ipbsVolumes[j] * lcd;
                }
                break;
                case 1:  // mirror particle charge on y-axis (sph. red. sym)
                {
                  double a = dist[0]*dist[0] + r[1]*r[1] + r_prime[1]*r_prime[1] + 2.0 * r[1] * r_prime[1];
                  double b = 4.0 * r[1] * r_prime[1];
                  double k = sqrt(b/a);
                  double E = gsl_sf_ellint_Ecomp (k, GSL_PREC_DOUBLE);
                  double K = gsl_sf_ellint_Kcomp (k, GSL_PREC_DOUBLE);
                  Dune::FieldVector<ctype,dim> E_field(0.);
                  E_field[0] = -2.0 * dist[0] / ((a-b)*sqrt(a)) * E;
                  E_field[1] = 2.0 * ( 2.0 * r_prime[1] * a - b * r_prime[1] - b * r[1])
                                          / ((a-b)*sqrt(a)*b) * E
                          + 2.0 * (-2.0 * r_prime[1] * a + 2.0 * b * r_prime[1]) /  ((a-b)*sqrt(a)*b) * K;
                  // now mirror
                  Dune::FieldVector<ctype,dim> tmpr_prime(r_prime);
                  tmpr_prime[0] *= -1.;
                  Dune::FieldVector<ctype,dim> tmpdist = r - tmpr_prime;
                  a = tmpdist[0]*tmpdist[0] + r[1]*r[1] + tmpr_prime[1]*tmpr_prime[1] + 2.0 * r[1] * tmpr_prime[1];
                  b = 4.0 * r[1] * tmpr_prime[1];
                  k = sqrt(b/a);
                  E = gsl_sf_ellint_Ecomp (k, GSL_PREC_DOUBLE);
                  K = gsl_sf_ellint_Kcomp (k, GSL_PREC_DOUBLE);
                  E_field[0] += -2.0 * tmpdist[0] / ((a-b)*sqrt(a)) * E;
                  E_field[1] += 2.0 * ( 2.0 * tmpr_prime[1] * a - b * tmpr_prime[1] - b * r[1])
                                          / ((a-b)*sqrt(a)*b) * E
                          + 2.0 * (-2.0 * tmpr_prime[1] * a + 2.0 * b * tmpr_prime[1]) /  ((a-b)*sqrt(a)*b) * K;
                  surfaceElem_flux = E_field * unitNormal;
                  surfaceElem_flux *= 2.0 * sysParams.get_bjerrum() * ipbsVolumes[j] * r_prime[1] * lcd;
                }
                break;
                case 2:     // spherical reduced symmetry
                {
                  double a = dist[0]*dist[0] + r[1]*r[1] + r_prime[1]*r_prime[1] + 2.0 * r[1] * r_prime[1];
                  double b = 4.0 * r[1] * r_prime[1];
                  double k = sqrt(b/a);
                  double E = gsl_sf_ellint_Ecomp (k, GSL_PREC_DOUBLE);
                  double K = gsl_sf_ellint_Kcomp (k, GSL_PREC_DOUBLE);
                  Dune::FieldVector<ctype,dim> E_field(0.);
                  E_field[0] = -2.0 * dist[0] / ((a-b)*sqrt(a)) * E;
                  E_field[1] = 2.0 * ( 2.0 * r_prime[1] * a - b * r_prime[1] - b * r[1])
                                   / ((a-b)*sqrt(a)*b) * E
                              + 2.0 * (-2.0 * r_prime[1] * a + 2.0 * b * r_prime[1]) /  ((a-b)*sqrt(a)*b) * K;
                  surfaceElem_flux = E_field * unitNormal;
                  surfaceElem_flux *= 2.0 * sysParams.get_bjerrum() * ipbsVolumes[j] * r_prime[1] * lcd;
                }
                break;
             }
              bEfield[i] += surfaceElem_flux;
            }
            else {
              surfaceElem_flux = -2. * sysParams.get_bjerrum() * sysParams.pi 
                                * ( boundary[ipbsType[i]]->get_charge_density() + inducedChargeDensity[i] );
              // in case of mirroring also include mirrored element
              if (sysParams.get_symmetry() == 1)
              {
                double lcd = boundary[ipbsType[i]]->get_charge_density() 
                              + inducedChargeDensity[i]; /**< The local charge density 
                                                           on this particular surface element */
                Dune::FieldVector<ctype,dim> r_prime (ipbsPositions[i]);
                r_prime[0] *= -1.;
                Dune::FieldVector<ctype,dim> dist = r - r_prime;
                Dune::FieldVector<ctype,dim> E_field(0.);
                double a = dist[0]*dist[0] + r[1]*r[1] + r_prime[1]*r_prime[1] + 2.0 * r[1] * r_prime[1];
                double b = 4.0 * r[1] * r_prime[1];
                double k = sqrt(b/a);
                double E = gsl_sf_ellint_Ecomp (k, GSL_PREC_DOUBLE);
                double K = gsl_sf_ellint_Kcomp (k, GSL_PREC_DOUBLE);
                E_field[0] = -2.0 * dist[0] / ((a-b)*sqrt(a)) * E;
                E_field[1] = 2.0 * ( 2.0 * r_prime[1] * a - b * r_prime[1] - b * r[1])
                                  / ((a-b)*sqrt(a)*b) * E
                  + 2.0 * (-2.0 * r_prime[1] * a + 2.0 * b * r_prime[1]) /  ((a-b)*sqrt(a)*b) * K;
                double tmp = E_field * unitNormal;
                tmp *= 2.0 * sysParams.get_bjerrum() * ipbsVolumes[i] * r_prime[1] * lcd;
                surfaceElem_flux += tmp;
              }
            }
            fluxes[i] += surfaceElem_flux;
          }
       }

        // Collect results from all nodes
        communicator.barrier();
        communicator.sum(&fluxes[0], fluxes.size());
        communicator.sum(&bEfield[0], fluxes.size());

        // Do the SOR 
        for (unsigned int i = my_offset; i < target; i++) {
          bContainer[i] = sysParams.get_alpha_ipbs() * fluxes[i]
                            + ( 1 - sysParams.get_alpha_ipbs()) * bContainer[i];
          double local_fluxError = fabs(2.0*(fluxes[i]-bContainer[i])
                        /(fluxes[i]+bContainer[i]));
          fluxError = std::max(fluxError, local_fluxError);
        }
        communicator.barrier();
        communicator.max(&fluxError, 1);
      }

      // ------------------------------------------------------------------------
      /// Force computation
      // ------------------------------------------------------------------------
      void forces(const U& u)
      {
        // Here we once more loop over all elements on this node (need of the element information
        // for the gradient calculation) and integrate Maxwell stress tensor over the particles surface
        // (see Hsu06a, eq. 61)

        // Open output file for force on particles
        std::ofstream force_file, vector_force_file;

        vector_force_file.open (filename_helper("vector_forces").c_str(), std::ios::out);
        if (communicator.rank() == 0) {
          force_file.open ("forces.dat", std::ios::out);
        }
        typedef typename GV::template Codim<0>::template Partition
                <Dune::Interior_Partition>::Iterator LeafIterator;
        typedef typename GV::IntersectionIterator IntersectionIterator;

        // Do the loop for boundary type 2 (iterated b.c.)
        // TODO: implement that!
        for (int i = 0; i < sysParams.get_npart(); i++)
        {
          std::cout << std::endl << "Calculating the force acting on particle id " << i << std::endl;
          Dune::FieldVector<Real, dim> F(0);

          for (LeafIterator it = gv.template begin<0,Dune::Interior_Partition>();
                   	it!=gv.template end<0,Dune::Interior_Partition>(); ++it)
          {
            if(it->hasBoundaryIntersections() == true) {
              for (IntersectionIterator ii = gv.ibegin(*it); ii != gv.iend(*it); ++ii) {
                if(ii->boundary() == true) {
                  if (boundaryIndexToEntity[ii->boundarySegmentIndex()] == i) // check if IPBS boundary
                  {
                    Dune::FieldVector<Real, dim> normal = ii->centerUnitOuterNormal();
                    Dune::FieldVector<Real, dim> forcevec;
                    normal *= -1.0 * ii->geometry().volume(); // Surface normal
                    Dune::FieldVector<Real, dim> evalPos = ii->geometry().center();
                    Dune::FieldMatrix<Real, dim, dim> sigma = maxwelltensor(gfs, it, evalPos, u);
                    //sigma.umv(normal, F);
                    sigma.mv(normal, forcevec);
                    if (sysParams.get_symmetry() > 0) {
                      // integration in theta
                      forcevec *= 2.*sysParams.pi*evalPos[1]; 
                    }
                    F += forcevec;
                    vector_force_file << evalPos << " " << forcevec << std::endl;
                  }
                }
              }
            }
          }
          // Sum up force of all nodes
          communicator.barrier();
          communicator.sum(&F[0], F.dim());
          if (communicator.rank() == 0)
            force_file << i << " " << F << std::endl;        
        }
    
      // close output
      if (communicator.rank() == 0) {
        force_file.close();
        vector_force_file.close();
        }
      }

  
  // ------------------------------------------------------------------------

  private:
    void initial_guess()
    {
      /** \brief Get an inital guess for the iterative boundaries */

      unsigned int target = my_offset + my_len;
      // For each element on this processor calculate the contribution to surface integral part of the flux
      for (unsigned int i = my_offset; i < target; i++)
      {
        // initialize with constant surface charge density
        bContainer[i] = -4. * sysParams.get_bjerrum() * sysParams.pi * boundary[ipbsType[i]]->get_charge_density();
      }
    }
 
    // ------------------------------------------------------------------------
    /// This method executes everything related to initialization
    // ------------------------------------------------------------------------

    void init()
    {
       /** \brief Traverse the grid and determine which boundaries shall be iterated
        *
        *   Assign a local list of iterative boundary elements and communicate this to all processors.
        */
      typedef typename GV::template Codim<0>::template Partition
              <Dune::Interior_Partition>::Iterator LeafIterator;
      typedef typename GV::IntersectionIterator IntersectionIterator;
    
      int counter = 0;
      int elemcounter = 0;
      // loop over elements on this processor and get information on the iterated boundaries
      for (LeafIterator it = gv.template begin<0,Dune::Interior_Partition>();
               	it!=gv.template end<0,Dune::Interior_Partition>(); ++it)
      {
        elemcounter++;
        if (it->hasBoundaryIntersections() == true)
          for (IntersectionIterator ii = it->ileafbegin(); ii!=it->ileafend(); ++ii)
            if(ii->boundary()==true && 
                boundary[ boundaryIndexToEntity[ii->boundarySegmentIndex()] ]->get_type() == 2)
                // check if IPBS boundary
            {
              ipbsPositions.push_back(ii->geometry().center());
              ipbsNormals.push_back(ii->centerUnitOuterNormal());
              indexLookupMap.insert(std::pair<int, int>(boundaryElemMapper.map(*it),counter));
              ipbsType.push_back(boundaryIndexToEntity[ii->boundarySegmentIndex()]);
              ipbsVolumes.push_back(ii->geometry().volume());
              counter++;
            }
      }
    }

    int communicateIpbsData() /** The containers now contain information on the iterative boundary elements on each processor.
                                 Collect the information into one container that is distributed on all processors.*/
    {
#if HAVE_MPI
      std::vector<unsigned int> length_on_processor;
      length_on_processor.resize(communicator.size(),0);
      // Collect on rank 0  - we seem to have to use the standard MPI calls, this is why we NEED to check isFake() before
      MPI_Status status;
      if (communicator.rank() == 0)
      {
        length_on_processor[0] = ipbsVolumes.size();
        for (int i = 1; i < communicator.size(); i++)
        {
          int temp;
          MPI_Recv(&temp,1,MPI_INT,i,i,MPI_COMM_WORLD,&status);
          length_on_processor[i]=temp;
        }
      }
      else
      {
        int temp = ipbsVolumes.size();
        MPI_Send(&temp,1,MPI_INT,0,communicator.rank(),MPI_COMM_WORLD); // Ensures we recover the correct order
      }
      int countBoundElems = 0;
      if (communicator.rank() == 0)
      {
         for (std::vector<unsigned int>::iterator i=length_on_processor.begin(); i != length_on_processor.end(); ++i)
         {
           // std::cout << std::endl << *i;
           countBoundElems += *i;
         }
         std::cout << "Detected " << countBoundElems << " boundary elements." << std::endl;
      }
      communicator.broadcast(&countBoundElems, 1, 0);
      communicator.broadcast(&length_on_processor[0], communicator.size(), 0);
      // Calculate the offset and length of data for each property-vector on each node
      for (int i = 0; i < communicator.rank(); i++) {
        my_offset += length_on_processor[i];
      }
      my_len = length_on_processor[communicator.rank()]; 
      // Now we send/receive the boundary data
      ipbsVolumes.resize(countBoundElems,0.);
      ipbsType.resize(countBoundElems,0.);
      ipbsNormals.resize(countBoundElems,Dune::FieldVector<ctype,dim>(0.));
      ipbsPositions.resize(countBoundElems,Dune::FieldVector<ctype,dim>(0.));
  
      // for communicating position-vectors we temporally store into arrays
      int indexcounter; // Count how many position-vectors we already got
      double* all_positions = (double*) malloc(dim*countBoundElems*sizeof(double)); // allocate on all processors
      double* all_normals = (double*) malloc(dim*countBoundElems*sizeof(double)); // allocate on all processors
      if( communicator.rank() !=0) // other nodes send their positions to master node
      {
        double* my_positions = (double*) malloc(dim*length_on_processor[communicator.rank()]*sizeof(double));
        double* my_normals = (double*) malloc(dim*length_on_processor[communicator.rank()]*sizeof(double));
        for (unsigned int i = 0; i<length_on_processor[communicator.rank()]; i++){
          for (int j = 0; j<dim; j++)
          {
            my_positions[i*dim+j] = ipbsPositions.at(i).vec_access(j);
            my_normals[i*dim+j] = ipbsNormals.at(i).vec_access(j);
          }
        }
        MPI_Send(my_positions,dim*length_on_processor[communicator.rank()],MPI_DOUBLE,0,0,MPI_COMM_WORLD); // pos sent on slot 0
        MPI_Send(my_normals,dim*length_on_processor[communicator.rank()],MPI_DOUBLE,0,1,MPI_COMM_WORLD); // normals sent on slot 1
      }
      else
      {   // Write positions of master node
        for (unsigned int i = 0; i<length_on_processor[communicator.rank()]; i++){
          for (int j = 0; j<dim; j++)
          {
            all_positions[i*dim+j] = ipbsPositions[i][j];
            all_normals[i*dim+j] = ipbsNormals[i][j];
          }
        }
        // get the right offset
        indexcounter = dim*length_on_processor[communicator.rank()];
        // get positions from other nodes
        for (int i = 1; i < communicator.size(); i++) {
          MPI_Recv(&all_positions[indexcounter],dim*length_on_processor[i],MPI_DOUBLE,i,0,MPI_COMM_WORLD,&status);
          MPI_Recv(&all_normals[indexcounter],dim*length_on_processor[i],MPI_DOUBLE,i,1,MPI_COMM_WORLD,&status);
          indexcounter += dim*length_on_processor[i];
        }
      }
      communicator.broadcast(all_positions,countBoundElems*dim,0); // communicate array
      communicator.broadcast(all_normals,countBoundElems*dim,0); // communicate array
      for(int i = 0; i < countBoundElems; i++) {
        for(int j=0; j<dim; j++)
        {
          ipbsNormals[i][j] = all_normals[i*dim+j];
          ipbsPositions[i][j] = all_positions[i*dim+j];
        }
      }

      // Commicate everything else
      /// determine the offset in the boundary proerty vectors on each node
      int offset = 0;
      for (int i = 1; i < communicator.rank(); i++) {
        offset += length_on_processor[i];
      }
      
      if( communicator.rank() !=0) // other nodes send their positions to master node
      {
        MPI_Send(&ipbsType[0],length_on_processor[communicator.rank()],MPI_INT,0,1,MPI_COMM_WORLD);
        MPI_Send(&ipbsVolumes[0],length_on_processor[communicator.rank()],MPI_DOUBLE,0,2,MPI_COMM_WORLD);
      }
      else
      {
        int tmpcounter = length_on_processor[0];
        // get positions from other nodes
        for (int i = 1; i < communicator.size(); i++) {
          MPI_Recv(&ipbsType[tmpcounter],length_on_processor[i],MPI_INT,i,1,MPI_COMM_WORLD,&status);
          MPI_Recv(&ipbsVolumes[tmpcounter],length_on_processor[i],MPI_DOUBLE,i,2,MPI_COMM_WORLD,&status);
          tmpcounter += length_on_processor[i];
        }
      }

      //if (communicator.rank() == 0)
      //{  for(unsigned int i=0;i<ipbsType.size();i++)
      //    std::cout << ipbsType[i] << " " << ipbsVolumes[i] << " " << ipbsPositions[i] << std::endl;
      //}
      communicator.broadcast(&ipbsType[0], countBoundElems, 0);
      communicator.broadcast(&ipbsVolumes[0], countBoundElems, 0);
      communicator.barrier();
      std::cout << "Myrank: " << communicator.rank() << " Mylen: " << my_len << std::endl;
      return 0;
#else
      my_len = ipbsType.size();
      my_offset = 0;
      return -1;
#endif
    }
 
    // ------------------------------------------------------------------------
    /// Filenamehelper for parallel output
    // ------------------------------------------------------------------------

    std::string filename_helper(const std::string &name)
    {
      // generate filename for process data
      std::ostringstream pieceName;
      if( communicator.size() > 1 )
      {
        pieceName << "s" << std::setfill( '0' ) << std::setw( 4 ) << communicator.size() << ":";
        pieceName << "p" << std::setfill( '0' ) << std::setw( 4 ) << communicator.rank() << ":";
      }
      pieceName << name << ".dat";
      return pieceName.str();
    }


    const GV& gv;
    /// The grid function space
    const GFS& gfs;
    Dune::MPIHelper& helper;
    const std::vector<int>& boundaryIndexToEntity;
    // provide a mapper for getting indices of iterated boundary elements
    typedef Dune::SingleCodimSingleGeomTypeMapper<GV, 0> BoundaryElemMapper;
    BoundaryElemMapper boundaryElemMapper;
    /// The communicator decides weither to use MPI or fake
    Dune::CollectiveCommunication<Dune::MPIHelper::MPICommunicator> communicator;

    /// Store the center of boundary intersections of iterative type
    std::vector<Dune::FieldVector<ctype,dim> > ipbsPositions;
    /// Store the normal vector at the center of boundary intersections of iterative type
    std::vector<Dune::FieldVector<ctype,dim> > ipbsNormals;
    /// Store the volume of boundary intersections of iterative type
    std::vector<Real> ipbsVolumes;
    /// Provide a vector storing the type of the iterative boundary @todo Maybe it's more reasonable to store its surface charge density
    std::vector<int> ipbsType;
    /// For fast access to the precomputed fluxes we use binary tree  - only local index is needed :-)
    typedef std::map<int, int> IndexLookupMap;
    IndexLookupMap indexLookupMap;
    typedef std::vector<double> bContainerType;
    bContainerType bContainer;  // Store the electric field on the surface elements caused by explicit charges in the system
    bContainerType inducedChargeDensity;  // Store the induced charge density
    bContainerType bEfield;   /**< Store the electric field on boundary elements, 
                                 which is calculated during updateBC(),
                                 \f[ \vec{E}(\vec{r}) \propto \int_V \sinh(\Phi(\vec{r}))
                                 \textnormal{d}\vec{r} \f] */
    /// Offset and length of data stream on each node
    unsigned int my_offset, my_len;
    unsigned int iterationCounter;
    double fluxError,  icError;
};

#endif  /* _IPBSOLVER_HH */
