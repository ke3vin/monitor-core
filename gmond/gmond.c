#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <apr.h>
#include <apr_strings.h>
#include <apr_hash.h>
#include <apr_time.h>
#include <apr_pools.h>
#include <apr_poll.h>
#include <apr_network_io.h>
#include <apr_signal.h>       
#include <apr_thread_proc.h>  /* for apr_proc_detach(). no threads. */
#include <apr_tables.h>

#include "cmdline.h"   /* generated by cmdline.sh which runs gengetopt */
#include "confuse.h"   /* header for libconfuse in ./srclib/confuse */
#include "conf.h"      /* configuration file definition in libconfuse format */
#include "become_a_nobody.h"
#include "libmetrics.h"/* libmetrics header in ./srclib/libmetrics */
#include "apr_net.h"   /* our private network functions based on apr */
#include "debug_msg.h" 
#include "protocol.h"  /* generated header from ./lib/protocol.x xdr definition file */

/* When this gmond was started */
apr_time_t started;
/* My name */
char myname[APRMAXHOSTLEN+1];
/* The commandline options */
struct gengetopt_args_info args_info;
/* The configuration file */
cfg_t *config_file;
/* The debug level (in debug_msg.c) */
extern int debug_level;
/* The global context */
apr_pool_t *global_context = NULL;
/* Deaf mode boolean */
int deaf;
/* Mute mode boolean */
int mute;
/* Maximum UDP message size.. TODO: allow tweakability */
int max_udp_message_len = 1472; /* mtu 1500 - 28 bytes for IP/UDP headers */
/* The pollset for incoming UDP messages */
apr_pollset_t *udp_recv_pollset = NULL;
/* The access control list for each of the UDP channels */
apr_array_header_t *udp_recv_acl_array = NULL;

/* The array for outgoing UDP message channels */
apr_array_header_t *udp_send_array = NULL;

/* The hash to hold the hosts (key = host IP) */
apr_hash_t *hosts;
/* The "hosts" hash contains values of type "hostdata" */
struct hostdata_t {
  /* Name of the host */
  char *hostname;
  /* Timestamp of when the remote host gmond started */
  unsigned int gmond_started;
  /* The pool used to malloc memory for this host */
  apr_pool_t *pool;
  /* A hash containing the data from the host */
  apr_hash_t *metrics;
  /* First heard from */
  apr_time_t first_heard_from;
  /* Last heard from */
  apr_time_t last_heard_from;
};
typedef struct hostdata_t hostdata_t;

static void
cleanup_configuration_file(void)
{
  cfg_free( config_file );
}

static void
process_configuration_file(void)
{
  config_file = cfg_init( gmond_opts, CFGF_NOCASE );

  init_validate_funcs();  /* in config.c */

  switch( cfg_parse( config_file, args_info.conf_arg ) )
    {
    case CFG_FILE_ERROR:
      /* Unable to open file so we'll go with the configuration defaults */
      fprintf(stderr,"Configuration file '%s' not found.\n", args_info.conf_arg);
      if(args_info.conf_given)
	{
	  /* If they explicitly stated a configuration file exit with error... */
	  exit(1);
	}
      /* .. otherwise use our default configuration */
      fprintf(stderr,"Using defaults.\n");
      if(cfg_parse_buf(config_file, DEFAULT_CONFIGURATION) == CFG_PARSE_ERROR)
	{
	  fprintf(stderr,"Your default configuration buffer failed to parse. Exiting.\n");
          exit(1);
	}
      break;
    case CFG_PARSE_ERROR:
      fprintf(stderr,"Parse error for '%s'\n", args_info.conf_arg);
      exit(1);
    case CFG_SUCCESS:
      break;
    default:
      /* I have no clue whats goin' on here... */
      exit(1);
    }
  /* Free memory for this configuration file at exit */
  atexit(cleanup_configuration_file);
}

static void
cleanup_apr_library( void )
{
  apr_pool_destroy(global_context);
  apr_terminate();
}

static void
initialize_apr_library( void )
{
  apr_status_t status;

  /* Initialize apr */
  status = apr_initialize();
  if(status != APR_SUCCESS)
    {
      fprintf(stderr,"Unable to initialize APR library. Exiting.\n");
      exit(1);
    }

  /* Create the global context */
  status = apr_pool_create( &global_context, NULL );
  if(status != APR_SUCCESS)
    {
      fprintf(stderr,"Unable to create global context. Exiting.\n");
      exit(1);
    }

  atexit(cleanup_apr_library);
}

static void
daemonize_if_necessary( char *argv[] )
{
  int should_daemonize;
  cfg_t *tmp;
  tmp = cfg_getsec( config_file, "behavior");
  should_daemonize = cfg_getbool( tmp, "daemonize");

  /* Commandline for debug_level trumps configuration file behaviour ... */
  if (args_info.debug_given) 
    {
      debug_level = args_info.debug_arg;
    }
  else
    {
      debug_level = cfg_getint ( tmp, "debug_level");
    }

  /* Daemonize if needed */
  if(!args_info.foreground_flag && should_daemonize && !debug_level)
    {
      apr_proc_detach(1);
    }
}

static void
setuid_if_necessary( void )
{
  cfg_t *tmp;
  int setuid;
  char *user;

  tmp    = cfg_getsec( config_file, "behavior");
  setuid = cfg_getbool( tmp, "setuid" );
  if(setuid)
    {
      user = cfg_getstr(tmp, "user" );
      become_a_nobody(user);
    }
}

static void
process_deaf_mute_mode( void )
{
  cfg_t *tmp = cfg_getsec( config_file, "behavior");
  deaf =       cfg_getbool( tmp, "deaf");
  mute =       cfg_getbool( tmp, "mute");
  if(deaf && mute)
    {
      fprintf(stderr,"Configured to run both deaf and mute. Nothing to do. Exiting.\n");
      exit(1);
    }
}

static void
setup_udp_recv_pollset( void )
{
  apr_status_t status;
  /* We will open sockets to listen for messages */
  int i, num_udp_recv_channels = cfg_size( config_file, "udp_recv_channel");

  /* Create my UDP recv pollset */
  apr_pollset_create(&udp_recv_pollset, num_udp_recv_channels, global_context, 0);

  /* Create my UDP recv access control array */
  udp_recv_acl_array = apr_array_make( global_context, num_udp_recv_channels,
                                   sizeof(apr_ipsubnet_t *));

  for(i = 0; i< num_udp_recv_channels; i++)
    {
      cfg_t *udp_recv_channel;
      char *mcast_join, *mcast_if, *bindaddr, *protocol, *allow_ip, *allow_mask;
      int port;
      apr_socket_t *socket = NULL;
      apr_ipsubnet_t *ipsub = NULL;
      apr_pollfd_t socket_pollfd;

      udp_recv_channel = cfg_getnsec( config_file, "udp_recv_channel", i);
      mcast_join     = cfg_getstr( udp_recv_channel, "mcast_join" );
      mcast_if       = cfg_getstr( udp_recv_channel, "mcast_if" );
      port           = cfg_getint( udp_recv_channel, "port");
      bindaddr       = cfg_getstr( udp_recv_channel, "bind");
      protocol       = cfg_getstr( udp_recv_channel, "protocol");
      allow_ip       = cfg_getstr( udp_recv_channel, "allow_ip");
      allow_mask     = cfg_getstr( udp_recv_channel, "allow_mask");

      debug_msg("udp_recv_channel mcast_join=%s mcast_if=%s port=%d bind=%s protocol=%s\n",
		  mcast_join? mcast_join:"NULL", 
		  mcast_if? mcast_if:"NULL", port,
		  bindaddr? bindaddr: "NULL",
		  protocol? protocol:"NULL");

      if( mcast_join )
	{
	  /* Listen on the specified multicast channel */
	  socket = create_mcast_server(global_context, mcast_join, port, bindaddr, mcast_if );
	  if(!socket)
	    {
	      fprintf(stderr,"Error creating multicast server mcast_join=%s port=%d mcast_if=%s. Exiting.\n",
		      mcast_join? mcast_join: "NULL", port, mcast_if? mcast_if:"NULL");
	      exit(1);
	    }

	}
      else
	{
	  /* Create a standard UDP server */
          socket = create_udp_server( global_context, port, bindaddr );
          if(!socket)
            {
              fprintf(stderr,"Error creating UDP server on port %d bind=%s. Exiting.\n",
		      port, bindaddr? bindaddr: "unspecified");
	      exit(1);
	    }
	}

      /* Build the socket poll file descriptor structure */
      socket_pollfd.desc_type   = APR_POLL_SOCKET;
      socket_pollfd.reqevents   = APR_POLLIN;
      socket_pollfd.desc.s      = socket;
      socket_pollfd.client_data = protocol;

      /* Add the socket to the pollset */
      status = apr_pollset_add(udp_recv_pollset, &socket_pollfd);
      if(status != APR_SUCCESS)
	{
	  fprintf(stderr,"Failed to add socket to pollset. Exiting.\n");
	  exit(1);
	}

      /* Save the ACL information */
      if(allow_ip)
	{
	  status = apr_ipsubnet_create(&ipsub, allow_ip, allow_mask, global_context);
	  if(status != APR_SUCCESS)
	    {
	      fprintf(stderr,"Unable to build ACL for ip=%s mask=%s. Exiting.\n",
		      allow_ip, allow_mask);
	      exit(1);
	    }
	}
      /* ipsub of NULL means no acl in effect */
      *(apr_ipsubnet_t **)apr_array_push(udp_recv_acl_array) = ipsub;
    }
}

/* TODO: This function needs to be updated later to handle proxy information */
static hostdata_t *
find_host_data( char *remoteip, apr_sockaddr_t *sa)
{
  apr_status_t status;
  apr_pool_t *pool;
  hostdata_t *hostdata;
  char *hostname = NULL;
  char *remoteipdup = NULL;

  hostdata =  (hostdata_t *)apr_hash_get( hosts, remoteip, APR_HASH_KEY_STRING );
  if(!hostdata)
    {
      /* Lookup the hostname (TODO: check for proxy info) */
      status = apr_getnameinfo(&hostname, sa, 0);
      if(status != APR_SUCCESS)
	{
	  hostname = remoteip;
	}

      /* This is the first time we've heard from this host.. create a new pool */
      status = apr_pool_create( &pool, global_context );
      if(status != APR_SUCCESS)
	{
	  return NULL;
	}

      /* Malloc the hostdata_t from the new pool */
      hostdata = apr_pcalloc( pool, sizeof( hostdata_t ));
      if(!hostdata)
	{
	  return NULL;
	}

      /* Save the pool address for later.. freeing this pool free everthing
       * for this particular host */
      hostdata->pool = pool;

      /* Save the hostname */
      hostdata->hostname = apr_pstrdup( pool, hostname );

      /* Dup the remoteip (it will be freed later) */
      remoteipdup = apr_pstrdup( pool, remoteip);

      /* Set the timestamps */
      hostdata->first_heard_from = hostdata->last_heard_from = apr_time_now();

      /* Create a hash for the metric data */
      hostdata->metrics = apr_hash_make( pool );
      if(!hostdata->metrics)
	{
	  apr_pool_destroy(pool);
	  return NULL;
	}

      /* Save this host data to the "hosts" hash */
      apr_hash_set( hosts, remoteipdup, APR_HASH_KEY_STRING, hostdata); 
    }
  else
    {
      /* We already have this host in our "hosts" hash undate timestamp */
      hostdata->last_heard_from = apr_time_now();
    }

  return hostdata;
}

static void
poll_udp_recv_channels(apr_interval_time_t timeout)
{
  apr_status_t status;
  const apr_pollfd_t *descs = NULL;
  apr_int32_t num = 0;

  /* Poll for data with given timeout */
  status = apr_pollset_poll(udp_recv_pollset, timeout, &num, &descs);
  if(status != APR_SUCCESS)
    return;

  if(num>0)
    {
      int i;

      /* We have data to read */
      for(i=0; i< num; i++)
        {
	  apr_socket_t *socket;
	  char buf[max_udp_message_len]; 
	  apr_size_t len = max_udp_message_len;
	  apr_sockaddr_t *remotesa = NULL;
	  char  *protocol, remoteip[256];
	  apr_ipsubnet_t *ipsub;
	  hostdata_t *hostdata = NULL;

	  socket         = descs[i].desc.s;
	  /* We could also use the apr_socket_data_get/set() functions
	   * to have per socket user data .. see APR docs */
	  protocol       = descs[i].client_data;

	  apr_socket_addr_get(&remotesa, APR_REMOTE, socket);


	  /* Grab the data */
	  status = apr_socket_recvfrom(remotesa, socket, 0, buf, &len);
	  if(status != APR_SUCCESS)
	    {
	      continue;
	    }	  

	  /* This function is in ./lib/apr_net.c and not APR. The
	   * APR counterpart is apr_sockaddr_ip_get() but we don't 
	   * want to malloc memory evertime we call this */
	  apr_sockaddr_ip_buffer_get(remoteip, 256, remotesa);

	  /* Check the ACL (we can make this better later) */
	  ipsub = ((apr_ipsubnet_t **)(udp_recv_acl_array->elts))[i];
	  if(ipsub)
	    {
	      if(!apr_ipsubnet_test( ipsub, remotesa))
		{
		  debug_msg("Ignoring data from %s\n", remoteip);
		  continue; /* to the next channel that needs read */
		}
	    }

	  /* Grab this host's data */
	  hostdata = find_host_data( remoteip, remotesa );
	  if(!hostdata)
	    {
	      continue;
	    }

	  fprintf(stderr,"Got a message from %s that is %d bytes long\n",
		  hostdata->hostname, len);
	    
#if 0
	  if(!strcasecmp(protocol, "xdr"))
	    {
	      XDR x;
	      gangliaMessage *msg = malloc(sizeof(gangliaMessage));

              /* Create the XDR receive stream */
	      xdrmem_create(&x, buf, max_udp_message_len, XDR_DECODE);

              /* Flush the data in the (last) received gangliaMessage 
	       * TODO: Free memory from xdr_string calls XDR_FREE */
	      memset( &hdr, 0, sizeof(gangliaMessageHeader));

	      /* Read the gangliaMessage from the stream */
	      if(!xdr_gangliaMessageHeader(&x, &hdr))
                {	
	          continue;
	        }

	      fprintf(stderr,"hdr.index=%d ", hdr.index);
	      if(hdr.index<1024)
		{
		  /* This is a 2.5.x data source */
                  fprintf(stderr,"2.5.x data source\n"); 
		}
	      else
		{
		  fprintf(stderr,"new data source\n");
		}


	      /* If I want to find out how much data I decoded 
	      decoded = xdr_getpos(&x); */
	    }
#endif

        } 
    }
}

static void
setup_udp_send_array( void )
{
  int i, num_udp_send_channels = cfg_size( config_file, "udp_send_channel");

  if(num_udp_send_channels <= 0)
    return;

  /* Create my UDP send array */
  udp_send_array = apr_array_make( global_context, num_udp_send_channels, 
				   sizeof(apr_socket_t *));

  for(i = 0; i< num_udp_send_channels; i++)
    {
      cfg_t *udp_send_channel;
      char *mcast_join, *mcast_if, *protocol, *ip;
      int port;
      apr_socket_t *socket = NULL;

      udp_send_channel = cfg_getnsec( config_file, "udp_send_channel", i);
      ip             = cfg_getstr( udp_send_channel, "ip" );
      mcast_join     = cfg_getstr( udp_send_channel, "mcast_join" );
      mcast_if       = cfg_getstr( udp_send_channel, "mcast_if" );
      port           = cfg_getint( udp_send_channel, "port");
      protocol       = cfg_getstr( udp_send_channel, "protocol");

      debug_msg("udp_send_channel mcast_join=%s mcast_if=%s ip=%s port=%d protocol=%s\n",
		  mcast_join? mcast_join:"NULL", 
		  mcast_if? mcast_if:"NULL",
		  ip,
		  port, 
		  protocol? protocol:"NULL");

      /* Create a standard UDP socket */
      socket = create_udp_client( global_context, ip, port );
      if(!socket)
        {
          fprintf(stderr,"Unable to create UDP client for %s:%d. Exiting.\n",
		      ip? ip: "NULL", port);
	  exit(1);
	}

      /* Join the specified multicast channel */
      if( mcast_join )
	{
	  /* We'll be listening on a multicast channel */
	  socket = NULL;
	  if(!socket)
	    {
	      fprintf(stderr,"Unable to join multicast channel %s:%d. Exiting\n",
		      mcast_join, port);
	      exit(1);
	    }
	}

      /* Add the socket to the array */
      *(apr_socket_t **)apr_array_push(udp_send_array) = socket;
    }
}

/* This function will send a datagram to every udp_send_channel specified */
static int
udp_send_message( char *buf, int len )
{
  apr_status_t status;
  int i;
  int num_errors = 0;
  apr_size_t size;

  /* Return if we have no data or we're muted */
  if(!buf || mute)
    return 1;

  for(i=0; i< udp_send_array->nelts; i++)
    {
      apr_socket_t *socket = ((apr_socket_t **)(udp_send_array->elts))[i];
      size   = len;
      status = apr_socket_send( socket, buf, &size );
      if(status != APR_SUCCESS)
	{
	  num_errors++;
	}
    }
  return num_errors;
}
 
int
process_collection_groups( void )
{
  int i, num_collection_groups = cfg_size( config_file, "collection_group" );

  for(i=0; i< num_collection_groups; i++)
    {
      int j, num_metrics;

      cfg_t *group = cfg_getnsec( config_file, "collection_group", i);
      char *name   = cfg_getstr( group, "name");
      num_metrics  = cfg_size( group, "metric" );

      for(j=0; j< num_metrics; j++)
	{
          cfg_t *metric = cfg_getnsec( group, "metric", j );

	  /* Process the data for this metric */
	  

	}
    }

  return 2;
}

int
main ( int argc, char *argv[] )
{
  apr_interval_time_t now, stop;
  int next_collection;

  /* Mark the time this gmond started */
  started = apr_time_now();

  if (cmdline_parser (argc, argv, &args_info) != 0)
    exit(1) ;

  if(args_info.default_config_flag)
    {
      fprintf(stdout, DEFAULT_CONFIGURATION);
      fflush( stdout );
      exit(0);
    }

  process_configuration_file();

  daemonize_if_necessary( argv );
  
  /* Initializes the apr library in ./srclib/apr */
  initialize_apr_library();

  /* Collect my hostname */
  apr_gethostname( myname, APRMAXHOSTLEN+1, global_context);

  /* Initialize the libmetrics library in ./srclib/libmetrics */
  libmetrics_init();

  apr_signal( SIGPIPE, SIG_IGN );

  setuid_if_necessary();

  process_deaf_mute_mode();

  if(!deaf)
    {
      setup_udp_recv_pollset();
    }

  if(!mute)
    {
      setup_udp_send_array();
    }

  /* Create the host hash table */
  hosts = apr_hash_make( global_context );

  udp_send_message("This is a test remove me later", 15);

  next_collection = 0;
  for(;;)
    {
      now  = apr_time_now();
      stop = now + (next_collection* APR_USEC_PER_SEC);
      /* Read data until we need to collect/write data */
      for(; now < stop ; )
	{
          if(!deaf)
	    {
              poll_udp_recv_channels(stop - now);  
	      /* accept_tcp_connections... */
	    }
	  now = apr_time_now();
	}

      if(!mute)
	{
	  next_collection = process_collection_groups();
	}
      else
	{
	  next_collection = 3600; /* if we're mute.
				  set the default timeout large...*/
	}
    }

  return 0;
}
