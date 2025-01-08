#include "tinyos.h"
#include "kernel_socket.h"
#include "kernel_dev.h"
#include "kernel_cc.h"


int socket_read(void* socket_cb_t, char *buf, unsigned int n){

	// Cast the input parameter to a socket control block (SCB) pointer
	socket_cb* peer_2_peer_READ = (socket_cb*) socket_cb_t;

	// Check if the SCB is NULL (invalid input)
	if(peer_2_peer_READ == NULL) return -1;

	// Ensure the socket is a peer socket and has a valid read pipe
	if(peer_2_peer_READ->peer_s.read_pipe==NULL || peer_2_peer_READ->type != SOCKET_PEER) 
		return -1;

	// Delegate the read operation to the pipe_read function
	else{
		return pipe_read(peer_2_peer_READ->peer_s.read_pipe, buf, n);
	}

	// Default return (unreachable but kept for clarity)
	return 0;
}


int socket_write(void* socket_cb_t, const char *buf, unsigned int n){

	// Cast the input parameter to a socket control block (SCB) pointer
	socket_cb* peer_2_peer_WRITE = (socket_cb*) socket_cb_t;

	// Check if the SCB is NULL (invalid input)
	if(peer_2_peer_WRITE == NULL) return -1;

	// Ensure the socket is a peer socket and has a valid write pipe
	if(peer_2_peer_WRITE->peer_s.write_pipe==NULL || peer_2_peer_WRITE->type != SOCKET_PEER) 
		return -1;

	 // Delegate the write operation to the pipe_write function
	else{
		return pipe_write(peer_2_peer_WRITE->peer_s.write_pipe, buf, n);
	}

	// Default return (unreachable but kept for clarity)
	return 0;
}


int socket_close(void* socket_cb_t){

	// Cast the input parameter to a socket control block (SCB) pointer
	socket_cb* CUR_SOCKET = (socket_cb*) socket_cb_t;

	// Check if the SCB is NULL (invalid input)
	if(CUR_SOCKET == NULL) return -1;

	// Handle the case where the socket is a listener
	if(CUR_SOCKET->type == SOCKET_LISTENER){
		// Remove the socket from the port map
		PORT_MAP[CUR_SOCKET->port] = NULL;

		// Notify all threads waiting on the request queue
		kernel_broadcast(&CUR_SOCKET->listener_s.req_available);
	}
	// Handle the case where the socket is a peer
	else if(CUR_SOCKET->type == SOCKET_PEER){
		// Close the read pipe if it exists
		if(CUR_SOCKET->peer_s.read_pipe != NULL){
			pipe_reader_close(CUR_SOCKET->peer_s.read_pipe);
			CUR_SOCKET->peer_s.read_pipe = NULL;
		}
		// Close the write pipe if it exists
		if(CUR_SOCKET->peer_s.write_pipe != NULL){
			pipe_writer_close(CUR_SOCKET->peer_s.write_pipe);
			CUR_SOCKET->peer_s.write_pipe = NULL;
		}

	}

	// Decrease the reference count
	CUR_SOCKET->refcount--;

	// Free the SCB memory if no references remain
	if(CUR_SOCKET->refcount == 0)
		free(CUR_SOCKET);

	// Return success
	return 0;
}


// Define the file operations structure for sockets
file_ops socket_fileops={
	.Open = NULL,	// Sockets do not support the Open operation
	.Read = socket_read, // Assign socket_read function for reading
	.Write = socket_write, // Assign socket_write function for writing
	.Close = socket_close // Assign socket_close function for closing
};




Fid_t sys_Socket(port_t port)
{
	// Check if the port number is within valid bounds
	if(port > MAX_PORT || port < 0) return NOFILE; // Return NOFILE if the port is invalid

	FCB* fcb; // Pointer to the File Control Block (FCB)
	Fid_t fid = 0; // File ID (unique identifier for the socket)

	// Check if the file ID is invalid
	if(fid == NOFILE) return NOFILE;

	// Reserve an FCB and assign it to the file ID
	if(FCB_reserve(1,&fid,&fcb)==0) return NOFILE; // Return NOFILE if reservation fails

	// Allocate memory for the socket control block (SCB)
	socket_cb* s_cb = (socket_cb*)xmalloc(sizeof(socket_cb));

	// Initialize the socket control block
	s_cb->port = port;	// Set the port number
	s_cb->refcount = 0;	// Initialize reference count to 0
	s_cb->fcb = fcb;	// Associate the FCB with the socket
	s_cb->type = SOCKET_UNBOUND; // Set the socket type to unbound

	// Associate the socket operations with the FCB
	fcb->streamfunc = &socket_fileops; 

	// Link the SCB to the FCB
	fcb->streamobj = s_cb;

	 // Return the file ID of the newly created socket
	return fid;

}


int sys_Listen(Fid_t sock)
{
	// Retrieve the File Control Block (FCB) for the given socket file ID
	FCB* CUR_FCB = get_fcb(sock);

	// Check if the FCB is NULL (invalid socket)
	if(CUR_FCB == NULL) return -1;

	// Check if the socket file ID is invalid
	if(sock == NOFILE) return -1;

	// Retrieve the socket control block (SCB) from the FCB
	socket_cb* s_cb = (socket_cb*)CUR_FCB->streamobj;

	// Validate the SCB and its properties
	if(s_cb==NULL || s_cb->port<0 || s_cb->port>MAX_PORT) return -1; // Return -1 if the SCB or port is invalid

	// Check if the port is already bound to another socket
	if(PORT_MAP[s_cb->port] != NULL) return -1;

	// Check if the port is unassigned
	if(s_cb->port == NOPORT) return -1;

	// Ensure the socket is unbound before it can become a listener
	if(s_cb->type != SOCKET_UNBOUND) return -1;

	// Bind the socket to the port and initialize as a listener
	PORT_MAP[s_cb->port] = s_cb; // Map the port to the SCB
	s_cb->type = SOCKET_LISTENER; // Set the type to listener

	// Initialize the listener-specific properties
	rlnode_init(&s_cb->listener_s.queue, NULL); // Initialize the request queue
	s_cb->listener_s.req_available = COND_INIT;	// Initialize the condition variable for requests

	 // Return success
	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{
	// Retrieve the File Control Block (FCB) for the listening socket
	FCB* fcb = get_fcb(lsock);

	// Check if the FCB is invalid or the socket does not exist
	if(fcb == NULL) return NOFILE;

	// Retrieve the socket control block (SCB) from the FCB
	socket_cb* this_socket = (socket_cb*) fcb->streamobj;

	// Validate the SCB and ensure it is a listener socket on a valid port
	if(this_socket == NULL || this_socket->port<0 || this_socket-> port>MAX_PORT) return NOFILE;

	// Ensure the socket is a listener socket
	if(this_socket->type != SOCKET_LISTENER) return NOFILE;

	// Increment the reference count to mark this socket as in use
	this_socket->refcount++;

	// Wait until a connection request is available or the socket is closed
	while(is_rlist_empty(&this_socket->listener_s.queue) && this_socket -> refcount != 0){
		// If the socket is removed from the port map, return NOFILE
		if(PORT_MAP[this_socket->port]==NULL) return NOFILE;
		// Wait for a connection request to be added to the queue
		kernel_wait(&this_socket->listener_s.req_available,SCHED_PIPE);
	}

	// Check again if the socket has been removed from the port map
	if(PORT_MAP[this_socket->port]==NULL) return NOFILE;

	// Retrieve the next connection request from the queue
	rlnode* this_node = rlist_pop_front(&this_socket->listener_s.queue);
	connection_request* this_request = (connection_request*)this_node -> connection_request;

	// Retrieve the peer socket control block from the connection request
	socket_cb* socket_cb1 = this_request -> peer;

	// Validate the peer socket control block
	if(socket_cb1 == NULL) return NOFILE;

	// Ensure the peer socket is unbound
	if(socket_cb1->type != SOCKET_UNBOUND) return NOFILE;

	// Create a new socket for the connection
	Fid_t s1 = sys_Socket(socket_cb1->port);

	// Retrieve the FCB for the new socket
	FCB* S2 = get_fcb(s1);

	// Validate the new socket
	if(s1==NOFILE) return NOFILE;

	// Retrieve the socket control block for the new socket
	socket_cb* socket_cb2 = (socket_cb*)S2->streamobj;

	// Validate the new socket control block
	if(socket_cb2 == NULL) return NOFILE;

	// Update both sockets to peer type
	socket_cb1->type = SOCKET_PEER;
	socket_cb2->type = SOCKET_PEER;

	// Allocate and initialize the first pipe (from socket_cb2 to socket_cb1)
	pipe_cb* this_1st_Pipe = (pipe_cb*)xmalloc(sizeof(pipe_cb));
	this_1st_Pipe->writer = socket_cb2->fcb;
	this_1st_Pipe->reader = socket_cb1->fcb;
	this_1st_Pipe->has_space = COND_INIT;
	this_1st_Pipe->has_data = COND_INIT;
	this_1st_Pipe->w_position = 0;
	this_1st_Pipe->r_position = 0;

	// Allocate and initialize the second pipe (from socket_cb1 to socket_cb2)
	pipe_cb* this_2nd_Pipe = (pipe_cb*)xmalloc(sizeof(pipe_cb));
	this_2nd_Pipe->writer = socket_cb1->fcb;
	this_2nd_Pipe->reader = socket_cb2->fcb;
	this_2nd_Pipe->has_space = COND_INIT;
	this_2nd_Pipe->has_data = COND_INIT;
	this_2nd_Pipe->w_position = 0;
	this_2nd_Pipe->r_position = 0;

	// Link the pipes and peers for socket_cb1
	socket_cb1->peer_s.peer = socket_cb2;
	socket_cb1->peer_s.read_pipe = this_1st_Pipe;
	socket_cb1->peer_s.write_pipe = this_2nd_Pipe;

	// Link the pipes and peers for socket_cb2
	socket_cb2->peer_s.peer = socket_cb1;
	socket_cb2->peer_s.read_pipe = this_2nd_Pipe;
	socket_cb2->peer_s.write_pipe = this_1st_Pipe;

	// Mark the connection request as admitted
	this_request->admitted = 1;

	// Signal the thread waiting on this connection request
	kernel_signal(&this_request->connected_cv);

	// Decrement the reference count of the listener socket
	this_socket->refcount--;

	// Return the file ID of the newly created socket
	return s1;


}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{

	//checking

FCB *curr = get_fcb(sock);

// If no FCB exists, the socket is invalid, return an error
    if(curr == NULL){

        return -1;

    }

   //get the scb that is linked with the fcb that has a fileid equal to sock 
    socket_cb *socket = curr->streamobj;

    //not legal
    // Check if the SCB is NULL or the socket is not in the unbound state, return an error
    if(socket == NULL || socket->type!=SOCKET_UNBOUND) return -1;

    //ilegal port

    if(port<0 || port>MAX_PORT) return -1;

    // Retrieve the listener socket control block for the given port
    socket_cb *listener = PORT_MAP[port];

    //no listener socket bound on given port

    if(listener == NULL) return -1;


	// Increment the reference count of the current socket.
    socket->refcount++;


    //create the connection request 
    connection_request* connect = (connection_request*)xmalloc(sizeof(connection_request));

    connect->admitted=0;
    connect->peer = socket;
    connect->connected_cv = COND_INIT;

    // add to the waiting list of the server socket
    rlnode_init(&connect->queue_node,connect);
    //insert request in accept queue
    rlist_push_back(&listener->listener_s.queue,&connect->queue_node);
    //wakes up request
    kernel_signal(&listener->listener_s.req_available);

    //timeout expire

    if(kernel_timedwait(&connect->connected_cv,SCHED_PIPE,timeout)==0){

        return -1;

    }

    // Free the connection request object
    free(connect);

    //free the socket and return an error if refcount = 0 
    if(socket->refcount==0) {

        free(socket);

        return -1;

    }
    // Decrese by 1 the socket refcount
    else{

        socket->refcount--;

        if(connect->admitted==0) {

            return -1;

        }else {
        	// Connection was successful
            return 0;

        }

    }
    //return 0
    return 0;

}


int sys_ShutDown(Fid_t sock, shutdown_mode how) {
	// Retrieve the File Control Block (FCB)
    FCB* fcb = get_fcb(sock);

    // Ensure the socket ID is within valid bounds(0< <16)
    if (fcb == NULL || sock < 0 || sock > MAX_FILEID) {
        return -1;
    }

    // Get the SCB linked with the file identifier socket
    socket_cb* socket = (socket_cb*)fcb->streamobj;

    // Perform the shutdown based on the specified mode
    if (socket == NULL || socket->type != SOCKET_PEER) {
        return -1;
    }

    // Perform the shutdown based on the specified mode
    switch (how) {

    	 // Handle shutdown of the read pipe
        case SHUTDOWN_READ:
        	// Check if the read pipe exists and close it if valid
            if (socket->peer_s.read_pipe != NULL) {
                pipe_reader_close(socket->peer_s.read_pipe);
                // Set the read pipe to NULL after closing
                socket->peer_s.read_pipe = NULL;
            }
            break;
           // Handle shutdown of the write pipe
        case SHUTDOWN_WRITE:
        	// Check if the write pipe exists and close it if valid
            if (socket->peer_s.write_pipe != NULL) {
                pipe_writer_close(socket->peer_s.write_pipe);
                 // Set the write pipe to NULL after closing
                socket->peer_s.write_pipe = NULL;
            }
            break;
            // Handle shutdown of both read and write pipes
        case SHUTDOWN_BOTH:
        	// Close the read pipe if it exists
            if (socket->peer_s.read_pipe != NULL) {
                pipe_reader_close(socket->peer_s.read_pipe);
                socket->peer_s.read_pipe = NULL;// Set the read pipe to NULL after closing
            }
            // Close the write pipe if it exists
            if (socket->peer_s.write_pipe != NULL) {
                pipe_writer_close(socket->peer_s.write_pipe);
                socket->peer_s.write_pipe = NULL;// Set the write pipe to NULL after closing
            }
            break;

        // Handle invalid shutdown mode
        default:
            return -1;
    }
    //return 0
    return 0;
}

