
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


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gian Marco Falcone");

#define MODNAME "MULTI-FLOW DEV"

#define AUDIT if(1)

static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_ioctl(struct file *, unsigned int, unsigned long);
int re_write_buffer(char *buffer, size_t off, size_t len);
bool test_float(const char *str);

#define DEVICE_NAME "my-new-dev"  /* Device file name in /dev/ - not mandatory  */

#define SINGLE_SESSION_OBJECT //just one session per I/O node at a time

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
   LOW_PRIORITY      //1 if low priority
};
#define TIMEOUT 2
#define BLOCKING 3
#define NON_BLOCKING 4

#define NO (0)
#define YES (NO+1)

typedef struct _object_state{
#ifdef SINGLE_SESSION_OBJECT
	struct mutex object_busy;
#endif
	struct mutex operation_synchronizer;
	int valid_bytes[2];
   int offset[2];
	char* stream_content[2];    //the I/O node is a buffer in memory
   enum priority priority;
   bool blocking;
   unsigned long timeout;
} object_state;

#define MINORS 128
object_state objects[MINORS];

#define OBJECT_MAX_SIZE  (4096) //just one page

typedef struct _packed_work{
   void* buffer;
   long code;
   struct work_struct the_work;
} packed_work;

typedef struct _control_record{
   struct task_struct *task;       
   int pid;
   int awake;
   struct hrtimer hr_timer;
} control_record;


void audit(unsigned long data){

   AUDIT{
      printk("%s: ------------------------------\n",MODNAME);
      printk("%s: this print comes from kworker daemon with PID=%d - running on CPU-core %d\n",MODNAME,current->pid,smp_processor_id());
   }

   AUDIT
   printk("%s: running task with code  %ld\n",MODNAME,container_of((void*)data,packed_work,the_work)->code);

   AUDIT
   printk("%s: releasing the task buffer at address %p - container of task is at %p\n",MODNAME,(void*)data,container_of((void*)data,packed_work,the_work));

   kfree((void*)container_of((void*)data,packed_work,the_work));

   module_put(THIS_MODULE);

}


int put_work(int core, int request_code){
   
   packed_work *the_task;

	if(core >= num_online_cpus()) {
      return -ENODEV;
   }

   if(!try_module_get(THIS_MODULE)) {
      return -ENODEV;
   }

   AUDIT{
      printk("%s: ------------------------\n",MODNAME);
      printk("%s: requested deferred work with request code: %d\n",MODNAME,request_code);
   }

   the_task = kzalloc(sizeof(packed_work),GFP_ATOMIC);     //non blocking memory allocation

   if (the_task == NULL) {
      printk("%s: tasklet buffer allocation failure\n",MODNAME);
      module_put(THIS_MODULE);
      return -1;
   }

   the_task->buffer = the_task;
   the_task->code = request_code;

   AUDIT
   printk("%s: work buffer allocation success - address is %p\n",MODNAME,the_task);


   __INIT_WORK(&(the_task->the_work), (void*)audit, (unsigned long)(&(the_task->the_work)));

   schedule_work_on(core, &the_task->the_work);

   return 0;
}


static enum hrtimer_restart my_hrtimer_callback( struct hrtimer *timer ){

   control_record *control;
   struct task_struct *the_task;

   control = (control_record*)container_of(timer,control_record, hr_timer);
   control->awake = YES;
   the_task = control->task;
   wake_up_process(the_task);

   return HRTIMER_NORESTART;
}


long goto_sleep(object_state *the_object){

	control_record data;
   control_record* control;
   ktime_t ktime_interval;
   DECLARE_WAIT_QUEUE_HEAD(the_queue);//here we use a private queue - wakeup is selective via wake_up_process

   if(the_object->timeout == 0) {
      return 0;
   }

   control = &data;//set the pointer to the current stack area

   AUDIT
   printk("%s: thread %d going to usleep for %lu microsecs\n", MODNAME, current->pid, the_object->timeout);

   ktime_interval = ktime_set(0, the_object->timeout*1000);

   //CLASSIC
   //control->awake = NO;
   //wait_event_interruptible_hrtimeout(the_queue, control->awake == YES, ktime_interval);

   control->task = current;
   control->pid  = current->pid;
   control->awake = NO;

   hrtimer_init(&(control->hr_timer), CLOCK_MONOTONIC, HRTIMER_MODE_REL);

   control->hr_timer.function = &my_hrtimer_callback;
   hrtimer_start(&(control->hr_timer), ktime_interval, HRTIMER_MODE_REL);

            
   wait_event_interruptible(the_queue, control->awake == YES);

   hrtimer_cancel(&(control->hr_timer));
   
   AUDIT
   printk("%s: thread %d exiting usleep\n",MODNAME, current->pid);

   if(unlikely(control->awake != YES)) {
      return -1;
   }

   return 0;

}


/* the actual driver */

static int dev_open(struct inode *inode, struct file *file) {

   int minor;
   minor = get_minor(file);

   if(minor >= MINORS){
	return -ENODEV;
   }

#ifdef SINGLE_SESSION_OBJECT
   if (!mutex_trylock(&(objects[minor].object_busy))) {
		goto open_failure;
   }
#endif

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
   minor = get_minor(file);

#ifdef SINGLE_SESSION_OBJECT
   mutex_unlock(&(objects[minor].object_busy));
#endif

   printk("%s: device file closed\n",MODNAME);
   //device closed by default nop
   return 0;
}



static ssize_t dev_write(struct file *filp, const char *buff, size_t len, loff_t *off) {

   int minor = get_minor(filp);
   int ret;
   int temp_ret;
   int offset;
   object_state *the_object;

   the_object = objects + minor;
   printk("%s: somebody called a write on dev with [major,minor] number [%d,%d]\n",
            MODNAME, get_major(filp), get_minor(filp));

retry_write:
   //need to lock in any case
   mutex_lock(&(the_object->operation_synchronizer));

   if(*off >= OBJECT_MAX_SIZE) {    //offset too large
      mutex_unlock(&(the_object->operation_synchronizer));
      return -ENOSPC;      //no space left on device
   } 

   if(*off > the_object->valid_bytes[the_object->priority]) {      //offset beyond the current stream size
      mutex_unlock(&(the_object->operation_synchronizer));
      return -ENOSR;    //out of stream resources
   } 

   if((OBJECT_MAX_SIZE - the_object->valid_bytes[the_object->priority]) < len) {
      mutex_unlock(&(the_object->operation_synchronizer));
      if (!the_object->blocking) {
         return -ENOSPC;      //no space left on device
      }
      else {
         goto_sleep(the_object);
         printk("BLOCCA");
         goto retry_write;
      }
   }
   offset = the_object->valid_bytes[the_object->priority] + the_object->offset[the_object->priority];
   if (len + offset >= OBJECT_MAX_SIZE) {
      // buffer is a the end
      if (the_object->priority == HIGH_PRIORITY) {

         // scrivo tanti byte quanti necessari a riempire il buffer
         temp_ret = copy_from_user(&(the_object->stream_content[the_object->priority][offset]),
            buff, OBJECT_MAX_SIZE - offset);

         // e rinizio a scrivere dall'inizio
         ret = copy_from_user(&(the_object->stream_content[the_object->priority][0]),
            buff, len - (OBJECT_MAX_SIZE - offset));

         ret = ret + temp_ret;
      }
      else {
         printk("CIAO");
      }  
   }
   else {
      if (the_object->priority == HIGH_PRIORITY) {
      ret = copy_from_user(&(the_object->stream_content[the_object->priority][offset]),
            buff, len);
      }
      else {
         printk("CIAO");
      }  
   }
   the_object->valid_bytes[the_object->priority] += (len - ret);

   mutex_unlock(&(the_object->operation_synchronizer));

   return len - ret;
}


static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off) {

   int minor = get_minor(filp);
   int ret;
   int temp_ret;
   object_state *the_object;

   the_object = objects + minor;
   printk("%s: somebody called a read on dev with [major,minor] number [%d,%d]\n",
            MODNAME, get_major(filp), get_minor(filp));

retry_read:
   //need to lock in any case
   mutex_lock(&(the_object->operation_synchronizer));

   if(*off > the_object->valid_bytes[the_object->priority]) {
 	   mutex_unlock(&(the_object->operation_synchronizer));
	   return 0;
   } 

   if((the_object->valid_bytes[the_object->priority]) < len) {
      if (the_object->blocking && the_object->timeout != 0) {
         printk("BLOCCA");
         mutex_unlock(&(the_object->operation_synchronizer));
         goto_sleep(the_object);
         goto retry_read;
      }
      else {
         len = the_object->valid_bytes[the_object->priority];
         //printk("Thread cannot go to sleep because the timeout is set to 0");
      }
   }
   if (the_object->offset[the_object->priority] + len >= OBJECT_MAX_SIZE) {

      temp_ret = copy_to_user(buff, &(the_object->stream_content[the_object->priority][the_object->offset[the_object->priority]]), 
         OBJECT_MAX_SIZE - the_object->offset[the_object->priority]);

      ret = copy_to_user(buff, &(the_object->stream_content[the_object->priority][0]), 
         len - (OBJECT_MAX_SIZE - the_object->offset[the_object->priority]));

      ret = ret + temp_ret;

   } 
   else {
      ret = copy_to_user(buff, &(the_object->stream_content[the_object->priority][the_object->offset[the_object->priority]]), len);
   }

   the_object->valid_bytes[the_object->priority] -= (len - ret);
   the_object->offset[the_object->priority] = (the_object->offset[the_object->priority] + (len - ret)) % OBJECT_MAX_SIZE;

   // potresti lavorare in buffer circolare => servono due indici (offset e validBytes)
   /*ret = re_write_buffer(the_object->stream_content[the_object->priority], the_object->offset, len);
   if (ret != 0) {
      printk("Error in re_write_buffer");
   }*/

   mutex_unlock(&(the_object->operation_synchronizer));

   return len - ret;
}


static long dev_ioctl(struct file *filp, unsigned int command, unsigned long param) {

   int minor = get_minor(filp);
   object_state *the_object;

   the_object = objects + minor;
   printk("%s: somebody called an ioctl on dev with [major,minor] number [%d,%d] and command %u \n",
            MODNAME, get_major(filp), get_minor(filp), command);

   //ATT!!! potrebbe dover essere unico per sessione? così cambia uno e cambiano tutti
   switch(command) {
         case HIGH_PRIORITY:
            the_object->priority = HIGH_PRIORITY;
            break;
         case LOW_PRIORITY:
            the_object->priority = LOW_PRIORITY;
            break;
         case TIMEOUT:
            //if (test_float(param)) {
            the_object->timeout = param;
            //}
            break;
         case BLOCKING:
            the_object->blocking = true;
            break;
         case NON_BLOCKING:
            the_object->blocking = false;
            break;
         default:
            printk("Unknown operation\n");
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


int re_write_buffer(char *buffer, size_t off, size_t len) {

   int i;
   if (len != 0) {
      for (i = 0; i < OBJECT_MAX_SIZE - len; i++) {
         buffer[i] = buffer[len+i];
      }
   }
   return 0;
}


bool test_float(const char *str)
{
    int len;
    float dummy = 0.0;
    if (sscanf(str, "%f %n", &dummy, &len) == 1 && len == (int)strlen(str))
        //printf("[%s] is valid (%.7g)\n", str, dummy);
        return true;
    else
        //printf("[%s] is not valid (%.7g)\n", str, dummy);
        return false;
}


int init_module(void) {

	int i;
   //int ret;

	//initialize the drive internal state
	for(i=0;i<MINORS;i++){
#ifdef SINGLE_SESSION_OBJECT
		mutex_init(&(objects[i].object_busy));
#endif
		mutex_init(&(objects[i].operation_synchronizer));
      objects[i].priority = HIGH_PRIORITY;
		objects[i].valid_bytes[0] = 0;
      objects[i].valid_bytes[1] = 0;
		objects[i].stream_content[0] = NULL;
		objects[i].stream_content[0] = (char*)__get_free_page(GFP_KERNEL);
      objects[i].stream_content[1] = NULL;
		objects[i].stream_content[1] = (char*)__get_free_page(GFP_KERNEL);
      objects[i].blocking = false;
      objects[i].timeout = 0.0;
		if ((objects[i].stream_content[0] == NULL) || (objects[i].stream_content[1] == NULL)){
         goto revert_allocation;
      }
	}

	Major = __register_chrdev(0, 0, 256, DEVICE_NAME, &fops);
	//actually allowed minors are directly controlled within this driver

	if (Major < 0) {
	   printk("%s: registering device failed\n",MODNAME);
	   return Major;
	}

	printk(KERN_INFO "%s: new device registered, it is assigned major number %d\n", MODNAME, Major);

	return 0;

revert_allocation:
	for(;i>=0;i--){
		free_page((unsigned long)objects[i].stream_content[0]);
      free_page((unsigned long)objects[i].stream_content[1]);
	}
	return -ENOMEM;
}


void cleanup_module(void) {

	int i;
	for(i=0;i<MINORS;i++){
		free_page((unsigned long)objects[i].stream_content[0]);
      free_page((unsigned long)objects[i].stream_content[1]);
	}

	unregister_chrdev(Major, DEVICE_NAME);

	printk(KERN_INFO "%s: new device unregistered, it was assigned major number %d\n", MODNAME, Major);

	return;
}
