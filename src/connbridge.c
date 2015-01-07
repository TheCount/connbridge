#define _GNU_SOURCE

#include<assert.h>
#include<errno.h>
#include<ev.h>
#include<netdb.h>
#include<signal.h>
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<unistd.h>

#include<arpa/inet.h>

#include<sys/socket.h>
#include<sys/types.h>

/**
 * Listener backlog.
 */
#define BACKLOG 1000

/**
 * R/W buffer size in bytes.
 */
#define FILE_BUFSIZE 8192

/**
 * EOF indicator.
 */
#define FILE_EOF ( -1 )

/**
 * Error indicator.
 */
#define FILE_ERROR ( -2 )

/**
 * Listener.
 */
struct Listener {
	/**
	 * Address.
	 */
	char * address;

	/**
	 * Watcher.
	 */
	ev_io watcher;
};

/**
 * Bridge.
 */
struct Bridge {
	/**
	 * Source watcher.
	 */
	ev_io srcwatcher;

	/**
	 * Destination watcher.
	 */
	ev_io destwatcher;

	/**
	 * Pointer to source output file,
	 * or a null pointer if the file is not open.
	 */
	FILE * srcFile;

	/**
	 * Pointer to destination output file,
	 * or a null pointer if the file is not open.
	 */
	FILE * destFile;

	/**
	 * Position of next unbridged data in source output.
	 */
	fpos_t srcPos;

	/**
	 * Position of next unbridged data in destination output.
	 */
	fpos_t destPos;

	/**
	 * Whether we received EOF from source.
	 */
	unsigned int eofFromSource : 1;

	/**
	 * Whether all bytes from the source have been bridged.
	 */
	unsigned int sourceFlushed : 1;

	/**
	 * Whether we are connected to the destination.
	 */
	unsigned int connectedToDestination : 1;

	/**
	 * Whether we received EOF from destination.
	 */
	unsigned int eofFromDestination : 1;

	/**
	 * Whether all bytes from the destination have been bridged.
	 */
	unsigned int destinationFlushed : 1;
};

/**
 * Address info hints.
 */
static const struct addrinfo ADDRINFO_HINTS = {
	.ai_flags = AI_V4MAPPED | AI_ALL,
	.ai_family = AF_UNSPEC,
	.ai_socktype = SOCK_STREAM,
	.ai_protocol = 0,
	.ai_addrlen = 0,
	.ai_addr = NULL,
	.ai_canonname = NULL,
	.ai_next = NULL
};

/**
 * Event loop.
 */
static struct ev_loop * loop;

/**
 * Destination address.
 */
static struct addrinfo * destaddr;

/**
 * Returns a string describing a socket address.
 *
 * @param addr Socket address.
 * @param socklen Address length.
 *
 * @return On success, a string describing the socket address is returned in a statically allocated buffer.
 * 	On error, a null pointer is returned, and @c errno is set appropriately.
 */
static const char * satos( struct sockaddr * addr, size_t socklen ) {
	assert( addr != NULL );

	char buf[INET6_ADDRSTRLEN];
	static char result[INET6_ADDRSTRLEN + 16]; // address, brackets, colon, port
	const char * rcp;
	unsigned int port;
	int rc;

	switch( addr->sa_family ) {
		case AF_INET:
			if ( socklen < sizeof( struct sockaddr_in ) ) {
				errno = EINVAL;
				return NULL;
			}
			struct sockaddr_in * addr_in = ( struct sockaddr_in * ) addr;
			rcp = inet_ntop( addr->sa_family, &addr_in->sin_addr, buf, sizeof( buf ) );
			port = ntohs( addr_in->sin_port );
			rc = snprintf( result, sizeof( result ), "%s:%u", rcp, port );
			break;

		case AF_INET6:
			if ( socklen < sizeof( struct sockaddr_in6 ) ) {
				errno = EINVAL;
				return NULL;
			}
			struct sockaddr_in6 * addr_in6 = ( struct sockaddr_in6 * ) addr;
			rcp = inet_ntop( addr->sa_family, &addr_in6->sin6_addr, buf, sizeof( buf ) );
			port = ntohs( addr_in6->sin6_port );
			rc = snprintf( result, sizeof( result ), "[%s]:%u", rcp, port );
			break;

		default:
			errno = EAFNOSUPPORT;
			return NULL;
	}

	if ( rc < 0 ) {
		errno = EILSEQ;
		return NULL;
	} else {
		return result;
	}
}

/**
 * Closes a file descriptor, printing an error message on failure.
 *
 * @param fd File descriptor.
 */
static void close_fd( int fd ) {
	for ( ;; ) {
		int rc = close( fd );
		if ( rc == 0 ) {
			return;
		} else if ( errno == EINTR ) {
			continue;
		} else {
			fprintf( stderr, "Unable to properly close fd %d: %s", fd, strerror( errno ) );
			return;
		}
	}
}

/**
 * Closes an output file, printing an error message on failure.
 *
 * @param f Pointer to file.
 */
static void close_output_file( FILE * f ) {
	assert( f != NULL );

	for ( ;; ) {
		int rc = fclose( f );
		if ( rc == 0 ) {
			return;
		} else if ( errno == EINTR ) {
			continue;
		} else {
			perror( "Unable to properly close file" );
			return;
		}
	}
}

/**
 * Opens an output file.
 *
 * @param addrlen Length of output address.
 * @param addr Pointer to output address.
 * @param pos Pointer to file position.
 *
 * @return On success, a pointer to the opened file is returned.\n
 * 	On error, a null pointer is returned.
 */
static FILE * open_output_file( socklen_t addrlen, struct sockaddr * addr, fpos_t * pos ) {
	assert( addrlen > 0 );
	assert( addr != NULL );
	assert( pos != NULL );

	/* Get name */
	const char * name = satos( addr, addrlen );
	if ( name == NULL ) {
		fputs( "Unable to obtain output file name.\n", stderr );
		goto noname;
	}

	/* Open file */
	FILE * result = fopen( name, "a+" );
	if ( result == NULL ) {
		fprintf( stderr, "Unable to open output file '%s': %s\n", name, strerror( errno ) );
		goto noopen;
	}

	/* Set position */
	int rc = fseek( result, 0, SEEK_END );
	if ( rc != 0 ) {
		fprintf( stderr, "Unable to seek to end of file '%s': %s\n", name, strerror( errno ) );
		goto noseek;
	}
	rc = fgetpos( result, pos );
	if ( rc != 0 ) {
		fprintf( stderr, "Unable to obtain file position of file '%s': %s\n", name, strerror( errno ) );
		goto nogetpos;
	}

	return result;

nogetpos:
noseek:
	close_output_file( result );
noopen:
noname:
	return NULL;
}

/**
 * Uninitialises a bridge.
 * Outstanding buffers are @em not written.
 *
 * @param bridge Pointer to bridge.
 */
static void bridge_fini( struct Bridge * bridge ) {
	assert( bridge != NULL );

	ev_io_stop( loop, &bridge->srcwatcher );
	ev_io_stop( loop, &bridge->destwatcher );
	close_fd( bridge->srcwatcher.fd );
	close_fd( bridge->destwatcher.fd );
	close_output_file( bridge->srcFile );
	close_output_file( bridge->destFile );
}

/**
 * Destroys a bridge.
 *
 * @param bridge Pointer to bridge.
 */
static void bridge_del( struct Bridge * bridge ) {
	assert( bridge != NULL );

	bridge_fini( bridge );
	free( bridge );
}

/**
 * Reads from a socket into a file.
 *
 * @param fd File descriptor to read data from.
 * @param f Pointer to file to write into.
 * 	The file should be opened in append mode.
 * @param count Pointer to a location where the number of read and written bytes can be stored.
 *
 * @return On success, the number of processed bytes is stored in @c *count, and @c 0 is returned.\n
 * 	If there are no more bytes from the specified file descriptor, the number of processed bytes is stored in @c *count, and #FILE_EOF is returned.\n
 * 	On error, @c *count is unspecified, and #FILE_ERROR is returned.
 */
static int read_into_file( int fd, FILE * f, size_t * count ) {
	assert( fd >= 0 );
	assert( f != NULL );
	assert( count != NULL );

	*count = 0;
	char buf[FILE_BUFSIZE];

	for ( ;; ) {
		/* Read data in chunks of FILE_BUFSIZE */
		ssize_t bytesRead = read( fd, buf, sizeof( buf ) );
		if ( bytesRead < 0 ) {
			if ( errno == EINTR ) {
				continue;
			} else if ( ( errno == EAGAIN ) || ( errno == EWOULDBLOCK ) ) {
				return 0;
			} else {
				fprintf( stderr, "Error reading from connection %d: %s\n", fd, strerror( errno ) );
				return FILE_EOF;
			}
		} else if ( bytesRead == 0 ) {
			return FILE_EOF;
		}
		/* Write data to file */
		size_t isWritten = fwrite( buf, bytesRead, 1, f );
		if ( !isWritten ) {
			fputs( "Unable to write to file\n", stderr );
			return FILE_ERROR;
		}
		*count += bytesRead;
	}
}

/**
 * Writes data from a file to a socket.
 *
 * @param fd File descriptor to write data to.
 * @param f Pointer to file to read data from.
 * @param pos Pointer to file position to start reading from.
 * 	Also used to store new file position for repeated calls.
 *
 * @return If all data from the specified file (up to end-of-file) could be written to the specified file descriptor, #FILE_EOF is returned.\n
 * 	If not all data from the specified file could be written to the specified file descriptor, but no error occurred, @c 0 is returned.\n
 * 	Otherwise, #FILE_ERROR is returned.
 */
static int write_from_file( int fd, FILE * f, fpos_t * pos ) {
	assert( fd >= 0 );
	assert( f != NULL );
	assert( pos != NULL );

	char buf[FILE_BUFSIZE];

	/* Set initial reading position */
	int rc = fsetpos( f, pos );
	if ( rc != 0 ) {
		perror( "Unable to seek to correct file position for reading" );
		return FILE_ERROR;
	}

	for ( ;; ) {
		/* Read from file */
		size_t bytesRead = fread( buf, 1, sizeof( buf ), f );
		if ( bytesRead == 0 ) {
			if ( feof( f ) ) {
				return FILE_EOF;
			}
			fputs( "Error reading from file\n", stderr );
			return FILE_ERROR;
		}
		/* Write to socket */
		for ( size_t bufpos = 0; bufpos != bytesRead; ) {
			ssize_t bytesWritten = write( fd, buf + bufpos, bytesRead - bufpos );
			if ( bytesWritten < 0 ) {
				if ( errno == EINTR ) {
					continue;
				} else if ( ( errno == EAGAIN ) || ( errno == EWOULDBLOCK ) ) {
					/* Update file position */
					rc = fsetpos( f, pos );
					if ( rc != 0 ) {
						perror( "Unable to set correct file position" );
						return FILE_ERROR;
					}
					if ( bufpos > 0 ) {
						rc = fseek( f, bufpos, SEEK_CUR );
						if ( rc != 0 ) {
							perror( "Unable to set correct file position" );
							return FILE_ERROR;
						}
						rc = fgetpos( f, pos );
						if ( rc != 0 ) {
							perror( "Unable to obtain current file position" );
							return FILE_ERROR;
						}
					}
					return 0;
				} else {
					fprintf( stderr, "Error writing to socket %d: %s\n", fd, strerror( errno ) );
					return FILE_ERROR;
				}
			}
			bufpos += bytesWritten;
		}
		/* Update file position */
		rc = fgetpos( f, pos );
		if ( rc != 0 ) {
			perror( "Unable to obtain current file position" );
			return FILE_ERROR;
		}
	}
}

/**
 * Bridge callback.
 *
 * @param loop Pointer to event loop.
 * @param watcher Pointer to IO watcher.
 * @param flags Event flags.
 */
static void bridge_cb( struct ev_loop * loop, ev_io * watcher, int flags ) {
	assert( loop != NULL );
	assert( watcher != NULL );

	int rc;

	struct Bridge * bridge = watcher->data;
	if ( flags & EV_ERROR ) {
		fputs( "Error in bridge callback\n", stderr );
		goto error;
	}
	assert( flags & ( EV_READ | EV_WRITE ) );

	/* Check if we need to complete the connection */
	if ( !bridge->connectedToDestination ) {
		assert( watcher == &bridge->destwatcher );
		int errcode;
		socklen_t errcodelen = sizeof( errcode );
		rc = getsockopt( watcher->fd, SOL_SOCKET, SO_ERROR, &errcode, &errcodelen );
		if ( rc != 0 ) {
			fprintf( stderr, "Unable to obtain connection completion information for socket %d: %s\n", watcher->fd, strerror( errno ) );
			goto error;
		}
		if ( errcode == 0 ) {
			/* Start normal operation */
			ev_io_stop( loop, &bridge->destwatcher );
			ev_io_set( &bridge->srcwatcher, bridge->srcwatcher.fd, EV_READ );
			ev_io_set( &bridge->destwatcher, bridge->destwatcher.fd, EV_READ );
			ev_io_start( loop, &bridge->srcwatcher );
			ev_io_start( loop, &bridge->destwatcher );
			bridge->connectedToDestination = 1;
			return;
		} else {
			fprintf( stderr, "Unable to complete connection for socket %d: %s\n", watcher->fd, strerror( errcode ) );
			goto error;
		}
	}

	/* Perform operation */
	/*+ Read source +*/
	size_t count = 0;
	rc = 0;
	if ( !bridge->eofFromSource ) {
		rc = read_into_file( bridge->srcwatcher.fd, bridge->srcFile, &count );
	}
	if ( rc == FILE_ERROR ) {
		fputs( "Error reading from source into source output file\n", stderr );
		goto error;
	} else if ( rc == FILE_EOF ) {
		bridge->eofFromSource = 1;
		rc = shutdown( bridge->srcwatcher.fd, SHUT_RD );
		if ( rc != 0 ) {
			fprintf( stderr, "Unable to shutdown source %d for reading: %s\n", bridge->srcwatcher.fd, strerror( errno ) );
		}
	}
	/*+ bridge source to destination +*/
	if ( !bridge->sourceFlushed || ( count > 0 ) ) {
		rc = write_from_file( bridge->destwatcher.fd, bridge->srcFile, &bridge->srcPos );
	} else {
		rc = FILE_EOF;
	}
	if ( rc == FILE_ERROR ) {
		fputs( "Error writing from source output file to destination\n", stderr );
		goto error;
	} else if ( rc == FILE_EOF ) {
		bridge->sourceFlushed = 1;
		if ( bridge->eofFromSource ) {
			rc = shutdown( bridge->destwatcher.fd, SHUT_WR );
			if ( rc != 0 ) {
				fprintf( stderr, "Unable to shutdown destination %d for writing: %s\n", bridge->destwatcher.fd, strerror( errno ) );
			}
		}
	} else {
		bridge->sourceFlushed = 0;
	}
	/*+ read destination +*/
	count = 0;
	rc = 0;
	if ( !bridge->eofFromDestination ) {
		rc = read_into_file( bridge->destwatcher.fd, bridge->destFile, &count );
	}
	if ( rc == FILE_ERROR ) {
		fputs( "Error reading from destination into destination output file\n", stderr );
		goto error;
	} else if ( rc == FILE_EOF ) {
		bridge->eofFromDestination = 1;
		rc = shutdown( bridge->destwatcher.fd, SHUT_RD );
		if ( rc != 0 ) {
			fprintf( stderr, "Unable to shutdown destination %d for reading: %s\n", bridge->destwatcher.fd, strerror( errno ) );
		}
	}
	/*+ bridge destination to source +*/
	if ( !bridge->destinationFlushed || ( count > 0 ) ) {
		rc = write_from_file( bridge->srcwatcher.fd, bridge->destFile, &bridge->destPos );
	} else {
		rc = FILE_EOF;
	}
	if ( rc == FILE_ERROR ) {
		fputs( "Error writing from destination output file to source\n", stderr );
		goto error;
	} else if ( rc == FILE_EOF ) {
		bridge->destinationFlushed = 1;
		if ( bridge->eofFromDestination ) {
			rc = shutdown( bridge->srcwatcher.fd, SHUT_WR );
			if ( rc != 0 ) {
				fprintf( stderr, "Unable to shutdown source %d for writing: %s\n", bridge->srcwatcher.fd, strerror( errno ) );
			}
		}
	} else {
		bridge->destinationFlushed = 0;
	}

	/* Recalibrate watchers */
	int newflags;
	if ( !bridge->eofFromSource ) {
		newflags = EV_READ;
	} else {
		newflags = 0;
	}
	if ( !bridge->destinationFlushed ) {
		newflags |= EV_WRITE;
	}
	if ( bridge->srcwatcher.events != newflags ) {
		ev_io_stop( loop, &bridge->srcwatcher );
		ev_io_set( &bridge->srcwatcher, bridge->srcwatcher.fd, newflags );
		if ( newflags != 0 ) {
			ev_io_start( loop, &bridge->srcwatcher );
		}
	}
	if ( !bridge->eofFromDestination ) {
		newflags = EV_READ;
	} else {
		newflags = 0;
	}
	if ( !bridge->sourceFlushed ) {
		newflags |= EV_WRITE;
	}
	if ( bridge->destwatcher.events != newflags ) {
		ev_io_stop( loop, &bridge->destwatcher );
		ev_io_set( &bridge->destwatcher, bridge->destwatcher.fd, newflags );
		if ( newflags != 0 ) {
			ev_io_start( loop, &bridge->destwatcher );
		}
	}
	if ( ( ( bridge->srcwatcher.events & ( EV_READ | EV_WRITE ) ) == 0 ) && ( ( bridge->destwatcher.events & ( EV_READ | EV_WRITE ) ) == 0 ) ) {
		/* All done */
		goto cleanup;
	}

	return;

cleanup:
error:
	bridge_del( bridge );
}

/**
 * Initialises a bridge.
 *
 * @param bridge Pointer to bridge to be initialised.
 * @param srcfd Source connection socket.
 * @param srcaddrlen Length of source peer address.
 * @param srcaddr Pointer to source peer address.
 *
 * @return On success, @c 0 is returned.\n
 * 	On error, @c -1 is returned.
 */
static int bridge_init( struct Bridge * bridge, int srcfd, socklen_t srcaddrlen, struct sockaddr * srcaddr ) {
	assert( bridge != NULL );
	assert( srcfd >= 0 );
	assert( srcaddrlen > 0 );
	assert( srcaddr != NULL );

	int rc;

	/* Initiate connection to destination */
	int destfd = socket( destaddr->ai_family, destaddr->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, destaddr->ai_protocol );
	if ( destfd < 0 ) {
		perror( "Unable to create destination socket" );
		goto nodestfd;
	}
	for ( ;; ) {
		rc = connect( destfd, destaddr->ai_addr, destaddr->ai_addrlen );
		if ( rc == 0 ) {
			bridge->connectedToDestination = 1;
			break;
		} else {
			if ( errno == EINTR ) {
				continue;
			} else if ( errno == EINPROGRESS ) {
				bridge->connectedToDestination = 0;
				break;
			} else {
				perror( "Unable to establish connection to destination" );
				goto noconnect;
			}
		}
	}

	/* Open output files */
	bridge->srcFile = open_output_file( srcaddrlen, srcaddr, &bridge->srcPos );
	if ( bridge->srcFile == NULL ) {
		fputs( "Unable to open source output file\n", stderr );
		goto nosrcfile;
	}
	struct sockaddr_storage destaddr;
	socklen_t destaddrlen = sizeof( destaddr );
	rc = getsockname( destfd, ( struct sockaddr * ) &destaddr, &destaddrlen );
	if ( rc != 0 ) {
		fprintf( stderr, "Unable to obtain destination socket name %d: %s\n", destfd, strerror( errno ) );
		goto nodestfile;
	}
	bridge->destFile = open_output_file( destaddrlen, ( struct sockaddr * ) &destaddr, &bridge->destPos );
	if ( bridge->destFile == NULL ) {
		fputs( "unable to open destination output file\n", stderr );
		goto nodestfile;
	}

	/* Init watchers */
	ev_io_init( &bridge->srcwatcher, bridge_cb, srcfd, EV_READ );
	ev_io_init( &bridge->destwatcher, bridge_cb, destfd, EV_READ );
	bridge->srcwatcher.data = bridge;
	bridge->destwatcher.data = bridge;
	if ( bridge->connectedToDestination ) {
		/* Watch for incoming data right away */
		ev_io_start( loop, &bridge->srcwatcher );
		ev_io_start( loop, &bridge->destwatcher );
	} else {
		/* Wait for destination connection to complete */
		ev_io_set( &bridge->destwatcher, destfd, EV_WRITE );
		ev_io_start( loop, &bridge->destwatcher );
	}

	/* Init remaining flags */
	bridge->eofFromSource = 0;
	bridge->sourceFlushed = 1;
	bridge->eofFromDestination = 0;
	bridge->destinationFlushed = 1;

	return 0;

nodestfile:
	close_output_file( bridge->srcFile );
nosrcfile:
noconnect:
	close_fd( destfd );
nodestfd:
	return -1;
}

/**
 * Creates a new bridge.
 *
 * @param fd Source connection socket.
 * @param srcaddrlen Length of source peer address.
 * @param srcaddr Pointer to source peer address.
 *
 * @return On success, a pointer to the new bridge is returned.\n
 * 	On error, a null pointer is returned.
 */
static struct Bridge * bridge_new( int fd, socklen_t srcaddrlen, struct sockaddr * srcaddr ) {
	assert( fd >= 0 );
	assert( srcaddrlen > 0 );
	assert( srcaddr != NULL );

	struct Bridge * result = malloc( sizeof( *result ) );
	if ( result == NULL ) {
		fprintf( stderr, "Insufficient memory to allocate bridge memory for socket %d\n", fd );
		goto nomem;
	}
	int rc = bridge_init( result, fd, srcaddrlen, srcaddr );
	if ( rc != 0 ) {
		fprintf( stderr, "Unable to initialise bridge for socket %d\n", fd );
		goto noinit;
	}

	return result;

noinit:
	free( result );
nomem:
	return NULL;
}

/**
 * Starts bridging a connection.
 *
 * @param fd Source connection socket.
 * @param srcaddrlen Length of source peer address.
 * @param srcaddr Pointer to source peer address.
 *
 * @return On success, @c 0 is returned.\n
 * 	On error, the connection socket is closed, and @c -1 is returned.
 */
static int start_bridge( int fd, socklen_t srcaddrlen, struct sockaddr * srcaddr ) {
	assert( fd >= 0 );
	assert( srcaddrlen > 0 );
	assert( srcaddr != NULL );

	/* Create new bridge */
	struct Bridge * bridge = bridge_new( fd, srcaddrlen, srcaddr );
	if ( bridge == NULL ) {
		fputs( "Unable to create new bridge\n", stderr );
		goto nobridge;
	}

	return 0;

nobridge:
	close_fd( fd );

	return -1;
}

/**
 * Accepts connections.
 *
 * @param loop Pointer to event loop.
 * @param watcher Pointer to server watcher.
 * @param flags Event flags.
 */
static void accept_cb( struct ev_loop * loop, ev_io * watcher, int flags ) {
	( void ) loop; /* Not used */

	/* Sanity checks */
	if ( flags & EV_ERROR ) {
		fputs( "Error in server watcher.\n", stderr );
		return;
	}
	assert( flags & EV_READ );

	/* Accept as many connections as possible */
	for ( ;; ) {
		/* Get next connection */
		struct sockaddr_storage addr;
		socklen_t addrlen = sizeof( addr );
		int fd = accept4( watcher->fd, ( struct sockaddr * ) &addr, &addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC );
		if ( fd < 0 ) {
			break;
		}

		/* Start bridge on connection */
		start_bridge( fd, addrlen, ( struct sockaddr * ) &addr );
	}
}

/**
 * Starts a listener.
 *
 * @param addr Socket address.
 * @param len Address length.
 *
 * @return On success, 0 is returned.
 * 	On error, -1 is returned.
 */
static int start_listener( struct sockaddr * addr, size_t len ) {
	const char * rcp = satos( addr, len );
	if ( rcp == NULL ) {
		rcp = strerror( errno );
	}

	/* Listen */
	struct Listener * listener = malloc( sizeof( *listener ) );
	if ( listener == NULL ) {
		fprintf( stderr, "Insufficient memory to create listener for address %s\n", rcp );
		goto nolistenermem;
	}
	listener->address = strdup( rcp );
	if ( listener->address == NULL ) {
		fprintf( stderr, "Insufficient memory to create listener address %s\n", rcp );
		goto noaddressmem;
	}
	int fd = socket( addr->sa_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0 );
	if ( fd < 0 ) {
		fprintf( stderr, "Unable to create server socket for address %s: %s\n", rcp, strerror( errno ) );
		goto nosocket;
	}
	int optval = 1;
	int rc = setsockopt( fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof( optval ) );
	if ( rc != 0 ) {
		fprintf( stderr, "Unable to make server socket %d for address %s reusable: %s\n", fd, rcp, strerror( errno ) );
	}
	rc = bind( fd, addr, len );
	if ( rc != 0 ) {
		fprintf( stderr, "Unable to bind server socket %d for address %s: %s\n", fd, rcp, strerror( errno ) );
		goto nobind;
	}
	rc = listen( fd, BACKLOG );
	if ( rc != 0 ) {
		fprintf( stderr, "Unable to listen on server socket %d for address %s: %s\n", fd, rcp, strerror( errno ) );
		goto nolisten;
	}

	/* Watch */
	listener->watcher.data = listener;
	ev_io_init( &listener->watcher, accept_cb, fd, EV_READ );
	ev_io_start( loop, &listener->watcher );

	printf( "Listener %d listening on %s\n", fd, rcp );

	return 0;

	ev_io_stop( loop, &listener->watcher );
nolisten:
nobind:
	close_fd( fd );
nosocket:
	free( listener->address );
noaddressmem:
	free( listener );
nolistenermem:
	return -1;
}

/**
 * Prints program usage on stderr.
 *
 * @param progname Pointer to program name string.
 */
static void print_usage( const char * progname ) {
	fprintf( stderr, "Usage: %s srcaddr srcport destaddr destport\n", progname );
}

int main( int argc, char ** argv ) {
#ifdef SIGPIPE
	/* We handle broken pipes manually */
	sighandler_t oh = signal( SIGPIPE, SIG_IGN );
	if ( oh == SIG_ERR ) {
		perror( "Unable to ignore SIGPIPE" );
		exit( EXIT_FAILURE );
	}
#endif

	/* Get args */
	if ( argc <= 4 ) {
		print_usage( argv[0] );
		exit( EXIT_FAILURE );
	}
	const char * srcnode = argv[1];
	const char * srcservice = argv[2];
	const char * destnode = argv[3];
	const char * destservice = argv[4];

	/* Get loop */
	loop = ev_default_loop( 0 );
	if ( loop == NULL ) {
		fputs( "Unable to obtain event loop.\n", stderr );
		exit( EXIT_FAILURE );
	}

	/* Get destination address */
	struct addrinfo * airesult;
	int rc = getaddrinfo( destnode, destservice, &ADDRINFO_HINTS, &airesult );
	if ( rc != 0 ) {
		fprintf( stderr, "Unable to lookup destination node '%s' service '%s': %s\n", destnode, destservice, gai_strerror( rc ) );
		exit( EXIT_FAILURE );
	}
	if ( airesult == NULL ) {
		fprintf( stderr, "No valid addresses found for destination node '%s' service '%s'\n", destnode, destservice );
		exit( EXIT_FAILURE );
	}
	destaddr = airesult;

	/* Get source addresses */
	rc = getaddrinfo( srcnode, srcservice, &ADDRINFO_HINTS, &airesult );
	if ( rc != 0 ) {
		fprintf( stderr, "Unable to lookup source node '%s' service '%s': %s\n", srcnode, srcservice, gai_strerror( rc ) );
		exit( EXIT_FAILURE );
	}
	if ( airesult == NULL ) {
		fprintf( stderr, "No valid addresses found for source node '%s' service '%s'\n", srcnode, srcservice );
		exit( EXIT_FAILURE );
	}

	/* Start listeners on source addresses */
	for ( struct addrinfo * info = airesult; info != NULL; info = airesult->ai_next ) {
		start_listener( info->ai_addr, info->ai_addrlen );
	}

	/* Enter main loop */
	ev_run( loop, 0 );

	/* Goodbye */
	fputs( "No more listeners.\n", stderr );

	return EXIT_SUCCESS;
}
