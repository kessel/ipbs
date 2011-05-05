/** \brief Precompute the IPBS boundary flux values.

    Here goes some explanation on what is done :-)
    \todo { Doc Me ! }   
*/

#include <gsl/gsl_sf_ellint.h>

template <class GV, class DGF, typename IndexLookupMap, typename BoundaryElemMapper>
void ipbs_boundary(const GV& gv, const DGF& udgf,
      const double positions[], const double normals[],
			double fluxContainer[], const int &countBoundElems,
      const std::vector<int>& boundaryIndexToEntity,
      const IndexLookupMap& indexLookupMap, const BoundaryElemMapper& boundaryElemMapper)
{
  // some typedef
  const int dim = GV::dimension;
  typedef typename GV::Grid::ctype ctype;
  typedef typename GV::template Codim<0>::template Partition
          <Dune::Interior_Partition>::Iterator LeafIterator;
  typedef typename DGF::Traits::RangeType RT;	// store potential during integration 
                                  						// (for calculating sinh-term)

  
  // Initialize functor for integrating coulomb flux
  CoulombFlux<ctype,dim> f;
  
  // Precompute fluxes
  // for(int i = 0; i < countBoundElems; i++)
  for(int i = countBoundElems - 1; i >= 0; i--)
  {
    // Get the unit normal vector of the surface element
    Dune::FieldVector<ctype,dim> unitNormal;
    Dune::FieldVector<ctype,dim> r;  // vector of iterative surface boundary center
    for(int j=0; j<dim; j++)
    {
        unitNormal[j] = normals[i*dim+j];
        r.vec_access(j) = positions[i*dim+j];
    }
    unitNormal*=-1.0;	// turn around unit vector as it is outer normal
   
    double fluxIntegrated = 0.0;
    double fluxCoulomb = 0.0;
	   
    for (LeafIterator it = gv.template begin<0,Dune::Interior_Partition>();
            	it!=gv.template end<0,Dune::Interior_Partition>(); ++it)
    {
      // Now we go over all elements on this processor
      // and sum up their flux contributions
	  
      // Get the position vector of the this element's center
      Dune::FieldVector<ctype,dim> r_prime = it->geometry().center();
      Dune::FieldVector<ctype,dim> dist = r - r_prime;
	  
      // integrate sinh over all elements but all the surface ones, where
      // the densiy of counterions is zero by definition
      bool isIPBS_Elem = false;
      double dA = 0.0;
      double a, b, k;  // parameters for elliptic
      Dune::FieldVector<ctype,dim> dist2;
      Dune::FieldVector<ctype,dim> r_prime2;
      typedef typename GV::IntersectionIterator IntersectionIterator;
      // check that we are not on an IPBS surface element
      if(it->hasBoundaryIntersections() == true)
      for (IntersectionIterator ii = gv.ibegin(*it); ii != gv.iend(*it); ++ii)
        if(ii->boundary() == true && boundaryIndexToEntity[ii->boundarySegmentIndex()] == 2)
        {
          isIPBS_Elem = true;
          dA = ii->geometry().volume();   // intersection area
          r_prime2 = ii->geometry().center();   // center of the intersection
          dist2 = r - r_prime2;   // correct distance vector
        }

      // ================================================================== 
      // Integral over all elements but the surface ones
      // ================================================================== 
      
      bool isItself = (indexLookupMap.find(boundaryElemMapper.map(*it))->second == i);
      if (isIPBS_Elem == false && isItself == false)
      {
        // Evaluate the potential at the elements center
	      RT value;
        udgf.evaluate(*it,it->geometry().center(),value);

        // integration depends on symmetry
        switch( sysParams.get_symmetry() )
        {
          case 1: // "2D_cylinder"
            {
              //fluxIntegrated += std::sinh(value) / (dist.two_norm() * dist.two_norm())
              //  * it->geometry().volume() * (dist * unitNormal)
              //  / sysParams.get_bjerrum()*sysParams.get_lambda2i()*1.0/4.0/sysParams.pi;
            }
            break;
          case 2:
	          {
              //a = (dist[0]*dist[0] + r[1]*r[1] + r_prime[1]*r_prime[1]);
	            //b = 2 * r[1] * r_prime[1];
	            //fluxIntegrated += sysParams.get_lambda2i()/(4.0*sysParams.pi)
              //                * eval_elliptic(a,b)
              //                * std::sinh(value)*it->geometry().volume()
              //          / 2.53705882; // MAGIC
	            //std::cout << "fluxIntegrated: " << fluxIntegrated << std::endl; 
	          }
	          break;

        }

      }

      // ================================================================== 
      // Integral over surface elements
      // ================================================================== 
      else if (isIPBS_Elem == true && isItself == false)
      {
        // we are on a surface element and do integration for coulomb flux
	      // add surface charge contribution from all other surface elements but this one
	      // (using standard coulomb field formula)
        // NOTE: For algorithm validation we use the pillowbox conribution

        // TODO either make sure you only use the pillow box or use correct
        // interation (1/dist-term and volume)

        switch ( sysParams.get_symmetry() )
          {
            case 1:	// "2D_cylinder"
              {
                // Pillow box contribution - for testing ...
                fluxCoulomb = 1.0 * sysParams.get_charge_density()
		             * sysParams.get_bjerrum() * 2.0 * sysParams.pi;
                
                // Integrated Coulomb flux - TODO: this is not working properly!
                // fluxCoulomb += 1.0 * sysParams.get_bjerrum()*sysParams.get_charge_density() 
                //   * (dist2*unitNormal) / (dist2.two_norm() * dist2.two_norm())
                //   *  dA;              
              }
              break;

            case 2: // "2D_sphere"
              {
                // Pillow Box contribution - for testing ...
                // fluxCoulomb = 1.0 * sysParams.get_charge_density()  * sysParams.get_bjerrum();

                // Calculate the integrated Coulomb flux
                a = (dist2[0]*dist2[0] + r[1]*r[1] + r_prime2[1]*r_prime2[1]);
	              b = 2.0 * r[1] * r_prime2[1];
                k = sqrt(2.0*b/(a+b));

                //std::cout << "a: " << a << " b: " << b << " K: " << k << std::endl;
                
                // Contribution from int(1/(a-b*cos(\phi))^(3/2))
                fluxCoulomb += 4.0*sqrt((a-b)/(a+b))*gsl_sf_ellint_Ecomp (k, GSL_PREC_APPROX)
                                / sqrt((a-b)*(a-b)*(a-b))
                                *(-(dist2[0]*unitNormal[0]) + (r[1]*unitNormal[1]));
                // Contribution from int(cos(\phi)/(a-b*cos(\phi))^(3/2))
                fluxCoulomb+= 4.0*sqrt((a-b)/(a+b)) * 
                    (a * gsl_sf_ellint_Kcomp(k, GSL_PREC_APPROX)
                   - a * gsl_sf_ellint_Ecomp(k, GSL_PREC_APPROX)
                   - b * gsl_sf_ellint_Kcomp(k, GSL_PREC_APPROX))
                   / sqrt((a-b)*(a-b)*(a-b)) / b 
                   * r_prime2[1] * unitNormal[1]; 

                // Multiply with prefactors
                fluxCoulomb *= sysParams.get_bjerrum() * sysParams.get_charge_density() * dA;
                        //* sysParams.get_radius() // metric factor for surface elem
                        //* sqrt(1-(r_prime2[0]*r_prime2[0])*sysParams.get_r2i());
                
                //std::cout << "At r = " << r << " added contribution from " << it->geometry().center()
                //  <<" flux is now " << fluxCoulomb  << std::endl;               
              }
            break;
          }
      }
    }
      
    // ================================================================== 
    // Do SOR step
    // ================================================================== 
    double flux = fluxCoulomb + fluxIntegrated;
    std::cout << "At r = " << r << "\tfluxCoulomb: " << fluxCoulomb << "\tfluxIntegrated: " 
      << fluxIntegrated << "\tflux: " << flux << std::endl; 
    // Do SOR step and add error
    flux = sysParams.get_alpha() * flux + (1.0 - sysParams.get_alpha()) * fluxContainer[i];
    double error = fabs(2.0*(flux-fluxContainer[i])/(flux+fluxContainer[i]));
    sysParams.add_error(error);
    // Store new flux
    fluxContainer[i] = flux;
  }
}
