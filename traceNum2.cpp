/*
 * traceid buffering tool
 * 
 * This tool collects trace id by filling a buffer.  
 * TraceID =(addr & 0x00ffffff)<<8|BBNumber; 
 * When the buffer overflows,the callback writes all
 * of the collected records to a file.
 *
 */


/*
Todo:
 IARG_TYPE IARG_CALL_ORDER cannot be used with the Buffer APIs.
 InsertIf and IARG_RETURN_REGS cannot be used together
 */

#include <iostream>
#include <fstream>
#include <stdlib.h>
#include <stddef.h>

#include "pin.H"
#include "portability.H"
#include "InstLib/instlib.H"
#include "InstLib/branch_pred.h"

using namespace INSTLIB;


FILTER filter;
UINT32 MASK=0;
UINT32 FLIP=0;
ADDRINT TraceAddr;
//UINT32 TraceId;


/* thread context definition */
typedef struct {

UINT32 BBNumber;
//UINT32 TraeId;

} thread_ctx_t;
REG thread_ctx_ptr;
REG ScratchReg;


UINT32 totalBuffersFilled = 0;
UINT64 totalElementsProcessed = 0;



/*
 *  Contains knobs to filter out things to instrument
 */
KNOB<string> KnobOutputFile(KNOB_MODE_WRITEONCE, "pintool", "o", "trace-num2.out", "output file");
KNOB<BOOL> KnobDumpBuffer(KNOB_MODE_WRITEONCE, "pintool", "dump_buffers", "0", "dump the filled buffers to thread specific file");
KNOB<UINT32> KnobNumPagesInBuffer(KNOB_MODE_WRITEONCE, "pintool", "num_pages_in_buffer", "16384", "number of pages in buffer"); //buffer size 64M, 6897664 elements
KNOB<UINT32> KnobNumBBsInTrace(KNOB_MODE_WRITEONCE, "pintool", "num_bbs_in_trace", "8", "limit of BBs per trace");




/*
 * The ID of the buffer
 */
BUFFER_ID bufId;

/*
 * Thread specific data
 */
TLS_KEY mlog_key;

/*
 * Number of OS pages for the buffer
 */
//#define NUM_BUF_PAGES 1024


/*
 * Record of memory references.  Rather than having two separate
 * buffers for reads and writes, we just use one struct that includes a
 * flag for type.
 */
struct TRACEREF
{
    UINT32      traceId;
//    UINT32      traceCount;
};

/*
 * MLOG - thread specific data that is not handled by the buffering API.
 */
class MLOG
{
  public:
    MLOG(THREADID tid);
    ~MLOG();

    VOID DumpBufferToFile( struct TRACEREF * reference, UINT64 numElements, THREADID tid );

    UINT32 NumBuffersFilled() {return _numBuffersFilled;}
    UINT32 NumElementsProcessed() {return _numElementsProcessed;}

  private:
    UINT32 _numBuffersFilled;
    UINT32 _numElementsProcessed;
    ofstream _ofile;
};


MLOG::MLOG(THREADID tid)
{

    _numBuffersFilled = 0;
    _numElementsProcessed = 0;
	string filename = KnobOutputFile.Value() + "." + decstr(getpid_portable()) + "." + decstr(tid);
    
    _ofile.open(filename.c_str());
    
    if ( ! _ofile )
    {
        cerr << "Error: could not open output file." << endl;
        exit(1);
    }
    
    _ofile << hex;
}


MLOG::~MLOG()
{
    _ofile.close();
}


VOID MLOG::DumpBufferToFile( struct TRACEREF * reference, UINT64 numElements, THREADID tid )
{

	_numBuffersFilled++;
	_numElementsProcessed += (UINT32)numElements;

	if (!KnobDumpBuffer)
    {
        return;
    }
  
  for(UINT64 i=0; i<numElements; i++, reference++)
	//for(UINT64 i=0; i<numElements; i++, reference++)
    {
     //   if (reference->ea != 0)
     //        _ofile << reference->traceId << "   " << reference->traceCount << endl;
     _ofile << reference->traceId<<endl;
    }
}

VOID PIN_FAST_ANALYSIS_CALL traceAddr(thread_ctx_t *thread_ctx)
{
//	TraceAddr = addr;
	thread_ctx->BBNumber=1;
} 


ADDRINT PIN_FAST_ANALYSIS_CALL setCJMP (thread_ctx_t *thread_ctx, ADDRINT addr, UINT32 taken) //inlined
{

  UINT32 traceID;
  thread_ctx->BBNumber+=(taken xor 1);
  traceID=(addr & 0x00ffffff)<<8|(thread_ctx->BBNumber);
  return traceID & (-taken);
 // return taken;
 // return (TraceAddr & 0x00ffffff)<<8|BBNumber);
}



/**************************************************************************
 *
 *  Instrumentation routines
 *
 **************************************************************************/

/*
 * Insert code to write data to a thread-specific buffer for instructions
 * that access memory.
 */
VOID Trace(TRACE trace, VOID *v)
{
    if (!filter.SelectTrace(trace))
        return;
       
    TRACE_InsertCall(trace, IPOINT_BEFORE, AFUNPTR(traceAddr),IARG_FAST_ANALYSIS_CALL, IARG_REG_VALUE, thread_ctx_ptr, IARG_END);
    ADDRINT trace_addr = TRACE_Address(trace);
    for(BBL bbl = TRACE_BblHead(trace); BBL_Valid(bbl); bbl=BBL_Next(bbl))
    {
    	INS ins = BBL_InsTail(bbl); //Last instruction of bbl
     
      if (INS_IsBranchOrCall(ins) || INS_IsRet(ins))
      {

    	  INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR)setCJMP,
     	 		 IARG_CALL_ORDER, CALL_ORDER_FIRST,
     	 		 IARG_FAST_ANALYSIS_CALL,
     	 		 IARG_REG_VALUE, thread_ctx_ptr,
     	 		 IARG_ADDRINT, trace_addr,
                 IARG_BRANCH_TAKEN,
                 IARG_RETURN_REGS, ScratchReg,
                 IARG_END);

  //  	    thread_ctx_t *temp = reinterpret_cast<thread_ctx_t *>(thread_ctx_ptr);
            INS_InsertFillBuffer(ins, IPOINT_TAKEN_BRANCH, bufId,
            		                IARG_REG_VALUE, ScratchReg, offsetof(struct TRACEREF, traceId),
 	 	     	                                       //  IARG_UINT32, TraceCount, offsetof(struct TRACEREF, traceCount),
 	 	     	       	 	     	                     IARG_END);

     	 	/*
            INS_InsertFillBuffer(ins, IPOINT_TAKEN_BRANCH, bufId,
            		                                    // IARG_ADDRINT, ((TRACE_Address(trace) & 0x00ffffff)<<8), offsetof(struct TRACEREF, traceId),
            		                                       IARG_REG_VALUE, ScratchReg, offsetof(struct TRACEREF, traceId),
            		                                    //  IARG_UINT32, TraceCount, offsetof(struct TRACEREF, traceCount),
 	 	     	       	 	     	                     IARG_END);
           */
    	/*
   	 	INS_InsertCall(ins, IPOINT_BEFORE, (AFUNPTR) setCJMP,
               IARG_FAST_ANALYSIS_CALL,
               IARG_BRANCH_TAKEN,
               IARG_END);
        INS_InsertFillBuffer(ins, IPOINT_TAKEN_BRANCH, bufId, IARG_ADDRINT, ((TraceAddr & 0x00ffffff)<<8|BBNumber), offsetof(struct TRACEREF, traceId), IARG_END);
        */
      }
    }
}


/**************************************************************************
 *
 *  Callback Routines
 *
 **************************************************************************/

/*!
 * Called when a buffer fills up, or the thread exits, so we can process it or pass it off
 * as we see fit.
 * @param[in] id		buffer handle
 * @param[in] tid		id of owning thread
 * @param[in] ctxt		application context
 * @param[in] buf		actual pointer to buffer
 * @param[in] numElements	number of records
 * @param[in] v			callback value
 * @return  A pointer to the buffer to resume filling.
 */
VOID * BufferFull(BUFFER_ID id, THREADID tid, const CONTEXT *ctxt, VOID *buf,
                  UINT64 numElements, VOID *v)
{
    struct TRACEREF * reference=(struct TRACEREF*)buf;

    MLOG * mlog = static_cast<MLOG*>( PIN_GetThreadData( mlog_key, tid ) );

    mlog->DumpBufferToFile( reference, numElements, tid );
    
    return buf;
}


/*
 * Note that opening a file in a callback is only supported on Linux systems.
 * See buffer-win.cpp for how to work around this issue on Windows.
 */
VOID ThreadStart(THREADID tid, CONTEXT *ctxt, INT32 flags, VOID *v)
{
    // There is a new MLOG for every thread.  Opens the output file.
    MLOG * mlog = new MLOG(tid);

    // A thread will need to look up its MLOG, so save pointer in TLS
    PIN_SetThreadData(mlog_key, mlog, tid);


	/* thread context pointer (ptr) */
	thread_ctx_t *tctx = NULL;
	/* allocate space for the thread context; optimized branch */
	if (unlikely((tctx = (thread_ctx_t *)calloc(1,
					sizeof(thread_ctx_t))) == NULL)) {
		/* error message */
		LOG(string(__func__) + ": thread_ctx_t allocation failed\n");
	}
	PIN_SetContextReg(ctxt, thread_ctx_ptr, (ADDRINT)tctx);
	PIN_SetContextReg(ctxt, ScratchReg, 0);
}


VOID ThreadFini(THREADID tid, const CONTEXT *ctxt, INT32 code, VOID *v)
{
    MLOG * mlog = static_cast<MLOG*>(PIN_GetThreadData(mlog_key, tid));
    totalBuffersFilled += mlog->NumBuffersFilled();
    totalElementsProcessed +=  mlog->NumElementsProcessed();
    printf ("totalBuffersFilled %u\ntotalElementsProcessed %14.0f\n", (totalBuffersFilled),
              static_cast<double>(totalElementsProcessed));

    delete mlog;

    PIN_SetThreadData(mlog_key, 0, tid);
	/* get the thread context */
	thread_ctx_t *tctx = (thread_ctx_t *)
		PIN_GetContextReg(ctxt, thread_ctx_ptr);

	/* free the allocated space */
	free(tctx);

}


/* ===================================================================== */
/* Print Help Message                                                    */
/* ===================================================================== */

INT32 Usage()
{
    cerr << "This tool demonstrates the basic use of the buffering API." << endl;
    cerr << endl << KNOB_BASE::StringKnobSummary() << endl;
    return -1;
}

VOID LimitTraces()
{
    CODECACHE_ChangeMaxBblsPerTrace(KnobNumBBsInTrace);
}

/* ===================================================================== */
/* Main                                                                  */
/* ===================================================================== */
/*!
 * The main procedure of the tool.
 * This function is called when the application image is loaded but not yet started.
 * @param[in]   argc            total number of elements in the argv array
 * @param[in]   argv            array of command line arguments, 
 *                              including pin -t <toolname> -- ...
 */
int main(int argc, char *argv[])
{
    // Initialize PIN library. Print help message if -h(elp) is specified
    // in the command line or the command line is invalid
	if( unlikely(PIN_Init(argc,argv) ))
    {
        return Usage();
    }
    
    // Initialize the memory reference buffer;
    // set up the callback to process the buffer.
    //
    bufId = PIN_DefineTraceBuffer(sizeof(struct TRACEREF), KnobNumPagesInBuffer,
                                  BufferFull, 0);

    if(unlikely(bufId == BUFFER_ID_INVALID))
    {
        cerr << "Error: could not allocate initial buffer" << endl;
        return 1;
    }

   // Obtain  a key for TLS storage.
    mlog_key = PIN_CreateThreadDataKey(0);

    CODECACHE_AddCacheInitFunction(LimitTraces, 0);

    thread_ctx_ptr = PIN_ClaimToolRegister();
    ScratchReg = PIN_ClaimToolRegister();
    if (unlikely(!(REG_valid(thread_ctx_ptr) && REG_valid(ScratchReg))))
    {
        std::cerr << "Cannot allocate a scratch register.\n";
        std::cerr << std::flush;
        return 1;
    }


    // add callbacks
    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddThreadFiniFunction(ThreadFini, 0);
    
    
        // add an instrumentation function
    TRACE_AddInstrumentFunction(Trace, 0);
    
    filter.Activate();

    // Start the program, never returns
    PIN_StartProgram();
    
    return 0;
}


