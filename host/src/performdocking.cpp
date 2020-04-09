/*

AutoDock-GPU, an OpenCL implementation of AutoDock 4.2 running a Lamarckian Genetic Algorithm
Copyright (C) 2017 TU Darmstadt, Embedded Systems and Applications Group, Germany. All rights reserved.
For some of the code, Copyright (C) 2019 Computational Structural Biology Center, the Scripps Research Institute.

AutoDock is a Trade Mark of the Scripps Research Institute.

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA

*/

#include <chrono>



#if defined (N1WI)
	#define KNWI " -DN1WI "
#elif defined (N2WI)
	#define KNWI " -DN2WI "
#elif defined (N4WI)
	#define KNWI " -DN4WI "
#elif defined (N8WI)
	#define KNWI " -DN8WI "
#elif defined (N16WI)
	#define KNWI " -DN16WI "
#elif defined (N32WI)
	#define KNWI " -DN32WI "
#elif defined (N64WI)
	#define KNWI " -DN64WI "
#elif defined (N128WI)
	#define KNWI " -DN128WI "
#elif defined (N256WI)
		#define KNWI " -DN256WI "
#else
	#define KNWI	" -DN64WI "
#endif

#if defined (REPRO)
	#define REP " -DREPRO "
#else
	#define REP " "
#endif


#ifdef __APPLE__
	#define KGDB_GPU	" -g -cl-opt-disable "
#else
	#define KGDB_GPU	" -g -O0 -Werror -cl-opt-disable "
#endif
#define KGDB_CPU	" -g3 -Werror -cl-opt-disable "
// Might work in some (Intel) devices " -g -s " KRNL_FILE

#if defined (DOCK_DEBUG)
	#if defined (CPU_DEVICE)
		#define KGDB KGDB_CPU
	#elif defined (GPU_DEVICE)
		#define KGDB KGDB_GPU
	#endif
#else
	#define KGDB " -cl-mad-enable"
#endif


#define OPT_PROG INC KNWI REP KGDB

#include "performdocking.h"
#include "correct_grad_axisangle.h"
#include "GpuData.h"


// CUDA kernels
void SetKernelsGpuData(GpuData* pData);
void GetKernelsGpuData(GpuData* pData);
void gpu_calc_initpop(uint32_t blocks, uint32_t threadsPerBlock, float* pConformations_current, float* pEnergies_current);
void gpu_sum_evals(uint32_t blocks, uint32_t threadsPerBlock);
void gpu_gen_and_eval_newpops(
    uint32_t blocks,
    uint32_t threadsPerBlock,
    float* pMem_conformations_current,
    float* pMem_energies_current,
    float* pMem_conformations_next,
    float* pMem_energies_next
);
void gpu_gradient_minAD(
    uint32_t blocks,
    uint32_t threads,
    float* pMem_conformations_next,
	float* pMem_energies_next
);

template <typename Clock, typename Duration1, typename Duration2>
double elapsed_seconds(std::chrono::time_point<Clock, Duration1> start,
                       std::chrono::time_point<Clock, Duration2> end)
{
  using FloatingPointSeconds = std::chrono::duration<double, std::ratio<1>>;
  return std::chrono::duration_cast<FloatingPointSeconds>(end - start).count();
}



inline float average(float* average_sd2_N)
{
	if(average_sd2_N[2]<1.0f)
		return 0.0;
	return average_sd2_N[0]/average_sd2_N[2];
}

inline float stddev(float* average_sd2_N)
{
	if(average_sd2_N[2]<1.0f)
		return 0.0;
	float sq = average_sd2_N[1]*average_sd2_N[2]-average_sd2_N[0]*average_sd2_N[0];
	if((fabs(sq)<=0.000001) || (sq<0.0)) return 0.0;
	return sqrt(sq)/average_sd2_N[2];
}

int docking_with_gpu(   const Gridinfo*  	mygrid,
                        float*      		cpu_floatgrids,
                        Dockpars*   		mypars,
                        const Liganddata*   myligand_init,
                        const Liganddata* 	myxrayligand,
                        const int*        	argc,
                        char**      		argv,
                        clock_t     		clock_start_program)
/* The function performs the docking algorithm and generates the corresponding result files.
parameter mygrid:
		describes the grid
		filled with get_gridinfo()
parameter cpu_floatgrids:
		points to the memory region containing the grids
		filled with get_gridvalues_f()
parameter mypars:
		describes the docking parameters
		filled with get_commandpars()
parameter myligand_init:
		describes the ligands
		filled with get_liganddata()
parameter myxrayligand:
		describes the xray ligand
		filled with get_xrayliganddata()
parameters argc and argv:
		are the corresponding command line arguments parameter clock_start_program:
		contains the state of the clock tick counter at the beginning of the program
filled with clock() */
{
    auto const t0 = std::chrono::steady_clock::now();

    // Initialize CUDA
    int device                                      = -1;
    int gpuCount                                    = 0;
    cudaError_t status;
    status = cudaGetDeviceCount(&gpuCount);
    RTERROR(status, "cudaGetDeviceCount failed");
    if (gpuCount == 0)
    {
        printf("No CUDA-capable devices found, exiting.\n");
        cudaDeviceReset();
        exit(-1);
    }
    else
    {
        // Select GPU with most memory available
        device = 1; // HACK to not use display GPU for now
    }
    status = cudaSetDevice(device);
    RTERROR(status, "cudaSetDevice failed");   
    cudaFree(NULL);   // Trick driver into creating context on current device
    status = cudaDeviceSetLimit(cudaLimitPrintfFifoSize, 200000000ull);
    RTERROR(status, "cudaDeviceSetLimit failed");      

    auto const t1 = std::chrono::steady_clock::now();
    printf("CUDA Setup time %fs\n", elapsed_seconds(t0 ,t1));


	Liganddata myligand_reference;

	float* cpu_init_populations;
	float* cpu_final_populations;
	float* cpu_energies;
	Ligandresult* cpu_result_ligands;
	unsigned int* cpu_prng_seeds;
	int* cpu_evals_of_runs;
	float* cpu_ref_ori_angles;

	size_t size_floatgrids;
	size_t size_populations;
	size_t size_energies;
	size_t size_prng_seeds;
	size_t size_evals_of_runs;

	int threadsPerBlock;
	int blocksPerGridForEachEntity;
	int blocksPerGridForEachRun;
	int blocksPerGridForEachLSEntity;
	int blocksPerGridForEachGradMinimizerEntity;

	unsigned long run_cnt;	/* int run_cnt; */
	int generation_cnt;
	int i;
	double progress;

	int curr_progress_cnt;
	int new_progress_cnt;

	clock_t clock_start_docking;
	clock_t	clock_stop_docking;
	clock_t clock_stop_program_before_clustering;

	//setting number of blocks and threads
	threadsPerBlock = NUM_OF_THREADS_PER_BLOCK;
	blocksPerGridForEachEntity = mypars->pop_size * mypars->num_of_runs;
	blocksPerGridForEachRun = mypars->num_of_runs;

	//allocating CPU memory for initial populations
	size_populations = mypars->num_of_runs * mypars->pop_size * GENOTYPE_LENGTH_IN_GLOBMEM*sizeof(float);
	cpu_init_populations = (float*) malloc(size_populations);
	memset(cpu_init_populations, 0, size_populations);

	//allocating CPU memory for results
	size_energies = mypars->pop_size * mypars->num_of_runs * sizeof(float);
	cpu_energies = (float*) malloc(size_energies);
	cpu_result_ligands = (Ligandresult*) malloc(sizeof(Ligandresult)*(mypars->num_of_runs));
	cpu_final_populations = cpu_init_populations;

	//allocating memory in CPU for reference orientation angles
	cpu_ref_ori_angles = (float*) malloc(mypars->num_of_runs*3*sizeof(float));

	//generating initial populations and random orientation angles of reference ligand
	//(ligand will be moved to origo and scaled as well)
	myligand_reference = *myligand_init;
	gen_initpop_and_reflig(mypars, cpu_init_populations, cpu_ref_ori_angles, &myligand_reference, mygrid);

	//allocating memory in CPU for pseudorandom number generator seeds and
	//generating them (seed for each thread during GA)
	size_prng_seeds = blocksPerGridForEachEntity * threadsPerBlock * sizeof(unsigned int);
	cpu_prng_seeds = (unsigned int*) malloc(size_prng_seeds);

	genseed(time(NULL));	//initializing seed generator

	for (i=0; i<blocksPerGridForEachEntity*threadsPerBlock; i++)
#if defined (REPRO)
		cpu_prng_seeds[i] = 1u;
#else
		cpu_prng_seeds[i] = genseed(0u);
#endif

	//allocating memory in CPU for evaluation counters
	size_evals_of_runs = mypars->num_of_runs*sizeof(int);
	cpu_evals_of_runs = (int*) malloc(size_evals_of_runs);
	memset(cpu_evals_of_runs, 0, size_evals_of_runs);

	//preparing the constant data fields for the GPU
	// ----------------------------------------------------------------------
	// The original function does CUDA calls initializing const Kernel data.
	// We create a struct to hold those constants
	// and return them <here> (<here> = where prepare_const_fields_for_gpu() is called),
	// so we can send them to Kernels from <here>, instead of from calcenergy.cpp as originally.
	// ----------------------------------------------------------------------
	// Constant struct

/*
	kernelconstant KerConst;

	if (prepare_const_fields_for_gpu(&myligand_reference, mypars, cpu_ref_ori_angles, &KerConst) == 1)
		return 1;
*/
    GpuData cData;
    kernelconstant_interintra 	    KerConst_interintra;
	kernelconstant_intracontrib 	KerConst_intracontrib;
	kernelconstant_intra 		    KerConst_intra;
	kernelconstant_rotlist 	        KerConst_rotlist;
    kernelconstant_conform 	        KerConst_conform;
	kernelconstant_grads            KerConst_grads;    
    
    
	if (prepare_const_fields_for_gpu(&myligand_reference, mypars, cpu_ref_ori_angles, 
					 &KerConst_interintra, 
					 &KerConst_intracontrib, 
					 &KerConst_intra, 
					 &KerConst_rotlist, 
					 &KerConst_conform,
					 &KerConst_grads) == 1) {
		return 1;
	}

	size_t sz_interintra_const	= MAX_NUM_OF_ATOMS*sizeof(float) + 
					  MAX_NUM_OF_ATOMS*sizeof(uint32_t);

	size_t sz_intracontrib_const	= 3*MAX_INTRAE_CONTRIBUTORS*sizeof(uint32_t);

	size_t sz_intra_const		= ATYPE_NUM*sizeof(float) + 
					  ATYPE_NUM*sizeof(float) + 
					  ATYPE_NUM*sizeof(unsigned int) + 
					  ATYPE_NUM*sizeof(unsigned int) + 
				          MAX_NUM_OF_ATYPES*MAX_NUM_OF_ATYPES*sizeof(float) + 
					  MAX_NUM_OF_ATYPES*MAX_NUM_OF_ATYPES*sizeof(float) + 
					  MAX_NUM_OF_ATYPES*sizeof(float) + 
					  MAX_NUM_OF_ATYPES*sizeof(float);

	size_t sz_rotlist_const		= MAX_NUM_OF_ROTATIONS*sizeof(int);

	size_t sz_conform_const		= 3*MAX_NUM_OF_ATOMS*sizeof(float) + 
					  3*MAX_NUM_OF_ROTBONDS*sizeof(float) + 
					  3*MAX_NUM_OF_ROTBONDS*sizeof(float) + 
					  4*MAX_NUM_OF_RUNS*sizeof(float);

    // Allocate kernel constant GPU memory
    status = cudaMalloc((void**)&cData.pKerconst_interintra, sz_interintra_const);
    RTERROR(status, "cData.pKerconst_interintra: failed to allocate GPU memory.\n");    
    status = cudaMalloc((void**)&cData.pKerconst_intracontrib, sz_intracontrib_const);
    RTERROR(status, "cData.pKerconst_intracontrib: failed to allocate GPU memory.\n");
    status = cudaMalloc((void**)&cData.pKerconst_intra, sz_intra_const);
    RTERROR(status, "cData.pKerconst_intra: failed to allocate GPU memory.\n");
    status = cudaMalloc((void**)&cData.pKerconst_rotlist, sz_rotlist_const);
    RTERROR(status, "cData.pKerconst_rotlist: failed to allocate GPU memory.\n");                      
    status = cudaMalloc((void**)&cData.pKerconst_conform, sz_conform_const);
    RTERROR(status, "cData.pKerconst_conform: failed to allocate GPU memory.\n");  
    
    // Upload kernel constant data
    status = cudaMemcpy(cData.pKerconst_interintra, &KerConst_interintra, sz_interintra_const, cudaMemcpyHostToDevice);
    RTERROR(status, "cData.pKerconst_interintra: failed to upload to GPU memory.\n");
    status = cudaMemcpy(cData.pKerconst_intracontrib, &KerConst_intracontrib, sz_intracontrib_const, cudaMemcpyHostToDevice);
    RTERROR(status, "cData.pKerconst_intracontrib: failed to upload to GPU memory.\n");
    status = cudaMemcpy(cData.pKerconst_intra, &KerConst_intra, sz_intra_const, cudaMemcpyHostToDevice);
    RTERROR(status, "cData.pKerconst_intra: failed to upload to GPU memory.\n");
    status = cudaMemcpy(cData.pKerconst_rotlist, &KerConst_rotlist, sz_rotlist_const, cudaMemcpyHostToDevice);
    RTERROR(status, "cData.pKerconst_rotlist: failed to upload to GPU memory.\n");
    status = cudaMemcpy(cData.pKerconst_conform, &KerConst_conform, sz_conform_const, cudaMemcpyHostToDevice);
    RTERROR(status, "cData.pKerconst_conform: failed to upload to GPU memory.\n");

    // Allocate mem data
    status = cudaMalloc((void**)&cData.pMem_angle_const, 1000 * sizeof(float));
    RTERROR(status, "cData.pMem_angle_const: failed to allocate GPU memory.\n");
    status = cudaMalloc((void**)&cData.pMem_dependence_on_theta_const, 1000 * sizeof(float));
    RTERROR(status, "cData.pMem_dependence_on_theta_const: failed to allocate GPU memory.\n");
    status = cudaMalloc((void**)&cData.pMem_dependence_on_rotangle_const, 1000 * sizeof(float));
    RTERROR(status, "cData.pMem_dependence_on_rotangle_const: failed to allocate GPU memory.\n");
    status = cudaMalloc((void**)&cData.pMem_rotbonds_const, 2*MAX_NUM_OF_ROTBONDS*sizeof(int));
    RTERROR(status, "cData.pMem_rotbonds_const: failed to allocate GPU memory.\n");
    status = cudaMalloc((void**)&cData.pMem_rotbonds_atoms_const, MAX_NUM_OF_ATOMS*MAX_NUM_OF_ROTBONDS*sizeof(int));
    RTERROR(status, "cData.pMem_rotbonds_atoms_const: failed to allocate GPU memory.\n");
    status = cudaMalloc((void**)&cData.pMem_num_rotating_atoms_per_rotbond_const, MAX_NUM_OF_ROTBONDS*sizeof(int));
    RTERROR(status, "cData.pMem_num_rotiating_atoms_per_rotbond_const: failed to allocate GPU memory.\n");

    // Upload mem data
    cudaMemcpy(cData.pMem_angle_const, angle, 1000 * sizeof(float), cudaMemcpyHostToDevice);
    RTERROR(status, "cData.pMem_angle_const: failed to upload to GPU memory.\n");
    cudaMemcpy(cData.pMem_dependence_on_theta_const, dependence_on_theta, 1000 * sizeof(float), cudaMemcpyHostToDevice);
    RTERROR(status, "cData.pMem_dependence_on_theta_const: failed to upload to GPU memory.\n");
    cudaMemcpy(cData.pMem_dependence_on_rotangle_const, dependence_on_rotangle, 1000 * sizeof(float), cudaMemcpyHostToDevice);
    RTERROR(status, "cData.pMem_dependence_on_rotangle_const: failed to upload to GPU memory.\n");       
    cudaMemcpy(cData.pMem_rotbonds_const, KerConst_grads.rotbonds, 2*MAX_NUM_OF_ROTBONDS*sizeof(int), cudaMemcpyHostToDevice);
    RTERROR(status, "cData.pMem_rotbonds_const: failed to upload to GPU memory.\n");
    cudaMemcpy(cData.pMem_rotbonds_atoms_const, KerConst_grads.rotbonds_atoms, MAX_NUM_OF_ATOMS*MAX_NUM_OF_ROTBONDS*sizeof(int), cudaMemcpyHostToDevice);
    RTERROR(status, "cData.pMem_rotbonds_atoms_const: failed to upload to GPU memory.\n");
    cudaMemcpy(cData.pMem_num_rotating_atoms_per_rotbond_const, KerConst_grads.num_rotating_atoms_per_rotbond, MAX_NUM_OF_ROTBONDS*sizeof(int), cudaMemcpyHostToDevice);
    RTERROR(status, "cData.pMem_num_rotating_atoms_per_rotbond_const failed to upload to GPU memory.\n"); 
    
/*    
    memcpy(cData.mem_angle_const, angle, 1000 * sizeof(float));
    memcpy(cData.mem_dependence_on_theta_const, dependence_on_theta, 1000 * sizeof(float));
    memcpy(cData.mem_dependence_on_rotangle_const, dependence_on_rotangle, 1000 * sizeof(float));        
    memcpy(cData.mem_rotbonds_const, KerConst_grads.rotbonds, 2*MAX_NUM_OF_ROTBONDS*sizeof(int));
    memcpy(cData.mem_rotbonds_atoms_const, KerConst_grads.rotbonds_atoms, MAX_NUM_OF_ATOMS*MAX_NUM_OF_ROTBONDS*sizeof(int));
    memcpy(cData.mem_num_rotating_atoms_per_rotbond_const, KerConst_grads.num_rotating_atoms_per_rotbond, MAX_NUM_OF_ROTBONDS*sizeof(int));
*/
 	//allocating GPU memory for populations, floatgirds,
	//energies, evaluation counters and random number generator states
	size_floatgrids = 4 * (sizeof(float)) * (mygrid->num_of_atypes+2) * (mygrid->size_xyz[0]) * (mygrid->size_xyz[1]) * (mygrid->size_xyz[2]);    
    float*      pMem_fgrids;
    float*      pMem_conformations1;
    float*      pMem_conformations2;
    float*      pMem_energies1;
    float*      pMem_energies2;
    int*        pMem_evals_of_new_entities;
    int*        pMem_gpu_evals_of_runs;
    uint32_t*   pMem_prng_states;
    
    // Allocate CUDA memory Buffers
    status = cudaMalloc((void**)&pMem_fgrids, size_floatgrids);
    RTERROR(status, "pMem_fgrids: failed to allocate GPU memory.\n");
    status = cudaMalloc((void**)&pMem_conformations1, size_populations);
    RTERROR(status, "pMem_conformations1: failed to allocate GPU memory.\n");   
    status = cudaMalloc((void**)&pMem_conformations2, size_populations);
    RTERROR(status, "pMem_conformations2: failed to allocate GPU memory.\n");     
    status = cudaMalloc((void**)&pMem_energies1, size_energies);
    RTERROR(status, "pMem_energies1: failed to allocate GPU memory.\n");    
    status = cudaMalloc((void**)&pMem_energies2, size_energies);
    RTERROR(status, "pMem_energies2: failed to allocate GPU memory.\n");  
    status = cudaMalloc((void**)&pMem_evals_of_new_entities, mypars->pop_size*mypars->num_of_runs*sizeof(int));
    RTERROR(status, "pMem_evals_of_new_Entities: failed to allocate GPU memory.\n");      
    status = cudaMallocManaged((void**)&pMem_gpu_evals_of_runs, size_evals_of_runs, cudaMemAttachGlobal);
    RTERROR(status, "pMem_gpu_evals_of_runs: failed to allocate GPU memory.\n");   
    status = cudaMalloc((void**)&pMem_prng_states, size_prng_seeds);
    RTERROR(status, "pMem_prng_states: failed to allocate GPU memory.\n");    
    
    // Flippable pointers
    float* pMem_conformations_current = pMem_conformations1;
    float* pMem_conformations_next = pMem_conformations2;
    float* pMem_energies_current = pMem_energies1;
    float* pMem_energies_next = pMem_energies2;
    
    // Set constant pointers
    cData.pMem_fgrids = pMem_fgrids;
    cData.pMem_evals_of_new_entities = pMem_evals_of_new_entities;
    cData.pMem_gpu_evals_of_runs = pMem_gpu_evals_of_runs;
    cData.pMem_prng_states = pMem_prng_states;
    
    // Set CUDA constants
    cData.warpmask = 31;
    cData.warpbits = 5;

    // Upload data
    status = cudaMemcpy(pMem_fgrids, cpu_floatgrids, size_floatgrids, cudaMemcpyHostToDevice);
    RTERROR(status, "pMem_fgrids: failed to upload to GPU memory.\n"); 
    status = cudaMemcpy(pMem_conformations_current, cpu_init_populations, size_populations, cudaMemcpyHostToDevice);
    RTERROR(status, "pMem_conformations_current: failed to upload to GPU memory.\n"); 
    status = cudaMemcpy(pMem_gpu_evals_of_runs, cpu_evals_of_runs, size_evals_of_runs, cudaMemcpyHostToDevice);
    RTERROR(status, "pMem_gpu_evals_of_runs: failed to upload to GPU memory.\n"); 
    status = cudaMemcpy(pMem_prng_states, cpu_prng_seeds, size_prng_seeds, cudaMemcpyHostToDevice);
    RTERROR(status, "pMem_prng_states: failed to upload to GPU memory.\n");

	//preparing parameter struct
	cData.dockpars.num_of_atoms                 = myligand_reference.num_of_atoms;
	cData.dockpars.num_of_atypes                = myligand_reference.num_of_atypes;
	cData.dockpars.num_of_intraE_contributors   = ((int) myligand_reference.num_of_intraE_contributors);
	cData.dockpars.gridsize_x                   = mygrid->size_xyz[0];
	cData.dockpars.gridsize_y                   = mygrid->size_xyz[1];
	cData.dockpars.gridsize_z                   = mygrid->size_xyz[2];
    cData.dockpars.gridsize_x_times_y           = cData.dockpars.gridsize_x * cData.dockpars.gridsize_y; 
    cData.dockpars.gridsize_x_times_y_times_z   = cData.dockpars.gridsize_x * cData.dockpars.gridsize_y * cData.dockpars.gridsize_z;     
	cData.dockpars.grid_spacing                 = ((float) mygrid->spacing);
	cData.dockpars.rotbondlist_length           = ((int) NUM_OF_THREADS_PER_BLOCK*(myligand_reference.num_of_rotcyc));
	cData.dockpars.coeff_elec                   = ((float) mypars->coeffs.scaled_AD4_coeff_elec);
	cData.dockpars.coeff_desolv                 = ((float) mypars->coeffs.AD4_coeff_desolv);
	cData.dockpars.pop_size                     = mypars->pop_size;
	cData.dockpars.num_of_genes                 = myligand_reference.num_of_rotbonds + 6;
	// Notice: dockpars.tournament_rate, dockpars.crossover_rate, dockpars.mutation_rate
	// were scaled down to [0,1] in host to reduce number of operations in device
	cData.dockpars.tournament_rate              = mypars->tournament_rate/100.0f; 
	cData.dockpars.crossover_rate               = mypars->crossover_rate/100.0f;
	cData.dockpars.mutation_rate                = mypars->mutation_rate/100.f;
	cData.dockpars.abs_max_dang                 = mypars->abs_max_dang;
	cData.dockpars.abs_max_dmov                 = mypars->abs_max_dmov;
	cData.dockpars.qasp 		                = mypars->qasp;
	cData.dockpars.smooth 	                    = mypars->smooth;
	cData.dockpars.lsearch_rate                 = mypars->lsearch_rate;

	if (cData.dockpars.lsearch_rate != 0.0f) 
	{
		cData.dockpars.num_of_lsentities        = (unsigned int) (mypars->lsearch_rate/100.0*mypars->pop_size + 0.5);
		cData.dockpars.rho_lower_bound          = mypars->rho_lower_bound;
		cData.dockpars.base_dmov_mul_sqrt3      = mypars->base_dmov_mul_sqrt3;
		cData.dockpars.base_dang_mul_sqrt3      = mypars->base_dang_mul_sqrt3;
		cData.dockpars.cons_limit               = (unsigned int) mypars->cons_limit;
		cData.dockpars.max_num_of_iters         = (unsigned int) mypars->max_num_of_iters;

		// The number of entities that undergo Solis-Wets minimization,
		blocksPerGridForEachLSEntity = cData.dockpars.num_of_lsentities * mypars->num_of_runs;

		// The number of entities that undergo any gradient-based minimization,
		// by default, it is the same as the number of entities that undergo the Solis-Wets minimizer
		blocksPerGridForEachGradMinimizerEntity = cData.dockpars.num_of_lsentities * mypars->num_of_runs;

		// Enable only for debugging.
		// Only one entity per reach run, undergoes gradient minimization
		//blocksPerGridForEachGradMinimizerEntity = mypars->num_of_runs;
	}
	
	printf("Local-search chosen method is: %s\n", (cData.dockpars.lsearch_rate == 0.0f)? "GA" :
						      (
						      (strcmp(mypars->ls_method, "sw")   == 0)?"Solis-Wets (sw)":
						      (strcmp(mypars->ls_method, "sd")   == 0)?"Steepest-Descent (sd)": 
						      (strcmp(mypars->ls_method, "fire") == 0)?"FIRE (fire)":
						      (strcmp(mypars->ls_method, "ad") == 0)?"ADADELTA (ad)": "Unknown")
						      );

	/*
	printf("dockpars.num_of_intraE_contributors:%u\n", dockpars.num_of_intraE_contributors);
	printf("dockpars.rotbondlist_length:%u\n", dockpars.rotbondlist_length);
	*/

	clock_start_docking = clock();

	//print progress bar
#ifndef DOCK_DEBUG
	if (mypars->autostop)
	{
		printf("\nExecuting docking runs, stopping automatically after either reaching %.2f kcal/mol standard deviation\nof the best molecules, %lu generations, or %lu evaluations, whichever comes first:\n\n",mypars->stopstd,mypars->num_of_generations,mypars->num_of_energy_evals);
		printf("Generations |  Evaluations |     Threshold    |  Average energy of best 10%%  | Samples |    Best energy\n");
		printf("------------+--------------+------------------+------------------------------+---------+-------------------\n");
	}
	else
	{
		printf("\nExecuting docking runs:\n");
		printf("        20%%        40%%       60%%       80%%       100%%\n");
		printf("---------+---------+---------+---------+---------+\n");
	}
#else
	printf("\n");
#endif
    SetKernelsGpuData(&cData);
	curr_progress_cnt = 0;

#ifdef DOCK_DEBUG
	// Main while-loop iterarion counter
	unsigned int ite_cnt = 0;
#endif

	/*
	// Added for printing intracontributor_pairs (autodockdevpy)
	for (unsigned int intrapair_cnt=0; 
			  intrapair_cnt<dockpars.num_of_intraE_contributors;
			  intrapair_cnt++) {
		if (intrapair_cnt == 0) {
			printf("%-10s %-10s %-10s\n", "#pair", "#atom1", "#atom2");
		}

		printf ("%-10u %-10u %-10u\n", intrapair_cnt,
					    KerConst.intraE_contributors_const[3*intrapair_cnt],
					    KerConst.intraE_contributors_const[3*intrapair_cnt+1]);
	}
	*/

	// Kernel1
	uint32_t kernel1_gxsize = blocksPerGridForEachEntity;
	uint32_t kernel1_lxsize = threadsPerBlock;
#ifdef DOCK_DEBUG
	printf("%-25s %10s %8lu %10s %4u\n", "K_INIT", "gSize: ", kernel1_gxsize, "lSize: ", kernel1_lxsize); fflush(stdout);
#endif
	// End of Kernel1

	// Kernel2
  	uint32_t kernel2_gxsize = blocksPerGridForEachRun;
  	uint32_t kernel2_lxsize = threadsPerBlock;
#ifdef DOCK_DEBUG
	printf("%-25s %10s %8lu %10s %4u\n", "K_EVAL", "gSize: ", kernel2_gxsize, "lSize: ",  kernel2_lxsize); fflush(stdout);
#endif
	// End of Kernel2

	// Kernel4
  	uint32_t kernel4_gxsize = blocksPerGridForEachEntity;
  	uint32_t kernel4_lxsize = threadsPerBlock;
#ifdef DOCK_DEBUG
	printf("%-25s %10s %8u %10s %4u\n", "K_GA_GENERATION", "gSize: ",  kernel4_gxsize, "lSize: ", kernel4_lxsize); fflush(stdout);
#endif
	// End of Kernel4

    uint32_t kernel3_gxsize, kernel3_lxsize;
    uint32_t kernel5_gxsize, kernel5_lxsize;
    uint32_t kernel6_gxsize, kernel6_lxsize;
    uint32_t kernel7_gxsize, kernel7_lxsize;            
	if (cData.dockpars.lsearch_rate != 0.0f) {

		if (strcmp(mypars->ls_method, "sw") == 0) {
			// Kernel3
			kernel3_gxsize = blocksPerGridForEachLSEntity * threadsPerBlock;
			kernel3_lxsize = threadsPerBlock;
  			#ifdef DOCK_DEBUG
	  		printf("%-25s %10s %8u %10s %4u\n", "K_LS_SOLISWETS", "gSize: ", kernel3_gxsize, "lSize: ", kernel3_lxsize); fflush(stdout);
  			#endif
			// End of Kernel3
		} else if (strcmp(mypars->ls_method, "sd") == 0) {
			// Kernel5
  			kernel5_gxsize = blocksPerGridForEachGradMinimizerEntity * threadsPerBlock;
  			kernel5_lxsize = threadsPerBlock;
			#ifdef DOCK_DEBUG
			printf("%-25s %10s %8u %10s %4u\n", "K_LS_GRAD_SDESCENT", "gSize: ", kernel5_gxsize, "lSize: ", kernel5_lxsize); fflush(stdout);
			#endif
			// End of Kernel5
		} else if (strcmp(mypars->ls_method, "fire") == 0) {
			// Kernel6
  			kernel6_gxsize = blocksPerGridForEachGradMinimizerEntity * threadsPerBlock;
  			kernel6_lxsize = threadsPerBlock;
			#ifdef DOCK_DEBUG
			printf("%-25s %10s %8u %10s %4u\n", "K_LS_GRAD_FIRE", "gSize: ", kernel6_gxsize, "lSize: ", kernel6_lxsize); fflush(stdout);
			#endif
			// End of Kernel6
		} else if (strcmp(mypars->ls_method, "ad") == 0) {
			// Kernel7
			kernel7_gxsize = blocksPerGridForEachGradMinimizerEntity;
			kernel7_lxsize = threadsPerBlock;
			#ifdef DOCK_DEBUG
			printf("%-25s %10s %8u %10s %4u\n", "K_LS_GRAD_ADADELTA", "gSize: ", kernel7_gxsize, "lSize: ", kernel7_lxsize); fflush(stdout);
			#endif
			// End of Kernel7
		}
	} // End if (dockpars.lsearch_rate != 0.0f)

	// Kernel1
	#ifdef DOCK_DEBUG
		printf("\nExecution starts:\n\n");
		printf("%-25s", "\tK_INIT");fflush(stdout);
        cudaDeviceSynchronize();
	#endif
    gpu_calc_initpop(kernel1_gxsize, kernel1_lxsize, pMem_conformations_current, pMem_energies_current);
	//runKernel1D(command_queue,kernel1,kernel1_gxsize,kernel1_lxsize,&time_start_kernel,&time_end_kernel);
	#ifdef DOCK_DEBUG
        cudaDeviceSynchronize();
		printf("%15s" ," ... Finished\n");fflush(stdout);
	#endif
	// End of Kernel1

	// Kernel2
	#ifdef DOCK_DEBUG
		printf("%-25s", "\tK_EVAL");fflush(stdout);
	#endif
	//runKernel1D(command_queue,kernel2,kernel2_gxsize,kernel2_lxsize,&time_start_kernel,&time_end_kernel);
    gpu_sum_evals(kernel2_gxsize, kernel2_lxsize);
	#ifdef DOCK_DEBUG
        cudaDeviceSynchronize();
		printf("%15s" ," ... Finished\n");fflush(stdout);
	#endif
	// End of Kernel2
	// ===============================================================================



	#if 0
	generation_cnt = 1;
	#endif
	generation_cnt = 0;
	bool first_time = true;
	float* energies;
	float threshold = 1<<24;
	float threshold_used;
	float thres_stddev = threshold;
	float curr_avg = -(1<<24);
	float curr_std = thres_stddev;
	float prev_avg = 0.0;
	unsigned int roll_count = 0;
	float rolling[4*3];
	float rolling_stddev;
	memset(&rolling[0],0,12*sizeof(float));
	unsigned int bestN = 1;
	unsigned int Ntop = mypars->pop_size;
	unsigned int Ncream = Ntop / 10;
	float delta_energy = 2.0 * thres_stddev / Ntop;
	float overall_best_energy;
	unsigned int avg_arr_size = (Ntop+1)*3;
	float average_sd2_N[avg_arr_size];
	unsigned long total_evals;

    auto const t2 = std::chrono::steady_clock::now();
    printf("Rest of Setup time %fs\n", elapsed_seconds(t1 ,t2));


	// -------- Replacing with memory maps! ------------
	while ((progress = check_progress(pMem_gpu_evals_of_runs, generation_cnt, mypars->num_of_energy_evals, mypars->num_of_generations, mypars->num_of_runs, total_evals)) < 100.0)
	// -------- Replacing with memory maps! ------------
	{
		if (mypars->autostop)
		{
			if (generation_cnt % 10 == 0) {
                cudaError_t status;
                status = cudaMemcpy(cpu_energies, pMem_energies_current, size_energies, cudaMemcpyDeviceToHost);
                RTERROR(status, "cudaMemcpy: couldn't downloaded pMem_energies_current");
				for(unsigned int count=0; (count<1+8*(generation_cnt==0)) && (fabs(curr_avg-prev_avg)>0.00001); count++)
				{
					threshold_used = threshold;
					overall_best_energy = 1<<24;
					memset(&average_sd2_N[0],0,avg_arr_size*sizeof(float));
					for (run_cnt=0; run_cnt < mypars->num_of_runs; run_cnt++)
					{
						energies = cpu_energies+run_cnt*mypars->pop_size;
						for (unsigned int i=0; i<mypars->pop_size; i++)
						{
							float energy = energies[i];
							if(energy < overall_best_energy)
								overall_best_energy = energy;
							if(energy < threshold)
							{
								average_sd2_N[0] += energy;
								average_sd2_N[1] += energy * energy;
								average_sd2_N[2] += 1.0;
								for(unsigned int m=0; m<Ntop; m++)
									if(energy < (threshold-2.0*thres_stddev)+m*delta_energy)
									{
										average_sd2_N[3*(m+1)] += energy;
										average_sd2_N[3*(m+1)+1] += energy*energy;
										average_sd2_N[3*(m+1)+2] += 1.0;
										break; // only one entry per bin
									}
							}
						}
					}
					if(first_time)
					{
						curr_avg = average(&average_sd2_N[0]);
						curr_std = stddev(&average_sd2_N[0]);
						bestN = average_sd2_N[2];
						thres_stddev = curr_std;
						threshold = curr_avg + thres_stddev;
						delta_energy = 2.0 * thres_stddev / (Ntop-1);
						first_time = false;
					}
					else
					{
						curr_avg = average(&average_sd2_N[0]);
						curr_std = stddev(&average_sd2_N[0]);
						bestN = average_sd2_N[2];
						average_sd2_N[0] = 0.0;
						average_sd2_N[1] = 0.0;
						average_sd2_N[2] = 0.0;
						unsigned int lowest_energy = 0;
						for(unsigned int m=0; m<Ntop; m++)
						{
							if((average_sd2_N[3*(m+1)+2]>=1.0) && (lowest_energy<Ncream))
							{
								if((average_sd2_N[2]<4.0) || fabs(average(&average_sd2_N[0])-average(&average_sd2_N[3*(m+1)]))<2.0*mypars->stopstd)
								{
//									printf("Adding %f +/- %f (%i)\n",average(&average_sd2_N[3*(m+1)]),stddev(&average_sd2_N[3*(m+1)]),(unsigned int)average_sd2_N[3*(m+1)+2]);
									average_sd2_N[0] += average_sd2_N[3*(m+1)];
									average_sd2_N[1] += average_sd2_N[3*(m+1)+1];
									average_sd2_N[2] += average_sd2_N[3*(m+1)+2];
									lowest_energy++;
								}
							}
						}
//						printf("---\n");
						if(lowest_energy>0)
						{
							curr_avg = average(&average_sd2_N[0]);
							curr_std = stddev(&average_sd2_N[0]);
							bestN = average_sd2_N[2];
						}
						if(curr_std<0.5f*mypars->stopstd)
							thres_stddev = mypars->stopstd;
						else
							thres_stddev = curr_std;
						threshold = curr_avg + Ncream * thres_stddev / bestN;
						delta_energy = 2.0 * thres_stddev / (Ntop-1);
					}
				}
				printf("%11lu | %12d |%8.2f kcal/mol |%8.2f +/-%8.2f kcal/mol |%8i |%8.2f kcal/mol\n",generation_cnt,total_evals/mypars->num_of_runs,threshold_used,curr_avg,curr_std,bestN,overall_best_energy);
				fflush(stdout);
				rolling[3*roll_count] = curr_avg * bestN;
				rolling[3*roll_count+1] = (curr_std*curr_std + curr_avg*curr_avg)*bestN;
				rolling[3*roll_count+2] = bestN;
				roll_count = (roll_count + 1) % 4;
				average_sd2_N[0] = rolling[0] + rolling[3] + rolling[6] + rolling[9];
				average_sd2_N[1] = rolling[1] + rolling[4] + rolling[7] + rolling[10];
				average_sd2_N[2] = rolling[2] + rolling[5] + rolling[8] + rolling[11];
				// Finish when the std.dev. of the last 4 rounds is below 0.1 kcal/mol
				if((stddev(&average_sd2_N[0])<mypars->stopstd) && (generation_cnt>30))
				{
					printf("------------+--------------+------------------+------------------------------+---------+-------------------\n");
					printf("\n%43s evaluation after reaching\n%40.2f +/-%8.2f kcal/mol combined.\n%34i samples, best energy %8.2f kcal/mol.\n","Finished",average(&average_sd2_N[0]),stddev(&average_sd2_N[0]),(unsigned int)average_sd2_N[2],overall_best_energy);
					fflush(stdout);
					break;
				}
			}
		}
		else
		{
#ifdef DOCK_DEBUG
			ite_cnt++;
			printf("\nLGA iteration # %u\n", ite_cnt);
			fflush(stdout);
#endif
			//update progress bar (bar length is 50)
			new_progress_cnt = (int) (progress/2.0+0.5);
			if (new_progress_cnt > 50)
				new_progress_cnt = 50;
			while (curr_progress_cnt < new_progress_cnt) {
				curr_progress_cnt++;
#ifndef DOCK_DEBUG
				printf("*");
#endif
				fflush(stdout);
			}
		}
		// Kernel4
		#ifdef DOCK_DEBUG
			printf("%-25s", "\tK_GA_GENERATION");fflush(stdout);
		#endif
        
		//runKernel1D(command_queue,kernel4,kernel4_gxsize,kernel4_lxsize,&time_start_kernel,&time_end_kernel);
        gpu_gen_and_eval_newpops(kernel4_gxsize, kernel4_lxsize, pMem_conformations_current, pMem_energies_current, pMem_conformations_next, pMem_energies_next);
		#ifdef DOCK_DEBUG
			printf("%15s", " ... Finished\n");fflush(stdout);
		#endif
		// End of Kernel4
		if (cData.dockpars.lsearch_rate != 0.0f) {
			if (strcmp(mypars->ls_method, "sw") == 0) {
				// Kernel3
				#ifdef DOCK_DEBUG
					printf("%-25s", "\tK_LS_SOLISWETS");fflush(stdout);
				#endif
				//runKernel1D(command_queue,kernel3,kernel3_gxsize,kernel3_lxsize,&time_start_kernel,&time_end_kernel);
				#ifdef DOCK_DEBUG
					printf("%15s" ," ... Finished\n");fflush(stdout);
				#endif
				// End of Kernel3
			} else if (strcmp(mypars->ls_method, "sd") == 0) {
				// Kernel5
				#ifdef DOCK_DEBUG
					printf("%-25s", "\tK_LS_GRAD_SDESCENT");fflush(stdout);
				#endif
				//runKernel1D(command_queue,kernel5,kernel5_gxsize,kernel5_lxsize,&time_start_kernel,&time_end_kernel);
				#ifdef DOCK_DEBUG
					printf("%15s" ," ... Finished\n");fflush(stdout);
				#endif
				// End of Kernel5
			} else if (strcmp(mypars->ls_method, "fire") == 0) {
				// Kernel6
				#ifdef DOCK_DEBUG
					printf("%-25s", "\tK_LS_GRAD_FIRE");fflush(stdout);
				#endif
				//runKernel1D(command_queue,kernel6,kernel6_gxsize,kernel6_lxsize,&time_start_kernel,&time_end_kernel);
				#ifdef DOCK_DEBUG
					printf("%15s" ," ... Finished\n");fflush(stdout);
				#endif
				// End of Kernel6
			} else if (strcmp(mypars->ls_method, "ad") == 0) {
				// Kernel7
				#ifdef DOCK_DEBUG
					printf("%-25s", "\tK_LS_GRAD_ADADELTA");fflush(stdout);
				#endif
                // runKernel1D(command_queue,kernel7,kernel7_gxsize,kernel7_lxsize,&time_start_kernel,&time_end_kernel);
                gpu_gradient_minAD(kernel7_gxsize, kernel7_lxsize, pMem_conformations_next, pMem_energies_next);
				#ifdef DOCK_DEBUG
					printf("%15s" ," ... Finished\n");fflush(stdout);
				#endif
				// End of Kernel7
			}
		} // End if (dockpars.lsearch_rate != 0.0f)
		// -------- Replacing with memory maps! ------------
		// -------- Replacing with memory maps! ------------
		// Kernel2
		#ifdef DOCK_DEBUG
			printf("%-25s", "\tK_EVAL");fflush(stdout);
		#endif
		//runKernel1D(command_queue,kernel2,kernel2_gxsize,kernel2_lxsize,&time_start_kernel,&time_end_kernel);
        gpu_sum_evals(kernel2_gxsize, kernel2_lxsize);       
        
		#ifdef DOCK_DEBUG
			printf("%15s" ," ... Finished\n");fflush(stdout);
		#endif
		// End of Kernel2
		// ===============================================================================
		// -------- Replacing with memory maps! ------------
#if defined (MAPPED_COPY)
		//map_cpu_evals_of_runs = (int*) memMap(command_queue, mem_gpu_evals_of_runs, CL_MAP_READ, size_evals_of_runs);
#else
		cudaMemcpy(cpu_evals_of_runs, pMem_gpu_evals_of_runs, size_evals_of_runs, cudaMemcpyDeviceToHost);
#endif
		// -------- Replacing with memory maps! ------------
		generation_cnt++;
		// ----------------------------------------------------------------------
		// ORIGINAL APPROACH: switching conformation and energy pointers (Probably the best approach, restored)
		// CURRENT APPROACH:  copy data from one buffer to another, pointers are kept the same
		// IMPROVED CURRENT APPROACH
		// Kernel arguments are changed on every iteration
		// No copy from dev glob memory to dev glob memory occurs
		// Use generation_cnt as it evolves with the main loop
		// No need to use tempfloat
		// No performance improvement wrt to "CURRENT APPROACH"

		// Kernel args exchange regions they point to
		// But never two args point to the same region of dev memory
		// NO ALIASING -> use restrict in Kernel
        
        // Flip conformation and energy pointers
        float* pTemp;
        pTemp = pMem_conformations_current;
        pMem_conformations_current = pMem_conformations_next;
        pMem_conformations_next = pTemp;
        pTemp = pMem_energies_current;
        pMem_energies_current = pMem_energies_next;
        pMem_energies_next = pTemp;
               
		// ----------------------------------------------------------------------
		#ifdef DOCK_DEBUG
			printf("\tProgress %.3f %%\n", progress);
			fflush(stdout);
		#endif
	} // End of while-loop

    auto const t3 = std::chrono::steady_clock::now();
    printf("\nDocking time %fs\n", elapsed_seconds(t2, t3));


	clock_stop_docking = clock();
	if (mypars->autostop==0)
	{
		//update progress bar (bar length is 50)mem_num_of_rotatingatoms_per_rotbond_const
		while (curr_progress_cnt < 50) {
			curr_progress_cnt++;
			printf("*");
			fflush(stdout);
		}
	}

	// ===============================================================================
	// Modification based on:
	// http://www.cc.gatech.edu/~vetter/keeneland/tutorial-2012-02-20/08-opencl.pdf
	// ===============================================================================
	//processing results
    status = cudaMemcpy(cpu_final_populations, pMem_conformations_current, size_populations, cudaMemcpyDeviceToHost);
    RTERROR(status, "cudaMemcpy: couldn't copy pMem_conformations_current to host.\n");
    status = cudaMemcpy(cpu_energies, pMem_energies_current, size_energies, cudaMemcpyDeviceToHost);
    RTERROR(status, "cudaMemcpy: couldn't copy pMem_energies_current to host.\n");

#if defined (DOCK_DEBUG)
	for (int cnt_pop=0;cnt_pop<size_populations/sizeof(float);cnt_pop++)
		printf("total_num_pop: %u, cpu_final_populations[%u]: %f\n",(unsigned int)(size_populations/sizeof(float)),cnt_pop,cpu_final_populations[cnt_pop]);
	for (int cnt_pop=0;cnt_pop<size_energies/sizeof(float);cnt_pop++)
		printf("total_num_energies: %u, cpu_energies[%u]: %f\n",    (unsigned int)(size_energies/sizeof(float)),cnt_pop,cpu_energies[cnt_pop]);
#endif
	// ===============================================================================
	for (run_cnt=0; run_cnt < mypars->num_of_runs; run_cnt++)
	{
		arrange_result(cpu_final_populations+run_cnt*mypars->pop_size*GENOTYPE_LENGTH_IN_GLOBMEM, cpu_energies+run_cnt*mypars->pop_size, mypars->pop_size);
		make_resfiles(cpu_final_populations+run_cnt*mypars->pop_size*GENOTYPE_LENGTH_IN_GLOBMEM, 
			      cpu_energies+run_cnt*mypars->pop_size, 
			      &myligand_reference,
			      myligand_init,
			      myxrayligand, 
			      mypars, 
			      cpu_evals_of_runs[run_cnt], 
			      generation_cnt, 
			      mygrid, 
			      cpu_floatgrids, 
			      cpu_ref_ori_angles+3*run_cnt, 
			      argc, 
			      argv, 
                              /*1*/0,
			      run_cnt, 
			      &(cpu_result_ligands [run_cnt]));
	}
	clock_stop_program_before_clustering = clock();
	clusanal_gendlg(cpu_result_ligands, mypars->num_of_runs, myligand_init, mypars,
					 mygrid, argc, argv, ELAPSEDSECS(clock_stop_docking, clock_start_docking)/mypars->num_of_runs,
					 ELAPSEDSECS(clock_stop_program_before_clustering, clock_start_program),generation_cnt,total_evals/mypars->num_of_runs);
	clock_stop_docking = clock();
    
    
    
    // Release all CUDA objects
    status = cudaFree(pMem_fgrids);
    RTERROR(status, "cudaFree: error freeing pMem_fgrids");
    status = cudaFree(pMem_conformations1);
    RTERROR(status, "cudaFree: error freeing pMem_conformations1");
    status = cudaFree(pMem_conformations2);
    RTERROR(status, "cudaFree: error freeing pMem_conformations2");
    status = cudaFree(pMem_energies1);
    RTERROR(status, "cudaFree: error freeing pMem_energies1");    
    status = cudaFree(pMem_energies2);
    RTERROR(status, "cudaFree: error freeing pMem_energies2"); 
    status = cudaFree(pMem_evals_of_new_entities);
    RTERROR(status, "cudaFree: error freeing pMem_evals_of_new_entities");
    status = cudaFree(pMem_gpu_evals_of_runs);
    RTERROR(status, "cudaFree: error freeing pMem_gpu_evals_of_runs");
    status = cudaFree(pMem_prng_states);
    RTERROR(status, "cudaFree: error freeing pMem_prng_states");
    cudaDeviceReset();
    
    
	free(cpu_init_populations);
	free(cpu_energies);
	free(cpu_result_ligands);
	free(cpu_prng_seeds);
	free(cpu_evals_of_runs);
	free(cpu_ref_ori_angles);

    auto const t4 = std::chrono::steady_clock::now();
    printf("Shutdown time %fs\n", elapsed_seconds(t3, t4));
	return 0;
}

double check_progress(int* evals_of_runs, int generation_cnt, int max_num_of_evals, int max_num_of_gens, int num_of_runs, unsigned long &total_evals)
//The function checks if the stop condition of the docking is satisfied, returns 0 if no, and returns 1 if yes. The fitst
//parameter points to the array which stores the number of evaluations performed for each run. The second parameter stores
//the generations used. The other parameters describe the maximum number of energy evaluations, the maximum number of
//generations, and the number of runs, respectively. The stop condition is satisfied, if the generations used is higher
//than the maximal value, or if the average number of evaluations used is higher than the maximal value.
{
	/*	Stops if every run reached the number of evals or number of generations

	int runs_finished;
	int i;

	runs_finished = 0;
	for (i=0; i<num_of_runs; i++)
		if (evals_of_runs[i] >= max_num_of_evals)
			runs_finished++;

	if ((runs_finished >= num_of_runs) || (generation_cnt >= max_num_of_gens))
		return 1;
	else
		return 0;
        */

	//Stops if the sum of evals of every run reached the sum of the total number of evals

	int i;
	double evals_progress;
	double gens_progress;

	//calculating progress according to number of runs
	total_evals = 0;
	for (i=0; i<num_of_runs; i++)
		total_evals += evals_of_runs[i];

	evals_progress = (double)total_evals/((double) num_of_runs)/max_num_of_evals*100.0;

	//calculating progress according to number of generations
	gens_progress = ((double) generation_cnt)/((double) max_num_of_gens)*100.0; //std::cout<< "gens_progress: " << gens_progress <<std::endl;

	if (evals_progress > gens_progress)
		return evals_progress;
	else
		return gens_progress;
}
