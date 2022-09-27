//! functions that run on the master node of a server
#include "p7_config.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>


#ifdef HAVE_MPI
#include <mpi.h>
#include "esl_mpi.h"
#endif /*HAVE_MPI*/
#include <setjmp.h>
#include <sys/socket.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>     /* On FreeBSD, you need netinet/in.h for struct sockaddr_in            */
#endif                      /* On OpenBSD, netinet/in.h is required for (must precede) arpa/inet.h */
#include <arpa/inet.h>
#include <syslog.h>
#include <assert.h>
#include <errno.h>
#ifndef HMMER_THREADS
#error "Program requires pthreads be enabled."
#endif /*HMMER_THREADS*/
#include "easel.h"
#include "esl_alphabet.h"
#include "esl_getopts.h"
#include "esl_sq.h"
#include "esl_sqio.h"
#include "esl_stack.h"
#include "esl_stopwatch.h"
#include "esl_threads.h"
#include "hmmer.h"
#include "hmmserver.h"
#include "master_node.h"
#include "shard.h"
#include "worker_node.h"
#include "hmmpgmd.h"
#include "hmmpgmd_shard.h"

#define MAX_BUFFER   4096
#include <unistd.h> // for testing

/* Functions to communicate with the client via sockets.  Taken from the original hmmpgmd*/
typedef struct {
  int             sock_fd;
  char            ip_addr[64];

  ESL_STACK      *cmdstack;	/* stack of commands that clients want done */
} CLIENTSIDE_ARGS;


typedef struct {
  HMMD_SEARCH_STATS   stats;
  HMMD_SEARCH_STATUS  status;
  P7_HIT           **hits;
  int                 nhits;
  int                 db_inx;
  int                 db_cnt;
  int                 errors;
} SEARCH_RESULTS;



static void
init_results(SEARCH_RESULTS *results)
{
  results->status.status     = eslOK;
  results->status.msg_size   = 0;

  results->stats.nhits       = 0;
  results->stats.nreported   = 0;
  results->stats.nincluded   = 0;

  results->stats.nmodels     = 0;
  results->stats.nnodes      = 0;
  results->stats.nseqs       = 0;
  results->stats.nres        = 0;
  results->stats.n_past_msv  = 0;
  results->stats.n_past_bias = 0;
  results->stats.n_past_vit  = 0;
  results->stats.n_past_fwd  = 0;
  results->stats.Z           = 0;

  results->hits              = NULL;
  results->nhits             = 0;
  results->db_inx            = 0;
  results->db_cnt            = 0;
  results->errors            = 0;
}

// Qsort comparison function to sort a list of pointers to P7_HITs
static int
hit_sorter2(const void *p1, const void *p2)
{
  int cmp;

  const P7_HIT *h1 = *((P7_HIT **) p1);
  const P7_HIT *h2 = *((P7_HIT **) p2);

  cmp  = (h1->sortkey < h2->sortkey);
  cmp -= (h1->sortkey > h2->sortkey);

  return cmp;
}

static void
forward_results(QUEUE_DATA_SHARD *query, SEARCH_RESULTS *results)
{
  P7_TOPHITS         th;
  P7_PIPELINE        *pli   = NULL;
  P7_DOMAIN         **dcl   = NULL;
  P7_HIT             *hits  = NULL;
  int fd;
  int n;
  uint8_t **buf, **buf2, **buf3, *buf_ptr, *buf2_ptr, *buf3_ptr;
  uint32_t nalloc, nalloc2, nalloc3, buf_offset, buf_offset2, buf_offset3;
  enum p7_pipemodes_e mode;

  // Initialize these pointers-to-pointers that we'll use for sending data
  buf_ptr = NULL;
  buf = &(buf_ptr);
  buf2_ptr = NULL;
  buf2 = &(buf2_ptr);
  buf3_ptr = NULL;
  buf3 = &(buf3_ptr);
  fd    = query->sock;

  if (query->cmd_type == HMMD_CMD_SEARCH) mode = p7_SEARCH_SEQS;
  else                                    mode = p7_SCAN_MODELS;
    
  /* sort the hits and apply score and E-value thresholds */
  if (results->nhits > 0) {
    if(results->stats.hit_offsets != NULL){
      if ((results->stats.hit_offsets = realloc(results->stats.hit_offsets, results->stats.nhits * sizeof(uint64_t))) == NULL) LOG_FATAL_MSG("malloc", errno);
    }
    else{
      if ((results->stats.hit_offsets = malloc(results->stats.nhits * sizeof(uint64_t))) == NULL) LOG_FATAL_MSG("malloc", errno);
    }

    // sort the hits 
    qsort(results->hits, results->stats.nhits, sizeof(P7_HIT *), hit_sorter2);

    th.unsrt     = NULL;
    th.N         = results->stats.nhits;
    th.nreported = 0;
    th.nincluded = 0;
    th.is_sorted_by_sortkey = 0;
    th.is_sorted_by_seqidx  = 0;
      
    pli = p7_pipeline_Create(query->opts, 100, 100, FALSE, mode);
    pli->nmodels     = results->stats.nmodels;
    pli->nnodes      = results->stats.nnodes;
    pli->nseqs       = results->stats.nseqs;
    pli->nres        = results->stats.nres;
    pli->n_past_msv  = results->stats.n_past_msv;
    pli->n_past_bias = results->stats.n_past_bias;
    pli->n_past_vit  = results->stats.n_past_vit;
    pli->n_past_fwd  = results->stats.n_past_fwd;

    pli->Z           = results->stats.Z;
    pli->domZ        = results->stats.domZ;
    pli->Z_setby     = results->stats.Z_setby;
    pli->domZ_setby  = results->stats.domZ_setby;


    th.hit = results->hits;

    p7_tophits_Threshold(&th, pli);

    /* after the top hits thresholds are checked, the number of sequences
     * and domains to be reported can change. */
    results->stats.nreported = th.nreported;
    results->stats.nincluded = th.nincluded;
    results->stats.domZ      = pli->domZ;
    results->stats.Z         = pli->Z;
  }

  /* Build the buffers of serialized results we'll send back to the client.  
     Use three buffers, one for each object, because we need to build them in reverse order.
     We need to serialize the hits to build the hits_offset array in HMMD_SEARCH_STATS.
     We need the length of the serialized hits and HMMD_SEARCH_STATS objects to fill out the msg_size
     field in status, but we want to send status, then stats, then hits */

  nalloc = 0;
  buf_offset = 0;

  // First, the buffer of hits
  for(int i =0; i< results->stats.nhits; i++){
   
    results->stats.hit_offsets[i] = buf_offset;
    if(p7_hit_Serialize(results->hits[i], buf, &buf_offset, &nalloc) != eslOK){
      LOG_FATAL_MSG("Serializing P7_HIT failed", errno);
    }

  }
  if(results->stats.nhits == 0){
    results->stats.hit_offsets = NULL;
  }

  // Second, the buffer with the HMMD_SEARCH_STATS object

  buf_offset2 = 0;
  nalloc2 = 0; 
  if(p7_hmmd_search_stats_Serialize(&(results->stats), buf2, &buf_offset2, &nalloc2) != eslOK){
    LOG_FATAL_MSG("Serializing HMMD_SEARCH_STATS failed", errno);
  }

  results->status.msg_size = buf_offset + buf_offset2; // set size of second message
  
  // Third, the buffer with the HMMD_SEARCH_STATUS object
  buf_offset3 = 0;
  nalloc3 = 0;
  if(hmmd_search_status_Serialize(&(results->status), buf3, &buf_offset3, &nalloc3) != eslOK){
    LOG_FATAL_MSG("Serializing HMMD_SEARCH_STATUS failed", errno);
  }

  // Now, send the buffers in the reverse of the order they were built
  /* send back a successful status message */
  n = buf_offset3;

  if (writen(fd, buf3_ptr, n) != n) {
    p7_syslog(LOG_ERR,"[%s:%d] - writing %s error %d - %s\n", __FILE__, __LINE__, query->ip_addr, errno, strerror(errno));
    goto CLEAR;
  }

  // and the stats object
  n=buf_offset2;

  if (writen(fd, buf2_ptr, n) != n) {
    p7_syslog(LOG_ERR,"[%s:%d] - writing %s error %d - %s\n", __FILE__, __LINE__, query->ip_addr, errno, strerror(errno));
    goto CLEAR;
  }
  printf("%p\n", results->hits[1]);
  // and finally the hits 
  n=buf_offset;

  if (writen(fd, buf_ptr, n) != n) {
    p7_syslog(LOG_ERR,"[%s:%d] - writing %s error %d - %s\n", __FILE__, __LINE__, query->ip_addr, errno, strerror(errno));
    goto CLEAR;
  }
  printf("Results for %s (%d) sent %" PRId64 " bytes\n", query->ip_addr, fd, results->status.msg_size);
  printf("Hits:%"PRId64 "  reported:%" PRId64 "  included:%"PRId64 "\n", results->stats.nhits, results->stats.nreported, results->stats.nincluded);
  fflush(stdout);

 CLEAR:
  // don't need to free the actual hits -- they'll get cleaned up when the tophits object they came from
  // gets destroyed
  free(results->hits);
  results->hits = NULL;

  if (pli)  p7_pipeline_Destroy(pli);
  if (hits) free(hits);
  if (dcl)  free(dcl);
  if(buf_ptr != NULL){
    free(buf_ptr);
  }
  if(buf2_ptr != NULL){
    free(buf2_ptr);
  }
  if(buf3_ptr != NULL){
    free(buf3_ptr);
  }
  if(results->stats.hit_offsets != NULL){
    free(results->stats.hit_offsets);
  }
  init_results(results);
  return;
}


static void
free_QueueData_shard(QUEUE_DATA_SHARD *data)
{
  /* free the query data */
  esl_getopts_Destroy(data->opts);

  if (data->abc != NULL) esl_alphabet_Destroy(data->abc);
  if (data->hmm != NULL) p7_hmm_Destroy(data->hmm);
  if (data->seq != NULL) esl_sq_Destroy(data->seq);
  if (data->cmd != NULL) free(data->cmd);
  memset(data, 0, sizeof(*data));
  free(data);
}

static void print_client_msg(int fd, int status, char *format, va_list ap)
{
  uint32_t nalloc =0;
  uint32_t buf_offset = 0;
  uint8_t *buf = NULL;
  char  ebuf[512];

  HMMD_SEARCH_STATUS s;

  memset(&s, 0, sizeof(HMMD_SEARCH_STATUS));

  s.status   = status;
  s.msg_size = vsnprintf(ebuf, sizeof(ebuf), format, ap) +1; /* +1 because we send the \0 */

  p7_syslog(LOG_ERR, ebuf);

  if(hmmd_search_status_Serialize(&s, &buf, &buf_offset, &nalloc) != eslOK){
    LOG_FATAL_MSG("Serializing HMMD_SEARCH_STATUS failed", errno);
  }
  /* send back an unsuccessful status message */

  if (writen(fd, buf, buf_offset) != buf_offset) {
    p7_syslog(LOG_ERR,"[%s:%d] - writing (%d) error %d - %s\n", __FILE__, __LINE__, fd, errno, strerror(errno));
    return;
  }
  if (writen(fd, ebuf, s.msg_size) != s.msg_size)  {
    p7_syslog(LOG_ERR,"[%s:%d] - writing (%d) error %d - %s\n", __FILE__, __LINE__, fd, errno, strerror(errno));
    return;
  }

  free(buf);
}

static void
client_msg(int fd, int status, char *format, ...)
{
  va_list ap;

  va_start(ap, format);
  print_client_msg(fd, status, format, ap);
  va_end(ap);
}

static void
client_msg_longjmp(int fd, int status, jmp_buf *env, char *format, ...)
{
  va_list ap;

  va_start(ap, format);
  print_client_msg(fd, status, format, ap);
  va_end(ap);

  longjmp(*env, 1);
}


static void
process_ServerCmd(char *ptr, CLIENTSIDE_ARGS *data)
{
  QUEUE_DATA_SHARD    *parms    = NULL;     /* cmd to queue           */
  HMMD_COMMAND_SHARD  *cmd      = NULL;     /* parsed cmd to process  */
  int            fd       = data->sock_fd;
  ESL_STACK     *cmdstack = data->cmdstack;
  char          *s;
  time_t         date;
  char           timestamp[32];

  /* skip leading white spaces */
  ++ptr;
  while (*ptr == ' ' || *ptr == '\t') ++ptr;

  /* skip to the end of the line */
  s = ptr;
  while (*s && (*s != '\n' && *s != '\r')) ++s;
  *s = 0;

  /* process the different commands */
  s = strsep(&ptr, " \t");
  if (strcmp(s, "shutdown") == 0) 
    {
      if ((cmd = malloc(sizeof(HMMD_HEADER))) == NULL) LOG_FATAL_MSG("malloc", errno);
      memset(cmd, 0, sizeof(HMMD_HEADER)); /* avoid uninit bytes & valgrind bitching. Remove, if we ever serialize structs correctly. */
      cmd->hdr.length  = 0;
      cmd->hdr.command = HMMD_CMD_SHUTDOWN;
    } 
  else 
    {
      client_msg(fd, eslEINVAL, "Unknown command %s\n", s);
      return;
    }

  if ((parms = malloc(sizeof(QUEUE_DATA_SHARD))) == NULL) LOG_FATAL_MSG("malloc", errno);
  memset(parms, 0, sizeof(QUEUE_DATA_SHARD)); /* avoid valgrind bitches about uninit bytes; remove if structs are serialized properly */

  parms->hmm  = NULL;
  parms->seq  = NULL;
  parms->abc  = NULL;
  parms->opts = NULL;
  parms->dbx  = -1;
  parms->cmd  = cmd;

  strcpy(parms->ip_addr, data->ip_addr);
  parms->sock       = fd;
  parms->cmd_type   = cmd->hdr.command;
  parms->query_type = 0;

  date = time(NULL);
  ctime_r(&date, timestamp);
  printf("\n%s", timestamp);	/* note ctime_r() leaves \n on end of timestamp */
  printf("Queuing command %d from %s (%d)\n", cmd->hdr.command, parms->ip_addr, parms->sock);
  fflush(stdout);

  esl_stack_PPush(cmdstack, parms);
}
static int
clientside_loop(CLIENTSIDE_ARGS *data)
{
  int                status;

  char              *ptr;
  char              *buffer;
  char               opt_str[MAX_BUFFER];

  int                dbx;
  int                buf_size;
  int                remaining;
  int                amount;
  int                eod;
  int                n;

  P7_HMM            *hmm     = NULL;     /* query HMM                      */
  ESL_SQ            *seq     = NULL;     /* query sequence                 */
  ESL_SCOREMATRIX   *sco     = NULL;     /* scoring matrix                 */
  P7_HMMFILE        *hfp     = NULL;
  ESL_ALPHABET      *abc     = NULL;     /* digital alphabet               */
  ESL_GETOPTS       *opts    = NULL;     /* search specific options        */
  HMMD_COMMAND_SHARD      *cmd     = NULL;     /* search cmd to send to workers  */

  ESL_STACK         *cmdstack = data->cmdstack;
  QUEUE_DATA_SHARD        *parms;
  jmp_buf            jmp_env;
  time_t             date;
  char               timestamp[32];

  buf_size = MAX_BUFFER;
  if ((buffer  = malloc(buf_size))   == NULL) LOG_FATAL_MSG("malloc", errno);
  ptr = buffer;
  remaining = buf_size;
  amount = 0;

  eod = 0;
  while (!eod) {
    int   l;
    char *s;

    /* Receive message from client */
    if ((n = read(data->sock_fd, ptr, remaining)) < 0) {
      p7_syslog(LOG_ERR,"[%s:%d] - reading %s error %d - %s\n", __FILE__, __LINE__, data->ip_addr, errno, strerror(errno));
      return 1;
    }

    if (n == 0) return 1;

    ptr += n;
    amount += n;
    remaining -= n;

    /* scan backwards till we hit the start of the line */
    l = amount;
    s = ptr - 1;
    while (l-- > 0 && (*s == '\n' || *s == '\r')) --s;
    while (l-- > 0 && (*s != '\n' && *s != '\r')) --s;
    eod = (amount > 1 && *(s + 1) == '/' && *(s + 2) == '/' );

    /* if the buffer is full, make it larger */
    if (!eod && remaining == 0) {
      if ((buffer = realloc(buffer, buf_size * 2)) == NULL) LOG_FATAL_MSG("realloc", errno);
      ptr = buffer + buf_size;
      remaining = buf_size;
      buf_size *= 2;
    }
  }

  /* zero terminate the buffer */
  if (remaining == 0) {
    if ((buffer = realloc(buffer, buf_size + 1)) == NULL) LOG_FATAL_MSG("realloc", errno);
    ptr = buffer + buf_size;
  }
  *ptr = 0;

  /* skip all leading white spaces */
  ptr = buffer;
  while (*ptr && isspace(*ptr)) ++ptr;

  opt_str[0] = 0;
  if (*ptr == '!') {
    process_ServerCmd(ptr, data);
    free(buffer);
    return 0;
  } else if (*ptr == '@') {
    char *s = ++ptr;

    /* skip to the end of the line */
    while (*ptr && (*ptr != '\n' && *ptr != '\r')) ++ptr;
    *ptr++ = 0;

    /* create a commandline string with dummy program name for
     * the esl_opt_ProcessSpoof() function to parse.
     */
    snprintf(opt_str, sizeof(opt_str), "hmmpgmd %s\n", s);

    /* skip remaining white spaces */
    while (*ptr && isspace(*ptr)) ++ptr;
  } else {
    client_msg(data->sock_fd, eslEFORMAT, "Missing options string");
    free(buffer);
    return 0;
  }

  if (strncmp(ptr, "//", 2) == 0) {
    client_msg(data->sock_fd, eslEFORMAT, "Missing search sequence/hmm");
    free(buffer);
    return 0;
  }

  if (!setjmp(jmp_env)) {
    dbx = 0;
    
    status = process_searchopts(data->sock_fd, opt_str, &opts);
    if (status != eslOK) {
      client_msg_longjmp(data->sock_fd, status, &jmp_env, "Failed to parse options string: %s", opts->errbuf);
    }

    /* the options string can handle an optional database */
    if (esl_opt_ArgNumber(opts) > 0) {
      client_msg_longjmp(data->sock_fd, status, &jmp_env, "Incorrect number of command line arguments.");
    }

    if (esl_opt_IsUsed(opts, "--seqdb")) {
      dbx = esl_opt_GetInteger(opts, "--seqdb");
    } else if (esl_opt_IsUsed(opts, "--hmmdb")) {
      dbx = esl_opt_GetInteger(opts, "--hmmdb");
    } else {
      client_msg_longjmp(data->sock_fd, eslEINVAL, &jmp_env, "No search database specified, --seqdb or --hmmdb.");
    }


    abc = esl_alphabet_Create(eslAMINO);
    seq = NULL;
    hmm = NULL;

    if (*ptr == '>') {
      /* try to parse the input buffer as a FASTA sequence */
      seq = esl_sq_CreateDigital(abc);
      /* try to parse the input buffer as a FASTA sequence */
      status = esl_sqio_Parse(ptr, strlen(ptr), seq, eslSQFILE_DAEMON);
      if (status != eslOK) client_msg_longjmp(data->sock_fd, status, &jmp_env, "Error parsing FASTA sequence");
      if (seq->n < 1) client_msg_longjmp(data->sock_fd, eslEFORMAT, &jmp_env, "Error zero length FASTA sequence");

    } else if (strncmp(ptr, "HMM", 3) == 0) {
      if (esl_opt_IsUsed(opts, "--hmmdb")) {
        client_msg_longjmp(data->sock_fd, status, &jmp_env, "A HMM cannot be used to search a hmm database");
      }

      /* try to parse the buffer as an hmm */
      status = p7_hmmfile_OpenBuffer(ptr, strlen(ptr), &hfp);
      if (status != eslOK) client_msg_longjmp(data->sock_fd, status, &jmp_env, "Failed to open query hmm buffer");

      status = p7_hmmfile_Read(hfp, &abc,  &hmm);
      if (status != eslOK) client_msg_longjmp(data->sock_fd, status, &jmp_env, "Error reading query hmm: %s", hfp->errbuf);

      p7_hmmfile_Close(hfp);

    } else {
      /* no idea what we are trying to parse */
      client_msg_longjmp(data->sock_fd, eslEFORMAT, &jmp_env, "Unknown query sequence/hmm format");
    }
  } else {
    /* an error occured some where, so try to clean up */
    if (opts != NULL) esl_getopts_Destroy(opts);
    if (abc  != NULL) esl_alphabet_Destroy(abc);
    if (hmm  != NULL) p7_hmm_Destroy(hmm);
    if (seq  != NULL) esl_sq_Destroy(seq);
    if (sco  != NULL) esl_scorematrix_Destroy(sco);

    free(buffer);
    return 0;
  }

  if ((parms = malloc(sizeof(QUEUE_DATA_SHARD))) == NULL) LOG_FATAL_MSG("malloc", errno);

  /* build the search structure that will be sent to all the workers */
  n = sizeof(HMMD_COMMAND_SHARD);
  n = n + strlen(opt_str) + 1;

  if (seq != NULL) {
    n = n + strlen(seq->name) + 1;
    n = n + strlen(seq->desc) + 1;
    n = n + seq->n + 2;
  } else {
    n = n + sizeof(P7_HMM);
    n = n + sizeof(float) * (hmm->M + 1) * p7H_NTRANSITIONS;
    n = n + sizeof(float) * (hmm->M + 1) * abc->K;
    n = n + sizeof(float) * (hmm->M + 1) * abc->K;
    if (hmm->name   != NULL)    n = n + strlen(hmm->name) + 1;
    if (hmm->acc    != NULL)    n = n + strlen(hmm->acc)  + 1;
    if (hmm->desc   != NULL)    n = n + strlen(hmm->desc) + 1;
    if (hmm->flags & p7H_RF)    n = n + hmm->M + 2;
    if (hmm->flags & p7H_MMASK) n = n + hmm->M + 2;
    if (hmm->flags & p7H_CONS)  n = n + hmm->M + 2;
    if (hmm->flags & p7H_CS)    n = n + hmm->M + 2;
    if (hmm->flags & p7H_CA)    n = n + hmm->M + 2;
    if (hmm->flags & p7H_MAP)   n = n + sizeof(int) * (hmm->M + 1);
  }

  if ((cmd = malloc(n)) == NULL) LOG_FATAL_MSG("malloc", errno);
  memset(cmd, 0, n);		/* silence valgrind bitching about uninit bytes; remove if we ever serialize structs properly */
  cmd->hdr.length       = n - sizeof(HMMD_HEADER);
  cmd->hdr.command      = (esl_opt_IsUsed(opts, "--seqdb")) ? HMMD_CMD_SEARCH : HMMD_CMD_SCAN;
  cmd->srch.db_inx      = dbx - 1;   /* the program indexes databases 0 .. n-1 */
  cmd->srch.opts_length = strlen(opt_str) + 1;

  ptr = cmd->srch.data;

  memcpy(ptr, opt_str, cmd->srch.opts_length);
  ptr += cmd->srch.opts_length;
  
  if (seq != NULL) {
    cmd->srch.query_type   = HMMD_SEQUENCE;
    cmd->srch.query_length = seq->n + 2;

    n = strlen(seq->name) + 1;
    memcpy(ptr, seq->name, n);
    ptr += n;

    n = strlen(seq->desc) + 1;
    memcpy(ptr, seq->desc, n);
    ptr += n;

    n = seq->n + 2;
    memcpy(ptr, seq->dsq, n);
    ptr += n;
  } else {
    cmd->srch.query_type   = HMMD_HMM;
    cmd->srch.query_length = hmm->M;

    n = sizeof(P7_HMM);
    memcpy(ptr, hmm, n);
    ptr += n;

    n = sizeof(float) * (hmm->M + 1) * p7H_NTRANSITIONS;
    memcpy(ptr, *hmm->t, n);
    ptr += n;

    n = sizeof(float) * (hmm->M + 1) * abc->K;
    memcpy(ptr, *hmm->mat, n);
    ptr += n;
    memcpy(ptr, *hmm->ins, n);
    ptr += n;

    if (hmm->name) { n = strlen(hmm->name) + 1;  memcpy(ptr, hmm->name, n);  ptr += n; }
    if (hmm->acc)  { n = strlen(hmm->acc)  + 1;  memcpy(ptr, hmm->acc, n);   ptr += n; }
    if (hmm->desc) { n = strlen(hmm->desc) + 1;  memcpy(ptr, hmm->desc, n);  ptr += n; }

    n = hmm->M + 2;
    if (hmm->flags & p7H_RF)    { memcpy(ptr, hmm->rf,        n); ptr += n; }
    if (hmm->flags & p7H_MMASK) { memcpy(ptr, hmm->mm,        n); ptr += n; }
    if (hmm->flags & p7H_CONS)  { memcpy(ptr, hmm->consensus, n); ptr += n; }
    if (hmm->flags & p7H_CS)    { memcpy(ptr, hmm->cs,        n); ptr += n; }
    if (hmm->flags & p7H_CA)    { memcpy(ptr, hmm->ca,        n); ptr += n; }

    if (hmm->flags & p7H_MAP) {
      n = sizeof(int) * (hmm->M + 1);
      memcpy(ptr, hmm->map, n);
      ptr += n;
    }
  }

  parms->hmm  = hmm;
  parms->seq  = seq;
  parms->abc  = abc;
  parms->opts = opts;
  parms->dbx  = dbx - 1;
  parms->cmd  = cmd;

  strcpy(parms->ip_addr, data->ip_addr);
  parms->sock       = data->sock_fd;
  parms->cmd_type   = cmd->hdr.command;
  parms->query_type = (seq != NULL) ? HMMD_SEQUENCE : HMMD_HMM;

  date = time(NULL);
  ctime_r(&date, timestamp);
  printf("\n%s", timestamp);	/* note ctime_r() leaves \n on end of timestamp */

  if (parms->seq != NULL) {
    printf("Queuing %s %s from %s (%d)\n", (cmd->hdr.command == HMMD_CMD_SEARCH) ? "search" : "scan", parms->seq->name, parms->ip_addr, parms->sock);
  } else {
    printf("Queuing hmm %s from %s (%d)\n", parms->hmm->name, parms->ip_addr, parms->sock);
  }
  printf("%s", opt_str);	/* note opt_str already has trailing \n */
  fflush(stdout);

  esl_stack_PPush(cmdstack, parms);

  free(buffer);
  return 0;
}



/* discard_function()
 * function handed to esl_stack_DiscardSelected() to remove
 * all commands in the stack that are associated with a
 * particular client socket, because we're closing that
 * client down. Prototype to this is dictate by the generalized
 * interface to esl_stack_DiscardSelected().
 */
static int
discard_function(void *elemp, void *args)
{
  QUEUE_DATA_SHARD  *elem = (QUEUE_DATA_SHARD *) elemp;
  int          fd   = * (int *) args;

  if (elem->sock == fd) 
    {
      free_QueueData_shard(elem);
      return TRUE;
    }
  return FALSE;
}


static void *
clientside_thread(void *arg)
{
  int              eof;
  CLIENTSIDE_ARGS *data = (CLIENTSIDE_ARGS *)arg;

  /* Guarantees that thread resources are deallocated upon return */
  pthread_detach(pthread_self()); 

  eof = 0;
  while (!eof) {
    eof = clientside_loop(data);
  }

  /* remove any commands in stack associated with this client's socket */
  esl_stack_DiscardSelected(data->cmdstack, discard_function, &(data->sock_fd));

  printf("Closing %s (%d)\n", data->ip_addr, data->sock_fd);
  fflush(stdout);

  close(data->sock_fd);
  free(data);

  pthread_exit(NULL);
}

static void *
client_comm_thread(void *arg)
{
  int                  n;
  int                  fd;
  int                  addrlen;
  pthread_t            thread_id;

  struct sockaddr_in   addr;

  CLIENTSIDE_ARGS     *targs    = NULL;
  CLIENTSIDE_ARGS     *data     = (CLIENTSIDE_ARGS *)arg;

  for ( ;; ) {

    /* Wait for a client to connect */
    n = sizeof(addr);
    if ((fd = accept(data->sock_fd, (struct sockaddr *)&addr, (unsigned int *)&n)) < 0) LOG_FATAL_MSG("accept", errno);

    if ((targs = malloc(sizeof(CLIENTSIDE_ARGS))) == NULL) LOG_FATAL_MSG("malloc", errno);
    targs->cmdstack   = data->cmdstack;
    targs->sock_fd    = fd;

    addrlen = sizeof(targs->ip_addr);
    strncpy(targs->ip_addr, inet_ntoa(addr.sin_addr), addrlen);
    targs->ip_addr[addrlen-1] = 0;

    if ((n = pthread_create(&thread_id, NULL, clientside_thread, targs)) != 0) LOG_FATAL_MSG("thread create", n);
  }
  
  pthread_exit(NULL);
}

static void 
setup_clientside_comm(ESL_GETOPTS *opts, CLIENTSIDE_ARGS *args)
{
  int                  n;
  int                  reuse;
  int                  sock_fd;
  pthread_t            thread_id;

  struct linger        linger;
  struct sockaddr_in   addr;

  /* Create socket for incoming connections */
  if ((sock_fd = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0) LOG_FATAL_MSG("socket", errno);
      
  /* incase the server went down in an ungraceful way, allow the port to be
   * reused avoiding the timeout.
   */
  reuse = 1;
  if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, (void *)&reuse, sizeof(reuse)) < 0) LOG_FATAL_MSG("setsockopt", errno);

  /* the sockets are never closed, so if the server exits, force the kernel to
   * close the socket and clear it so the server can be restarted immediately.
   */
  linger.l_onoff = 1;
  linger.l_linger = 0;
  if (setsockopt(sock_fd, SOL_SOCKET, SO_LINGER, (void *)&linger, sizeof(linger)) < 0) LOG_FATAL_MSG("setsockopt", errno);

  /* Construct local address structure */
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(esl_opt_GetInteger(opts, "--cport"));

  /* Bind to the local address */
  if (bind(sock_fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) LOG_FATAL_MSG("bind", errno);

  /* Mark the socket so it will listen for incoming connections */
  if (listen(sock_fd, esl_opt_GetInteger(opts, "--ccncts")) < 0) LOG_FATAL_MSG("listen", errno);
  args->sock_fd = sock_fd;

  if ((n = pthread_create(&thread_id, NULL, client_comm_thread, (void *)args)) != 0) LOG_FATAL_MSG("socket", n);
}



//#define HAVE_MPI
// p7_server_masternode_Create
/*! \brief Creates and initializes a P7_SERVER_MASTERNODE_STATE object
 *  \param [in] num_shards The number of shards each database will be divided into.
 *  \param [in] num_worker_nodes The number of worker nodes associated with this master node.
 *  \returns The initialized P7_SERVER_MASTERNODE_STATE object.  Calls p7_Fail to end the program if unable to complete successfully
 */
P7_SERVER_MASTERNODE_STATE *p7_server_masternode_Create(uint32_t num_shards, int num_worker_nodes){
  int status; // return code from ESL_ALLOC

  P7_SERVER_MASTERNODE_STATE *the_node = NULL;

  // allocate memory for the base object
  ESL_ALLOC(the_node, sizeof(P7_SERVER_MASTERNODE_STATE));
  
  // allocate memory for the work queues, one work queue per shard
  ESL_ALLOC(the_node->work_queues, num_shards * sizeof(P7_MASTER_WORK_DESCRIPTOR));

  the_node->num_shards = num_shards;
  int i;
  
  // Initialize the work queues to empty (start == -1 means empty)
  for(i = 0; i < num_shards; i++){
    the_node->work_queues[i].start = (uint64_t) -1;
    the_node->work_queues[i].end = 0;
  }

  // We start off with no hits reported
  the_node->tophits = p7_tophits_Create();

  the_node->num_worker_nodes = num_worker_nodes;

  // No worker nodes are done with searches at initializaiton time
  the_node->worker_nodes_done = 0;
  the_node->worker_stats_received = 0;
  // Create 10 empty message buffers by default.  Don't need to get this exactly right
  // as the message receiving code will allocate more if necessary
  the_node->empty_hit_message_pool = NULL;
  for(i = 0; i < 10; i++){
    P7_SERVER_MESSAGE *the_message;
    the_message = p7_server_message_Create();
    if(the_message == NULL){
      p7_Fail("Unable to allocate memory in p7_server_masternode_Create\n");
    }
    the_message->next = (P7_SERVER_MESSAGE *) the_node->empty_hit_message_pool;
    the_node->empty_hit_message_pool = the_message;
  }

  // This starts out empty, since no messages have been received
  the_node->full_hit_message_pool = NULL;

  if(pthread_mutex_init(&(the_node->hit_wait_lock), NULL)){
      p7_Fail("Unable to create mutex in p7_server_masternode_uCreate");
    }

  if(pthread_mutex_init(&(the_node->empty_hit_message_pool_lock), NULL)){
      p7_Fail("Unable to create mutex in p7_server_masternode_Create");
    }

  if(pthread_mutex_init(&(the_node->full_hit_message_pool_lock), NULL)){
      p7_Fail("Unable to create mutex in p7_server_masternode_Create");
    }

  // Hit thread starts out not ready.
  the_node->hit_thread_ready = 0;

  // init the start contition variable
  pthread_cond_init(&(the_node->start), NULL);
  the_node->shutdown = 0; // Don't shutdown immediately
  return(the_node);

  // GOTO target used to catch error cases from ESL_ALLOC
ERROR:
  p7_Fail("Unable to allocate memory in p7_server_masternode_Create");  
}


// p7_server_masternode_Destroy
/*! \brief Frees all memory associated with a P7_SERVER_MASTERNODE_STATE object
 *  \param [in,out] masternode The object to be destroyed, which is modified (freed) during execution
 *  \returns Nothing
 */
void p7_server_masternode_Destroy(P7_SERVER_MASTERNODE_STATE *masternode){
  int i;

  // Clean up the database shards
  for(i = 0; i < masternode->num_databases; i++){
    p7_shard_Destroy(masternode->database_shards[i]);
  }
  free(masternode->database_shards);

  // and the message pools
  P7_SERVER_MESSAGE *current, *next;

  current = (P7_SERVER_MESSAGE *) masternode->empty_hit_message_pool;
  while(current != NULL){
    next = current->next;
    p7_tophits_Destroy(current->tophits);
    free(current->buffer);
    free(current);
    current = next;
  }

  current = (P7_SERVER_MESSAGE *) masternode->full_hit_message_pool;
  while(current != NULL){
    next = current->next;
    p7_tophits_Destroy(current->tophits);
    free(current);
    current = next;
  }
  
  // clean up the pthread mutexes
  pthread_mutex_destroy(&(masternode->empty_hit_message_pool_lock));
  pthread_mutex_destroy(&(masternode->full_hit_message_pool_lock));
  pthread_mutex_destroy(&(masternode->hit_wait_lock));

  free(masternode->work_queues);
  
  p7_tophits_Destroy(masternode->tophits);
  // and finally, the base object
  free(masternode);
}

// p7_server_masternode_Setup
/*! \brief Loads the specified databases into the master node
 *  \param [in] num_shards The number of shards each database will be divided into.
 *  \param [in] num_databases The number of databases to be read from disk.
 *  \param [in] database names An array of pointers to the names of the databases to be read.  Must contain num_databases entries.
 *  \param [in,out] masternode The master node's P7_SERVER_MASTERNODE_STATE object.
 *  \returns Nothing
 *  \warning This function is deprecated, and is only included to support server development.  In production, the master node will
 *  not load the databases in order to reduce memory use, and this function will be eliminated.
 */
void p7_server_masternode_Setup(uint32_t num_shards, uint32_t num_databases, char **database_names, P7_SERVER_MASTERNODE_STATE *masternode){
   // Read databases from disk and install shards in the workernode
  
  int i, status;
  FILE *datafile;
  char id_string[13];

  masternode->num_shards = num_shards;
  masternode->num_databases = num_databases;
  ESL_ALLOC(masternode->database_shards, num_databases*sizeof(P7_SHARD *));

  for(i = 0; i < num_databases; i++){
    P7_SHARD *current_shard;

    datafile = fopen(database_names[i], "r");
    fread(id_string, 13, 1, datafile); //grab the first 13 characters of the file to determine the type of database it holds
    fclose(datafile);
        
    if(!strncmp(id_string, "HMMER3", 5)){
      // This is an HMM file
      current_shard = p7_shard_Create_hmmfile(database_names[i], 1, 0);
      if(current_shard == NULL){
        p7_Fail("Unable to allocate memory in p7_server_masternode_Create\n");
      }
    }
    else if(!strncmp(id_string, ">", 1)){
      // its a dsqdata file
      current_shard = p7_shard_Create_sqdata(database_names[i], 1, 0);
      if(current_shard == NULL){
        p7_Fail("Unable to allocate memory in p7_server_masternode_Create\n");
      }
    }
    else{
      p7_Fail("Couldn't determine type of datafile for database %s in p7_server_masternode_setup\n", database_names[i]);
    }

    if(i > masternode->num_databases){
      p7_Fail("Attempted to install shard into non-existent database slot in masternode\n");
    }
    else{
      masternode->database_shards[i] = current_shard;
    }

  }
  return;

ERROR:
  p7_Fail("Unable to allocate memory in p7_server_masternode_Setup");  
}

// p7_server_message_Create
/*! \brief Creates and initialized a P7_SERVER_MESSAGE object
 *  \returns The initialized object.  Calls p7_Fail() to exit the program if unable to complete successfully
 */
P7_SERVER_MESSAGE *p7_server_message_Create(){

    int status;
    P7_SERVER_MESSAGE *the_message;

    // Allocatte the base structure
    ESL_ALLOC(the_message, sizeof(P7_SERVER_MESSAGE));
    the_message->tophits = NULL;
    the_message->next = NULL;
    the_message->buffer_alloc = 1024;
    ESL_ALLOC(the_message->buffer, the_message->buffer_alloc);
    return the_message;
ERROR:
  p7_Fail("Unable to allocate memory in p7_server_message_Create");  
}

/* Sends a search to the worker nodes, waits for results, and returns them to the client */
int process_search(P7_SERVER_MASTERNODE_STATE *masternode, QUEUE_DATA_SHARD *query, MPI_Datatype *server_mpitypes, ESL_GETOPTS *go){
  #ifndef HAVE_MPI
  p7_Fail("process_search requires MPI and HMMER was compiled without MPI support");
#endif

#ifdef HAVE_MPI
  struct timeval start, end;
  uint64_t search_increment = 1;
  int status;
  P7_SERVER_COMMAND the_command;
  the_command.type = P7_SERVER_HMM_VS_SEQUENCES;
  the_command.db = 0;
  SEARCH_RESULTS results;
  P7_SERVER_MESSAGE *buffer, **buffer_handle; //Use this to avoid need to re-aquire message buffer
  // when we poll and don't find a message
  buffer = NULL;
  buffer_handle = &(buffer);
  init_results(&results);
  char *hmmbuffer;
  ESL_ALLOC(hmmbuffer, 100000* sizeof(char));  // take a wild guess at a buffer that will hold most hmms
  
  int hmm_buffer_size;
  hmm_buffer_size = 100000;

  P7_PROFILE *gm;
  int hmm_length, pack_position;

  // For testing, search the entire database
  P7_SHARD *database_shard = masternode->database_shards[0];

  masternode->pipeline =  p7_pipeline_Create(go, 100, 100, FALSE, p7_SEARCH_SEQS);
  masternode->hit_messages_received = 0;
  gettimeofday(&start, NULL);
  
  uint64_t search_start, search_end, chunk_size;
    
    // For now_search the entire database
    search_start = database_shard->directory[0].index;
    search_end = database_shard->directory[database_shard->num_objects -1].index;
    uint64_t search_length = (search_end - search_start) +1;

    chunk_size = ((search_end - search_start) / (masternode->num_worker_nodes * 128 )) +1; // round this up to avoid small amount of leftover work at the end

    // set up the work queues
    int which_shard;
    for(which_shard = 0; which_shard < masternode->num_shards; which_shard++){
      masternode->work_queues[which_shard].start = search_start;
      masternode->work_queues[which_shard].end = search_end;
      masternode->work_queues[which_shard].chunk_size = chunk_size;
    }

    // Synchronize with the hit processing thread
    while(pthread_mutex_trylock(&(masternode->hit_wait_lock))){  // Wait until hit thread is sitting at cond_wait
    }
    masternode->worker_nodes_done = 0;  // None of the workers are finished at search start time
    masternode->worker_stats_received = 0; // and none have sent pipeline statistics
    pthread_cond_broadcast(&(masternode->start)); // signal hit processing thread to start
    pthread_mutex_unlock(&(masternode->hit_wait_lock));

    // Get the HMM this search will compare against
    gm = p7_profile_Create (query->hmm->M, query->abc);
    P7_BG *bg = p7_bg_Create(gm->abc);
    p7_ProfileConfig(query->hmm, bg, gm, 100, p7_LOCAL); /* 100 is a dummy length for now; and MSVFilter requires local mode */
    
    // Pack the HMM into a buffer for broadcast
    p7_profile_MPIPackSize(gm, MPI_COMM_WORLD, &hmm_length); // get the length of the profile
    the_command.compare_obj_length = hmm_length;

    if(hmm_length > hmm_buffer_size){ // need to grow the send buffer
      ESL_REALLOC(hmmbuffer, hmm_length); 
    }
    pack_position = 0; // initialize this to the start of the buffer 

    if(p7_profile_MPIPack    (gm, hmmbuffer, hmm_length, &pack_position, MPI_COMM_WORLD) != eslOK){
      p7_Fail("Packing profile failed in master_node_main\n");
    }    // pack the hmm for sending
  

    // First, send the command to start the search
    MPI_Bcast(&the_command, 1, server_mpitypes[P7_SERVER_COMMAND_MPITYPE], 0, MPI_COMM_WORLD);

    // Now, broadcast the HMM
    MPI_Bcast(hmmbuffer, the_command.compare_obj_length, MPI_CHAR, 0, MPI_COMM_WORLD);

    // loop here until all of the workers are done with the search
    while((masternode->worker_nodes_done < masternode->num_worker_nodes) || (masternode->worker_stats_received < masternode->num_worker_nodes))
    {
      p7_masternode_message_handler(masternode, buffer_handle, server_mpitypes, go);  //Poll for incoming messages
    }

    gettimeofday(&end, NULL);
    double ncells = (double) gm->M * (double) database_shard->total_length;
    double elapsed_time = ((double)((end.tv_sec * 1000000 + (end.tv_usec)) - (start.tv_sec * 1000000 + start.tv_usec)))/1000000.0;
    double gcups = (ncells/elapsed_time) / 1.0e9;
    printf("%s, %lf, %d, %lf\n", gm->name, elapsed_time, gm->M, gcups);
    

    //Send results back to client
    results.nhits = masternode->tophits->N;
    results.stats.nhits = masternode->tophits->N;
    results.stats.hit_offsets = NULL;
    ESL_ALLOC(results.hits, results.nhits *sizeof(P7_HIT *));
    int counter;
    for(counter = 0; counter <results.nhits; counter++){
      results.hits[counter] =masternode->tophits->unsrt + counter;
    }

    //Put pipeline statistics in results
    results.stats.elapsed = elapsed_time;
    results.stats.Z = masternode->pipeline->Z;
    results.stats.domZ = masternode->pipeline->domZ;
    results.stats.domZ_setby = masternode->pipeline->domZ_setby;
    if(masternode->pipeline->mode == p7_SEARCH_SEQS){
      results.stats.nmodels = 1;
      results.stats.nnodes = gm->M;
      results.stats.nseqs = masternode->pipeline->nseqs;
      results.stats.nres = masternode->pipeline->nres;
    }
    else{
      results.stats.nmodels = masternode->pipeline->nmodels;
      results.stats.nnodes = masternode->pipeline->nnodes;
      results.stats.nseqs = 1;
      results.stats.nres = -1; /* Fix when support scan */
    }
    printf("results.stats.nres = %lld\n", results.stats.nres);
    results.stats.n_past_msv = masternode->pipeline->n_past_msv;
    results.stats.n_past_bias = masternode->pipeline->n_past_bias;
    results.stats.n_past_vit = masternode->pipeline->n_past_vit;
    results.stats.n_past_fwd = masternode->pipeline->n_past_fwd;
    results.stats.nreported = masternode->tophits->nreported;
    results.stats.nincluded = masternode->tophits->nincluded;

    // Send results back to client
    forward_results(query, &results); 
    p7_pipeline_Destroy(masternode->pipeline);

    p7_tophits_Destroy(masternode->tophits); //Reset for next search
    masternode->tophits = p7_tophits_Create();
    p7_bg_Destroy(bg);
    p7_profile_Destroy(gm);
    return eslOK;
  ERROR:
    p7_Fail("Unable to allocate memory in process_search");
#endif
}

void process_shutdown(P7_SERVER_MASTERNODE_STATE *masternode, MPI_Datatype *server_mpitypes){
  P7_SERVER_COMMAND the_command;
  the_command.type = P7_SERVER_SHUTDOWN_WORKERS;
#ifndef HAVE_MPI
  p7_Fail("process_shutdown requires MPI and HMMER was compiled without MPI support");
#endif

#ifdef HAVE_MPI
  MPI_Bcast(&the_command, 1, server_mpitypes[P7_SERVER_COMMAND_MPITYPE], 0, MPI_COMM_WORLD);
  while(pthread_mutex_trylock(&(masternode->hit_wait_lock))){  // Acquire this to prevent races
  }
  masternode->shutdown = 1;
  pthread_mutex_unlock(&(masternode->hit_wait_lock));

  pthread_cond_broadcast(&(masternode->start));

  // spurious barrier for testing so that master doesn't exit immediately
  MPI_Barrier(MPI_COMM_WORLD);

  // Clean up memory

  p7_server_masternode_Destroy(masternode);
#endif
}
// p7_master_node_main
/*! \brief Top-level function run on each master node
 *  \details Creates and initializes the main P7_MASTERNODE_STATE object for the master node, then enters a loop
 *  where it starts the number of searches specified on the command line, and reports the amount of time each search took.
 *  \param [in] argc The argument count (argc) passed to the program that invoked p7_master_node_main
 *  \param [in] argv The command-line arguments (argv) passed to the program that invoked p7_master_node_main
 *  \param [in] server_mpitypes Data structure that defines the custom MPI datatypes we use
 *  \warning This function will be significantly overhauled as we implement the server's UI, so that it receives search commands from
 *  user machines and executes them.  The current version is only valid for performance testing. 
 *  \bug Currently requires that the server be run on at least two nodes.  Need to modify the code to run correctly on one node, possibly
 *  as two MPI ranks.
 */
void p7_server_master_node_main(int argc, char ** argv, MPI_Datatype *server_mpitypes, ESL_GETOPTS *go){
#ifndef HAVE_MPI
  p7_Fail("P7_master_node_main requires MPI and HMMER was compiled without MPI support");
#endif

#ifdef HAVE_MPI
  // For now, we only use one shard.  This will change in the future
  int num_shards = 1; // Change this to calculate number of shards based on database size
  
  char           *seqfile = esl_opt_GetArg(go, 1);

  uint64_t num_searches = esl_opt_GetInteger(go, "-n");

  ESL_ALPHABET   *abc     = NULL;
  int status; // return code from ESL routines

  // For performance tests, we only use two databases.  That will become a command-line argument
  char *database_names[1];
  database_names[0] = seqfile;
  impl_Init();                  /* processor specific initialization */
  p7_FLogsumInit();		/* we're going to use table-driven Logsum() approximations at times */

  int num_nodes;

  // Get the number of nodes that we're running on 
  MPI_Comm_size(MPI_COMM_WORLD, &num_nodes);

  if(num_nodes < 2){
    p7_Fail("Found 0 worker ranks.  Please re-run with at least 2 MPI ranks so that there can be a master and at least one worker rank.");
  }

  // Set up the masternode state object for this node
  P7_SERVER_MASTERNODE_STATE *masternode;
  masternode = p7_server_masternode_Create(num_shards, num_nodes -1);
  if(masternode == NULL){
    p7_Fail("Unable to allocate memory in master_node_main\n");
  }

  // load the databases.  To be removed when we implement the UI
  p7_server_masternode_Setup(num_shards, 1, database_names, masternode);



  // Tell all of the workers how many shards we're using
  MPI_Bcast(&num_shards, 1, MPI_INT, 0, MPI_COMM_WORLD);

  // Create hit processing thread
  P7_SERVER_MASTERNODE_HIT_THREAD_ARGUMENT hit_argument;
  hit_argument.masternode = masternode;
  pthread_attr_t attr;

  // block until everyone is ready to go
  MPI_Barrier(MPI_COMM_WORLD);

  //create pthread attribute structure
  if(pthread_attr_init(&attr)){
    p7_Fail("Couldn't create pthread attr structure in master_node_main");
  }
  if(pthread_create(&(masternode->hit_thread_object), &attr, p7_server_master_hit_thread, (void *) &hit_argument)){
    p7_Fail("Unable to create hit thread in master_node_main");
  }
  if(pthread_attr_destroy(&attr)){
    p7_Fail("Couldn't destroy pthread attr structure in master_node_main");
  }

  while(!masternode->hit_thread_ready){
    // wait for the hit thread to start up;
  }
  struct timeval start, end;

    /* initialize the search stack, set it up for interthread communication  */
  ESL_STACK *cmdstack = esl_stack_PCreate();
  esl_stack_UseMutex(cmdstack);
  esl_stack_UseCond(cmdstack);

  /* start the communications with the web clients */
  CLIENTSIDE_ARGS client_comm;
  client_comm.cmdstack = cmdstack;
  setup_clientside_comm(go, &client_comm);
  printf("clientside communication started\n");

  int shutdown = 0;
  QUEUE_DATA_SHARD         *query      = NULL;
  while (!shutdown &&  esl_stack_PPop(cmdstack, (void **) &query) == eslOK) {
    printf("Processing command %d from %s\n", query->cmd_type, query->ip_addr);
    fflush(stdout);

    //worker_comm.range_list = NULL;

    switch(query->cmd_type) {
    case HMMD_CMD_SEARCH:
/*      if (esl_opt_IsUsed(query->opts, "--seqdb_ranges")) {
        ESL_ALLOC(worker_comm.range_list, sizeof(RANGE_LIST));
        hmmpgmd_GetRanges(worker_comm.range_list, esl_opt_GetString(query->opts, "--seqdb_ranges"));
      }*/
      process_search(masternode, query, server_mpitypes, go); 
      break;
    case HMMD_CMD_SCAN:        process_search(masternode, query, server_mpitypes, go); break;
    case HMMD_CMD_SHUTDOWN:    
      process_shutdown(masternode, server_mpitypes);
      p7_syslog(LOG_ERR,"[%s:%d] - shutting down...\n", __FILE__, __LINE__);
      shutdown = 1;
      break;
    default:
      p7_syslog(LOG_ERR,"[%s:%d] - unknown command %d from %s\n", __FILE__, __LINE__, query->cmd_type, query->ip_addr);
      break;
    }

    free_QueueData_shard(query);
  }

  exit(0);
  // GOTO target used to catch error cases from ESL_ALLOC
  ERROR:
    p7_Fail("Unable to allocate memory in p7_master_node_main");
#endif
}

// p7_server_master_hit_thread
/*! \brief Main function for the master node's hit thread
 *  \details Waits for the main thread to place one or more received hit messages in masternode->full_hit_message_pool. Pulls messages
 *  out of the pool in FIFO order, as opposed to the LIFO order of the pool itself, and puts the hits they contain into the hit tree.  
 *  \param argument The hit thread's P7_SERVER_MASTERNODE_HIT_THREAD_ARGUMENT object
 *  \returns Nothing.
 */
void *p7_server_master_hit_thread(void *argument){
  #ifndef HAVE_MPI
  p7_Fail("P7_master_hit_thread requires MPI and HMMER was compiled without MPI support");
#endif

#ifdef HAVE_MPI
  // unpack our argument object
  P7_SERVER_MASTERNODE_HIT_THREAD_ARGUMENT *the_argument;
  the_argument = (P7_SERVER_MASTERNODE_HIT_THREAD_ARGUMENT *) argument;
  P7_SERVER_MASTERNODE_STATE *masternode = the_argument->masternode;

  while(pthread_mutex_trylock(&(masternode->hit_wait_lock))){  // Acquire this lock to prevent race on first go signal
    }


  masternode->hit_thread_ready = 1; // tell main thread we're ready to go

  while(1){ // loop until master tells us to exit
    pthread_cond_wait(&(masternode->start), &(masternode->hit_wait_lock)); // wait until master tells us to go
 
    if(masternode->shutdown){ //main thread wants us to exit
      pthread_exit(NULL);
    }
    
    // If we weren't told to exit, we're doing a search, so loop until all of the worker nodes are done with the search
    while(masternode->worker_nodes_done < masternode->num_worker_nodes){

      if(masternode->full_hit_message_pool != NULL){
        // There's at least one message of hits for us to handle
        P7_SERVER_MESSAGE *the_message, *prev;
        prev = NULL;
        while(pthread_mutex_trylock(&(masternode->full_hit_message_pool_lock))){
          // spin to acquire the lock
        }

        the_message = (P7_SERVER_MESSAGE *) masternode->full_hit_message_pool;
        // Go through the pool of hit messages to find the last one in the chain, which is the first one that was received
        while(the_message->next != NULL){
          prev = the_message;
          the_message = the_message->next;
        }

        if(prev == NULL){
          // there was only one message in the pool
          masternode->full_hit_message_pool = NULL;
        }
        else{ // take the last message off the list
          prev->next = NULL;
        }

        pthread_mutex_unlock(&(masternode->full_hit_message_pool_lock));
        p7_tophits_Merge(masternode->tophits, the_message->tophits);
        p7_tophits_Destroy(the_message->tophits);
        the_message->tophits = NULL;
        if(the_message->status.MPI_TAG == HMMER_HIT_FINAL_MPI_TAG){
          //this hit message was the last one from a thread, so increment the number of threads that have finished
          masternode->worker_nodes_done++; //we're the only thread that changes this during a search, so no need to lock
        }

        // Put the message back on the empty list now that we've dealt with it.
        while(pthread_mutex_trylock(&(masternode->empty_hit_message_pool_lock))){
          // spin to acquire the lock
        } 
        the_message->next = (P7_SERVER_MESSAGE *) masternode->empty_hit_message_pool;
        masternode->empty_hit_message_pool = the_message;

        pthread_mutex_unlock(&(masternode->empty_hit_message_pool_lock));
      }
    }
  }
  p7_Fail("Master node hit thread somehow reached unreachable end point\n");
  #endif
}



// p7_masternode_message_handler
/*! \brief Function that processes MPI messages received by the master node
 *  \details The master node receives two types of messages during searches: work requests and messages of hits.
 *  Work requests can be handled quickly by just grabbing a chunk of work from the appropriate queue, so this function does that and 
 *  replies to the work request message with the requested chunk of work or a value that informs the requestor that the master node is
 *  out of work for it to do.
 *  Hit messages take longer to process, so this function receives them into buffers and places the buffers on the list of hit messages
 *  to be processed by the hit thread.  We handle hit messages this way rather than having the hit thread receive hit messages itself because
 *  we've configured MPI in a mode that promises that only one thread per rank will call MPI routines.  This is supposed to deliver 
 *  better performance than configuring MPI so that any thread can call MPI routines, but does make things a bit more complicated.
 *  \param [in, out] masternode The node's P7_SERVER_MASTERNODE_STATE structure, which is modified during execution.
 *  \param [in, out] buffer_handle A pointer to a pointer to a P7_SERVER_MESSAGE object, which p7_masternode_message_handler will
 *  copy a hit message into if it receives one. (NULL may be passed in, in which case, we'll grab a message buffer off of the free pool)  
 *  We pass this buffer in so that we only acquire a message buffer from the free pool
 *  when we receive a hit message, which reduces overhead.
 *  \param [in] server_mpitypes A data structure that defines the custom MPI datatypes that the server uses
 *  \returns Nothing.  Calls p7_Fail() to exit the program if unable to complete successfully.
 */ 
void p7_masternode_message_handler(P7_SERVER_MASTERNODE_STATE *masternode, P7_SERVER_MESSAGE **buffer_handle, MPI_Datatype *server_mpitypes, ESL_GETOPTS *go){
#ifndef HAVE_MPI
  p7_Fail("Attempt to call p7_masternode_message_handler when HMMER was compiled without MPI support");
#endif

#ifdef HAVE_MPI
  int status; // return code from ESL_*ALLOC
  int hit_messages_received = 0;
  if(*buffer_handle == NULL){
    //Try to grab a message buffer from the empty message pool
    while(pthread_mutex_trylock(&(masternode->empty_hit_message_pool_lock))){
        // spin to acquire the lock
      }
    if(masternode->empty_hit_message_pool != NULL){
      (*buffer_handle) = (P7_SERVER_MESSAGE *) masternode->empty_hit_message_pool;
      masternode->empty_hit_message_pool = (*buffer_handle)->next;
    }
    else{ // need to create a new message buffer because they're all full.  THis should be rare
      *buffer_handle = (P7_SERVER_MESSAGE *) p7_server_message_Create();
      if(buffer_handle == NULL){
        p7_Fail("Unable to allocate memory in p7_masternode_message_handler\n");
      }
    }
    pthread_mutex_unlock(&(masternode->empty_hit_message_pool_lock));
  }

  //Now, we have a buffer to potentially receive the message into
  int found_message = 0;
  MPI_Status temp_status;
  if(MPI_Iprobe(MPI_ANY_SOURCE, MPI_ANY_TAG, MPI_COMM_WORLD, &found_message, &((*buffer_handle)->status)) != MPI_SUCCESS){
    p7_Fail("MPI_Iprobe failed in p7_masternode_message_handler\n");
  }

  int message_length;
  uint32_t requester_shard;
  P7_SERVER_CHUNK_REPLY the_reply;
  
  if(found_message){
    //printf("Masternode received message\n");
    // Do different things for each message type
    switch((*buffer_handle)->status.MPI_TAG){
    case HMMER_PIPELINE_STATE_MPI_TAG:
      P7_PIPELINE *temp_pipeline;
      p7_pipeline_MPIRecv((*buffer_handle)->status.MPI_SOURCE, (*buffer_handle)->status.MPI_TAG, MPI_COMM_WORLD, &((*buffer_handle)->buffer), &((*buffer_handle)->buffer_alloc), go, &temp_pipeline);
      p7_pipeline_Merge(masternode->pipeline, temp_pipeline);
      p7_pipeline_Destroy(temp_pipeline);
      masternode->worker_stats_received++;
      break;
    case HMMER_HIT_FINAL_MPI_TAG:
      printf("HMMER_HIT_FINAL_MPI_TAG message received\n");
    case HMMER_HIT_MPI_TAG:
      printf("HMMER_HIT_MPI_TAG message received\n");
      // The message was a set of hits from a worker node, so copy it into the buffer and queue the buffer for processing by the hit thread
      masternode->hit_messages_received++;
      p7_tophits_MPIRecv((*buffer_handle)->status.MPI_SOURCE, (*buffer_handle)->status.MPI_TAG, MPI_COMM_WORLD, &((*buffer_handle)->buffer), &((*buffer_handle)->buffer_alloc), &((*buffer_handle)->tophits));

      // Put the message in the list for the hit thread to process
      while (pthread_mutex_trylock(&(masternode->full_hit_message_pool_lock)))
      {
        // spin to acquire the lock
        }

        (*buffer_handle)->next = (P7_SERVER_MESSAGE *) masternode->full_hit_message_pool;
        masternode->full_hit_message_pool = *buffer_handle;
        (*buffer_handle) = NULL;  // Make sure we grab a new buffer next time
        pthread_mutex_unlock(&(masternode->full_hit_message_pool_lock));
        break;
      case HMMER_WORK_REQUEST_TAG:
        // Work request messages can be handled quickly, so process them directly.
        
        // Get the shard that the requestor wants work for
        if(MPI_Recv(&requester_shard, 1, MPI_UNSIGNED, (*buffer_handle)->status.MPI_SOURCE, (*buffer_handle)->status.MPI_TAG, MPI_COMM_WORLD, &((*buffer_handle)->status)) != MPI_SUCCESS){
          p7_Fail("MPI_Recv failed in p7_masternode_message_handler\n");
        }

        if(requester_shard >= masternode->num_shards){
          // The requestor asked for a non-existent shard
          p7_Fail("Out-of-range shard %d sent in work request", requester_shard);
        }

        // Get some work out of the appropriate queue
        if(masternode->work_queues[requester_shard].end > masternode->work_queues[requester_shard].start){
          // There's work left to give out, so grab a chunk
          the_reply.start = masternode->work_queues[requester_shard].start;
          the_reply.end = the_reply.start + masternode->work_queues[requester_shard].chunk_size;
      
          if(the_reply.end >= masternode->work_queues[requester_shard].end){
            // The chunk includes all of the remaining work, so shorten it if necessary and mark the queue empty
            the_reply.end = masternode->work_queues[requester_shard].end;
            masternode->work_queues[requester_shard].start = -1;
          }
          else{
            // Change the start-of-queue value to reflect that we've taken a chunk off
            masternode->work_queues[requester_shard].start = the_reply.end + 1;
          }
        }
        else{
          // There was no work to give, so send an empty chunk to notify the worker that we're out of work
          the_reply.start = -1;
          the_reply.end = -1;
        }

        //send the reply
        if ( MPI_Send(&the_reply, 1, server_mpitypes[P7_SERVER_COMMAND_MPITYPE],  (*buffer_handle)->status.MPI_SOURCE, HMMER_WORK_REPLY_TAG, MPI_COMM_WORLD) != MPI_SUCCESS) p7_Fail("MPI send failed in p7_masternode_message_handler");

        break;
      default:
        // If we get here, someone's sent us a message of an unknown type
        p7_Fail("Unexpected message tag found in p7_masternode_message_handler");
    }
  }

  return;
  ERROR:  // handle errors in ESL_REALLOC
    p7_Fail("Couldn't realloc memory in p7_masternode_message_handler");  
    return;
#endif    
}

