/*  
 *  implementation of the multi-flow char device driver
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
#include <linux/string.h>

#include "structs.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(3, 13, 0)
//defined in order to put task in sleep with WQ_FLAG_EXCLUSIVE
#define __my_wait_event_timeout(wq_head, condition, timeout)			\
      ___wait_event(wq_head, ___wait_cond_timeout(condition),			\
         TASK_UNINTERRUPTIBLE, 1, timeout,				               \
         __ret = schedule_timeout(__ret))       

#define my_wait_event_timeout(wq_head, condition, timeout)				   \
({										                                          \
	long __ret = timeout;							                           \
	might_sleep();								                                 \
   if (!___wait_cond_timeout(condition))					               \
      __ret = __my_wait_event_timeout(wq_head, condition, timeout);	\
   __ret;							                                       \
})
#else
#define my_wait_event_timeout(wq, condition, timeout)       \
({                                                                   \
	wait_event_timeout(wq, condition, timeout);		 \
})     
#endif

#define control_awake_cond(condition, mutex)        \
({                                              \
   int __ret = 0;                               \
   if (mutex_trylock(mutex)) {                  \
      if (condition)   {                        \
         __ret = 1;                             \
      } else {                                  \
         mutex_unlock(mutex);                   \
      }                                         \
   }                                            \
   __ret;                                       \
})


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Francesco Quaglia");
MODULE_AUTHOR("Gian Marco Falcone");

#define MODNAME "MULTI-FLOW DEV"

#define AUDIT

#define MINORS 128
object_state objects[MINORS];

#define PAGE_DIM (4096) //the size of one page
#define MAX_PAGES (10)  //the max number of elements in the list


int enabled[MINORS];
module_param_array(enabled, int, NULL, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);   //0660

long high_priority_valid_bytes[MINORS];
module_param_array(high_priority_valid_bytes, long, NULL, S_IRUSR | S_IRGRP);    //0440

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


int goto_sleep_mutex(object_state *the_object, session_state *session){

	control_record data;
   control_record* control;
   int priority = session->priority;
   int ret;

   if(session->timeout == 0) {
      return -1;
   }

   control = &data;     //set the pointer to the current stack area

   AUDIT
   printk("%s: thread %d going to usleep for %lu millisecs\n", MODNAME, current->pid, session->timeout);

   //threads waiting for data or mutex
   if (priority == 0) {
      high_priority_waiting_threads[the_object->minor] += 1;
   }
   else {
      low_priority_waiting_threads[the_object->minor] += 1;
   }


   control->task = current;
   control->pid  = current->pid;
   control->minor = the_object->minor;
   control->priority = priority;


   //timeout is in jiffies = 10 milliseconds
   ret = my_wait_event_timeout(the_object->wait_queue[session->priority], 
      mutex_trylock(&(the_object->operation_synchronizer[session->priority])) == 1, session->timeout*HZ/1000);

   AUDIT
   printk("%s: thread %d exiting usleep\n",MODNAME, current->pid);

   if (priority == 0) {
      high_priority_waiting_threads[control->minor] -= 1;
   }
   else {
      low_priority_waiting_threads[control->minor] -= 1;
   }

   if (ret == 0) {
      return -1;
   }
   return 0;
}


int my_lock(object_state *the_object, session_state *session) {

   int ret = mutex_trylock(&(the_object->operation_synchronizer[session->priority]));
   if (ret != 1) {
      if (session->blocking && session->timeout != 0) {
         ret = goto_sleep_mutex(the_object, session);
         if (ret == -1) {
            printk("%s: The timeout elapsed\n", MODNAME);
            return -1;
         }
      }
      else {
         printk("%s: Device already in use\n", MODNAME);
         return -EBUSY;      //device is busy
      }
   }
   return 0;
}


int goto_sleep(session_state *session, int type, object_state *the_object, size_t len){

	control_record data;
   control_record* control;
   int priority = session->priority;

   if(session->timeout == 0) {
      return -1;
   }

   control = &data;     //set the pointer to the current stack area

   AUDIT
   printk("%s: thread %d going to usleep for %lu millisecs\n", MODNAME, current->pid, session->timeout);

   //Are taken into account both threads waiting to read and threads waiting to write.
   //It is very unlikely that both are present at the same time.
   if (priority == 0) {
      high_priority_waiting_threads[the_object->minor] += 1;
   }
   else {
      low_priority_waiting_threads[the_object->minor] += 1;
   }


   control->task = current;
   control->pid  = current->pid;
   control->minor = the_object->minor;
   control->priority = priority;

   if (type == SLEEP_READ) {
      //timeout is in jiffies = 10 millisecondi
      my_wait_event_timeout(the_object->wait_queue[control->priority], control_awake_cond(the_object->valid_bytes[priority] > 0,
            &(the_object->operation_synchronizer[control->priority])), session->timeout*HZ/1000);
   } else if ((type == SLEEP_WRITE) && (priority == LOW_PRIORITY)) {
      my_wait_event_timeout(the_object->wait_queue[control->priority], control_awake_cond(len <= (((PAGE_DIM * MAX_PAGES) - the_object->reserved_bytes) - the_object->valid_bytes[priority]), 
            &(the_object->operation_synchronizer[control->priority])), session->timeout*HZ/1000);
   }
   else if ((type == SLEEP_WRITE) && (priority == HIGH_PRIORITY)) {
      my_wait_event_timeout(the_object->wait_queue[control->priority], control_awake_cond(len <= ((PAGE_DIM * MAX_PAGES) - the_object->valid_bytes[priority]),
            &(the_object->operation_synchronizer[control->priority])), session->timeout*HZ/1000);
   }
   
   AUDIT
   printk("%s: thread %d exiting usleep\n",MODNAME, current->pid);

   if (control->priority == 0) {
      high_priority_waiting_threads[control->minor] -= 1;
   }
   else {
      low_priority_waiting_threads[control->minor] -= 1;
   }

   //different condition based on the type of the operation and the priority
   if ((type == READ && the_object->valid_bytes[control->priority] <= 0)
         || (type == WRITE && control->priority == 0 && len > ((PAGE_DIM * MAX_PAGES) - the_object->valid_bytes[0]))
         || (type == WRITE && control->priority == 1 && len > (((PAGE_DIM * MAX_PAGES) - the_object->reserved_bytes) - the_object->valid_bytes[1]))) {
      return -1;
   }
   return 0;
}


void asynchronous_write(unsigned long data){

   object_state *the_object = container_of((void*)data,packed_work,the_work)->the_object;
   const char *buff = container_of((void*)data,packed_work,the_work)->buffer;
   size_t len = container_of((void*)data,packed_work,the_work)->len;
   struct file *filp = container_of((void*)data,packed_work,the_work)->filp;
   int offset;
   int pages;
   list_stream* current_node;
   
   AUDIT{
   printk("%s: this print comes from kworker daemon with PID=%d - running on CPU-core %d\n", MODNAME, current->pid, smp_processor_id());
   printk("%s: releasing the task buffer at address %p \n",MODNAME, (void*)data);
   }

   printk("%s: somebody called a write on dev with [major,minor] number [%d,%d]\n",
            MODNAME, get_major(filp), get_minor(filp));

   //need to lock in any case, work queue allows blocking operations
   //we are sure about the low_priority flow
   mutex_lock(&(the_object->operation_synchronizer[1]));

   offset = (the_object->valid_bytes[1] + the_object->offset[1]) % PAGE_DIM;
   pages = (the_object->valid_bytes[1] + the_object->offset[1]) / PAGE_DIM;

   current_node = the_object->stream_content[1];
   while (pages > 0) {
      current_node = current_node->next;
      pages--;
   }

   if (len + offset >= (PAGE_DIM)) {      //the page is a the end
      //the new memory space has already been allocated

      //as many bytes are written as necessary to fill the prev buffer
      memcpy(&(current_node->buffer[offset]), buff, PAGE_DIM - offset);

      //the remaining bytes are written from the beginning of the new buffer
      current_node = current_node->next;
      while(current_node->next != NULL) {
         memcpy(&(current_node->buffer[0]), &buff[PAGE_DIM - offset + (pages * PAGE_DIM)], PAGE_DIM);
         current_node = current_node->next;
         pages++;
      }

      memcpy(&(current_node->buffer[0]), &buff[PAGE_DIM - offset + (pages * PAGE_DIM)], len - (PAGE_DIM - offset) - (pages * PAGE_DIM));


   }
   else {
      memcpy(&(current_node->buffer[offset]), buff, len);
   }  

   the_object->valid_bytes[1] += len;
   low_priority_valid_bytes[get_minor(filp)] += len;
   the_object->reserved_bytes -= len;

   mutex_unlock(&(the_object->operation_synchronizer[1]));
   //only the first thread in the queue wakes up (state exclusive)
   wake_up(&the_object->wait_queue[1]);

   kfree((void*)buff);
   kfree(container_of((void*)data, packed_work, the_work));
   module_put(THIS_MODULE);
   return;
}


int put_work(object_state *the_object, char *buff, size_t len, struct file *filp){
   
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

   the_task->struct_addr = the_task;
   the_task->buffer = buff;
   the_task->the_object = the_object;
   the_task->len = len;
   the_task->filp = filp;

   AUDIT
   printk("%s: work buffer allocation success - address is %p\n", MODNAME, the_task);

   __INIT_WORK(&(the_task->the_work), (void*)asynchronous_write, (unsigned long)(&(the_task->the_work)));
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

   if (enabled[minor] == 0) {    //device file is disabled
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

   kfree(file->private_data);

   printk("%s: device file closed\n",MODNAME);
   //device closed by default nop
   return 0;
}


static ssize_t dev_write(struct file *filp, const char *buff, size_t len, loff_t *off) {

   int minor = get_minor(filp);
   int ret;
   int ret_copy;
   int offset;
   object_state *the_object;
   session_state *session = (session_state *)filp->private_data;
   list_stream* current_node;
   list_stream* new_node;
   list_stream* temp_node;
   char* temp_buff;
   int new_pages;

   int pages = 0;
   the_object = objects + minor;
   
   printk("%s: somebody called a write on dev with [major,minor] number [%d,%d]\n",
            MODNAME, get_major(filp), get_minor(filp));

   temp_buff = (char*)kmalloc(len, GFP_ATOMIC);     //non blocking memory allocation
   ret_copy = copy_from_user(temp_buff, buff, len);

   //need to lock in any case
   ret = my_lock(the_object, session);
   if (ret != 0) {
      kfree((void*)temp_buff);
      return -EBUSY;
   }

   //field reserved_bytes only matters for low_priority flow
   if ((session->priority == HIGH_PRIORITY && ((PAGE_DIM * MAX_PAGES) - the_object->valid_bytes[session->priority]) < len) ||
      (session->priority == LOW_PRIORITY && (((PAGE_DIM * MAX_PAGES) - the_object->reserved_bytes) - the_object->valid_bytes[session->priority]) < len)) {
      mutex_unlock(&(the_object->operation_synchronizer[session->priority]));
      if (session->blocking && session->timeout != 0) {
         ret = goto_sleep(session, SLEEP_WRITE, the_object, len);
         if (ret == -1) {
            printk("%s: The timeout elapsed and there are not enough available space\n", MODNAME);
            kfree((void*)temp_buff);
            return 0;      //no enough data on device
         }
      }
      else {
         kfree((void*)temp_buff);
         return -ENOSPC;      //no space left on device
      }
   }
   offset = (the_object->valid_bytes[session->priority] + the_object->offset[session->priority]) % PAGE_DIM;

   current_node = the_object->stream_content[session->priority];
   while (current_node->next != NULL) {
      pages +=1;
      current_node = current_node->next;
   }

   if (len + offset >= (PAGE_DIM)) {      //the page is a the end

      new_pages = (len + offset) / PAGE_DIM;
      //new elements must be allocated for the list for the current flow, compliant with variable MAX_PAGES
      if (pages + new_pages >= MAX_PAGES) {
         printk("%s: The memory reserved for the buffer is terminated\n", MODNAME);
         mutex_unlock(&(the_object->operation_synchronizer[session->priority]));
         kfree((void*)temp_buff);
         return -ENOSPC; 
      }

      temp_node = current_node;
      while (new_pages > 0) {
         new_node = kzalloc(sizeof(list_stream), GFP_ATOMIC);     //non blocking memory allocation
         new_node->buffer = (char*)__get_free_page(GFP_ATOMIC);
         //checking that memory allocation does not fail
         if ((new_node == NULL) || (new_node->buffer == NULL)) {
            free_page((unsigned long)new_node->buffer);
            kfree((void*)new_node);
            //if fails, all the new allocated buffers have to be deleted
            while(current_node->next != NULL) {
               temp_node = current_node->next;
               current_node->next = temp_node->next;
               free_page((unsigned long)temp_node->buffer);
               kfree((void*)temp_node);
            }
            kfree((void*)temp_buff);
            mutex_unlock(&(the_object->operation_synchronizer[session->priority]));
            return -ENOMEM; 
         }
      
         new_node->prev = temp_node;
         new_node->next = NULL;
         temp_node->next = new_node;
         temp_node = new_node;
         new_pages--;
      }

      if (session->priority == HIGH_PRIORITY) {

         //as many bytes are written as necessary to fill the prev buffer
         memcpy(&(current_node->buffer[offset]), temp_buff, PAGE_DIM - offset);

         //the remaining bytes are written from the beginning of the new buffers
         current_node = current_node->next;
         while(current_node->next != NULL) {
            memcpy(&(current_node->buffer[0]), &temp_buff[PAGE_DIM - offset + (new_pages * PAGE_DIM)], PAGE_DIM);
            current_node = current_node->next;
            new_pages++;
         }

         memcpy(&(current_node->buffer[0]), &temp_buff[PAGE_DIM - offset + (new_pages * PAGE_DIM)], len - (PAGE_DIM - offset) - (new_pages * PAGE_DIM));

         the_object->valid_bytes[session->priority] += (len - ret_copy);
         high_priority_valid_bytes[minor] += (len - ret_copy);

         kfree((void*)temp_buff);
      }
      else {
         //I need to reserve the bytes for the asynchronous write to be sure that will be available space
         the_object->reserved_bytes += len;
         mutex_unlock(&(the_object->operation_synchronizer[session->priority]));
         ret = put_work(the_object, temp_buff, len, filp);
         if (ret != 0) {
            printk("%s: There was an error with deferred work\n", MODNAME);
         }
         //only the first thread in the queue wakes up (state exclusive)
         wake_up(&the_object->wait_queue[session->priority]);
         return len - ret_copy;
      }  
   }
   else {
      if (session->priority == HIGH_PRIORITY) {
         memcpy(&(current_node->buffer[offset]), temp_buff, len);

         the_object->valid_bytes[session->priority] += (len - ret_copy);
         high_priority_valid_bytes[minor] += (len - ret_copy);

         kfree((void*)temp_buff);
      }
      else {
         //I need to reserve the bytes for the asynchronous write to be sure that will be available space
         the_object->reserved_bytes += len;
         mutex_unlock(&(the_object->operation_synchronizer[session->priority]));
         ret = put_work(the_object, temp_buff, len, filp);
         if (ret != 0) {
            printk("%s: There was an error with deferred work\n", MODNAME);
         }
         //only the first thread in the queue wakes up (state exclusive)
         wake_up(&the_object->wait_queue[session->priority]);
         return len - ret_copy;
      }  
   }

   mutex_unlock(&(the_object->operation_synchronizer[session->priority]));
   //only the first thread in the queue wakes up (state exclusive)
   wake_up(&the_object->wait_queue[session->priority]);

   return len - ret_copy;
}


static ssize_t dev_read(struct file *filp, char *buff, size_t len, loff_t *off) {

   int minor = get_minor(filp);
   int ret;
   object_state *the_object;
   list_stream* current_node;
   char *temp_buff;
   int pages_to_read;
   int pages_read = 0;

   session_state *session = (session_state *)filp->private_data;

   the_object = objects + minor;
   printk("%s: somebody called a read on dev with [major,minor] number [%d,%d]\n",
            MODNAME, get_major(filp), get_minor(filp));

   temp_buff = (char*)kmalloc(len, GFP_ATOMIC);     //non blocking memory allocation

   //verify that the len requested fits the limits of the user buffer. 
   len = len - clear_user(buff, len);

   //need to lock in any case
   ret = my_lock(the_object, session);
   if (ret != 0) {
      kfree((void*)temp_buff);
      return -EBUSY;
   }

   current_node = the_object->stream_content[session->priority];

   if (the_object->valid_bytes[session->priority] == 0) {
      if (session->blocking && session->timeout != 0) {
         mutex_unlock(&(the_object->operation_synchronizer[session->priority]));
         ret = goto_sleep(session, SLEEP_READ, the_object, len);
         if (ret == -1) {
            printk("%s: The timeout elapsed and there are not enough data to read\n", MODNAME);
            return 0;      //no enough data on device
         }
      }
      else {
         printk("%s: Not enough data to read\n", MODNAME);
         mutex_unlock(&(the_object->operation_synchronizer[session->priority]));
         kfree((void*)temp_buff);
         return 0;      //no enough data on device
      }
   }
   if (len > the_object->valid_bytes[session->priority]) {
      len = the_object->valid_bytes[session->priority];
   }

   if (the_object->offset[session->priority] + len >= (PAGE_DIM)) {

      pages_to_read = ((the_object->offset[session->priority] + len) / PAGE_DIM) - 1;

      memcpy(temp_buff, &(current_node->buffer[the_object->offset[session->priority]]), PAGE_DIM - the_object->offset[session->priority]);

      while (pages_read < pages_to_read) {
         memcpy(&temp_buff[(PAGE_DIM - the_object->offset[session->priority]) + (pages_read * PAGE_DIM)],
            &(current_node->next->buffer[0]), PAGE_DIM);

         //it is possible to deallocate the previous buffers (data are completely read)
         the_object->stream_content[session->priority] = current_node->next;
         current_node->next->prev = NULL;
         free_page((unsigned long)current_node->buffer);
         kfree((void*)current_node);

         current_node = the_object->stream_content[session->priority];
         pages_read++;
      }

      memcpy(&temp_buff[(PAGE_DIM - the_object->offset[session->priority]) + (pages_read * PAGE_DIM)], &(current_node->next->buffer[0]), 
         len - (PAGE_DIM - the_object->offset[session->priority]));
      
      //it is possible to deallocate the previous buffers (data are completely read)
      the_object->stream_content[session->priority] = current_node->next;
      current_node->next->prev = NULL;
      free_page((unsigned long)current_node->buffer);
      kfree((void*)current_node);
   } 
   else {
      memcpy(temp_buff, &(current_node->buffer[the_object->offset[session->priority]]), len);
   }

   the_object->valid_bytes[session->priority] -= (len - ret);
   the_object->offset[session->priority] = (the_object->offset[session->priority] + (len - ret)) % PAGE_DIM;

   if (session->priority == 0) {
      high_priority_valid_bytes[minor] -= (len - ret);
   }
   else {
      low_priority_valid_bytes[minor] -= (len - ret);
   }

   mutex_unlock(&(the_object->operation_synchronizer[session->priority]));

   ret = copy_to_user(buff, temp_buff, len);
   kfree((void*)temp_buff);

   //only the first thread in the queue wakes up (state exclusive)
   wake_up(&the_object->wait_queue[session->priority]);
   
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
   .owner = THIS_MODULE,   //do not forget this
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

	//initialize the driver internal state
	for(i = 0; i < MINORS; i++){
  
      //Initialize wait queues
      init_waitqueue_head(&objects[i].wait_queue[0]); //a high_priority queue for each minor
      init_waitqueue_head(&objects[i].wait_queue[1]); //a low_priority queue for each minor

      high_priority_content = kzalloc(sizeof(list_stream), GFP_KERNEL);     //blocking memory allocation
      high_priority_content->buffer = (char*)__get_free_page(GFP_KERNEL);
      high_priority_content->prev = NULL;
      high_priority_content->next = NULL;

      low_priority_content = kzalloc(sizeof(list_stream), GFP_KERNEL);     //blocking memory allocation
      low_priority_content->buffer = (char*)__get_free_page(GFP_KERNEL);
      low_priority_content->prev = NULL;
      low_priority_content->next = NULL;

      if ((high_priority_content == NULL) || (low_priority_content == NULL) 
            || (high_priority_content->buffer == NULL) || (low_priority_content->buffer == NULL)){
         goto revert_allocation;
      }

#ifdef SINGLE_SESSION_OBJECT
		mutex_init(&(objects[i].object_busy));
#endif

      objects[i].minor = i;
		mutex_init(&(objects[i].operation_synchronizer[0]));
      mutex_init(&(objects[i].operation_synchronizer[1]));
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

	Major = __register_chrdev(0, 0, 256, DEVICE_NAME, &fops);
	//actually allowed minors are directly controlled within this driver

	if (Major < 0) {
	   printk("%s: registering device failed\n",MODNAME);
	   return Major;
	}

	printk(KERN_INFO "%s: new device registered, it is assigned major number %d\n", MODNAME, Major);

	return 0;

revert_allocation:
	for(; i >= 0; i--){
		free_page((unsigned long)objects[i].stream_content[0]->buffer);
      free_page((unsigned long)objects[i].stream_content[1]->buffer);
      kfree((void*)objects[i].stream_content[0]);
      kfree((void*)objects[i].stream_content[1]);
	}
	return -ENOMEM;
}


void cleanup_module(void) {

	int i;
   list_stream* current_page;
	for(i = 0; i < MINORS; i++){
      current_page = objects[i].stream_content[0];
      while (current_page->next != NULL) {
         current_page = current_page->next;
         free_page((unsigned long)current_page->prev->buffer);
         kfree((void*)current_page->prev);
      }
      free_page((unsigned long)current_page->buffer);
      kfree((void*)current_page);
      
      current_page = objects[i].stream_content[1];
      while (current_page->next != NULL) {
         current_page = current_page->next;
         free_page((unsigned long)current_page->prev->buffer);
         kfree((void*)current_page->prev);
      }
      free_page((unsigned long)current_page->buffer); 
      kfree((void*)current_page);
	}

	unregister_chrdev(Major, DEVICE_NAME);

	printk(KERN_INFO "%s: new device unregistered, it was assigned major number %d\n", MODNAME, Major);

	return;
}
