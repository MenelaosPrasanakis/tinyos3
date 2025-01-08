#include "tinyos.h"
#include "kernel_streams.h"
#include "kernel_dev.h"
#include "kernel_cc.h"

// Define file operations for the writer
static file_ops writer_file_ops = {
	.Write = pipe_write,// Function pointer for writing to the pipe
	.Close = pipe_writer_close // Function pointer for closing the writer
};

// Define file operations for the reader
static file_ops reader_file_ops = {
	.Read = pipe_read, // Function pointer for reading from the pipe
	.Close = pipe_reader_close // Function pointer for closing the reader
};

// Implementation of the sys_Pipe system call
int sys_Pipe(pipe_t* pipe)
{
	Fid_t fid[2]; // Array to hold two file IDs (one for reading and one for writing)
	FCB* fcb[2];  // Array to hold two File Control Blocks (FCBs) for the pipe endpoints
	

    // Check if there are enough FCBs and file IDs available
	if(!FCB_reserve(2,fid,fcb))
		return -1; // Return -1 if not enough resources are available

	/**@brief Proceed to the construction of the pipe via xmalloc*/
	pipe_cb* P_CB = (pipe_cb*)xmalloc(sizeof(pipe_cb));


    // Initialize the pipe's reader and writer with the allocated FCBs
	P_CB->reader = fcb[0]; /**@brief Initialize the reader*/
	P_CB->writer = fcb[1]; /**@brief Initialize the writer*/

    // Initialize condition variables for the pipe
    // These are used to synchronize access to the buffer
	P_CB->has_space = COND_INIT; /**brief Just a condition variable to check the space in the buffer*/
	P_CB->has_data	= COND_INIT; /**brief Just a condition variable to check the written data in the buffer*/
	
    // Initialize pipe state variables
	P_CB->w_position = 0; // Write position in the buffer
	P_CB->r_position = 0; // Read position in the buffer
	P_CB->counter = 0; // Counter for tracking the number of bytes in the buffer
	

	// Associate the reader FCB with the pipe control block and operations
	fcb[0]->streamobj = P_CB; // Link reader FCB to the pipe control block
	fcb[0]->streamfunc = &reader_file_ops; // Assign reader operations

	// Associate the writer FCB with the pipe control block and operations
	fcb[1]->streamobj = P_CB; // Link writer FCB to the pipe control block
	fcb[1]->streamfunc = &writer_file_ops; // Assign writer operations

	// Store the file IDs for reading and writing in the pipe structure
	pipe->read = fid[0]; // File ID for reading
	pipe->write = fid[1]; // File ID for writing

	// Return success
	return 0;

}


int pipe_write(void* pipecb_t, const char *buf, unsigned int n)
{
	pipe_cb* CUR_PIPE = (pipe_cb*) pipecb_t; // Cast the input parameter to the correct type
	int written_data = 0; // Tracks the number of bytes written to the pipe

	// Check if the pipe control block or its writer/reader descriptors are invalid
	if(CUR_PIPE == NULL || CUR_PIPE->writer == NULL || CUR_PIPE->reader == NULL)
		return -1; // Return -1 to indicate an error

	// Wait until there is space in the pipe's buffer
    // Loop runs if the buffer is full and the reader still exists
	while((CUR_PIPE->w_position+1)%PIPE_BUFFER_SIZE == CUR_PIPE->r_position && CUR_PIPE->reader != NULL){

		kernel_wait(&CUR_PIPE->has_space, SCHED_PIPE); // Wait for the reader to consume data
	}


	// If the reader has been closed while waiting, return an error
	if(CUR_PIPE->reader == NULL)
		return -1;

	// Write data to the buffer as long as there's space and more data to write
	while(((CUR_PIPE->w_position+1)%PIPE_BUFFER_SIZE != CUR_PIPE->r_position) && written_data<n)
	{
		CUR_PIPE->BUFFER[CUR_PIPE->w_position] = buf[written_data]; // Write the next byte to the buffer
		CUR_PIPE-> counter++;  // Increment the counter tracking buffer usage
		written_data++; // Increment the number of bytes written
		CUR_PIPE->w_position++; // Move the write pointer forward

		// Wrap around the write pointer if it reaches the end of the buffer
		if(CUR_PIPE->w_position >= PIPE_BUFFER_SIZE)
			CUR_PIPE->w_position = 0;
	}


	// Reset the counter if all data has been written 
	if(CUR_PIPE->counter == n-1)
		CUR_PIPE->counter = 0;

	/**@brief Wake up the reader that data is available in the buffer*/
	kernel_broadcast(&CUR_PIPE->has_data);

	// Return the total number of bytes successfully written to the pipe
	return written_data;

}

int pipe_read(void* pipecb_t, char *buf, unsigned int n) {
    // Cast 
    pipe_cb* CUR_PIPE = (pipe_cb*) pipecb_t;

    // Variable to keep track of the number of bytes read
    unsigned int byte_num = 0;

    /*Check that the pipe and its reader are created properly or
        return error indicator -1 if either one is NULL*/
    if (CUR_PIPE == NULL || CUR_PIPE->reader == NULL)
        return -1; 



    // Wait if the write position equals the read position and there is a writer.
    // This indicates that the pipe is empty and data is expected from the writer.
    while (CUR_PIPE->w_position == CUR_PIPE->r_position && CUR_PIPE->writer != NULL)
        kernel_wait(&CUR_PIPE->has_data, SCHED_PIPE);

    // Read data from the pipe while:
    // The read position does not match the write position (indicating data is available).
    // The number of bytes read is less than the requested number `n`.
    while ((CUR_PIPE->r_position != CUR_PIPE->w_position) && byte_num < n) {
        // Copy a byte from the pipe's buffer to the output buffer.
        buf[byte_num] = CUR_PIPE->BUFFER[CUR_PIPE->r_position];
        CUR_PIPE->counter++; // Increment the counter (to track data usage).
        byte_num++;          // Increment the number of bytes read.
        CUR_PIPE->r_position++; // Move the read position forward.

        // Wrap around the read position if it exceeds the buffer size.
        if (CUR_PIPE->r_position >= PIPE_BUFFER_SIZE)
            CUR_PIPE->r_position = 0;
    }

    // Reset the counter if it equals the number of requested bytes (all data processed).
    if (CUR_PIPE->counter == n)
        CUR_PIPE->counter = 0;

    // Notify 
    kernel_broadcast(&CUR_PIPE->has_space);

    // Return the total number of bytes successfully read.
    return byte_num;
}

int pipe_writer_close(void* _pipecb) {

		// Cast 
        pipe_cb* CUR_PIPE= (pipe_cb*) _pipecb;


        // Check if the provided pointer is NULL
        if (_pipecb == NULL) return -1; 

        

        // Mark the writer as closed by setting it to NULL
        CUR_PIPE->writer = NULL;

        // If there is no reader left, free the pipe memory
        if (CUR_PIPE->reader == NULL) {
                free(CUR_PIPE); // Free memory since both writer and reader are closed
        } else {
                kernel_broadcast(&CUR_PIPE->has_data);
        }

        // Return 0 s
        return 0;
}


int pipe_reader_close(void* _pipecb) {

    // Cast 
    pipe_cb* CUR_PIPE = (pipe_cb*) _pipecb;


    // Check if the provided pointer is NULL
    if (_pipecb == NULL) {
        return -1; // Return -1 
    } 


    CUR_PIPE->reader = NULL;

    // If the writer is also closed, free the pipe memory
    if (CUR_PIPE->writer == NULL) {
        free(CUR_PIPE); // Free the pipe control block memory
    } else {
        kernel_broadcast(&CUR_PIPE->has_space);
    }

    // Return 0 to indicate success
    return 0;
}



