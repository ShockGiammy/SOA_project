
/*  
 *  baseline char device driver with limitation on minor numbers - configurable in terms of concurrency 
 */

#define EXPORT_SYMTAB
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/sched.h>	
#include <linux/pid.h>		/* For pid types */
#include <linux/tty.h>		/* For the tty declarations */
#include <linux/version.h>	/* For LINUX_VERSION_CODE */
#include <linux/slab.h>

//#include <structs.h>


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gian Marco Falcone");

#define MODNAME "MULTI-FLOW DEV"

#define AUDIT if(1)

#define MINORS 128

#define BLOCKING 3
#define NON_BLOCKING 4
#define TIMEOUT 5
#define ENABLE 6
#define DISABLE 7

#define NO (0)
#define YES (NO+1)

#define PAGE_DIM (4096) //the size of one page
#define MAX_PAGES (10)

#define SESSIONS 64

int enabled[MINORS];
module_param_array(enabled, int, NULL, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
//S_IROTH = lettura per altri
//S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP = 0660

long high_priority_valid_bytes[MINORS];
module_param_array(high_priority_valid_bytes, long, NULL, S_IRUSR | S_IRGRP);

long high_priority_waiting_threads[MINORS];
module_param_array(high_priority_waiting_threads, long, NULL, S_IRUSR | S_IRGRP);

long low_priority_valid_bytes[MINORS];
module_param_array(low_priority_valid_bytes, long, NULL, S_IRUSR | S_IRGRP);

long low_priority_waiting_threads[MINORS];
module_param_array(low_priority_waiting_threads, long, NULL, S_IRUSR | S_IRGRP);


static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_ioctl(struct file *, unsigned int, unsigned long);

#define DEVICE_NAME "my-new-dev"  /* Device file name in /dev/ - not mandatory  */

//#define SINGLE_SESSION_OBJECT //just one session per I/O node at a time

static int Major;            /* Major number assigned to broadcast device driver */

#if LINUX_VERSION_CODE >= KERNEL_VERSION(4, 0, 0)
#define get_major(session)	MAJOR(session->f_inode->i_rdev)
#define get_minor(session)	MINOR(session->f_inode->i_rdev)
#else
#define get_major(session)	MAJOR(session->f_dentry->d_inode->i_rdev)
#define get_minor(session)	MINOR(session->f_dentry->d_inode->i_rdev)
#endif

enum priority{
   HIGH_PRIORITY,    //0 if high priority
   LOW_PRIORITY,     //1 if low priority
   FREE_ENTRY        //2 if entry is free
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
	struct mutex operation_synchronizer;
	int valid_bytes[2];
   int offset[2];
	list_stream *stream_content[2];    //the I/O node is a buffer in memory
   int reserved_bytes;
} object_state;

typedef struct _session_state{
   enum priority priority;
   bool blocking;
   unsigned long timeout;
} session_state;

object_state objects[MINORS];

typedef struct _packed_work{
   object_state *the_object;
   char *buffer;
   size_t len;
   struct file *filp;
   struct work_struct the_work;
} packed_work;

typedef struct _control_record{
   struct task_struct *task;       
   int pid;
   int awake;
   int minor;
   int priority;
   struct hrtimer hr_timer;
} control_record;


static enum hrtimer_restart my_hrtimer_callback(struct hrtimer *timer){

   control_record *control;
   struct task_struct *the_task;

   control = (control_record*)container_of(timer, control_record, hr_timer);
   control->awake = YES;
   the_task = control->task;
   wake_up_process(the_task);
   if (control->priority == 0) {
      high_priority_waiting_threads[control->minor] -= 1;
   }
   else {
      low_priority_waiting_threads[control->minor] -= 1;
   }

   return HRTIMER_NORESTART;
}


long goto_sleep(session_state *session, int minor, object_state *the_object, size_t len){

	control_record data;
   control_record* control;
   ktime_t ktime_interval;
   DECLARE_WAIT_QUEUE_HEAD(the_queue);    //here we use a private queue - wakeup is selective via wake_up_process

   if(session->timeout == 0) {
      return -1;
   }

   control = &data;     //set the pointer to the current stack area

   AUDIT
   printk("%s: thread %d going to usleep for %lu millisecs\n", MODNAME, current->pid, session->timeout);

   ktime_interval = ktime_set(0, session->timeout*1000000);

   control->task = current;
   control->pid  = current->pid;
   control->awake = NO;
   control->minor = minor;
   control->priority = session->priority;

   hrtimer_init(&(control->hr_timer), CLOCK_MONOTONIC, HRTIMER_MODE_REL);

   control->hr_timer.function = &my_hrtimer_callback;
   hrtimer_start(&(control->hr_timer), ktime_interval, HRTIMER_MODE_REL);
      
   wait_event_interruptible(the_queue, control->awake == YES);

   //Are taken into account both threads waiting to read and threads waiting to write.
   //It is very unlikely that both are present at the same time.
   if (session->priority == 0) {
      high_priority_waiting_threads[minor] += 1;
   }
   else {
      low_priority_waiting_threads[minor] += 1;
   }

   hrtimer_cancel(&(control->hr_timer));
   
   AUDIT
   printk("%s: thread %d exiting usleep\n",MODNAME, current->pid);

   if(unlikely(control->awake != YES)) {
      return -1;
   }
   return 0;
}


void asynchronous_write(unsigned long data){

   object_state *the_object = container_of((void*)data,packed_work,the_work)->the_object;
   char *buff = container_of((void*)data,packed_work,the_work)->buffer;
   size_t len = container_of((void*)data,packed_work,the_work)->len;
   struct file *filp = container_of((void*)data,packed_work,the_work)->filp;
   int ret;
   int temp_ret;
   int offset;
   int pages;
   session_state *session = (session_state *)filp->private_data;
   list_stream* current_page;
   
   AUDIT{
   printk("%s: this print comes from kworker daemon with PID=%d - running on CPU-core %d\n", MODNAME, current->pid, smp_processor_id());
   printk("%s: releasing the task buffer at address %p \n",MODNAME, (void*)data);
   }

  printk("%s: somebody called a write on dev with [major,minor] number [%d,%d]\n",
            MODNAME, get_major(filp), get_minor(filp));

   //need to lock in any case
   mutex_lock(&(the_object->operation_synchronizer));
   offset = (the_object->valid_bytes[session->priority] + the_object->offset[session->priority]) % PAGE_DIM;

   current_page = the_object->stream_content[session->priority];
   while (current_page->next != NULL) {
      pages +=1;
      current_page = current_page->next;
   }

   if (len + offset >= (PAGE_DIM)) {
      // buffer is a the end
      // occorre allocare un nuovo buffer, e dai limite per evitare attacco Dos

      // scrivo tanti byte quanti necessari a riempire il buffer
      temp_ret = copy_from_user(&(current_page->prev->buffer[offset]), buff, PAGE_DIM - offset);
      //if vanno probabilmente tolti
      if (temp_ret != 0) {
         printk("%s: There was an error in the write\n", MODNAME);
      }

      // e rinizio a scrivere dall'inizio
      ret = copy_from_user(&(current_page->buffer[0]), buff, len - (PAGE_DIM - offset));
      if (ret != 0) {
         printk("%s: There was an error in the write\n", MODNAME);
      }

      ret = ret + temp_ret;
   }
   else {
      ret = copy_from_user(&(current_page->buffer[offset]), buff, len);
      if (ret != 0) {
         printk("%s: There was an error in the write\n", MODNAME);
      }
   }  

   the_object->valid_bytes[session->priority] += (len - ret);
   low_priority_valid_bytes[get_minor(filp)] += (len - ret);
   the_object->reserved_bytes -= (len - ret);

   mutex_unlock(&(the_object->operation_synchronizer));

   kfree(container_of((void*)data,packed_work,the_work));
   module_put(THIS_MODULE);
   return;
}


int put_work(object_state *the_object, const char *buff, size_t len, struct file *filp){
   
   packed_work *the_task;

   if(!try_module_get(THIS_MODULE)) {
      return -ENODEV;
   }

   AUDIT{
      printk("%s: requested deferred work\n", MODNAME);
   }

   the_task = kzalloc(sizeof(packed_work), GFP_ATOMIC);     //non blocking memory allocation

   if (the_task == NULL) {
      printk("%s: tasklet buffer allocation failure\n",MODNAME);
      module_put(THIS_MODULE);
      return -1;
   }

   the_task->buffer = (char*)buff;
   the_task->the_object = the_object;
   the_task->len = len;
   the_task->filp = filp;

   AUDIT
   printk("%s: work buffer allocation success - address is %p\n", MODNAME, the_task);

   __INIT_WORK(&(the_task->the_work), (void*)asynchronous_write, (unsigned long)(&(the_task->the_work)));
   mutex_unlock(&(the_object->operation_synchronizer));
   schedule_work(&the_task->the_work);

   return 0;
}


/* the actual driver */

static int dev_open(struct inode *inode, struct file *file) {

   int minor;
   session_state *session;

   minor = get_minor(file);

   if(minor >= MINORS){
	   return -ENODEV;
   }

   if (enabled[minor] == 0) {
      printk("%s: device file is disabled for object with minor %d\n", MODNAME, minor);
      return -EBUSY;
   }

#ifdef SINGLE_SESSION_OBJECT
   if (!mutex_trylock(&(objects[minor].object_busy))) {
		goto open_failure;
   }
#endif

   session = kzalloc(sizeof(session_state), GFP_ATOMIC);     //non blocking memory allocation
   session->priority = HIGH_PRIORITY;
   session->blocking = false;
   session->timeout = 0.0;
   file->private_data = session;

   printk("%s: device file successfully opened for object with minor %d\n", MODNAME, minor);
   //device opened by a default nop
   return 0;

#ifdef SINGLE_SESSION_OBJECT
open_failure:
   return -EBUSY;
#endif

}


static int dev_release(struct inode *inode, struct file *file) {

   int minor;
   object_state *the_object;

   minor = get_minor(file);
   the_object = objects + minor;

#ifdef SINGLE_SESSION_OBJECT
   mutex_unlock(&(objects[minor].object_busy));
#endif

   enabled[minor] = 1;
   //the_object->priority[*(int *)file->private_data] = FREE_ENTRY;
   kfree(file->private_data);

   printk("%s: device file closed\n",MODNAME);
   //device closed by default nop
   return 0;
}


static ssize_t dev_write(struct file *filp, const char *buff, size_t len, loff_t *off) {

   int minor = get_minor(filp);
   int ret;
   int temp_ret;
   int offset;
   int pages;
   object_state *the_object;
   session_state *session = (session_state *)filp->private_data;
   list_stream* current_page;
   list_stream* content;

   the_object = objects + minor;
   printk("%s: somebody called a write on dev with [major,minor] number [%d,%d]\n",
            MODNAME, get_major(filp), get_minor(filp));

retry_write:
   //need to lock in any case
   mutex_lock(&(the_object->operation_synchronizer));

   if(*off >= ((PAGE_DIM * MAX_PAGES) - the_object->reserved_bytes)) {    //offset too large
      mutex_unlock(&(the_object->operation_synchronizer));
      return -ENOSPC;      //no space left on device
   } 

   if(*off > the_object->valid_bytes[session->priority]) {      //offset beyond the current stream size
      mutex_unlock(&(the_object->operation_synchronizer));
      return -ENOSR;    //out of stream resources
   } 

   if((((PAGE_DIM * MAX_PAGES) - the_object->reserved_bytes) - the_object->valid_bytes[session->priority]) < len) {
      mutex_unlock(&(the_object->operation_synchronizer));
      if (session->blocking && session->timeout != 0) {
         goto_sleep(session, minor, the_object, len);
         goto retry_write;
      }
      else {
         return -ENOSPC;      //no space left on device
      }
   }
   offset = (the_object->valid_bytes[session->priority] + the_object->offset[session->priority]) % PAGE_DIM;

   current_page = the_object->stream_content[session->priority];
   while (current_page->next != NULL) {
      pages +=1;
      current_page = current_page->next;
   }

   if (len + offset >= (PAGE_DIM)) {
      // buffer is a the end
      // occorre allocare un nuovo buffer, e dai limite per evitare attacco Dos
      if (pages == MAX_PAGES) {
         printk("%s: The memory reserved for the buffer is terminated\n", MODNAME);
         return -ENOSPC; 
      }

      content = kzalloc(sizeof(list_stream), GFP_ATOMIC);     //non blocking memory allocation
      content->buffer = (char*)__get_free_page(GFP_KERNEL);
      //controlla che allocazione non fallisca
   
      content->prev = current_page;
      content->next = NULL;
      current_page->next = content;

      if (session->priority == HIGH_PRIORITY) {

         // scrivo tanti byte quanti necessari a riempire il buffer
         temp_ret = copy_from_user(&(current_page->buffer[offset]), buff, PAGE_DIM - offset);

         // e rinizio a scrivere dall'inizio
         ret = copy_from_user(&(content->buffer[0]), buff, len - (PAGE_DIM - offset));

         ret = ret + temp_ret;
         the_object->valid_bytes[session->priority] += (len - ret);
         high_priority_valid_bytes[minor] += (len - ret);
      }
      else {
         // riservo dei byte! altro campo nella struct
         the_object->reserved_bytes += len;
         ret = put_work(the_object, buff, len, filp);
         if (ret != 0) {
            printk("%s: There was an error with deferred work\n", MODNAME);
            return -1;
         }
      }  
   }
   else {
      if (session->priority == HIGH_PRIORITY) {
         ret = copy_from_user(&(current_page->buffer[offset]), buff, len);

         the_object->valid_bytes[session->priority] += (len - ret);
         high_priority_valid_bytes[minor] += (len - ret);
      }
      else {
         // riservo dei byte! altro campo nella struct
         the_object->reserved_bytes += len;
         ret = put_work(the_object, buff, len, filp);
         if (ret != 0) {
            printk("%s: There was an error with deferred work\n", MODNAME);
            return -1;
         }
      }  
   }

   mutex_unlock(&(the_object->operation_synchronizer));

   return len - ret;
}


static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off) {

   int minor = get_minor(filp);
   int ret;
   int temp_ret;
   object_state *the_object;
   list_stream* current_page;

   session_state *session = (session_state *)filp->private_data;

   the_object = objects + minor;
   printk("%s: somebody called a read on dev with [major,minor] number [%d,%d]\n",
            MODNAME, get_major(filp), get_minor(filp));

retry_read:
   //need to lock in any case
   mutex_lock(&(the_object->operation_synchronizer));

   if(*off > the_object->valid_bytes[session->priority]) {
 	   mutex_unlock(&(the_object->operation_synchronizer));
	   return -1;
   }

   current_page = the_object->stream_content[session->priority];

   if((the_object->valid_bytes[session->priority]) < len) {
      if (session->blocking && session->timeout != 0) {
         mutex_unlock(&(the_object->operation_synchronizer));
         goto_sleep(session, minor, the_object, len);
         goto retry_read;
      }
      else {
         printk("%s: Not enough data to read\n", MODNAME);
         mutex_unlock(&(the_object->operation_synchronizer));
         return -1;      //no enough data on device
      }
   }
   if (the_object->offset[session->priority] + len >= (PAGE_DIM)) {

      //si può deallocare il buffer precedente
      temp_ret = copy_to_user(buff, &(current_page->buffer[the_object->offset[session->priority]]), 
         PAGE_DIM - the_object->offset[session->priority]);

      ret = copy_to_user(buff, &(current_page->next->buffer[0]), 
         len - (PAGE_DIM - the_object->offset[session->priority]));

      the_object->stream_content[session->priority] = current_page->next;
      current_page->next->prev = NULL;
      free_page((unsigned long)current_page->buffer);
      kfree((void*)current_page);

      ret = ret + temp_ret;
   } 
   else {
      ret = copy_to_user(buff, &(current_page->buffer[the_object->offset[session->priority]]), len);
   }

   the_object->valid_bytes[session->priority] -= (len - ret);
   the_object->offset[session->priority] = (the_object->offset[session->priority] + (len - ret)) % PAGE_DIM;

   if (session->priority == 0) {
      high_priority_valid_bytes[minor] -= (len - ret);
   }
   else {
      low_priority_valid_bytes[minor] -= (len - ret);
   }

   mutex_unlock(&(the_object->operation_synchronizer));

   return len - ret;
}


static long dev_ioctl(struct file *filp, unsigned int command, unsigned long param) {

   int minor = get_minor(filp);
   object_state *the_object;

   session_state *session = (session_state *)filp->private_data;

   the_object = objects + minor;
   printk("%s: somebody called an ioctl on dev with [major,minor] number [%d,%d] and command %u \n",
            MODNAME, get_major(filp), get_minor(filp), command);

   switch(command) {
         case HIGH_PRIORITY:
            session->priority = HIGH_PRIORITY;
            break;
         case LOW_PRIORITY:
            session->priority = LOW_PRIORITY;
            break;
         case BLOCKING:
            session->blocking = true;
            break;
         case NON_BLOCKING:
            session->blocking = false;
            break;
         case TIMEOUT:
            session->timeout = param;
            printk("%s: Timeout is set to %ld\n", MODNAME, session->timeout);
            break;
         case ENABLE:
            enabled[minor] = 1;
            break;
         case DISABLE:
            enabled[minor] = 0;
            break;
         default:
            printk("%s: Unknown operation\n", MODNAME);
            break;
   }
   return 0;
}


static struct file_operations fops = {
   .owner = THIS_MODULE,//do not forget this
   .write = dev_write,
   .read = dev_read,
   .open =  dev_open,
   .release = dev_release,
   .unlocked_ioctl = dev_ioctl
};


int init_module(void) {

	int i;

   list_stream* high_priority_content;
   list_stream* low_priority_content;

	//initialize the drive internal state
	for(i = 0; i < MINORS; i++){

      high_priority_content = kzalloc(sizeof(list_stream), GFP_ATOMIC);     //non blocking memory allocation
      high_priority_content->buffer = (char*)__get_free_page(GFP_KERNEL);
      high_priority_content->prev = NULL;
      high_priority_content->next = NULL;

      low_priority_content = kzalloc(sizeof(list_stream), GFP_ATOMIC);     //non blocking memory allocation
      low_priority_content->buffer = (char*)__get_free_page(GFP_KERNEL);
      low_priority_content->prev = NULL;
      low_priority_content->next = NULL;

      if ((high_priority_content == NULL) || (low_priority_content == NULL)){
         goto revert_allocation;
      }

#ifdef SINGLE_SESSION_OBJECT
		mutex_init(&(objects[i].object_busy));
#endif
		mutex_init(&(objects[i].operation_synchronizer));
      enabled[i] = 1;
      high_priority_waiting_threads[i] = 0;
      low_priority_waiting_threads[i] = 0;
      high_priority_valid_bytes[i] = 0;
      low_priority_valid_bytes[i] = 0;
		objects[i].valid_bytes[0] = 0;
      objects[i].valid_bytes[1] = 0;
		objects[i].stream_content[0] = high_priority_content;
      objects[i].stream_content[1] = low_priority_content;
	}

	Major = __register_chrdev(0, 0, MINORS, DEVICE_NAME, &fops);
	//actually allowed minors are directly controlled within this driver

	if (Major < 0) {
	   printk("%s: registering device failed\n",MODNAME);
	   return Major;
	}

	printk(KERN_INFO "%s: new device registered, it is assigned major number %d\n", MODNAME, Major);

	return 0;

revert_allocation:
	for(;i>=0;i--){
		free_page((unsigned long)objects[i].stream_content[0]->buffer);
      free_page((unsigned long)objects[i].stream_content[1]->buffer);
      kfree((void*)objects[i].stream_content[0]);
      kfree((void*)objects[i].stream_content[1]);
	}
	return -ENOMEM;
}


void cleanup_module(void) {

	int i;
	for(i = 0 ;i < MINORS; i++){
		free_page((unsigned long)objects[i].stream_content[0]->buffer);
      free_page((unsigned long)objects[i].stream_content[1]->buffer);
      kfree((void*)objects[i].stream_content[0]);
      kfree((void*)objects[i].stream_content[1]);
	}

	unregister_chrdev(Major, DEVICE_NAME);

	printk(KERN_INFO "%s: new device unregistered, it was assigned major number %d\n", MODNAME, Major);

	return;
}
