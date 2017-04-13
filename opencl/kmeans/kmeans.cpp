#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <iostream>
#include <string>
#include "kmeans.h"
#include <omp.h>

#ifdef WIN
	#include <windows.h>
#else
	#include <pthread.h>
	#include <sys/time.h>
	double gettime() {
		struct timeval t;
		gettimeofday(&t,NULL);
		return t.tv_sec+t.tv_usec*1e-6;
	}
#endif


#ifdef NV 
	#include <oclUtils.h>
#else
	#include <CL/cl.h>
#endif

#ifndef FLT_MAX
#define FLT_MAX 3.40282347e+38
#endif

#ifdef RD_WG_SIZE_0_0
        #define BLOCK_SIZE RD_WG_SIZE_0_0
#elif defined(RD_WG_SIZE_0)
        #define BLOCK_SIZE RD_WG_SIZE_0
#elif defined(RD_WG_SIZE)
        #define BLOCK_SIZE RD_WG_SIZE
#else
        #define BLOCK_SIZE 128
#endif

#ifdef RD_WG_SIZE_1_0
     #define BLOCK_SIZE2 RD_WG_SIZE_1_0
#elif defined(RD_WG_SIZE_1)
     #define BLOCK_SIZE2 RD_WG_SIZE_1
#elif defined(RD_WG_SIZE)
     #define BLOCK_SIZE2 RD_WG_SIZE
#else
     #define BLOCK_SIZE2 128
#endif

#define TWO_GPUS


// local variables
static cl_context	    context;
static cl_command_queue cmd_queue;
#ifdef TWO_GPUS
static cl_command_queue cmd_queue2;
#endif
static cl_device_type   device_type;
static cl_device_id   * device_list;
static cl_int           num_devices;

static int initialize(int use_gpu)
{
	cl_int result;
	size_t size;

	// create OpenCL context
	cl_platform_id platform_id;
	if (clGetPlatformIDs(1, &platform_id, NULL) != CL_SUCCESS) { printf("ERROR: clGetPlatformIDs(1,*,0) failed\n"); return -1; }
	cl_context_properties ctxprop[] = { CL_CONTEXT_PLATFORM, (cl_context_properties)platform_id, 0};
	device_type = use_gpu ? CL_DEVICE_TYPE_GPU : CL_DEVICE_TYPE_CPU;
	context = clCreateContextFromType( ctxprop, device_type, NULL, NULL, NULL );
	if( !context ) { printf("ERROR: clCreateContextFromType(%s) failed\n", use_gpu ? "GPU" : "CPU"); return -1; }

	// get the list of GPUs
	result = clGetContextInfo( context, CL_CONTEXT_DEVICES, 0, NULL, &size );
	num_devices = (int) (size / sizeof(cl_device_id));	
	if( result != CL_SUCCESS || num_devices < 1 ) { printf("ERROR: clGetContextInfo() failed\n"); return -1; }
	device_list = new cl_device_id[num_devices];
	
	if( !device_list ) { printf("ERROR: new cl_device_id[] failed\n"); return -1; }
	result = clGetContextInfo( context, CL_CONTEXT_DEVICES, size, device_list, NULL );
	if( result != CL_SUCCESS ) { printf("ERROR: clGetContextInfo() failed\n"); return -1; }

	// create command queue for the first device
	cmd_queue = clCreateCommandQueue( context, device_list[0], 0, NULL );
	if( !cmd_queue ) { printf("ERROR: clCreateCommandQueue() failed\n"); return -1; }
#ifdef TWO_GPUS
	cmd_queue2 = clCreateCommandQueue( context, device_list[1], 0, NULL );
	if( !cmd_queue2 ) { printf("ERROR: clCreateCommandQueue() failed\n"); return -1; }
#endif

	return 0;
}

static int shutdown()
{
	// release resources
	if( cmd_queue ) clReleaseCommandQueue( cmd_queue );
#ifdef TWO_GPUS	
	if( cmd_queue2 ) clReleaseCommandQueue( cmd_queue2 );
#endif
	if( context ) clReleaseContext( context );
	if( device_list ) delete device_list;

	// reset all variables
	cmd_queue = 0;
	context = 0;
	device_list = 0;
	num_devices = 0;
	device_type = 0;

	return 0;
}

cl_mem d_feature;
cl_mem d_feature_swap;
cl_mem d_cluster;
cl_mem d_membership;

#ifdef TWO_GPUS
cl_mem d_feature2;
cl_mem d_feature_swap2;
cl_mem d_cluster2;
cl_mem d_membership2;
#endif


cl_kernel kernel;
cl_kernel kernel_s;
cl_kernel kernel2;

#ifdef TWO_GPUS
cl_kernel kernel_2;
cl_kernel kernel_s_2;
cl_kernel kernel2_2;
#endif

int   *membership_OCL;
int   *membership_d;
float *feature_d;
float *clusters_d;
float *center_d;

int allocate(int n_points, int n_features, int n_clusters, float **feature)
{

	int sourcesize = 1024*1024;
	char * source = (char *)calloc(sourcesize, sizeof(char)); 
	if(!source) { printf("ERROR: calloc(%d) failed\n", sourcesize); return -1; }

	// read the kernel core source
	char * tempchar = "./kmeans.cl";
	FILE * fp = fopen(tempchar, "rb"); 
	if(!fp) { printf("ERROR: unable to open '%s'\n", tempchar); return -1; }
	fread(source + strlen(source), sourcesize, 1, fp);
	fclose(fp);
		
	// OpenCL initialization
	int use_gpu = 1;
	if(initialize(use_gpu)) return -1;

	// compile kernel
	cl_int err = 0;
	const char * slist[2] = { source, 0 };
	cl_program prog = clCreateProgramWithSource(context, 1, slist, NULL, &err);
	if(err != CL_SUCCESS) { printf("ERROR: clCreateProgramWithSource() => %d\n", err); return -1; }
	err = clBuildProgram(prog, 0, NULL, NULL, NULL, NULL);
	{ /* show warnings/errors
		static char log[65536]; memset(log, 0, sizeof(log));
		cl_device_id device_id = 0;
//		err = clGetContextInfo(context, CL_CONTEXT_DEVICES, sizeof(device_id), &device_id, NULL);
		clGetProgramBuildInfo(prog, device_id, CL_PROGRAM_BUILD_LOG, sizeof(log)-1, log, NULL);
		if(err || strstr(log,"warning:") || strstr(log, "error:")) printf("<<<<\n%s\n>>>>\n", log);
	*/}
	if(err != CL_SUCCESS) { printf("ERROR: clBuildProgram() => %d\n", err); return -1; }
	
	char * kernel_kmeans_c  = "kmeans_kernel_c";
	char * kernel_swap  = "kmeans_swap";	
		
	kernel_s = clCreateKernel(prog, kernel_kmeans_c, &err);  
	if(err != CL_SUCCESS) { printf("ERROR: clCreateKernel() 0 => %d\n", err); return -1; }
	kernel2 = clCreateKernel(prog, kernel_swap, &err);  
	if(err != CL_SUCCESS) { printf("ERROR: clCreateKernel() 0 => %d\n", err); return -1; }

#ifdef TWO_GPUS
	kernel_s_2 = clCreateKernel(prog, kernel_kmeans_c, &err);  
	if(err != CL_SUCCESS) { printf("ERROR: clCreateKernel() 0 => %d\n", err); return -1; }
	kernel2_2 = clCreateKernel(prog, kernel_swap, &err);  
	if(err != CL_SUCCESS) { printf("ERROR: clCreateKernel() 0 => %d\n", err); return -1; }
#endif

		
	clReleaseProgram(prog);	

	int divider=1; //TODO: ONLY WORKS IF n_points is a multiple of 2
	
#ifdef TWO_GPUS
	divider=2;
#endif
	
	d_feature = clCreateBuffer(context, CL_MEM_READ_WRITE, n_points * n_features * sizeof(float)/divider, NULL, &err );
	if(err != CL_SUCCESS) { printf("ERROR: clCreateBuffer d_feature (size:%d) => %d\n", n_points * n_features, err); return -1;}
	d_feature_swap = clCreateBuffer(context, CL_MEM_READ_WRITE, n_points * n_features * sizeof(float)/divider, NULL, &err );
	if(err != CL_SUCCESS) { printf("ERROR: clCreateBuffer d_feature_swap (size:%d) => %d\n", n_points * n_features, err); return -1;}
	d_cluster = clCreateBuffer(context, CL_MEM_READ_WRITE, n_clusters * n_features  * sizeof(float), NULL, &err );
	if(err != CL_SUCCESS) { printf("ERROR: clCreateBuffer d_cluster (size:%d) => %d\n", n_clusters * n_features, err); return -1;}
	d_membership = clCreateBuffer(context, CL_MEM_READ_WRITE, n_points * sizeof(int)/divider, NULL, &err );
	if(err != CL_SUCCESS) { printf("ERROR: clCreateBuffer d_membership (size:%d) => %d\n", n_points, err); return -1;}

#ifdef TWO_GPUS		
	d_feature2 = clCreateBuffer(context, CL_MEM_READ_WRITE, n_points * n_features * sizeof(float)/divider, NULL, &err );
	if(err != CL_SUCCESS) { printf("ERROR: clCreateBuffer d_feature (size:%d) => %d\n", n_points * n_features, err); return -1;}
	d_feature_swap2 = clCreateBuffer(context, CL_MEM_READ_WRITE, n_points * n_features * sizeof(float)/divider, NULL, &err );
	if(err != CL_SUCCESS) { printf("ERROR: clCreateBuffer d_feature_swap (size:%d) => %d\n", n_points * n_features, err); return -1;}
	d_cluster2 = clCreateBuffer(context, CL_MEM_READ_WRITE, n_clusters * n_features  * sizeof(float), NULL, &err );
	if(err != CL_SUCCESS) { printf("ERROR: clCreateBuffer d_cluster (size:%d) => %d\n", n_clusters * n_features, err); return -1;}
	d_membership2 = clCreateBuffer(context, CL_MEM_READ_WRITE, n_points * sizeof(int)/divider, NULL, &err );
	if(err != CL_SUCCESS) { printf("ERROR: clCreateBuffer d_membership (size:%d) => %d\n", n_points, err); return -1;}
#endif

	//CHANGED TO NON-BLOCKING
	//write buffers
	err = clEnqueueWriteBuffer(cmd_queue, d_feature, 0, 0, n_points * n_features * sizeof(float)/divider, feature[0], 0, 0, 0);
	if(err != CL_SUCCESS) { printf("ERROR: clEnqueueWriteBuffer d_feature (size:%d) => %d\n", n_points * n_features, err); return -1; }
#ifdef TWO_GPUS
	err = clEnqueueWriteBuffer(cmd_queue2, d_feature2, 0, 0, n_points * n_features * sizeof(float)/divider, feature[n_points/divider], 0, 0, 0);
	if(err != CL_SUCCESS) { printf("ERROR: clEnqueueWriteBuffer d_feature (size:%d) => %d\n", n_points * n_features, err); return -1; }
#endif


	int div_points=n_points/divider;
	clSetKernelArg(kernel2, 0, sizeof(void *), (void*) &d_feature);
	clSetKernelArg(kernel2, 1, sizeof(void *), (void*) &d_feature_swap);
	clSetKernelArg(kernel2, 2, sizeof(cl_int), (void*) &div_points);
	clSetKernelArg(kernel2, 3, sizeof(cl_int), (void*) &n_features);

#ifdef TWO_GPUS
	clSetKernelArg(kernel2_2, 0, sizeof(void *), (void*) &d_feature2);
	clSetKernelArg(kernel2_2, 1, sizeof(void *), (void*) &d_feature_swap2);
	clSetKernelArg(kernel2_2, 2, sizeof(cl_int), (void*) &div_points);
	clSetKernelArg(kernel2_2, 3, sizeof(cl_int), (void*) &n_features);
#endif

	size_t global_work[3] = { n_points/divider, 1, 1 };
	/// Ke Wang adjustable local group size 2013/08/07 10:37:33
	size_t local_work_size= BLOCK_SIZE; // work group size is defined by RD_WG_SIZE_0 or RD_WG_SIZE_0_0 2014/06/10 17:00:51
	if(global_work[0]%local_work_size !=0)
	  global_work[0]=(global_work[0]/local_work_size+1)*local_work_size;
	

	printf("%lu, %lu\n", global_work[0], local_work_size);
	err = clEnqueueNDRangeKernel(cmd_queue, kernel2, 1, NULL, global_work, &local_work_size, 0, 0, 0);
	if(err != CL_SUCCESS) { printf("ERROR: clEnqueueNDRangeKernel()=>%d failed\n", err); return -1; }

#ifdef TWO_GPUS
	err = clEnqueueNDRangeKernel(cmd_queue2, kernel2_2, 1, NULL, global_work, &local_work_size, 0, 0, 0);
	if(err != CL_SUCCESS) { printf("ERROR: clEnqueueNDRangeKernel()=>%d failed\n", err); return -1; }	
#endif

	membership_OCL = (int*) malloc(n_points * sizeof(int));
}

void deallocateMemory()
{
	clReleaseMemObject(d_feature);
	clReleaseMemObject(d_feature_swap);
	clReleaseMemObject(d_cluster);
	clReleaseMemObject(d_membership);

#ifdef TWO_GPUS
	clReleaseMemObject(d_feature2);
	clReleaseMemObject(d_feature_swap2);
	clReleaseMemObject(d_cluster2);
	clReleaseMemObject(d_membership2);
#endif

	free(membership_OCL);

}


int main( int argc, char** argv) 
{
	printf("WG size of kernel_swap = %d, WG size of kernel_kmeans = %d \n", BLOCK_SIZE, BLOCK_SIZE2);

	setup(argc, argv);
	shutdown();
}

int	kmeansOCL(float **feature,    /* in: [npoints][nfeatures] */
           int     n_features,
           int     n_points,
           int     n_clusters,
           int    *membership,
		   float **clusters,
		   int     *new_centers_len,
           float  **new_centers)	
{
  
	int delta = 0;
	int i, j, k;
	cl_int err = 0;
	
	int divider=1;

#ifdef TWO_GPUS
	divider=2;
#endif
	size_t global_work[3] = { n_points/divider, 1, 1 }; 


	/// Ke Wang adjustable local group size 2013/08/07 10:37:33
	size_t local_work_size=BLOCK_SIZE2; // work group size is defined by RD_WG_SIZE_1 or RD_WG_SIZE_1_0 2014/06/10 17:00:41
	if(global_work[0]%local_work_size !=0)
	  global_work[0]=(global_work[0]/local_work_size+1)*local_work_size;


	global_work[0]=global_work[0];
	

	//CHANGED TO NON BLOCKING
	err = clEnqueueWriteBuffer(cmd_queue, d_cluster, 0, 0, n_clusters * n_features * sizeof(float), clusters[0], 0, 0, 0);
	if(err != CL_SUCCESS) { printf("ERROR: clEnqueueWriteBuffer d_cluster (size:%d) => %d\n", n_points, err); return -1; }
#ifdef TWO_GPUS
	err = clEnqueueWriteBuffer(cmd_queue2, d_cluster2, 0, 0, n_clusters * n_features * sizeof(float), clusters[0], 0, 0, 0);
	if(err != CL_SUCCESS) { printf("ERROR: clEnqueueWriteBuffer d_cluster (size:%d) => %d\n", n_points, err); return -1; }
#endif

	int size = 0; int offset = 0;

	int div_points=n_points/divider;
					
	clSetKernelArg(kernel_s, 0, sizeof(void *), (void*) &d_feature_swap);
	clSetKernelArg(kernel_s, 1, sizeof(void *), (void*) &d_cluster);
	clSetKernelArg(kernel_s, 2, sizeof(void *), (void*) &d_membership);
	clSetKernelArg(kernel_s, 3, sizeof(cl_int), (void*) &div_points);
	clSetKernelArg(kernel_s, 4, sizeof(cl_int), (void*) &n_clusters);
	clSetKernelArg(kernel_s, 5, sizeof(cl_int), (void*) &n_features);
	clSetKernelArg(kernel_s, 6, sizeof(cl_int), (void*) &offset);
	clSetKernelArg(kernel_s, 7, sizeof(cl_int), (void*) &size);


#ifdef TWO_GPUS
	clSetKernelArg(kernel_s_2, 0, sizeof(void *), (void*) &d_feature_swap2);
	clSetKernelArg(kernel_s_2, 1, sizeof(void *), (void*) &d_cluster2);
	clSetKernelArg(kernel_s_2, 2, sizeof(void *), (void*) &d_membership2);
	clSetKernelArg(kernel_s_2, 3, sizeof(cl_int), (void*) &div_points);
	clSetKernelArg(kernel_s_2, 4, sizeof(cl_int), (void*) &n_clusters);
	clSetKernelArg(kernel_s_2, 5, sizeof(cl_int), (void*) &n_features);
	clSetKernelArg(kernel_s_2, 6, sizeof(cl_int), (void*) &offset);
	clSetKernelArg(kernel_s_2, 7, sizeof(cl_int), (void*) &size);
#endif


	//printf("Lanzando %d threads en GPU0\n", global_work[0]);
	err = clEnqueueNDRangeKernel(cmd_queue, kernel_s, 1, NULL, global_work, &local_work_size, 0, 0, 0);
	if(err != CL_SUCCESS) { printf("ERROR: clEnqueueNDRangeKernel()=>%d failed\n", err); return -1; }
	err = clEnqueueReadBuffer(cmd_queue, d_membership, 0, 0, n_points * sizeof(int)/divider, membership_OCL, 0, 0, 0);
	if(err != CL_SUCCESS) { printf("ERROR: Memcopy Out\n"); return -1; }
	//printf("Leyendo %d bytes en GPU0\n", n_points * sizeof(int)/divider);

#ifdef TWO_GPUS
	//printf("Lanzando %d threads en GPU1\n", global_work[0]);
	err = clEnqueueNDRangeKernel(cmd_queue2, kernel_s_2, 1, NULL, global_work, &local_work_size, 0, 0, 0);
	if(err != CL_SUCCESS) { printf("ERROR: clEnqueueNDRangeKernel()=>%d failed\n", err); return -1; }
	err = clEnqueueReadBuffer(cmd_queue2, d_membership2, 0, 0, n_points * sizeof(int)/divider, membership_OCL+(n_points/divider), 0, 0, 0);
	if(err != CL_SUCCESS) { printf("ERROR: Memcopy Out-> %d\n", err); return -1; }
	//printf("Leyendo %d bytes en GPU1\n", n_points * sizeof(int)/divider);
	clFinish(cmd_queue2);
#endif

	clFinish(cmd_queue);
	
	delta = 0;


//	omp_set_num_threads(8);
//	#pragma omp parallel for schedule(guided) private(i,j) shared(membership_OCL, membership,new_centers) reduction (+:delta) /*reduction(+:new_centers_len[:n_clusters])*/
// needs array reduction for new_centers and new_centers_len
// one of the loops that take smost to execute 
//float reduce_timing = omp_get_wtime() ;
	for (i = 0; i < n_points; i++)
	{
	//	printf("%d==%d?\n", n_points, i);
		int cluster_id = membership_OCL[i];
	//	printf("%d->%d\n", i, cluster_id);
//		#pragma omp atomic
		new_centers_len[cluster_id]++;
		if (membership_OCL[i] != membership[i])
		{
			delta++;
			membership[i] = membership_OCL[i];
		}
		for (j = 0; j < n_features; j++)
		{
//		#pragma omp atomic
			new_centers[cluster_id][j] += feature[i][j];
		}
	}
//reduce_timing = omp_get_wtime() - reduce_timing;
//printf("Time for reduction: %.5fsec\n", reduce_timing);
	return delta;
}
