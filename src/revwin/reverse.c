#ifdef HAVE_REVERSE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/mman.h>
#include <errno.h>

#include <revwin/reverse.h>
//#include <mm/dymelor.h>
#include <core/timer.h>
#include <mm/slab.h>



// #define revwin_overflow() do { \
// 	printf("[LP%d] :: event %d win at %p\n", current_lp, current_evt->type, win); \
// 	printf("Code size is %lld bytes and data size is %lld bytes\n", ((long long)win->code - (long long)win->data), ((long long)win->data - sizeof(revwin_t))); \
// 	fprintf(stderr, "Insufficent reverse window memory heap!\n"); \
// 	exit(-ENOMEM); \
// } while(0)

// #define revwin_check_space(size) do { \
// 	if (((long long)win->code - (long long)win->data) < (size)) { \
// 		revwin_overflow(); \
// 	} while(0)

static inline void revwin_overflow(revwin_t *win) {
	printf("[LP%d] :: event %d win at %p\n", current_lp, current_evt->type, win);
	printf("Code size is %lld bytes and data size is %lld bytes\n", ((long long)win->code - (long long)win->data), ((long long)win->data - sizeof(revwin_t)));
	fprintf(stderr, "Insufficent reverse window memory heap!\n");
	exit(-ENOMEM); \
}

static inline void revwin_check_space(revwin_t *win, size_t size) {
	if (((long long)win->code - (long long)win->data) < (size)) {
		revwin_overflow(win);
	}
}


static int strategy_id = STRATEGY_SINGLE;
static int dominated_count = 0;

static unsigned int revwin_size = REVWIN_SIZE;


//! Internal software cache to keep track of the reversed instructions
__thread reverse_cache_t cache;

//! Keeps track of the current reversing strategy
//__thread strategy_t strategy;

//! Handling of the reverse windows
__thread struct slab_chain *slab_chain;


/*
 * Adds the passed exeutable code to the executable section of the reverse window.
 *
 * @author Davide Cingolani
 *
 * @param bytes Pointer to the buffer to write
 * @param size Number of bytes to write
 */
static void revwin_add_code(revwin_t *win, unsigned char *bytes, size_t size) {

	revwin_check_space(win, size);

	// Since the structure is used as a stack, it is needed to create room for the instruction
	win->code = (void *)((char *)win->code - size);

	// copy the instructions to the heap
	memcpy(win->code, bytes, size);

//	printf("Added %ld bytes to the reverse window\n", size);
}

static void revwin_add_data(revwin_t *win, void *address, size_t size) {

	revwin_check_space(win, size);

	memcpy(win->data, address, size);

	win->data = (void *)((char *)win->data + size);
}


/*
 * Generates the reversing instruction for the whole chunk.
 *
 * @author Davide Cingolani
 *
 * @param address The starting address from which to copy
 * @param size The number of bytes to reverse
 */
static void reverse_chunk(revwin_t *win, unsigned long long address, size_t size) {
	unsigned char code[36] = {
		0x48, 0xc7, 0xc1, 0x00, 0x00, 0x00, 0x00,							// mov 0x0,%rcx
		0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,			// movabs 0x0,%rax
		0x48, 0x89, 0xc6,													// mov %rax,%rsi
		0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,			// movabs 0x0,%rax
		0x48, 0x89, 0xc7,													// mov %rax,%rdi
		0xf3, 0x48, 0xa5													// rep movsq
	};
	// TODO: movsq non è sicura, i chunk potrebbero non essere multipli di 8 byte
	// TODO: usare i registri xmm per spostare 128 bit alla volta (attenzione all'allineamento, però!!)

	unsigned char *mov_rcx = code;
	unsigned char *mov_rsi = code + 7;
	unsigned char *mov_rdi = code + 20;


	// Check whether there is enough available space to store data
	revwin_check_space(win, size);

	// Dump the chunk to the reverse window data section
	memcpy(win->data, (void *)address, size);

	#ifdef REVERSE_SSE_SUPPORT
	// TODO: support sse instructions
	#else

	// Copy the chunk size in RCx
	memcpy(mov_rcx+3, &size, 4);
	
	// Copy the first address
	memcpy(mov_rsi+2, &win->data, 8);

	// Compute and copy the second part of the address
	memcpy(mov_rdi+2, &address, 8);
	#endif

	win->data = (void *)((char *)win->data + size);

	//printf("Chunk addresses reverse code generated\n");

	// Now 'code' contains the right code to add in the reverse window
	revwin_add_code(win, code, sizeof(code));
}


/**
 * This function creates the relative instruction to reverse a 64
 * bits word of data.
 *
 * @author Davide Cingolani
 *
 * @param win Reverse window
 * @param address The address to be reversed
 * @parm bsize The size of the single reverse block (must be 4, 8 or 16)
 *
 */
static void reverse_single_xmm(revwin_t *win, unsigned long long address, int bsize) {
	unsigned long long rip_relative;

	unsigned char revasm[22] = {
		0x48,0xb8,  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,		// movabs 0x0, %rax
		0xf3,0x0f,0x6f,0x0d,  0x0a,0x00,0x00,0x00,					// movdqu 0xa(%rip),%xmm1
		0xf3,0x0f,0x7f,0x08											// movdqu %xmm1, (%rax)
	};

	// Check whether there is enough available space to store data
	revwin_check_space(win, bsize);

	// Copy the word pointed to by address
	// and store it in a proper space of the revwin's data section
	memcpy(win->data, (void *)address, bsize);

	// Computes the rip relative displacement
	rip_relative = win->code - win->data;

	// Update the data section ponter of reverse window
	win->data += bsize;

	// Builds the assembly reverse code with RIP-relative addressing
	memcpy(revasm+2, &address, 8);
	memcpy(revasm+15, &rip_relative, 4);

	// Actually add the code to the revwin's executable section
	revwin_add_code(win, revasm, sizeof(revasm));
}


/*
 * Generates the reversing instruction for a single addres,
 * i.e. the one passed as argument.
 *
 * @author Davide Cingolani
 *
 * @param address The starting address from which to copy
 * @param size The number of bytes to reverse
 */
static void reverse_single_embedded(revwin_t *win, const unsigned long long address, size_t size) {
	unsigned long value, value_lower;
	unsigned char *code;
	unsigned short size_code;

	unsigned char revcode_quadword[23] = {
		0x48, 0xb8, 0xda, 0xd0, 0xda, 0xd0, 0x00, 0x00, 0x00, 0x00,			// movabs $0x0, %rax
		0xc7, 0x00, 0xd3, 0xb0, 0x00, 0x00,									// movl $0x0, (%rax)
		0xc7, 0x40, 0x04, 0xb0, 0xd3, 0x00, 0x00 							// movl $0x0, 4(%rax)
	};

	// Get the value pointed to by 'address'
	memcpy(&value, (void *)address, 8);

	// Quadword
	code = revcode_quadword;
	size_code = sizeof(revcode_quadword);
	value_lower = ((value >> 32) & 0x0FFFFFFFF);
	memcpy(code+12, &value, 4);
	memcpy(code+19, &value_lower, 4);

	// Copy the destination address into the binary code
	// of MOVABS (first 2 bytes are the opcode)
	memcpy(code+2, &address, 8);

	// Now 'code' contains the right code to add in the reverse window
	revwin_add_code(win, code, size_code);
}


/**
 * Check if the address is dirty by looking at the hash map. In case the address
 * is not present adds it and return 0.
 *
 * @author Davide Cingolani
 *
 * @param address The address to check
 *
 * @return true if the reverse MOV instruction relative to 'address' would be
 * the dominant one, false otherwise
 */
static bool check_dominance(unsigned long long address) {
	unsigned long long chunk_address;
	reverse_cache_line_t *entry;


	// Inquiry DyMeLoR's API to retrieve the address of the memory
	// area (i.e. chunk) associated with the current address
	//chunk_address = get_area(address);

	// Get the actual cache line associated with the selected cluster
	entry = get_cache_line(&cache, chunk_address);


	// Check whether the tag matches the one associated to the current address;
	// that is, it belongs to the correct chunk in cache (i.e. a chunk hit)
	// If not, simply reset the cache line to contain the new value
	if(entry->tag != get_address_tag(address)) {

		// In case of a cache miss no update were made to the cache's usefulness
		// since there is not enough information to compute anything
		// A cache miss cannot affect the cache usefulness, this not means that
		// the model is walking towards single- or chunk-based accesses

		// Reset the whole cache line
		memset(entry, 0, sizeof(reverse_cache_line_t));

		// Update the cache line tag with the current one
		entry->tag = get_address_tag(address);
	}

	
	// Increase the total number of chunk hits
	entry->total_hits++;
	
	// Now, verify that the address has been previously referenced or not
	if(cache_check_bit(entry, address) == 1) {
		// This is a cache hit for the current address
		return true;
	}

	// The address was not references before, therefore it is not predominate
	// by no other previous access

	// If not, update the bitmap and increase the count of distinct addresses
	// referenced so far
	cache_set_bit(entry, address);
	entry->distinct_hits++;

	// Update cache usefulness as:
	// U = distinct_hits / width
	cache.usefulness += ((double)entry->distinct_hits / (double)CACHE_LINE_SIZE);
	cache.usefulness /= 2;

	return false;
}


void revwin_flush_cache(void) {
	memset(cache.lines, 0, CACHE_LINE_SIZE*CACHE_NUM_LINES);
}


revwin_t *revwin_create(void) {
	revwin_t *win;

	unsigned char code_closing[2] = {0x58, 0xc3};

	// Query the slab allocator to retrieve a new reverse window
	// executable area. The address represents the base address of
	// the revwin descriptor which contains the reverse code itself.
	win = slab_alloc(slab_chain);
	if(win == NULL) {
		printf("Unable to allocate a new reverse window: SLAB failure\n");
		abort();
	}
	memset(win, 0, revwin_size);


	// Initialize reverse window's code field in order to point to very
	// last byte of the raw section of this reverse window
	win->code_start = (void *)((char *)win->raw + revwin_size - 1);
	win->data_start = win->raw;

	// Allocate a new slot in the reverse mapping, accorndigly to
	// the number of yet allocated windows
#if RANDOMIZE_REVWIN
	win->code_start = (void *)((char *)win->code_start - (rand() % REVWIN_RZONE_SIZE));
#endif

	win->code = win->code_start;
	win->data = win->data_start;

	// No particular initialization for data pointer is actually required

	// Initialize the executable code area with the closing
	// instructions at the end of the actual window.
	// In this way we are sure the exection will correctly returns
	// once the whole revwin has been reverted.
	revwin_add_code(win, code_closing, sizeof(code_closing));

	// Update the code_start in order not to have to rewrite
	// the closing instructions at each reset
	win->code_start = win->code;

	return win;
}


void revwin_free(unsigned int lid, revwin_t *win) {

	// Sanity check
	if (win == NULL) {
		return;
	}

	// Free the slab area
	slab_free(slab_chain, win);
}


/**
 * Initializes a the reverse memory region of executables reverse windows. Each slot
 * is managed by a slab allocator and represents a reverse window to be executed.
 * Reverse widnows are bound to events by a pointer in their message descriptor.
 *
 * @author Davide Cingolani
 *
 * @param window_size The size of the reverse window
 */
 // FIXME
 FILE *f;
void reverse_init(size_t window_size) {

	// Allocate the structure needed by the slab allocator
	slab_chain = rsalloc(sizeof(struct slab_chain));
	if(slab_chain == NULL) {
		printf("Unable to allocate memory for the SLAB structure\n");
		abort();
	}

	// In this step we should initialize the slab allocator in order
	// to fast handle allocation and deallocation of reverse windows
	// which will be created by each event indipendently.
	// The size passed as argument is the size of each slab the allocator
	// will return, i.e. a reverse window

	// A different value than the default is used, if provided
	if(window_size != 0)
		revwin_size = window_size;

	slab_init(slab_chain, revwin_size);

	// Reset the cluster cache
	revwin_flush_cache();

	// For statistical and debug reasons open a file
	// in which to save all the touched addresses
	char *f_name = "addresses.log";
	if ( (f = fopen(f_name, "w")) == NULL)  {
		rootsim_error(true, "Cannot open %s\n", f_name);
	}
}


void reverse_fini(void) {
	// TODO: implementare
	// Free each revwin still allocated ?

	// Destroy the SLAB allocator
	slab_destroy(slab_chain);

	// DEBUG:
	printf("dominated_count = %d\n", dominated_count);

	fclose(f);
}


/*
 * Reset the reverse window intruction pointer
 */
void revwin_reset(unsigned int lid, revwin_t *win) {

	// Sanity check
	if (win == NULL) {
		// We dont care about NULL revwin
		return;
	}

	// Resets the instruction pointer to the first byte AFTER the closing
	// instruction at the base of the window (which counts 2 bytes)
	win->code = win->code_start;
	win->data = win->data_start;
}


/**
 * Adds new reversing instructions to the current reverse window.
 * Genereate the reverse MOV instruction staring from the knowledge of which
 * memory address will be accessed and the data width of the write.
 * 
 * @author Davide Cingolani
 *
 * @param address The address of the memeory location to which the MOV refers
 * @param size The size of data will be written by MOV
 */

#define SIMULATED_INCREMENTAL_CKPT if(0)
static bool use_xmm = true;

void reverse_code_generator(const unsigned long long address, const size_t size) {
	unsigned long long chunk_address;
	size_t chunk_size;
	bool dominant;
	revwin_t *win;

	printf("address is %p - size is %d\n",address,size);

	//SIMULATED_INCREMENTAL_CKPT return;

	// We have to retrieve the current event structure bound to this LP
	// in order to bind this reverse window to it.
	win = current_evt->revwin;
	if(win == NULL) {
		printf("No revwin has been defined for the event\n");
		abort();
	}

	timer t;
	timer_start(t);


	// Check whether the current address' update dominates over some other
	// update on the same memory region. If so, we can return earlier.
	dominant = check_dominance(address);
	if(dominant) {
		// If the current address is dominated by some other update,
		// then there is no need to generate any reversing instruction
		dominated_count++;
		return;
	}

	fprintf(f, "%p of %d bytes - cache %d\n", address, size, dominant ? 1 : 0);


	void (*reversing_function)(revwin_t *, unsigned long long, size_t);

	// Act accordingly to the currrent selected reversing strategy
	if(cache.usefulness > 0.5) {
		
		reversing_function = reverse_chunk;
		
		if(strategy_id == STRATEGY_SINGLE) {
			strategy_id == STRATEGY_CHUNK;
			printf("Swith to chunk reversal (%f)\n", cache.usefulness);
		}

	} else {

		if(use_xmm) {
			reversing_function = reverse_single_xmm;
		} else {
			reversing_function = reverse_single_embedded;
		}

		if(strategy_id == STRATEGY_CHUNK) {
			strategy_id == STRATEGY_SINGLE;
			printf("Swith to single reversal (%f)\n", cache.usefulness);
		}
	}	

	// Act accordingly to the currrent selected reversing strategy
	/*if(cache.usefulness > 0.5) {
	
		// Reverse the whole malloc_area chunk passing the pointer
		// of the target memory chunk to reverse (not the malloc_area one)
		chunk_address = address & ADDRESS_PREFIX;
		chunk_size = CLUSTER_SIZE;
		//reverse_chunk(win, chunk_address, chunk_size);
		reverse_single(win, address, size);

	} else {
	
		// Reverse the single buffer access
		reverse_single(win, address, size);
	}*/

	reversing_function(win, address, size);

	// Gather statistics data
	double elapsed = (double)timer_value_micro(t);
	statistics_post_lp_data(current_lp, STAT_REVERSE_GENERATE, 1.0);
	statistics_post_lp_data(current_lp, STAT_REVERSE_GENERATE_TIME, elapsed);

//	printf("[%d] :: Reverse MOV instruction generated to save value %lx\n", tid, *((unsigned long *)address));
}


/**
 * Executes the code actually present in the reverse window
 *
 * @author Davide Cingolani
 *
 * @param w Pointer to the actual window to execute
 */
void execute_undo_event(unsigned int lid, revwin_t *win) {
	unsigned char push = 0x50;
	int err;


	// Sanity check
	if (win == NULL) {
		// There is nothing to execute, actually
		return;
	}

	//revcode_size = ((win->base + win->size - 3) - win->top);
	//printf("UNDO :: [%p - %p] revcode size= %d\n", win, event, revcode_size);
	//printf("UNDO :: [%p]  revcode size= %d\n", win, revcode_size);
	/*
	if (revcode_size <= 0) {
		printf("Empty reverse code\n");
		return;
	}
	*/

	// Statistics
	timer reverse_block_timer;
	timer_start(reverse_block_timer);


	// Add the complementary push %rax instruction to the top
	revwin_add_code(win, &push, sizeof(push));

	// Calls the reversing function
	((void (*)(void))win->code) ();

	double elapsed = (double)timer_value_micro(reverse_block_timer);
	statistics_post_lp_data(lid, STAT_REVERSE_EXECUTE, 1.0);
	statistics_post_lp_data(lid, STAT_REVERSE_EXECUTE_TIME, elapsed);
	statistics_post_lp_data(current_lp, STAT_REVERSE_WINDOW_CODE_SIZE, revwin_avail_code_size());
	statistics_post_lp_data(current_lp, STAT_REVERSE_WINDOW_DATA_SIZE, revwin_avail_data_size());


//	printf("===> [%d] :: undo event executed (size = %ld bytes)\n", tid, revwin_size(win));
}

#endif /* HAVE_REVERSE */
