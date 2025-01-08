#include "tinyos.h"
#include "kernel_dev.h"
#include "kernel_streams.h"

// Forward declaration of the socket control block (SCB) structure
typedef struct socket_control_block socket_cb;

// Enum representing the different types of sockets
typedef enum{
SOCKET_LISTENER, // A socket used to listen for incoming connections
SOCKET_UNBOUND,  // A socket that is not yet bound to a port or connection
SOCKET_PEER		 // A socket connected to a peer
}socket_type;

// Structure defining a listener socket (used for accepting connections)
typedef struct listener_socket{
	rlnode queue; // Queue to hold incoming connection requests
	CondVar req_available; // Condition variable to signal availability of requests
}listener_socket;

// Structure defining an unbound socket (not connected or bound yet)
typedef struct unbound_socket{
	rlnode unbound_socket; // Node to represent this unbound socket
}unbound_socket;


// Structure defining a peer socket (connected to another socket)
typedef struct peer_socket{
	socket_cb* peer; // Pointer to the peer's socket control block
	pipe_cb* write_pipe; // Pipe used for writing data to the peer
	pipe_cb* read_pipe; // Pipe used for reading data from the peer
}peer_socket; 


// Main structure representing a socket control block
typedef struct socket_control_block{
	uint refcount; // Reference count for this socket
	FCB* fcb; 	   // File Control Block associated with this socket
	socket_type type; // Type of socket (listener, unbound, or peer)
	port_t port;	  // Port number associated with the socket

	// Union to store data specific to the type of socket
	//*Note: The union is used here to save memory, because only one socket type is valide at a time
	union{
		listener_socket listener_s; // Data specific to listener sockets
		unbound_socket unbound_s; // Data specific to unbound sockets
		peer_socket peer_s; // Data specific to peer sockets
	};

}socket_cb;


// Structure representing a connection request
typedef struct connection_request{
	int admitted;	// Flag indicating whether the connection is admitted
	socket_cb* peer;	// Pointer to the peer's socket control block
	CondVar connected_cv; // Condition variable to signal connection establishment
	rlnode queue_node;	 // Node to represent this request in a queue
}connection_request;

// Array mapping port numbers to socket control blocks
socket_cb* PORT_MAP[MAX_PORT];

// Function prototypes for socket operations
int socket_read(void* socket_cb_t, char *buf, unsigned int n); // Read data from a socket
int socket_write(void* socket_cb_t, const char *buf, unsigned int n); // Write data to a socket
int socket_close(void* socket_cb_t); // Close a socket

