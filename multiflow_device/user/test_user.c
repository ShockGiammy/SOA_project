#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>

int i;
char buff[4096];
#define DATA "ciao a tutti\n"
#define SIZE strlen(DATA)

void * the_thread(void* path){

	char* device;
	int fd;
	int ret;

	device = (char*)path;
	sleep(1);

	printf("opening device %s\n",device);
	fd = open(device,O_RDWR);
	if(fd == -1) {
		printf("open error on device %s\n",device);
		return NULL;
	}
	printf("device %s successfully opened\n",device);
	ioctl(fd, 1); 	//low_priority
	ioctl(fd, 3);	//blocking
	ioctl(fd, 5, 30000);	//timeout
	//ioctl(fd, 4);	//non-blocking
	//ioctl(fd, 0);	//high_priority
	ret = write(fd, DATA, SIZE);
	//ret = read(fd, buff, SIZE);
	printf("%d\n", ret);
	return NULL;

}
int main(int argc, char** argv){

	int ret;
	int major;
	int minors;
	char *path;
	int threads;

	if(argc<4){
		printf("useg: prog pathname #threads\n");
		return -1;
	}

	path = argv[1];
	major = strtol(argv[2],NULL,10);
	threads = strtol(argv[3],NULL,10);
	printf("creating %d thread for device %s with major %d\n", threads, path, major);
	//sprintf(buff,"mknod %s0 c %d 0\n", path, major);
	//system(buff);

	pthread_t tid[threads];

	for(i = 0; i < threads; i++){
		sprintf(buff,"%s0",path);
		pthread_create(&tid[i],NULL,the_thread,strdup(buff));
	}
	for (int i = 0; i < threads; i++) {
		pthread_join(tid[i], NULL);
	}
	return 0;

}
