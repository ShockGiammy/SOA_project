/*  
 *  Header file that contains the definition of the structs used by the multi-flow device driver
 */

enum ioctl_op{
    BLOCKING = 3,
    NON_BLOCKING = 4,
    TIMEOUT = 5,
    ENABLE = 6,
    DISABLE = 7
};

enum sleep_type{
    SLEEP_READ,     //1 if sleep is caused by a read operation
    SLEEP_WRITE     //0 if sleep is caused by a write operation
};

enum priority{
   HIGH_PRIORITY,    //0 if high priority
   LOW_PRIORITY      //1 if low priority
};

typedef struct _list_stream{
   char* buffer;
   struct _list_stream *next;
   struct _list_stream *prev;
} list_stream;

typedef struct _object_state{
#ifdef SINGLE_SESSION_OBJECT
	struct mutex object_busy;
#endif
   int minor;
	struct mutex operation_synchronizer[2];
	int valid_bytes[2];
   int offset[2];
	list_stream *stream_content[2];           //the I/O node is a buffer in memory
   int reserved_bytes;
   wait_queue_head_t wait_queue[2];
} object_state;

typedef struct _session_state{
   enum priority priority;
   bool blocking;
   unsigned long timeout;
} session_state;

typedef struct _packed_work{
   void* struct_addr;
   object_state *the_object;
   const char *buffer;
   size_t len;
   struct file *filp;
   struct work_struct the_work;
} packed_work;

typedef struct _control_record{
   struct task_struct *task;       
   int pid;
   int minor;
   int priority;
} control_record;