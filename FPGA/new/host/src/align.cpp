#include "f5c.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>
#include <cstring>
#include "CL/opencl.h"
#include "AOCLUtils/aocl_utils.h"

#include "dump_functions.h"

using namespace aocl_utils;

#include "error.h"
// #include "f5c.h"
#include "f5cmisc_cu.h"
#include "f5cmisc.h"

#define AOCL_ALIGNMENT 64

#ifndef CPU_GPU_PROC

#define STRING_BUFFER_LEN 1024

// Runtime constants
// Used to define the work set over which this kernel will execute.
static const size_t work_group_size = 8; // 8 threads in the demo workgroup
// Defines kernel argument value, which is the workitem ID that will
// execute a printf call
static const int thread_id_to_output = 2;

// OpenCL runtime configuration
static cl_platform_id platform = NULL;
static cl_device_id device = NULL;
static cl_context context = NULL;
static cl_command_queue queue = NULL;
static cl_kernel align_kernel_pre_2d = NULL;
static cl_kernel align_kernel_core_2d_shm = NULL;
static cl_kernel align_kernel_post = NULL;
static cl_program program1 = NULL;
static cl_program program2 = NULL;
static cl_program program3 = NULL;
cl_int status;

// Function prototypes
bool init();
void cleanup();
static void device_info_ulong(cl_device_id device, cl_device_info param, const char *name);
static void device_info_uint(cl_device_id device, cl_device_info param, const char *name);
static void device_info_bool(cl_device_id device, cl_device_info param, const char *name);
static void device_info_string(cl_device_id device, cl_device_info param, const char *name);
static void display_device_info(cl_device_id device);

static void align_ocl(core_t *core, db_t *db);

// Entry point.
int main(int argc, char *argv[])
{

  if (!init())
  {
    fprintf(stderr, "init() unsuccessful\n");
    return -1;
  }

  fprintf(stderr, "init() successful\n\n");

  // Load dump files
  const char *align_args_dump_dir = argv[1];

  db_t *db;
  db = (db_t *)malloc(sizeof(db_t));

  core_t *core;
  core = (core_t *)malloc(sizeof(core_t));
  core->opt.verbosity = 2;

  load_n_bam_rec(db, align_args_dump_dir);
  // db->n_bam_rec = 5;

  // core->model = (model_t*)malloc(sizeof(model_t)*db->n_bam_rec);
  posix_memalign((void **)&core->model, AOCL_ALIGNMENT, sizeof(model_t) * NUM_KMER);

  load_core(core, align_args_dump_dir);

  fprintf(stderr, "num of reads:\t%d\n", db->n_bam_rec);

  db->n_event_align_pairs = (int32_t *)malloc(sizeof(int32_t) * db->n_bam_rec);
  db->event_align_pairs = (AlignedPair **)malloc(sizeof(AlignedPair *) * db->n_bam_rec);
  // db->read_len = (int32_t *)malloc(sizeof(int32_t) * db->n_bam_rec);
  posix_memalign((void **)&db->read_len, AOCL_ALIGNMENT, sizeof(int32_t) * db->n_bam_rec);
  db->read = (char **)malloc(sizeof(char *) * db->n_bam_rec);
  db->et = (event_table *)malloc(sizeof(event_table) * db->n_bam_rec);
  // db->scalings = (scalings_t *)malloc(sizeof(scalings_t) * db->n_bam_rec);
  posix_memalign((void **)&db->scalings, AOCL_ALIGNMENT, sizeof(scalings_t) * db->n_bam_rec);
  db->f5 = (fast5_t **)malloc(sizeof(fast5_t *) * db->n_bam_rec);

  db_t *db_out;
  db_out = (db_t *)malloc(sizeof(db_t));
  db_out->n_event_align_pairs = (int32_t *)malloc(sizeof(int32_t) * db->n_bam_rec);
  db_out->event_align_pairs = (AlignedPair **)malloc(sizeof(AlignedPair *) * db->n_bam_rec);

  // printf("db, core initialized\n");

  for (int i = 0; i < db->n_bam_rec; i++)
  {

    load_align_arguments(db, i, align_args_dump_dir);
    load_align_outputs(db_out, i, align_args_dump_dir);
    db->event_align_pairs[i] = (AlignedPair *)malloc(sizeof(AlignedPair) * db_out->n_event_align_pairs[i]);
  }

  align_ocl(core, db);

  for (int i = 0; i < db->n_bam_rec; i++)
  {
    // compare with original output
    int32_t n_event_align_pairs = db->n_event_align_pairs[i];
    int32_t n_event_align_pairs_out = db_out->n_event_align_pairs[i];

    if (n_event_align_pairs != n_event_align_pairs_out)
    {
      fprintf(stderr, "%d=\toutput: %d (%d)\tFailed\n", i, n_event_align_pairs, n_event_align_pairs_out);
      //break;
    }
    else
    {
      fprintf(stderr, "%d=\toutput: %d (%d)\tPassed ", i, n_event_align_pairs, n_event_align_pairs_out);
      // if (check_event_align_pairs(db->event_align_pairs[i], db_out->event_align_pairs[i], n_event_align_pairs) == 0)
      // {
      //   // fprintf(stderr, "%d=\t Found conflict in event_align_pairs\n", i);
      //   fprintf(stderr, "%d=\toutput: %d, expected: %d\tFailed\n", i, n_event_align_pairs, n_event_align_pairs_out);
      // }
      // else
      // {
      //   // fprintf(stderr, "%d=\t Run pass\n", i);
      //   fprintf(stderr, "%d=\toutput: %d, expected: %d\tPassed\n", i, n_event_align_pairs, n_event_align_pairs_out);
      // }
      check_event_align_pairs(db->event_align_pairs[i], db_out->event_align_pairs[i], n_event_align_pairs);
      fprintf(stderr, "\n");
    }

    // printf("readpos:%d, refpos:%d\n",db_out->event_align_pairs[0]->read_pos, db_out->event_align_pairs[0]->ref_pos);
  }
  return 0;
}

void align_ocl(core_t *core, db_t *db)
{

  int32_t i;
  int32_t n_bam_rec = db->n_bam_rec;
  double realtime1;

  /**cuda pointers*/
  char *read_host;      //flattened reads sequences
  ptr_t *read_ptr_host; //index pointer for flattedned "reads"
  int32_t *read_len_host;
  int64_t sum_read_len;
  int32_t *n_events_host;
  event_t *event_table_host;
  ptr_t *event_ptr_host;
  int64_t sum_n_events;
  scalings_t *scalings_host;
  AlignedPair *event_align_pairs_host;
  int32_t *n_event_align_pairs_host;
  float *bands_host;
  uint8_t *trace_host;
  EventKmerPair *band_lower_left_host;

  /*time measurements*/
  cl_event event_write_buffers;
  cl_event event_read_buffers;
  cl_double g_NDRangePureExecTimeMs;
  cl_double host_to_device_transfer_time = 0;
  cl_double device_to_host_transfer_time = 0;
  cl_ulong start = 0, end = 0;

  realtime1 = realtime();

  //int32_t cuda_device_num = core->opt.cuda_dev_id;

  // read_ptr_host = (ptr_t *)malloc(sizeof(ptr_t) * n_bam_rec);
  posix_memalign((void **)&read_ptr_host, AOCL_ALIGNMENT, n_bam_rec * sizeof(ptr_t));
  MALLOC_CHK(read_ptr_host);

  // #endif
  sum_read_len = 0;

  int32_t max_read_len = 0;

  //read sequences : needflattening
  for (i = 0; i < n_bam_rec; i++)
  {
    if (max_read_len < db->read_len[i])
      max_read_len = db->read_len[i];

    read_ptr_host[i] = sum_read_len;
    sum_read_len += (db->read_len[i] + 1); //with null term
                                           //printf("sum_read_len:%d += (db->read_len[i]: %d + 1)\n", sum_read_len, db->read_len[i]);
  }

  fprintf(stderr, "num of bases:\t%lu\n", sum_read_len - n_bam_rec);
  fprintf(stderr, "mean read len:\t%d\n", (sum_read_len - n_bam_rec) / n_bam_rec);
  fprintf(stderr, "max read len:\t%lu\n", max_read_len);

  // fprintf(stderr, "n_bam_rec %d, sum_read_len %d\n", n_bam_rec, sum_read_len);
  //form the temporary flattened array on host
  // read_host = (char *)malloc(sizeof(char) * sum_read_len);
  posix_memalign((void **)&read_host, AOCL_ALIGNMENT, sizeof(char) * sum_read_len);
  MALLOC_CHK(read_host);

  for (i = 0; i < n_bam_rec; i++)
  {
    ptr_t idx = read_ptr_host[i];
    strcpy(&read_host[idx], db->read[i]);
  }

  //now the events : need flattening
  //num events : need flattening
  //get the total size and create the pointers
  // #ifdef CUDA_PRE_MALLOC
  //     int32_t* n_events_host = core->cuda->n_events_host;
  //     ptr_t* event_ptr_host = core->cuda->event_ptr_host;
  // #else

  // n_events_host = (int32_t *)malloc(sizeof(int32_t) * n_bam_rec);
  posix_memalign((void **)&n_events_host, AOCL_ALIGNMENT, n_bam_rec * sizeof(int32_t));
  MALLOC_CHK(n_events_host);

  // event_ptr_host = (ptr_t *)malloc(sizeof(ptr_t) * n_bam_rec);
  posix_memalign((void **)&event_ptr_host, AOCL_ALIGNMENT, n_bam_rec * sizeof(ptr_t));
  MALLOC_CHK(event_ptr_host);
  // #endif

  sum_n_events = 0;
  for (i = 0; i < n_bam_rec; i++)
  {
    n_events_host[i] = db->et[i].n;
    event_ptr_host[i] = sum_n_events;
    sum_n_events += db->et[i].n;
  }

  //event table flatten
  //form the temporary flattened array on host
  // event_table_host = (event_t *)malloc(sizeof(event_t) * sum_n_events);
  posix_memalign((void **)&event_table_host, AOCL_ALIGNMENT, sum_n_events * sizeof(event_t));
  MALLOC_CHK(event_table_host);

  for (i = 0; i < n_bam_rec; i++)
  {
    ptr_t idx = event_ptr_host[i];
    memcpy(&event_table_host[idx], db->et[i].event,
           sizeof(event_t) * db->et[i].n);
  }

  // event_align_pairs_host =
  //     (AlignedPair *)malloc(2 * sum_n_events * sizeof(AlignedPair));
  posix_memalign((void **)&event_align_pairs_host, AOCL_ALIGNMENT, 2 * sum_n_events * sizeof(AlignedPair));
  MALLOC_CHK(event_align_pairs_host);

  core->align_cuda_preprocess += (realtime() - realtime1);

  /** Start OpenCL memory allocation**/
  realtime1 = realtime();

  //read_ptr
  if (core->opt.verbosity > 1)
    print_size("read_ptr array", n_bam_rec * sizeof(ptr_t));
  //cudaMalloc((void**)&read_ptr, n_bam_rec * sizeof(ptr_t));
  cl_mem read_ptr = clCreateBuffer(context, CL_MEM_READ_WRITE, n_bam_rec * sizeof(ptr_t), NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  //read_len
  if (core->opt.verbosity > 1)
    print_size("read_lens", n_bam_rec * sizeof(int32_t));
  //cudaMalloc((void**)&read_len, n_bam_rec * sizeof(int32_t));
  cl_mem read_len = clCreateBuffer(context, CL_MEM_READ_WRITE, n_bam_rec * sizeof(int32_t), NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  //n_events
  if (core->opt.verbosity > 1)
    print_size("n_events", n_bam_rec * sizeof(int32_t));
  //cudaMalloc((void**)&n_events, n_bam_rec * sizeof(int32_t))
  cl_mem n_events = clCreateBuffer(context, CL_MEM_READ_WRITE, n_bam_rec * sizeof(int32_t), NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  //event ptr
  if (core->opt.verbosity > 1)
    print_size("event ptr", n_bam_rec * sizeof(ptr_t));
  // cudaMalloc((void**)&event_ptr, n_bam_rec * sizeof(ptr_t));
  cl_mem event_ptr = clCreateBuffer(context, CL_MEM_READ_WRITE, n_bam_rec * sizeof(ptr_t), NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  //scalings : already linear
  if (core->opt.verbosity > 1)
    print_size("Scalings", n_bam_rec * sizeof(scalings_t));
  // cudaMalloc((void**)&scalings, n_bam_rec * sizeof(scalings_t));
  cl_mem scalings = clCreateBuffer(context, CL_MEM_READ_WRITE, n_bam_rec * sizeof(scalings_t), NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  //model : already linear
  if (core->opt.verbosity > 1)
    print_size("model", NUM_KMER * sizeof(model_t));
  //cudaMalloc((void**)&model, NUM_KMER * sizeof(model_t));
  cl_mem model = clCreateBuffer(context, CL_MEM_READ_WRITE, NUM_KMER * sizeof(model_t), NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  //sum_read_len
  if (core->opt.verbosity > 1)
    print_size("read array", sum_read_len * sizeof(char));
  // cudaMalloc((void**)&read, sum_read_len * sizeof(char)); //with null char
  cl_mem read = clCreateBuffer(context, CL_MEM_READ_WRITE, sum_read_len * sizeof(char), NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  //event_table
  if (core->opt.verbosity > 1)
    print_size("event table", sum_n_events * sizeof(event_t));
  // cudaMalloc((void**)&event_table, sum_n_events * sizeof(event_t));
  cl_mem event_table = clCreateBuffer(context, CL_MEM_READ_WRITE, sum_n_events * sizeof(event_t), NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  //model_kmer_cache
  if (core->opt.verbosity > 1)
    print_size("model kmer cache", sum_read_len * sizeof(model_t));
  // cudaMalloc((void**)&model_kmer_cache, sum_read_len * sizeof(model_t));
  cl_mem model_kmer_cache = clCreateBuffer(context, CL_MEM_READ_WRITE, sum_read_len * sizeof(model_t), NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  /**allocate output arrays**/

  if (core->opt.verbosity > 1)
    print_size("event align pairs", 2 * sum_n_events * sizeof(AlignedPair));
  // cudaMalloc((void**)&event_align_pairs,2 * sum_n_events *sizeof(AlignedPair)); //todo : need better huristic
  cl_mem event_align_pairs = clCreateBuffer(context, CL_MEM_READ_WRITE, 2 * sum_n_events * sizeof(AlignedPair), NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  if (core->opt.verbosity > 1)
    print_size("n_event_align_pairs", n_bam_rec * sizeof(int32_t));
  // cudaMalloc((void**)&n_event_align_pairs, n_bam_rec * sizeof(int32_t));
  cl_mem n_event_align_pairs = clCreateBuffer(context, CL_MEM_READ_WRITE, n_bam_rec * sizeof(int32_t), NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  // #endif
  //scratch arrays
  size_t sum_n_bands = sum_n_events + sum_read_len; //todo : can be optimised
  if (core->opt.verbosity > 1)
    print_size("bands", sizeof(float) * sum_n_bands * ALN_BANDWIDTH);
  // cudaMalloc((void**)&bands,sizeof(float) * sum_n_bands * ALN_BANDWIDTH);
  cl_mem bands = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(float) * sum_n_bands * ALN_BANDWIDTH, NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  if (core->opt.verbosity > 1)
    print_size("trace", sizeof(uint8_t) * sum_n_bands * ALN_BANDWIDTH);
  // cudaMalloc((void**)&trace, sizeof(uint8_t) * sum_n_bands * ALN_BANDWIDTH);
  cl_mem trace = clCreateBuffer(context, CL_MEM_READ_WRITE, sizeof(uint8_t) * sum_n_bands * ALN_BANDWIDTH, NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  //cudaMemset(trace, 0, sizeof(uint8_t) * sum_n_bands * ALN_BANDWIDTH); //initialise the trace array to 0
  uint8_t zeros[n_bam_rec];
  for (i = 0; i < n_bam_rec; i++)
  {
    zeros[i] = 0;
  }
  status = clEnqueueWriteBuffer(queue, trace, CL_TRUE, 0, n_bam_rec * sizeof(uint8_t), zeros, 0, NULL, &event_write_buffers);
  checkError(status, "Failed clEnqueueWriteBuffer");
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  host_to_device_transfer_time += (cl_double)(end - start) * (cl_double)(1e-06);

  if (core->opt.verbosity > 1)
    print_size("band_lower_left", sizeof(EventKmerPair) * sum_n_bands);
  // cudaMalloc((void**)&band_lower_left, sizeof(EventKmerPair)* sum_n_bands);
  cl_mem band_lower_left = clCreateBuffer(context, CL_MEM_READ_WRITE, sum_n_bands * sizeof(EventKmerPair), NULL, &status);
  checkError(status, "Failed clCreateBuffer");

  core->align_cuda_malloc += (realtime() - realtime1);

  /* cuda mem copys*/
  realtime1 = realtime();

  //cudaMemcpy(read_ptr, read_ptr_host, n_bam_rec * sizeof(ptr_t),cudaMemcpyHostToDevice);
  status = clEnqueueWriteBuffer(queue, read_ptr, CL_TRUE, 0, n_bam_rec * sizeof(ptr_t), read_ptr_host, 0, NULL, &event_write_buffers);
  checkError(status, "Failed clEnqueueWriteBuffer");
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  host_to_device_transfer_time += (cl_double)(end - start) * (cl_double)(1e-06);

  //cudaMemcpy(read, read_host, sum_read_len * sizeof(char), cudaMemcpyHostToDevice);
  status = clEnqueueWriteBuffer(queue, read, CL_TRUE, 0, sum_read_len * sizeof(char), read_host, 0, NULL, &event_write_buffers);
  checkError(status, "Failed clEnqueueWriteBuffer");
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  host_to_device_transfer_time += (cl_double)(end - start) * (cl_double)(1e-06);

  //read length : already linear hence direct copy
  // cudaMemcpy(read_len, db->read_len, n_bam_rec * sizeof(int32_t),cudaMemcpyHostToDevice);
  status = clEnqueueWriteBuffer(queue, read_len, CL_TRUE, 0, n_bam_rec * sizeof(int32_t), db->read_len, 0, NULL, &event_write_buffers);
  checkError(status, "Failed clEnqueueWriteBuffer");
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  host_to_device_transfer_time += (cl_double)(end - start) * (cl_double)(1e-06);

  // cudaMemcpy(n_events, n_events_host, n_bam_rec * sizeof(int32_t),cudaMemcpyHostToDevice);
  status = clEnqueueWriteBuffer(queue, n_events, CL_TRUE, 0, n_bam_rec * sizeof(int32_t), n_events_host, 0, NULL, &event_write_buffers);
  checkError(status, "Failed clEnqueueWriteBuffer");
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  host_to_device_transfer_time += (cl_double)(end - start) * (cl_double)(1e-06);

  // cudaMemcpy(event_ptr, event_ptr_host, n_bam_rec * sizeof(ptr_t),cudaMemcpyHostToDevice);
  status = clEnqueueWriteBuffer(queue, event_ptr, CL_TRUE, 0, n_bam_rec * sizeof(ptr_t), event_ptr_host, 0, NULL, &event_write_buffers);
  checkError(status, "Failed clEnqueueWriteBuffer");
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  host_to_device_transfer_time += (cl_double)(end - start) * (cl_double)(1e-06);

  // cudaMemcpy(event_table, event_table_host, sizeof(event_t) * sum_n_events,cudaMemcpyHostToDevice);
  status = clEnqueueWriteBuffer(queue, event_table, CL_TRUE, 0, sizeof(event_t) * sum_n_events, event_table_host, 0, NULL, &event_write_buffers);
  checkError(status, "Failed clEnqueueWriteBuffer");
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  host_to_device_transfer_time += (cl_double)(end - start) * (cl_double)(1e-06);

  //model : already linear //move to cuda_init
  // cudaMemcpy(model, core->model, NUM_KMER * sizeof(model_t), cudaMemcpyHostToDevice);
  status = clEnqueueWriteBuffer(queue, model, CL_TRUE, 0, NUM_KMER * sizeof(model_t), core->model, 0, NULL, &event_write_buffers);
  checkError(status, "Failed clEnqueueWriteBuffer");
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  host_to_device_transfer_time += (cl_double)(end - start) * (cl_double)(1e-06);
  //can be interleaved
  // cudaMemcpy(scalings, db->scalings, sizeof(scalings_t) * n_bam_rec, cudaMemcpyHostToDevice);
  status = clEnqueueWriteBuffer(queue, scalings, CL_TRUE, 0, sizeof(scalings_t) * n_bam_rec, db->scalings, 0, NULL, &event_write_buffers);
  checkError(status, "Failed clEnqueueWriteBuffer");
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_write_buffers, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  host_to_device_transfer_time += (cl_double)(end - start) * (cl_double)(1e-06);

  fprintf(stderr, "Data transfer time %.3f ms \n", host_to_device_transfer_time);
  core->align_cuda_memcpy += (realtime() - realtime1);

  realtime1 = realtime();

  /* blockpre == threads per block == local
      gridpre == num blocks
      global = gridpre .* blockpre
    */

  //******************************************************************************************************
  /*pre kernel*/
  //******************************************************************************************************

  // Set the kernel argument (argument 0)
  status = clSetKernelArg(align_kernel_pre_2d, 0, sizeof(cl_mem), &read);
  checkError(status, "Failed to set kernel args to align_kernel_pre_2d");

  status = clSetKernelArg(align_kernel_pre_2d, 1, sizeof(cl_mem), &read_len);
  checkError(status, "Failed to set kernel args to align_kernel_pre_2d");
  status = clSetKernelArg(align_kernel_pre_2d, 2, sizeof(cl_mem), &read_ptr);
  checkError(status, "Failed to set kernel args to align_kernel_pre_2d");
  status = clSetKernelArg(align_kernel_pre_2d, 3, sizeof(cl_mem), &n_events);
  checkError(status, "Failed to set kernel args to align_kernel_pre_2d");
  status = clSetKernelArg(align_kernel_pre_2d, 4, sizeof(cl_mem), &event_ptr);
  checkError(status, "Failed to set kernel args to align_kernel_pre_2d");
  status = clSetKernelArg(align_kernel_pre_2d, 5, sizeof(cl_mem), &model);
  checkError(status, "Failed to set kernel args to align_kernel_pre_2d");
  status = clSetKernelArg(align_kernel_pre_2d, 6, sizeof(int32_t), &n_bam_rec);
  checkError(status, "Failed to set kernel args to align_kernel_pre_2d");
  status = clSetKernelArg(align_kernel_pre_2d, 7, sizeof(cl_mem), &model_kmer_cache);
  checkError(status, "Failed to set kernel args to align_kernel_pre_2d");
  status = clSetKernelArg(align_kernel_pre_2d, 8, sizeof(cl_mem), &bands);
  checkError(status, "Failed to set kernel args to align_kernel_pre_2d");
  status = clSetKernelArg(align_kernel_pre_2d, 9, sizeof(cl_mem), &trace);
  checkError(status, "Failed to set kernel args to align_kernel_pre_2d");
  status = clSetKernelArg(align_kernel_pre_2d, 10, sizeof(cl_mem), &band_lower_left);
  checkError(status, "Failed to set kernel args to align_kernel_pre_2d");

  assert(BLOCK_LEN_BANDWIDTH >= ALN_BANDWIDTH);
  // dim3 gridpre(1,(db->n_bam_rec + BLOCK_LEN_READS - 1) / BLOCK_LEN_READS);
  // dim3 blockpre(BLOCK_LEN_BANDWIDTH,BLOCK_LEN_READS);
  const size_t gridpre[2] = {BLOCK_LEN_BANDWIDTH, (size_t)(db->n_bam_rec + BLOCK_LEN_READS - 1)}; //global
  const size_t blockpre[2] = {BLOCK_LEN_BANDWIDTH, BLOCK_LEN_READS};                              //local

  if (core->opt.verbosity > 1)
    fprintf(stderr, "grid %zu,%zu, block %zu,%zu\n", gridpre[0], gridpre[1], blockpre[0], blockpre[1]);

  //   align_kernel_pre_2d<<<gridpre, blockpre>>>( read,
  //       read_len, read_ptr, n_events,
  //       event_ptr, model, n_bam_rec, model_kmer_cache,bands,trace,band_lower_left);
  printf("Calling Pre kernel\n");
  cl_event event_pre;
  cl_event event_core;
  cl_event event_post;
  clEnqueueNDRangeKernel(queue, align_kernel_pre_2d, 2, NULL, gridpre, blockpre, 0, NULL, &event_pre);
  status = clFinish(queue);
  checkError(status, "Failed to finish");
  //********** Pre-Kernel execution time *************************

  clGetEventProfilingInfo(event_pre, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_pre, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  g_NDRangePureExecTimeMs = (cl_double)(end - start) * (cl_double)(1e-06);
  fprintf(stderr, "Pre-kernel execution time %.3f ms\n", g_NDRangePureExecTimeMs);

  // if (core->opt.verbosity > 1)
  //   fprintf(stderr, "[%s::%.3f*%.2f] align-pre kernel done\n", __func__, realtime() - realtime1, cputime() / (realtime() - realtime1));
  core->align_kernel_time += (realtime() - realtime1);
  core->align_pre_kernel_time += (realtime() - realtime1);

  realtime1 = realtime();

  //******************************************************************************************************
  /*core kernel*/
  //******************************************************************************************************

  // Set the kernel argument (argument 0)
  status = clSetKernelArg(align_kernel_core_2d_shm, 0, sizeof(cl_mem), &read_len);
  checkError(status, "Failed to set kernel args to align_kernel_core_2d_shm");
  status = clSetKernelArg(align_kernel_core_2d_shm, 1, sizeof(cl_mem), &read_ptr);
  checkError(status, "Failed to set kernel args to align_kernel_core_2d_shm");
  status = clSetKernelArg(align_kernel_core_2d_shm, 2, sizeof(cl_mem), &event_table);
  checkError(status, "Failed to set kernel args to align_kernel_core_2d_shm");
  status = clSetKernelArg(align_kernel_core_2d_shm, 3, sizeof(cl_mem), &n_events);
  checkError(status, "Failed to set kernel args to align_kernel_core_2d_shm");
  status = clSetKernelArg(align_kernel_core_2d_shm, 4, sizeof(cl_mem), &event_ptr);
  checkError(status, "Failed to set kernel args to align_kernel_core_2d_shm");
  status = clSetKernelArg(align_kernel_core_2d_shm, 5, sizeof(cl_mem), &scalings);
  checkError(status, "Failed to set kernel args to align_kernel_core_2d_shm");
  status = clSetKernelArg(align_kernel_core_2d_shm, 6, sizeof(int32_t), &n_bam_rec);
  checkError(status, "Failed to set kernel args to align_kernel_core_2d_shm");
  status = clSetKernelArg(align_kernel_core_2d_shm, 7, sizeof(cl_mem), &model_kmer_cache);
  checkError(status, "Failed to set kernel args to align_kernel_core_2d_shm");
  status = clSetKernelArg(align_kernel_core_2d_shm, 8, sizeof(cl_mem), &bands);
  checkError(status, "Failed to set kernel args to align_kernel_core_2d_shm");
  status = clSetKernelArg(align_kernel_core_2d_shm, 9, sizeof(cl_mem), &trace);
  checkError(status, "Failed to set kernel args to align_kernel_core_2d_shm");
  status = clSetKernelArg(align_kernel_core_2d_shm, 10, sizeof(cl_mem), &band_lower_left);
  checkError(status, "Failed to set kernel args to align_kernel_core_2d_shm");

  assert(BLOCK_LEN_BANDWIDTH >= ALN_BANDWIDTH);
  //dim3 grid1(1,(db->n_bam_rec + BLOCK_LEN_READS - 1) / BLOCK_LEN_READS);
  //dim3 block1(BLOCK_LEN_BANDWIDTH,BLOCK_LEN_READS);

  const size_t grid1[2] = {BLOCK_LEN_BANDWIDTH, (size_t)(db->n_bam_rec + BLOCK_LEN_READS - 1)}; //global
  const size_t block1[2] = {BLOCK_LEN_BANDWIDTH, BLOCK_LEN_READS};                              //local

  // const size_t grid1[3] = {1, 1, 1};
  // const size_t block1[3] = {1, 1, 1};

  if (core->opt.verbosity > 1)
    fprintf(stderr, "grid %zu,%zu, block %zu,%zu\n", grid1[0], grid1[1], block1[0], block1[1]);

  printf("Calling core kernel\n");
  //align_kernel_core_2d_shm<<<grid1, block1>>>(read_len, read_ptr, event_table, n_events,event_ptr, scalings, n_bam_rec, model_kmer_cache,bands,trace,band_lower_left );

  clEnqueueNDRangeKernel(queue, align_kernel_core_2d_shm, 2, NULL, grid1, block1, 0, NULL, &event_core);
  status = clFinish(queue);
  checkError(status, "Failed to finish");
  //********** Core-Kernel execution time *************************

  clGetEventProfilingInfo(event_core, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_core, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  //END-START gives you hints on kind of “pure HW execution time”
  //the resolution of the events is 1e-09 sec
  g_NDRangePureExecTimeMs = (cl_double)(end - start) * (cl_double)(1e-06);
  fprintf(stderr, "Core-kernel execution time %.3f ms\n", g_NDRangePureExecTimeMs);

  // if (core->opt.verbosity > 1)
  //   fprintf(stderr, "[%s::%.3f*%.2f] align-core kernel done\n", __func__,
  //           realtime() - realtime1, cputime() / (realtime() - realtime1));
  core->align_kernel_time += (realtime() - realtime1);
  core->align_core_kernel_time += (realtime() - realtime1);
  realtime1 = realtime();

  //******************************************************************************************************
  /*post kernel*/
  //******************************************************************************************************

  // Set the kernel argument (argument 0)
  status = clSetKernelArg(align_kernel_post, 0, sizeof(cl_mem), &event_align_pairs);
  checkError(status, "Failed to set kernel args to align_kernel_post");
  status = clSetKernelArg(align_kernel_post, 1, sizeof(cl_mem), &n_event_align_pairs);
  checkError(status, "Failed to set kernel args to align_kernel_post");
  status = clSetKernelArg(align_kernel_post, 2, sizeof(cl_mem), &read_len);
  checkError(status, "Failed to set kernel args to align_kernel_post");
  status = clSetKernelArg(align_kernel_post, 3, sizeof(cl_mem), &read_ptr);
  checkError(status, "Failed to set kernel args to align_kernel_post");
  status = clSetKernelArg(align_kernel_post, 4, sizeof(cl_mem), &event_table);
  checkError(status, "Failed to set kernel args to align_kernel_post");
  status = clSetKernelArg(align_kernel_post, 5, sizeof(cl_mem), &n_events);
  checkError(status, "Failed to set kernel args to align_kernel_post");
  status = clSetKernelArg(align_kernel_post, 6, sizeof(cl_mem), &event_ptr);
  checkError(status, "Failed to set kernel args to align_kernel_post");
  status = clSetKernelArg(align_kernel_post, 7, sizeof(int32_t), &n_bam_rec);
  checkError(status, "Failed to set kernel args to align_kernel_post");
  status = clSetKernelArg(align_kernel_post, 8, sizeof(cl_mem), &scalings);
  checkError(status, "Failed to set kernel args to align_kernel_post");
  status = clSetKernelArg(align_kernel_post, 9, sizeof(cl_mem), &model_kmer_cache);
  checkError(status, "Failed to set kernel args to align_kernel_post");
  status = clSetKernelArg(align_kernel_post, 10, sizeof(cl_mem), &bands);
  checkError(status, "Failed to set kernel args to align_kernel_post");
  status = clSetKernelArg(align_kernel_post, 11, sizeof(cl_mem), &trace);
  checkError(status, "Failed to set kernel args to align_kernel_post");
  status = clSetKernelArg(align_kernel_post, 12, sizeof(cl_mem), &band_lower_left);
  checkError(status, "Failed to set kernel args to align_kernel_post");

  // int32_t BLOCK_LEN = core->opt.cuda_block_size;
  int32_t BLOCK_LEN = 64;

  //dim3 gridpost((db->n_bam_rec + BLOCK_LEN - 1) / BLOCK_LEN);
  //dim3 blockpost(BLOCK_LEN);

  // const size_t gridpost[1] = {(size_t)(((db->n_bam_rec + BLOCK_LEN - 1) / BLOCK_LEN) * BLOCK_LEN)}; //global ***************check
  // const size_t blockpost[1] = {(size_t)BLOCK_LEN}; //global
  const size_t gridpost[1] = {(size_t)db->n_bam_rec};
  const size_t blockpost[1] = {1};

  if (core->opt.verbosity > 1)
    fprintf(stderr, "grid %zu, block %zu\n", gridpost[0], blockpost[0]);

#ifndef WARP_HACK
  // align_kernel_post<<<gridpost, blockpost>>>(event_align_pairs, n_event_align_pairs,
  //     read_len, read_ptr, event_table, n_events,
  //     event_ptr,scalings, n_bam_rec, model_kmer_cache,bands,trace,band_lower_left );
  printf("Calling post kernel. 'WARP_HACK' not set\n");
  clEnqueueNDRangeKernel(queue, align_kernel_post, 1, NULL, gridpost, blockpost, 0, NULL, &event_post);
#else
  assert(BLOCK_LEN >= 32);
  //dim3 grid1post((db->n_bam_rec + (BLOCK_LEN/32) - 1) / (BLOCK_LEN/32));
  const size_t grid1post[1] = {(db->n_bam_rec + (BLOCK_LEN / 32) - 1) / (BLOCK_LEN / 32)};
  //if(core->opt.verbosity>1) fprintf(stderr,"grid new %d\n",grid1post.x);
  if (core->opt.verbosity > 1)
    fprintf(stderr, "grid new %d\n", grid1post[0]);
  // align_kernel_post<<<grid1post, blockpost>>>(event_align_pairs, n_event_align_pairs,
  //     read_len, read_ptr, event_table, n_events,
  //     event_ptr, scalings, n_bam_rec, model_kmer_cache,bands,trace,band_lower_left );
  printf("Calling post kernel. 'WARP_HACK' set\n");
  clEnqueueNDRangeKernel(queue, align_kernel_post, 2, NULL, gridpost, blockpost, 0, NULL, &event_post);

#endif
  //cudaDeviceSynchronize();CUDA_CHK();
  status = clFinish(queue);
  checkError(status, "Failed to finish");
  //********** Post-Kernel execution time *************************

  clGetEventProfilingInfo(event_post, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_post, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  //END-START gives you hints on kind of “pure HW execution time”
  //the resolution of the events is 1e-09 sec
  g_NDRangePureExecTimeMs = (cl_double)(end - start) * (cl_double)(1e-06);
  fprintf(stderr, "Post-kernel execution time %.3f ms\n", g_NDRangePureExecTimeMs);

  // if (core->opt.verbosity > 1)
  //   fprintf(stderr, "[%s::%.3f*%.2f] align-post kernel done\n", __func__,
  //           realtime() - realtime1, cputime() / (realtime() - realtime1));
  core->align_kernel_time += (realtime() - realtime1);
  core->align_post_kernel_time += (realtime() - realtime1);

  realtime1 = realtime();

  //cudaMemcpy(db->n_event_align_pairs, n_event_align_pairs,n_bam_rec * sizeof(int32_t), cudaMemcpyDeviceToHost);
  status = clEnqueueReadBuffer(queue, n_event_align_pairs, CL_TRUE, 0, sizeof(int32_t) * n_bam_rec, db->n_event_align_pairs, 0, NULL, &event_read_buffers);
  // CUDA_CHK();
  checkError(status, "clEnqueueReadBuffer");
  clGetEventProfilingInfo(event_read_buffers, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_read_buffers, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  device_to_host_transfer_time += (cl_double)(end - start) * (cl_double)(1e-06);

  //cudaMemcpy(event_align_pairs_host, event_align_pairs,2 * sum_n_events * sizeof(AlignedPair), cudaMemcpyDeviceToHost);
  status = clEnqueueReadBuffer(queue, event_align_pairs, CL_TRUE, 0, 2 * sum_n_events * sizeof(AlignedPair), event_align_pairs_host, 0, NULL, &event_read_buffers);
  checkError(status, "clEnqueueReadBuffer");
  clGetEventProfilingInfo(event_read_buffers, CL_PROFILING_COMMAND_START, sizeof(cl_ulong), &start, NULL);
  clGetEventProfilingInfo(event_read_buffers, CL_PROFILING_COMMAND_END, sizeof(cl_ulong), &end, NULL);
  device_to_host_transfer_time += (cl_double)(end - start) * (cl_double)(1e-06);

  fprintf(stderr, "Data transfer time DEVICE to HOST %.3f ms \n", device_to_host_transfer_time);

  core->align_cuda_memcpy += (realtime() - realtime1);

  realtime1 = realtime();
#ifndef CUDA_PRE_MALLOC
  //cudaFree(read_ptr);
  clReleaseMemObject(read_ptr);
  //cudaFree(read_len);
  clReleaseMemObject(read_len);
  // cudaFree(n_events);
  clReleaseMemObject(n_events);
  // cudaFree(event_ptr);
  clReleaseMemObject(event_ptr);
  // cudaFree(model); //constant memory
  clReleaseMemObject(model);
  // cudaFree(scalings);
  clReleaseMemObject(scalings);
  // cudaFree(n_event_align_pairs);
  clReleaseMemObject(n_event_align_pairs);
#endif
  // cudaFree(read); //with null char
  clReleaseMemObject(read);
  // cudaFree(event_table);
  clReleaseMemObject(event_table);
  // cudaFree(event_align_pairs);
  clReleaseMemObject(event_align_pairs);
  // cudaFree(bands);
  clReleaseMemObject(bands);
  // cudaFree(trace);
  clReleaseMemObject(trace);
  // cudaFree(band_lower_left);
  clReleaseMemObject(band_lower_left);
  // cudaFree(model_kmer_cache);
  clReleaseMemObject(model_kmer_cache);

  core->align_cuda_malloc += (realtime() - realtime1);

  /** post work**/
  realtime1 = realtime();

  // printf("readpos:%d, refpos:%d\n",event_align_pairs_host[0].read_pos, event_align_pairs_host[0].ref_pos);

  //copy back
  for (i = 0; i < n_bam_rec; i++)
  {
    ptr_t idx = event_ptr_host[i];
    memcpy(db->event_align_pairs[i], &event_align_pairs_host[idx * 2], sizeof(AlignedPair) * db->n_event_align_pairs[i]);
  }

  //free the temp arrays on host
#ifndef CUDA_PRE_MALLOC
  free(read_ptr_host);
  free(n_events_host);
  free(event_ptr_host);
#endif
  free(read_host);
  free(event_table_host);
  free(event_align_pairs_host);

  core->align_cuda_postprocess += (realtime() - realtime1);

  cleanup();
}

/////// HELPER FUNCTIONS ///////

bool init()
{
  cl_int status;

  if (!setCwdToExeDir())
  {
    return false;
  }

  // Get the OpenCL platform.
  platform = findPlatform("Intel(R) FPGA SDK for OpenCL(TM)");
  if (platform == NULL)
  {
    printf("ERROR: Unable to find Intel(R) FPGA OpenCL platform.\n");
    return false;
  }

  // User-visible output - Platform information
  {
    char char_buffer[STRING_BUFFER_LEN];
    printf("Querying platform for info:\n");
    printf("==========================\n");
    clGetPlatformInfo(platform, CL_PLATFORM_NAME, STRING_BUFFER_LEN, char_buffer, NULL);
    printf("%-40s = %s\n", "CL_PLATFORM_NAME", char_buffer);
    clGetPlatformInfo(platform, CL_PLATFORM_VENDOR, STRING_BUFFER_LEN, char_buffer, NULL);
    printf("%-40s = %s\n", "CL_PLATFORM_VENDOR ", char_buffer);
    clGetPlatformInfo(platform, CL_PLATFORM_VERSION, STRING_BUFFER_LEN, char_buffer, NULL);
    printf("%-40s = %s\n\n", "CL_PLATFORM_VERSION ", char_buffer);
  }

  // Query the available OpenCL devices.
  scoped_array<cl_device_id> devices;
  cl_uint num_devices;

  devices.reset(getDevices(platform, CL_DEVICE_TYPE_ALL, &num_devices));

  // We'll just use the first device.
  device = devices[0];

  // Display some device information.
  display_device_info(device);

  // Create the context.
  context = clCreateContext(NULL, 1, &device, &oclContextCallback, NULL, &status);
  checkError(status, "Failed to create context");

  // Create the command queue.
  queue = clCreateCommandQueue(context, device, CL_QUEUE_PROFILING_ENABLE, &status);
  checkError(status, "Failed to create command queue");

  // Create the program.
  std::string binary_file1 = getBoardBinaryFile("pre", device);
  printf("Using AOCX: %s\n", binary_file1.c_str());
  program1 = createProgramFromBinary(context, binary_file1.c_str(), &device, 1);

  std::string binary_file2 = getBoardBinaryFile("core", device);
  printf("Using AOCX: %s\n", binary_file2.c_str());
  program2 = createProgramFromBinary(context, binary_file2.c_str(), &device, 1);

  std::string binary_file3 = getBoardBinaryFile("post", device);
  printf("Using AOCX: %s\n", binary_file3.c_str());
  program3 = createProgramFromBinary(context, binary_file3.c_str(), &device, 1);

  // Build the program that was just created.
  status = clBuildProgram(program1, 0, NULL, "", NULL, NULL);
  checkError(status, "Failed to build program");

  // Build the program that was just created.
  status = clBuildProgram(program2, 0, NULL, "", NULL, NULL);
  checkError(status, "Failed to build program");

  // Build the program that was just created.
  status = clBuildProgram(program3, 0, NULL, "", NULL, NULL);
  checkError(status, "Failed to build program");

  // Create the kernel - name passed in here must match kernel name in the
  // original CL file, that was compiled into an AOCX file using the AOC tool
  const char *kernel1_name = "align_kernel_pre_2d";      // Kernel name, as defined in the CL file
  const char *kernel2_name = "align_kernel_core_2d_shm"; // Kernel name, as defined in the CL file
  const char *kernel3_name = "align_kernel_post";        // Kernel name, as defined in the CL file

  align_kernel_pre_2d = clCreateKernel(program1, kernel1_name, &status);
  checkError(status, "Failed to pre create kernel");
  align_kernel_core_2d_shm = clCreateKernel(program2, kernel2_name, &status);
  checkError(status, "Failed to core create kernel");
  align_kernel_post = clCreateKernel(program3, kernel3_name, &status);
  checkError(status, "Failed to post create kernel");

  return true;
}

// Free the resources allocated during initialization
void cleanup()
{

  if (align_kernel_pre_2d)
  {
    clReleaseKernel(align_kernel_pre_2d);
  }
  if (align_kernel_core_2d_shm)
  {
    clReleaseKernel(align_kernel_core_2d_shm);
  }
  if (align_kernel_post)
  {
    clReleaseKernel(align_kernel_post);
  }
  if (program1)
  {
    clReleaseProgram(program1);
  }
  if (program2)
  {
    clReleaseProgram(program2);
  }
  if (program3)
  {
    clReleaseProgram(program3);
  }
  if (queue)
  {
    clReleaseCommandQueue(queue);
  }
  if (context)
  {
    clReleaseContext(context);
  }
}

// Helper functions to display parameters returned by OpenCL queries
static void device_info_ulong(cl_device_id device, cl_device_info param, const char *name)
{
  cl_ulong a = 99;
  clGetDeviceInfo(device, param, sizeof(cl_ulong), &a, NULL);
  printf("%-40s = %lu\n", name, a);
}
static void device_info_uint(cl_device_id device, cl_device_info param, const char *name)
{
  cl_uint a;
  clGetDeviceInfo(device, param, sizeof(cl_uint), &a, NULL);
  printf("%-40s = %u\n", name, a);
}
static void device_info_bool(cl_device_id device, cl_device_info param, const char *name)
{
  cl_bool a;
  clGetDeviceInfo(device, param, sizeof(cl_bool), &a, NULL);
  printf("%-40s = %s\n", name, (a ? "true" : "false"));
}
static void device_info_string(cl_device_id device, cl_device_info param, const char *name)
{
  char a[STRING_BUFFER_LEN];
  clGetDeviceInfo(device, param, STRING_BUFFER_LEN, &a, NULL);
  printf("%-40s = %s\n", name, a);
}

// Query and display OpenCL information on device and runtime environment
static void display_device_info(cl_device_id device)
{

  printf("Querying device for info:\n");
  printf("========================\n");
  device_info_string(device, CL_DEVICE_NAME, "CL_DEVICE_NAME");
  device_info_string(device, CL_DEVICE_VENDOR, "CL_DEVICE_VENDOR");
  device_info_uint(device, CL_DEVICE_VENDOR_ID, "CL_DEVICE_VENDOR_ID");
  device_info_string(device, CL_DEVICE_VERSION, "CL_DEVICE_VERSION");
  device_info_string(device, CL_DRIVER_VERSION, "CL_DRIVER_VERSION");
  device_info_uint(device, CL_DEVICE_ADDRESS_BITS, "CL_DEVICE_ADDRESS_BITS");
  device_info_bool(device, CL_DEVICE_AVAILABLE, "CL_DEVICE_AVAILABLE");
  device_info_bool(device, CL_DEVICE_ENDIAN_LITTLE, "CL_DEVICE_ENDIAN_LITTLE");
  device_info_ulong(device, CL_DEVICE_GLOBAL_MEM_CACHE_SIZE, "CL_DEVICE_GLOBAL_MEM_CACHE_SIZE");
  device_info_ulong(device, CL_DEVICE_GLOBAL_MEM_CACHELINE_SIZE, "CL_DEVICE_GLOBAL_MEM_CACHELINE_SIZE");
  device_info_ulong(device, CL_DEVICE_GLOBAL_MEM_SIZE, "CL_DEVICE_GLOBAL_MEM_SIZE");
  device_info_bool(device, CL_DEVICE_IMAGE_SUPPORT, "CL_DEVICE_IMAGE_SUPPORT");
  device_info_ulong(device, CL_DEVICE_LOCAL_MEM_SIZE, "CL_DEVICE_LOCAL_MEM_SIZE");
  device_info_ulong(device, CL_DEVICE_MAX_CLOCK_FREQUENCY, "CL_DEVICE_MAX_CLOCK_FREQUENCY");
  device_info_ulong(device, CL_DEVICE_MAX_COMPUTE_UNITS, "CL_DEVICE_MAX_COMPUTE_UNITS");
  device_info_ulong(device, CL_DEVICE_MAX_CONSTANT_ARGS, "CL_DEVICE_MAX_CONSTANT_ARGS");
  device_info_ulong(device, CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE, "CL_DEVICE_MAX_CONSTANT_BUFFER_SIZE");
  device_info_uint(device, CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS, "CL_DEVICE_MAX_WORK_ITEM_DIMENSIONS");
  device_info_uint(device, CL_DEVICE_MEM_BASE_ADDR_ALIGN, "CL_DEVICE_MEM_BASE_ADDR_ALIGN");
  device_info_uint(device, CL_DEVICE_MIN_DATA_TYPE_ALIGN_SIZE, "CL_DEVICE_MIN_DATA_TYPE_ALIGN_SIZE");
  device_info_uint(device, CL_DEVICE_PREFERRED_VECTOR_WIDTH_CHAR, "CL_DEVICE_PREFERRED_VECTOR_WIDTH_CHAR");
  device_info_uint(device, CL_DEVICE_PREFERRED_VECTOR_WIDTH_SHORT, "CL_DEVICE_PREFERRED_VECTOR_WIDTH_SHORT");
  device_info_uint(device, CL_DEVICE_PREFERRED_VECTOR_WIDTH_INT, "CL_DEVICE_PREFERRED_VECTOR_WIDTH_INT");
  device_info_uint(device, CL_DEVICE_PREFERRED_VECTOR_WIDTH_LONG, "CL_DEVICE_PREFERRED_VECTOR_WIDTH_LONG");
  device_info_uint(device, CL_DEVICE_PREFERRED_VECTOR_WIDTH_FLOAT, "CL_DEVICE_PREFERRED_VECTOR_WIDTH_FLOAT");
  device_info_uint(device, CL_DEVICE_PREFERRED_VECTOR_WIDTH_DOUBLE, "CL_DEVICE_PREFERRED_VECTOR_WIDTH_DOUBLE");

  {
    cl_command_queue_properties ccp;
    clGetDeviceInfo(device, CL_DEVICE_QUEUE_PROPERTIES, sizeof(cl_command_queue_properties), &ccp, NULL);
    printf("%-40s = %s\n", "Command queue out of order? ", ((ccp & CL_QUEUE_OUT_OF_ORDER_EXEC_MODE_ENABLE) ? "true" : "false"));
    printf("%-40s = %s\n", "Command queue profiling enabled? ", ((ccp & CL_QUEUE_PROFILING_ENABLE) ? "true" : "false"));
  }
}

#endif